#include "vmlinux.h"
#include <bpf/bpf_helpers.h>
#include <bpf/bpf_tracing.h>

#define TASK_COMM_LEN 16

char LICENSE[] SEC("license") = "Dual BSD/GPL";


/*
 * Store the timestamp of when a task was last switched OUT.
 * key   = pid/tid (u32)
 * value = ktime_get_ns() at switch-out (u64)
 */
struct {
    __uint(type, BPF_MAP_TYPE_HASH);
    __uint(max_entries, 16384);
    __type(key, u32);
    __type(value, u64);
} start SEC(".maps");


/*
 * Event sent to userspace via ring buffer.
 * Represents one completed off-CPU interval for a task.
 */
struct event {
    u32 pid;            /* PID of the task that just woke up   */
    u64 latency_ns;     /* time spent off-CPU, in nanoseconds   */
    char comm[TASK_COMM_LEN]; /* comm of the waking task        */
};

struct {
    __uint(type, BPF_MAP_TYPE_RINGBUF);
    __uint(max_entries, 256 * 1024);
} events SEC(".maps");


/*
 * sched_switch tracepoint handler.
 *
 * Fired on every context switch:
 *   prev = task being switched OUT  (going to sleep / preempted)
 *   next = task being switched IN   (waking up / resuming)
 *
 * We record when prev goes off-CPU, and compute how long next
 * was off-CPU when it comes back on.
 */
SEC("tracepoint/sched/sched_switch")
int handle_sched_switch(struct trace_event_raw_sched_switch *ctx)
{
    u32 prev_pid = ctx->prev_pid;
    u32 next_pid = ctx->next_pid;
    u64 ts = bpf_ktime_get_ns();

    struct event *e;
    u64 *tsp;

    /*
     * CASE 1: task switching OUT → record its departure timestamp.
     * Skip PID 0 (idle task): it has no meaningful off-CPU latency.
     */
    if (prev_pid != 0) {
        bpf_map_update_elem(&start, &prev_pid, &ts, BPF_ANY);
    }

    /*
     * CASE 2: task switching IN → compute how long it was off-CPU.
     * Skip PID 0 (idle task): waking idle is not a real context switch
     * of interest and would produce misleading latency values.
     */
    if (next_pid == 0)
        return 0;

    tsp = bpf_map_lookup_elem(&start, &next_pid);
    if (!tsp)
        return 0;   /* first time we see this PID, no baseline yet */

    u64 delta = ts - *tsp;

    /* Clean up the map entry — we've consumed this timestamp */
    bpf_map_delete_elem(&start, &next_pid);

    /* Reserve space in the ring buffer and fill the event */
    e = bpf_ringbuf_reserve(&events, sizeof(*e), 0);
    if (!e)
        return 0;   /* ring buffer full — drop this sample */

    e->pid        = next_pid;
    e->latency_ns = delta;

    /*
     * bpf_get_current_comm() returns the comm of the task currently
     * executing on this CPU.  At this point in sched_switch the kernel
     * has already context-switched, so "current" is next — correct.
     */
    bpf_get_current_comm(e->comm, sizeof(e->comm));

    bpf_ringbuf_submit(e, 0);

    return 0;
}