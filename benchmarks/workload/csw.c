#include <stdio.h>
#include <signal.h>
#include <unistd.h>
#include <stdint.h>

#include <bpf/libbpf.h>
#include "csw.skel.h"  

#define TASK_COMM_LEN 16

static volatile int running = 1;

static void sig_handler(int sig)
{
    running = 0;
}

/*
 * Mirror of the event struct defined in csw.bpf.c
 * Must match exactly (same field order, same types).
 */
struct event {
    __u32 pid;
    __u64 latency_ns;
    char  comm[TASK_COMM_LEN];
};

/*
 * Callback invoked for each event arriving from the ring buffer.
 */
static int handle_event(void *ctx, void *data, size_t data_sz)
{
    const struct event *e = data;

    printf("COMM=%-16s PID=%-6u LAT=%llu ns\n",
           e->comm,
           e->pid,
           (unsigned long long)e->latency_ns);

    return 0;
}

int main(void)
{
    struct csw_bpf *skel;   /* ← correct struct: csw_bpf (from csw_skel.h) */
    struct ring_buffer  *rb  = NULL;
    int err;

    /* Ctrl+C / SIGTERM → clean shutdown */
    signal(SIGINT,  sig_handler);
    signal(SIGTERM, sig_handler);

    /* 1. Open: parse the embedded BPF object */
    skel = csw_bpf__open();
    if (!skel) {
        fprintf(stderr, "Failed to open BPF skeleton\n");
        return 1;
    }

    /* 2. Load: verify & load programs/maps into the kernel */
    err = csw_bpf__load(skel);
    if (err) {
        fprintf(stderr, "Failed to load BPF program: %d\n", err);
        goto cleanup;
    }

    /* 3. Attach: hook programs to their tracepoints */
    err = csw_bpf__attach(skel);
    if (err) {
        fprintf(stderr, "Failed to attach BPF program: %d\n", err);
        goto cleanup;
    }

    /* 4. Create ring buffer reader
     *    ring_buffer__new(map_fd, callback, ctx, opts)
     *    Pass NULL for opts to use defaults.          */
    rb = ring_buffer__new(bpf_map__fd(skel->maps.events),
                          handle_event,
                          NULL,
                          NULL);   /* ← 4th arg required by modern libbpf */
    if (!rb) {
        fprintf(stderr, "Failed to create ring buffer\n");
        err = 1;
        goto cleanup;
    }

    printf("csw_bench running — tracing context switches. Ctrl+C to stop.\n");
    printf("%-16s %-6s %s\n", "COMM", "PID", "OFF-CPU LAT (ns)");

    /* 5. Poll loop */
    while (running) {
        err = ring_buffer__poll(rb, 100 /* ms timeout */);
        if (err < 0 && err != -EINTR) {
            fprintf(stderr, "Error polling ring buffer: %d\n", err);
            break;
        }
        err = 0;   /* -EINTR is normal on Ctrl+C */
    }

cleanup:
    ring_buffer__free(rb);
    csw_bpf__destroy(skel);
    return err;
}