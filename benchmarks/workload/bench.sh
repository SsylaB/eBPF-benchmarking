#!/usr/bin/env bash
# bench.sh — Redis uprobe benchmark automation
set -euo pipefail

CPUSETS="0,0-1,0-3,0-7"
DURATION=30
OUTDIR="results"
TRIALS=1
BINARY="/usr/bin/redis-server"
CSW_BIN="./csw"
RUN_PLOT=yes

usage() { grep '^#' "$0" | grep -v '#!/' | sed 's/^# \?//'; exit 0; }
log()   { echo "[bench] $*"; }
warn()  { echo "[bench] WARN: $*" >&2; }
die()   { echo "[bench] ERROR: $*" >&2; exit 1; }

# --- The missing argument parser! ---
while getopts "c:d:o:n:b:p:h" opt; do
    case $opt in
        c) CPUSETS="$OPTARG"  ;;
        d) DURATION="$OPTARG" ;;
        o) OUTDIR="$OPTARG"   ;;
        n) TRIALS="$OPTARG"   ;;
        b) BINARY="$OPTARG"   ;;
        p) RUN_PLOT="$OPTARG" ;;
        h) usage ;;
        *) usage ;;
    esac
done

[[ $EUID -eq 0 ]] || die "Must run as root (sudo ./bench.sh)"
command -v "$CSW_BIN" &>/dev/null || die "$CSW_BIN not found"
command -v "$BINARY"  &>/dev/null || die "$BINARY not found"
command -v python3    &>/dev/null || die "python3 not found"

mkdir -p "$OUTDIR"

log "Finding processCommand offset in $BINARY ..."
OFFSETS=$(python3 find_redis_offsets.py "$BINARY") || die "find_redis_offsets.py failed"
eval "$OFFSETS"
log "  FUNC_OFFSET=$FUNC_OFFSET"

# Helper to count how many CPUs are in a set (e.g. "0-3" = 4)
count_cpus() {
    python3 -c "
s='$1'; total=0
for p in s.split(','):
    a,b = (p.split('-')+[p.split('-')[0]])[:2]
    total += int(b)-int(a)+1
print(total)
"
}

IFS=',' read -ra CPU_SPECS <<< "$CPUSETS"
CSV_FILES=(); LABELS=()

for cpuset in "${CPU_SPECS[@]}"; do
    n_cpu=$(count_cpus "$cpuset")
    log "=== CPU spec: $cpuset  ($n_cpu CPUs) ==="

    for trial in $(seq 1 "$TRIALS"); do
        run_tag="${n_cpu}cpu_t${trial}"
        csv_file="$OUTDIR/${run_tag}.csv"
        
        # 1. Launch Redis server on specific CPU
        taskset -c "$cpuset" "$BINARY" --port 6379 --save "" --appendonly no >/dev/null 2>&1 &
        WORKER_PID=$!
        sleep 1 # Wait for it to bind to port
        
        # 2. Launch redis-benchmark to blast it with traffic (in background)
        redis-benchmark -h 127.0.0.1 -p 6379 -t set,get -c 50 -n 100000000 -q >/dev/null 2>&1 &
        BENCH_PID=$!

        log "  Tracing Redis PID: $WORKER_PID (Load generator PID: $BENCH_PID) -> $csv_file"

        # 3. Start tracer
        sudo "$CSW_BIN" \
            -p "$WORKER_PID" \
            -f "$FUNC_OFFSET" \
            -b "$BINARY" \
            -d "$DURATION" \
            -o "$csv_file" \
            2>"$OUTDIR/${run_tag}_csw.log"

        # 4. Clean up processes
        kill "$BENCH_PID" 2>/dev/null || true
        kill "$WORKER_PID" 2>/dev/null || true
        wait "$WORKER_PID" 2>/dev/null || true

        if [[ -f "$csv_file" ]]; then
            CSV_FILES+=("$csv_file"); LABELS+=("$n_cpu")
        fi
        sleep 1
    done
done

if [[ "$RUN_PLOT" == "yes" ]]; then
    log "Running analyze.py..."
    analyze_args=()
    for i in "${!CSV_FILES[@]}"; do analyze_args+=("--csv" "${CSV_FILES[$i]}" "--label" "${LABELS[$i]}"); done
    python3 analyze.py "${analyze_args[@]}" --outdir "$OUTDIR/plots"
fi

log "Done. Results in $OUTDIR/"