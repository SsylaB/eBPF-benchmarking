/*
 * csw.c — userspace loader for the off-CPU / context-switch benchmark.
 *
 * Usage:
 *   sudo ./csw -p <PID> [-d <duration_s>] [-o <output.csv>]
 *
 * Output CSV columns:
 *   timestamp_ns, pid, cpu, event_type, duration_ns, prev_cpu, comm
 *
 * Live summary is printed to stderr every second.
 * Final metrics are printed to stdout when the run ends.
 */

#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <unistd.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>
#include <time.h>
#include <math.h>
#include <getopt.h>

#include <bpf/libbpf.h>
#include "csw.skel.h"

#define TASK_COMM_LEN  16
#define MAX_LATENCIES  (1 << 22)   /* 4M samples max in-memory for percentiles */
#define PRINT_INTERVAL_S 1

/* ----------------------------------------------------------------------- */
/* Mirrored event struct — must match csw.bpf.c exactly                    */
/* ----------------------------------------------------------------------- */
#define EVT_OFFCPU   0
#define EVT_ONCPU    1
#define EVT_MIGRATE  2

struct event {
    __u64 timestamp_ns;
    __u32 pid;
    __u32 cpu;
    __u8  type;
    __u8  pad[3];
    __u64 duration_ns;
    __u32 prev_cpu;
    char  comm[TASK_COMM_LEN];
};

/* ----------------------------------------------------------------------- */
/* Global state                                                             */
/* ----------------------------------------------------------------------- */
static volatile int g_running = 1;

static struct {
    /* counters */
    __u64 n_offcpu;       /* number of off-CPU events (= stalls)           */
    __u64 n_oncpu;        /* number of on-CPU slices                       */
    __u64 n_migrate;      /* number of CPU migrations                      */

    /* time accumulators (ns) */
    __u64 total_offcpu_ns;
    __u64 total_oncpu_ns;

    /* latency sample store for percentiles */
    __u64 *offcpu_samples;
    __u32  n_offcpu_samples;

    /* observation window */
    __u64 start_ns;       /* first event timestamp                         */
    __u64 last_ns;        /* most recent event timestamp                   */

    /* live display state */
    time_t last_print;
    __u64  last_n_offcpu;
} g_stats;

static FILE   *g_csv   = NULL;
static int     g_target_pid = 0;
static int     g_duration   = 0;   /* 0 = run until Ctrl+C */

/* ----------------------------------------------------------------------- */
/* Helpers                                                                  */
/* ----------------------------------------------------------------------- */
static void sig_handler(int sig) { g_running = 0; }

static const char *evt_name(__u8 type)
{
    switch (type) {
    case EVT_OFFCPU:  return "OFFCPU";
    case EVT_ONCPU:   return "ONCPU";
    case EVT_MIGRATE: return "MIGRATE";
    default:          return "UNKNOWN";
    }
}

static int cmp_u64(const void *a, const void *b)
{
    __u64 x = *(__u64 *)a, y = *(__u64 *)b;
    return (x > y) - (x < y);
}

static __u64 percentile(__u64 *sorted, __u32 n, double pct)
{
    if (n == 0) return 0;
    __u32 idx = (__u32)(pct / 100.0 * (n - 1) + 0.5);
    if (idx >= n) idx = n - 1;
    return sorted[idx];
}

static void print_live_stats(void)
{
    __u64 elapsed_ns = g_stats.last_ns - g_stats.start_ns;
    if (elapsed_ns == 0) return;

    double elapsed_s   = elapsed_ns / 1e9;
    double stall_hz    = g_stats.n_offcpu / elapsed_s;
    double migrate_hz  = g_stats.n_migrate / elapsed_s;

    double run_frac = 0.0;
    __u64  total_ns = g_stats.total_offcpu_ns + g_stats.total_oncpu_ns;
    if (total_ns > 0)
        run_frac = (double)g_stats.total_oncpu_ns / total_ns * 100.0;

    double avg_offcpu_us = 0.0;
    if (g_stats.n_offcpu > 0)
        avg_offcpu_us = (double)g_stats.total_offcpu_ns / g_stats.n_offcpu / 1000.0;

    fprintf(stderr,
        "\r[%5.1fs] C=%.1f%% | stalls=%llu (%.0f/s) | "
        "avg_offcpu=%.1f us | migrations=%llu (%.1f/s)   ",
        elapsed_s,
        run_frac,
        (unsigned long long)g_stats.n_offcpu,
        stall_hz,
        avg_offcpu_us,
        (unsigned long long)g_stats.n_migrate,
        migrate_hz);
    fflush(stderr);
}

