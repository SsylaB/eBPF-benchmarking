#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <unistd.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>
#include <time.h>
#include <getopt.h>

#include <bpf/libbpf.h>
#include "csw.skel.h"

#define TASK_COMM_LEN   16
#define MAX_SAMPLES     (1 << 22)   /* 4M samples for percentiles */
#define PRINT_INTERVAL  1           /* seconds between live updates */

#define EVT_EXEC 0
#define EVT_GAP  1

struct event {
    __u64 timestamp_ns;
    __u64 duration_ns;
    __u32 pid;
    __u32 tid;
    __u32 cpu;
    __u8  type;
    __u8  pad[3];
    char  comm[TASK_COMM_LEN];
};

static volatile int g_running = 1;
static void sig_handler(int s) { g_running = 0; }

static struct {
    __u64 n_exec;
    __u64 n_gap;
    __u64 total_exec_ns;
    __u64 total_gap_ns;
    __u64 *exec_samples;
    __u32  n_exec_samples;
    __u64 start_ns;
    __u64 last_ns;
    time_t last_print;
} g_stats;

static FILE *g_csv        = NULL;
static int   g_target_pid = 0;
static int   g_duration   = 0;

static int cmp_u64(const void *a, const void *b)
{
    __u64 x = *(__u64 *)a, y = *(__u64 *)b;
    return (x > y) - (x < y);
}

static __u64 percentile(__u64 *sorted, __u32 n, double p)
{
    if (!n) return 0;
    __u32 idx = (__u32)(p / 100.0 * (n - 1) + 0.5);
    if (idx >= n) idx = n - 1;
    return sorted[idx];
}

static void print_live(void)
{
    __u64 elapsed_ns = g_stats.last_ns - g_stats.start_ns;
    if (!elapsed_ns) return;
    double elapsed_s = elapsed_ns / 1e9;

    double continuity = 0.0;
    __u64 total = g_stats.total_exec_ns + g_stats.total_gap_ns;
    if (total) continuity = (double)g_stats.total_exec_ns / total * 100.0;

    double avg_exec_us = g_stats.n_exec
        ? (double)g_stats.total_exec_ns / g_stats.n_exec / 1000.0 : 0.0;
    double avg_gap_us  = g_stats.n_gap
        ? (double)g_stats.total_gap_ns  / g_stats.n_gap  / 1000.0 : 0.0;

    fprintf(stderr,
        "\r[%5.1fs] C=%.1f%% | exec=%llu (avg %.1fus) | gaps=%llu (avg %.1fus)   ",
        elapsed_s, continuity,
        (unsigned long long)g_stats.n_exec, avg_exec_us,
        (unsigned long long)g_stats.n_gap,  avg_gap_us);
    fflush(stderr);
}

static void print_report(void)
{
    __u64 elapsed_ns = g_stats.last_ns - g_stats.start_ns;
    if (!elapsed_ns) { fprintf(stderr, "\nNo events captured.\n"); return; }
    double elapsed_s = elapsed_ns / 1e9;

    if (g_stats.n_exec_samples > 0)
        qsort(g_stats.exec_samples, g_stats.n_exec_samples,
              sizeof(__u64), cmp_u64);

    __u64 p50  = percentile(g_stats.exec_samples, g_stats.n_exec_samples, 50.0);
    __u64 p95  = percentile(g_stats.exec_samples, g_stats.n_exec_samples, 95.0);
    __u64 p99  = percentile(g_stats.exec_samples, g_stats.n_exec_samples, 99.0);
    __u64 p999 = percentile(g_stats.exec_samples, g_stats.n_exec_samples, 99.9);

    double continuity = 0.0;
    __u64 total = g_stats.total_exec_ns + g_stats.total_gap_ns;
    if (total) continuity = (double)g_stats.total_exec_ns / total;

    double avg_exec_us = g_stats.n_exec
        ? (double)g_stats.total_exec_ns / g_stats.n_exec / 1000.0 : 0.0;
    double avg_gap_us  = g_stats.n_gap
        ? (double)g_stats.total_gap_ns  / g_stats.n_gap  / 1000.0 : 0.0;

    fprintf(stderr, "\n\n");
    fprintf(stdout,
        "=== Redis uprobe report ===\n"
        "PID              : %d\n"
        "Observation      : %.3f s\n"
        "\n"
        "--- Continuity ---\n"
        "  C (exec / total) : %.4f  (%.1f%%)\n"
        "  Total exec time  : %.3f s\n"
        "  Total gap time   : %.3f s\n"
        "\n"
        "--- Command execution time ---\n"
        "  count  : %llu\n"
        "  avg    : %.1f us\n"
        "  p50    : %.1f us\n"
        "  p95    : %.1f us\n"
        "  p99    : %.1f us\n"
        "  p99.9  : %.1f us\n"
        "\n"
        "--- Inter-command gaps (off-CPU / idle) ---\n"
        "  count  : %llu\n"
        "  avg    : %.1f us\n"
        "  stall rate: %.2f /s\n"
        "===================================\n",
        g_target_pid,
        elapsed_s,
        continuity, continuity * 100.0,
        (double)g_stats.total_exec_ns / 1e9,
        (double)g_stats.total_gap_ns  / 1e9,
        (unsigned long long)g_stats.n_exec,
        avg_exec_us,
        p50  / 1000.0, p95  / 1000.0,
        p99  / 1000.0, p999 / 1000.0,
        (unsigned long long)g_stats.n_gap,
        avg_gap_us,
        g_stats.n_gap / elapsed_s);
}

