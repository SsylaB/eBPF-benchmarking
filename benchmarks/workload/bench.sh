#!/usr/bin/env bash
# bench.sh — Full benchmark automation
# =====================================
# Launches a workload under different CPU affinities, attaches the csw tracer,
# collects CSV data, and saves results per run.
#
# Usage:
#   sudo ./bench.sh [OPTIONS]
#
# Options:
#   -w WORKLOAD   : stress-ng | sysbench | dcperf | CUSTOM_CMD (default: stress-ng)
#   -c CPUSETS    : comma-separated list of taskset CPU specs (default: "0,0-1,0-3,0-7")
#   -d DURATION   : seconds per run (default: 30)
#   -o OUTDIR     : output directory (default: results/)
#   -n TRIALS     : number of trials per CPU spec (default: 1)
#   -p PLOT       : run analyze.py after all runs (default: yes)
#   -h            : show this help
#
# After completion, run:
#   python3 analyze.py --outdir plots/ $(ls results/*.csv | \
#     awk '{printf "--csv %s --label %s ", $1, $1}')
#
set -euo pipefail

# ---------------------------------------------------------------------------
# Defaults
# ---------------------------------------------------------------------------
WORKLOAD="stress-ng"
CPUSETS="0,0-1,0-3,0-7"
DURATION=30
OUTDIR="results"
TRIALS=1
RUN_PLOT=yes
CSW_BIN="./csw"

# ---------------------------------------------------------------------------
# Parse args
# ---------------------------------------------------------------------------
usage() {
    grep '^#' "$0" | grep -v '#!/' | sed 's/^# \?//'
    exit 0
}

while getopts "w:c:d:o:n:p:h" opt; do
    case $opt in
        w) WORKLOAD="$OPTARG" ;;
        c) CPUSETS="$OPTARG"  ;;
        d) DURATION="$OPTARG" ;;
        o) OUTDIR="$OPTARG"   ;;
        n) TRIALS="$OPTARG"   ;;
        p) RUN_PLOT="$OPTARG" ;;
        h) usage ;;
        *) usage ;;
    esac
done

mkdir -p "$OUTDIR"

# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------
log()  { echo "[bench] $*"; }
warn() { echo "[bench] WARN: $*" >&2; }
die()  { echo "[bench] ERROR: $*" >&2; exit 1; }

require_root() {
    [[ $EUID -eq 0 ]] || die "Must run as root (sudo ./bench.sh ...)"
}

check_deps() {
    for dep in taskset "$CSW_BIN"; do
        command -v "$dep" &>/dev/null || die "Dependency not found: $dep"
    done
}

count_cpus_in_spec() {
    # Returns number of CPUs in a taskset spec like "0-3" → 4, "0,1" → 2
    local spec="$1"
    # Use taskset/nproc to expand ranges
    local tmp
    tmp=$(taskset -c "$spec" sh -c 'cat /proc/self/status' 2>/dev/null \
          | awk '/^Cpus_allowed_list:/ {print $2}') || echo "$spec"
    # Count by expanding the list
    python3 -c "
import sys
s='$spec'
total=0
for part in s.split(','):
    if '-' in part:
        a,b = part.split('-')
        total += int(b)-int(a)+1
    else:
        total += 1
print(total)
"
}

# ---------------------------------------------------------------------------
# Workload launchers
# Each function must:
#   1. Launch the workload in the background using taskset
#   2. Print the PID on stdout (just the number)
#   3. The calling code stores it in $WORKLOAD_PID
# ---------------------------------------------------------------------------

launch_stress_ng() {
    local cpuset="$1" dur="$2"
    taskset -c "$cpuset" stress-ng --cpu 1 --timeout "${dur}s" \
        --metrics-brief 2>/dev/null &
    echo $!
}

launch_sysbench() {
    local cpuset="$1" dur="$2"
    taskset -c "$cpuset" sysbench cpu \
        --cpu-max-prime=20000 \
        --time="$dur" \
        run &>/dev/null &
    echo $!
}

launch_dcperf() {
    local cpuset="$1" dur="$2"
    # Adjust path to your DCPerf binary / runner
    local dcperf_bin="${DCPERF_BIN:-./dcperf}"
    [[ -x "$dcperf_bin" ]] || die "DCPerf not found at $dcperf_bin. Set DCPERF_BIN."
    taskset -c "$cpuset" "$dcperf_bin" &>/dev/null &
    echo $!
}