static void print_final_report(void)
{
    __u64 elapsed_ns = g_stats.last_ns - g_stats.start_ns;
    if (elapsed_ns == 0) {
        fprintf(stderr, "\nNo events captured.\n");
        return;
    }

    double elapsed_s = elapsed_ns / 1e9;

    /* Sort samples for percentiles */
    if (g_stats.n_offcpu_samples > 0) {
        qsort(g_stats.offcpu_samples,
              g_stats.n_offcpu_samples,
              sizeof(__u64),
              cmp_u64);
    }

    __u64 p50   = percentile(g_stats.offcpu_samples, g_stats.n_offcpu_samples, 50.0);
    __u64 p95   = percentile(g_stats.offcpu_samples, g_stats.n_offcpu_samples, 95.0);
    __u64 p99   = percentile(g_stats.offcpu_samples, g_stats.n_offcpu_samples, 99.0);
    __u64 p999  = percentile(g_stats.offcpu_samples, g_stats.n_offcpu_samples, 99.9);

    double avg_offcpu_us = 0.0;
    if (g_stats.n_offcpu > 0)
        avg_offcpu_us = (double)g_stats.total_offcpu_ns / g_stats.n_offcpu / 1000.0;

    double run_frac = 0.0;
    __u64  total_ns = g_stats.total_offcpu_ns + g_stats.total_oncpu_ns;
    if (total_ns > 0)
        run_frac = (double)g_stats.total_oncpu_ns / total_ns;

    fprintf(stderr, "\n\n");
    fprintf(stdout,
        "=== csw benchmark report ===\n"
        "PID              : %d\n"
        "Observation time : %.3f s\n"
        "\n"
        "--- Continuity ---\n"
        "  Continuity ratio C : %.4f  (%.1f%%)\n"
        "  Total on-CPU       : %.3f s\n"
        "  Total off-CPU      : %.3f s\n"
        "\n"
        "--- Stall frequency ---\n"
        "  Context switches   : %llu\n"
        "  Stall rate         : %.2f /s\n"
        "\n"
        "--- Off-CPU latency ---\n"
        "  avg    : %.1f us\n"
        "  p50    : %.1f us\n"
        "  p95    : %.1f us\n"
        "  p99    : %.1f us\n"
        "  p99.9  : %.1f us\n"
        "\n"
        "--- CPU migration ---\n"
        "  migrations         : %llu\n"
        "  migration rate     : %.2f /s\n"
        "============================\n",
        g_target_pid,
        elapsed_s,
        run_frac, run_frac * 100.0,
        (double)g_stats.total_oncpu_ns / 1e9,
        (double)g_stats.total_offcpu_ns / 1e9,
        (unsigned long long)g_stats.n_offcpu,
        g_stats.n_offcpu / elapsed_s,
        avg_offcpu_us,
        p50  / 1000.0,
        p95  / 1000.0,
        p99  / 1000.0,
        p999 / 1000.0,
        (unsigned long long)g_stats.n_migrate,
        g_stats.n_migrate / elapsed_s);
}

/* ----------------------------------------------------------------------- */
/* Ring-buffer callback                                                     */
/* ----------------------------------------------------------------------- */
static int handle_event(void *ctx, void *data, size_t data_sz)
{
    const struct event *e = data;

    /* Track observation window */
    if (g_stats.start_ns == 0)
        g_stats.start_ns = e->timestamp_ns;
    g_stats.last_ns = e->timestamp_ns;

    /* Accumulate stats */
    switch (e->type) {
    case EVT_OFFCPU:
        g_stats.n_offcpu++;
        g_stats.total_offcpu_ns += e->duration_ns;
        /* Store sample for percentile computation */
        if (g_stats.n_offcpu_samples < MAX_LATENCIES) {
            g_stats.offcpu_samples[g_stats.n_offcpu_samples++] = e->duration_ns;
        }
        break;
    case EVT_ONCPU:
        g_stats.n_oncpu++;
        g_stats.total_oncpu_ns += e->duration_ns;
        break;
    case EVT_MIGRATE:
        g_stats.n_migrate++;
        break;
    }

    /* Write CSV row */
    if (g_csv) {
        fprintf(g_csv, "%llu,%u,%u,%s,%llu,%u,%s\n",
                (unsigned long long)e->timestamp_ns,
                e->pid,
                e->cpu,
                evt_name(e->type),
                (unsigned long long)e->duration_ns,
                e->prev_cpu,
                e->comm);
    }

    /* Live stats update */
    time_t now = time(NULL);
    if (now - g_stats.last_print >= PRINT_INTERVAL_S) {
        print_live_stats();
        g_stats.last_print = now;
    }

    /* Duration-based auto-stop */
    if (g_duration > 0) {
        double elapsed_s = (g_stats.last_ns - g_stats.start_ns) / 1e9;
        if (elapsed_s >= g_duration)
            g_running = 0;
    }

    return 0;
}