static int handle_event(void *ctx, void *data, size_t sz)
{
    const struct event *e = data;

    if (!g_stats.start_ns) g_stats.start_ns = e->timestamp_ns;
    g_stats.last_ns = e->timestamp_ns;

    switch (e->type) {
    case EVT_EXEC:
        g_stats.n_exec++;
        g_stats.total_exec_ns += e->duration_ns;
        if (g_stats.n_exec_samples < MAX_SAMPLES)
            g_stats.exec_samples[g_stats.n_exec_samples++] = e->duration_ns;
        break;
    case EVT_GAP:
        g_stats.n_gap++;
        g_stats.total_gap_ns += e->duration_ns;
        break;
    }

    if (g_csv) {
        const char *type_str = e->type == EVT_EXEC ? "EXEC" : "GAP";
        fprintf(g_csv, "%llu,%u,%u,%u,%s,%llu,%s\n",
                (unsigned long long)e->timestamp_ns,
                e->pid, e->tid, e->cpu,
                type_str,
                (unsigned long long)e->duration_ns,
                e->comm);
    }

    time_t now = time(NULL);
    if (now - g_stats.last_print >= PRINT_INTERVAL) {
        print_live();
        g_stats.last_print = now;
    }

    if (g_duration > 0) {
        double elapsed_s = (g_stats.last_ns - g_stats.start_ns) / 1e9;
        if (elapsed_s >= g_duration) g_running = 0;
    }

    return 0;
}

static void usage(const char *prog)
{
    fprintf(stderr,
        "Usage: %s -p <PID> -f <func_offset>\n"
        "          [-b <binary>] [-d <secs>] [-o <output.csv>]\n"
        "\n"
        "  -p PID            : redis-server PID (TGID)\n"
        "  -f OFFSET         : hex offset of processCommand\n"
        "  -b BINARY         : path to redis-server (default: /usr/bin/redis-server)\n"
        "  -d SECS           : stop after N seconds\n"
        "  -o FILE           : write CSV to FILE\n",
        prog);
}

int main(int argc, char **argv)
{
    const char *binary      = "/usr/bin/redis-server";
    const char *csv_path    = NULL;
    long func_offset        = 0;
    int  opt, err           = 0;

    while ((opt = getopt(argc, argv, "p:f:b:d:o:h")) != -1) {
        switch (opt) {
        case 'p': g_target_pid  = atoi(optarg);              break;
        case 'f': func_offset   = strtol(optarg, NULL, 16);  break;
        case 'b': binary        = optarg;                    break;
        case 'd': g_duration    = atoi(optarg);              break;
        case 'o': csv_path      = optarg;                    break;
        case 'h': usage(argv[0]); return 0;
        default:  usage(argv[0]); return 1;
        }
    }

    if (!g_target_pid || !func_offset) {
        fprintf(stderr, "Error: -p and -f are required.\n\n");
        usage(argv[0]);
        return 1;
    }

    g_stats.exec_samples = calloc(MAX_SAMPLES, sizeof(__u64));
    if (csv_path) {
        g_csv = fopen(csv_path, "w");
        fprintf(g_csv, "timestamp_ns,pid,tid,cpu,event_type,duration_ns,comm\n");
    }

    signal(SIGINT,  sig_handler);
    signal(SIGTERM, sig_handler);

    struct csw_bpf *skel = csw_bpf__open_and_load();
    if (!skel) {
        fprintf(stderr, "Failed to load BPF skeleton\n");
        return 1;
    }
    
    __u32 key = 0, pid = (__u32)g_target_pid;
    bpf_map__update_elem(skel->maps.target_pid, &key, sizeof(key), &pid, sizeof(pid), BPF_ANY);

    /* Attach standard uprobe to ENTRY */
    struct bpf_link *link_enter = bpf_program__attach_uprobe(
        skel->progs.redis_enter, false, g_target_pid, binary, func_offset);

    if (!link_enter) {
        fprintf(stderr, "Failed to attach entry uprobe\n");
        return 1;
    }

    /* Attach uretprobe to EXIT */
    struct bpf_link *link_exit = bpf_program__attach_uprobe(
        skel->progs.redis_exit, true, g_target_pid, binary, func_offset);

    if (!link_exit) {
        fprintf(stderr, "Failed to attach exit uretprobe\n");
        return 1;
    }

    struct ring_buffer *rb = ring_buffer__new(
        bpf_map__fd(skel->maps.events), handle_event, NULL, NULL);

    fprintf(stderr, "Tracing Redis PID %d at offset 0x%lx\n", g_target_pid, func_offset);
    g_stats.last_print = time(NULL);

    while (g_running) {
        err = ring_buffer__poll(rb, 100);
        if (err < 0 && err != -EINTR) break;
    }

    print_report();
    ring_buffer__free(rb);
    bpf_link__destroy(link_exit);
    bpf_link__destroy(link_enter);
    csw_bpf__destroy(skel);
    if (g_csv) fclose(g_csv);
    free(g_stats.exec_samples);
    return 0;
}