launch_workload() {
    local cpuset="$1" dur="$2"
    case "$WORKLOAD" in
        stress-ng) launch_stress_ng "$cpuset" "$dur" ;;
        sysbench)  launch_sysbench  "$cpuset" "$dur" ;;
        dcperf)    launch_dcperf    "$cpuset" "$dur" ;;
        *)
            # Custom command: eval with taskset prefix
            taskset -c "$cpuset" eval "$WORKLOAD" &>/dev/null &
            echo $!
            ;;
    esac
}

# ---------------------------------------------------------------------------
# Wait for PID to actually be running (up to 2 seconds)
# ---------------------------------------------------------------------------
wait_for_pid() {
    local pid="$1"
    local retries=20
    while ! kill -0 "$pid" 2>/dev/null; do
        sleep 0.1
        retries=$((retries - 1))
        [[ $retries -gt 0 ]] || die "Workload PID $pid never became live"
    done
}

# FIX: Function to find the actual child worker PID
get_worker_pid() {
    local parent_pid="$1"
    # Wait for child to appear
    for i in {1..10}; do
        local child_pid
        child_pid=$(pgrep -P "$parent_pid" | head -n 1)
        if [[ -n "$child_pid" ]]; then
            echo "$child_pid"
            return 0
        fi
        sleep 0.1
    done
    echo "$parent_pid" # Fallback to parent
}

# ---------------------------------------------------------------------------
# Main benchmark loop
# ---------------------------------------------------------------------------
require_root
check_deps

IFS=',' read -ra CPU_SPECS <<< "$CPUSETS"

log "Workload   : $WORKLOAD"
log "CPU specs  : ${CPU_SPECS[*]}"
log "Duration   : ${DURATION}s per run"
log "Trials     : $TRIALS"
log "Output     : $OUTDIR/"
echo ""

CSV_FILES=()
LABELS=()

for cpuset in "${CPU_SPECS[@]}"; do
    n_cpu=$(count_cpus_in_spec "$cpuset")
    log "=== CPU spec: $cpuset  ($n_cpu CPUs) ==="

    for trial in $(seq 1 "$TRIALS"); do
        run_tag="${n_cpu}cpu_t${trial}"
        csv_file="$OUTDIR/${run_tag}.csv"

        log "  Trial $trial/$TRIALS → $csv_file"
        
        # DEBUG: Check if workload started
        wait_for_pid "$WORKLOAD_PID"
        
        # FIX: Find the actual worker PID if using stress-ng
        ACTUAL_PID=$(get_worker_pid "$WORKLOAD_PID")
        log "  Workload Parent: $WORKLOAD_PID | Target Worker: $ACTUAL_PID"

        # 1. Launch workload
        WORKLOAD_PID=$(launch_workload "$cpuset" "$DURATION")
        wait_for_pid "$WORKLOAD_PID"
        log "  Workload PID: $WORKLOAD_PID"

        # 2. Launch tracer targeting the worker
        "$CSW_BIN" -p "$ACTUAL_PID" -d "$DURATION" -o "$csv_file" \
            2>"$OUTDIR/${run_tag}_csw.log" &
        TRACER_PID=$!

        # 3. Wait for workload to finish
        wait "$WORKLOAD_PID" 2>/dev/null || true
        log "  Workload done"

        # 4. Give tracer a moment to flush, then stop it
        sleep 0.5
        kill "$TRACER_PID" 2>/dev/null || true
        wait "$TRACER_PID" 2>/dev/null || true
        log "  Tracer done"

        # 5. Verify CSV was written
        if [[ -f "$csv_file" ]]; then
            n_rows=$(wc -l < "$csv_file")
            log "  Events: $((n_rows - 1)) rows"
            CSV_FILES+=("$csv_file")
            LABELS+=("$n_cpu")
        else
            warn "CSV not produced for $run_tag"
        fi

        # Small gap between trials
        [[ $trial -lt $TRIALS ]] && sleep 2
    done

    echo ""
done

# ---------------------------------------------------------------------------
# Summary
# ---------------------------------------------------------------------------
log "All runs complete."
log "CSV files:"
for f in "${CSV_FILES[@]}"; do
    echo "    $f"
done

# ---------------------------------------------------------------------------
# Optional: run analysis
# ---------------------------------------------------------------------------
if [[ "$RUN_PLOT" == "yes" ]] && command -v python3 &>/dev/null; then
    log "Running analyze.py…"
    analyze_args=()
    for i in "${!CSV_FILES[@]}"; do
        analyze_args+=("--csv" "${CSV_FILES[$i]}" "--label" "${LABELS[$i]}")
    done
    python3 analyze.py "${analyze_args[@]}" --outdir "$OUTDIR/plots" || \
        warn "analyze.py failed (non-fatal)"
fi

log "Done. Results in $OUTDIR/"