/* ----------------------------------------------------------------------- */
/* Main                                                                     */
/* ----------------------------------------------------------------------- */
static void usage(const char *prog)
{
    fprintf(stderr,
        "Usage: %s -p <PID> [-d <duration_s>] [-o <output.csv>]\n"
        "  -p PID       : target PID to trace (required)\n"
        "  -d SECS      : stop after this many seconds (default: run until Ctrl+C)\n"
        "  -o FILE      : write CSV events to FILE (default: stdout is report only)\n",
        prog);
}

int main(int argc, char **argv)
{
    struct csw_bpf *skel = NULL;
    struct ring_buffer *rb = NULL;
    const char *csv_path = NULL;
    int err = 0;
    int opt;

    while ((opt = getopt(argc, argv, "p:d:o:h")) != -1) {
        switch (opt) {
        case 'p': g_target_pid = atoi(optarg); break;
        case 'd': g_duration   = atoi(optarg); break;
        case 'o': csv_path     = optarg;       break;
        case 'h': usage(argv[0]); return 0;
        default:  usage(argv[0]); return 1;
        }
    }

    if (g_target_pid <= 0) {
        fprintf(stderr, "Error: -p <PID> is required.\n");
        usage(argv[0]);
        return 1;
    }

    /* Allocate sample buffer for percentiles */
    g_stats.offcpu_samples = calloc(MAX_LATENCIES, sizeof(__u64));
    if (!g_stats.offcpu_samples) {
        fprintf(stderr, "Failed to allocate sample buffer\n");
        return 1;
    }

    /* Open CSV file */
    if (csv_path) {
        g_csv = fopen(csv_path, "w");
        if (!g_csv) {
            fprintf(stderr, "Cannot open CSV file: %s: %s\n",
                    csv_path, strerror(errno));
            return 1;
        }
        fprintf(g_csv, "timestamp_ns,pid,cpu,event_type,duration_ns,prev_cpu,comm\n");
    }

    signal(SIGINT,  sig_handler);
    signal(SIGTERM, sig_handler);

    /* 1. Open BPF skeleton */
    skel = csw_bpf__open();
    if (!skel) {
        fprintf(stderr, "Failed to open BPF skeleton\n");
        err = 1;
        goto cleanup;
    }

    /* 2. Load */
    err = csw_bpf__load(skel);
    if (err) {
        fprintf(stderr, "Failed to load BPF program: %d\n", err);
        goto cleanup;
    }

    /* 3. Write target PID into the config map BEFORE attaching */
    {
        __u32 key = 0;
        __u32 pid = (__u32)g_target_pid;
        err = bpf_map__update_elem(skel->maps.target_pid,
                                   &key, sizeof(key),
                                   &pid, sizeof(pid),
                                   BPF_ANY);
        if (err) {
            fprintf(stderr, "Failed to set target_pid: %d\n", err);
            goto cleanup;
        }
    }

    /* 4. Attach */
    err = csw_bpf__attach(skel);
    if (err) {
        fprintf(stderr, "Failed to attach BPF program: %d\n", err);
        goto cleanup;
    }

    /* 5. Create ring buffer reader */
    rb = ring_buffer__new(bpf_map__fd(skel->maps.events),
                          handle_event, NULL, NULL);
    if (!rb) {
        fprintf(stderr, "Failed to create ring buffer\n");
        err = 1;
        goto cleanup;
    }

    fprintf(stderr,
        "Tracing PID %d%s. Ctrl+C to stop.\n",
        g_target_pid,
        g_duration > 0 ? " (duration-limited)" : "");
    fprintf(stderr, "%-16s %-6s %-8s %-12s %-6s\n",
            "COMM", "PID", "CPU", "EVENT", "DUR(us)");

    g_stats.last_print = time(NULL);

    /* 6. Poll loop */
    while (g_running) {
        err = ring_buffer__poll(rb, 100 /* ms */);
        if (err < 0 && err != -EINTR) {
            fprintf(stderr, "Error polling ring buffer: %d\n", err);
            break;
        }
        err = 0;
    }

    print_final_report();

cleanup:
    ring_buffer__free(rb);
    csw_bpf__destroy(skel);
    if (g_csv)
        fclose(g_csv);
    free(g_stats.offcpu_samples);
    return err;
}