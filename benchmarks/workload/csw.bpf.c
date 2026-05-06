#include "vmlinux.h"
#include <bpf/bpf_helpers.h>
#include <bpf/bpf_tracing.h>

#define TASK_COMM_LEN 16

char LICENSE[] SEC("license") = "Dual BSD/GPL";

struct {
    __uint(type, BPF_MAP_TYPE_ARRAY);
    __uint(max_entries, 1);
    __type(key, u32);
    __type(value, u32);
} target_pid SEC(".maps");

struct task_state {
    u64 switch_out_ts;
    u64 switch_in_ts;
    u32 last_cpu;
    u32 initialized;
};

struct {
    __uint(type, BPF_MAP_TYPE_HASH);
    __uint(max_entries, 1024);
    __type(key, u32);
    __type(value, struct task_state);
} task_states SEC(".maps");

#define EVT_OFFCPU   0
#define EVT_ONCPU    1
#define EVT_MIGRATE  2

struct event {
    u64  timestamp_ns;
    u32  pid;
    u32  cpu;
    u8   type;
    u8   pad[3];
    u64  duration_ns;
    u32  prev_cpu;
    char comm[TASK_COMM_LEN];
};

struct {
    __uint(type, BPF_MAP_TYPE_RINGBUF);
    __uint(max_entries, 1 << 20);
} events SEC(".maps");

static __always_inline int emit_event(u32 pid, u32 cpu, u8 type,
                                      u64 duration_ns, u32 prev_cpu, u64 ts)
{
    struct event *e = bpf_ringbuf_reserve(&events, sizeof(*e), 0);
    if (!e) return -1;
    e->timestamp_ns = ts;
    e->pid          = pid;
    e->cpu          = cpu;
    e->type         = type;
    e->duration_ns  = duration_ns;
    e->prev_cpu     = prev_cpu;
    bpf_get_current_comm(e->comm, sizeof(e->comm));
    bpf_ringbuf_submit(e, 0);
    return 0;
}

SEC("tracepoint/sched/sched_switch")
int handle_sched_switch(struct trace_event_raw_sched_switch *ctx)
{
    u32 key = 0;
    u32 *tpid = bpf_map_lookup_elem(&target_pid, &key);
    if (!tpid || *tpid == 0)
        return 0;

    u32 filter   = *tpid;
    u32 prev_pid = ctx->prev_pid;
    u32 next_pid = ctx->next_pid;
    u64 ts       = bpf_ktime_get_ns();
    u32 cpu      = bpf_get_smp_processor_id();

    // DEBUG: Print every time the filter PID is involved in a switch
    if (prev_pid == filter || next_pid == filter) {
        bpf_printk("CSW: Match PID %d | prev=%d next=%d cpu=%d", filter, prev_pid, next_pid, cpu);
    }

    if (prev_pid == filter) {
        struct task_state *st = bpf_map_lookup_elem(&task_states, &prev_pid);
        if (!st) {
            struct task_state new_st = { .switch_out_ts = ts, .last_cpu = cpu, .initialized = 1 };
            bpf_map_update_elem(&task_states, &prev_pid, &new_st, BPF_ANY);
        } else {
            if (st->switch_in_ts != 0) {
                emit_event(prev_pid, cpu, EVT_ONCPU, ts - st->switch_in_ts, st->last_cpu, ts);
            }
            st->switch_out_ts = ts;
            st->switch_in_ts  = 0;
            st->last_cpu      = cpu;
        }
    }

    if (next_pid == filter) {
        struct task_state *st = bpf_map_lookup_elem(&task_states, &next_pid);
        if (!st) {
            struct task_state new_st = { .switch_in_ts = ts, .last_cpu = cpu, .initialized = 1 };
            bpf_map_update_elem(&task_states, &next_pid, &new_st, BPF_ANY);
        } else {
            if (st->initialized && st->last_cpu != cpu)
                emit_event(next_pid, cpu, EVT_MIGRATE, 0, st->last_cpu, ts);
            if (st->switch_out_ts != 0) {
                emit_event(next_pid, cpu, EVT_OFFCPU, ts - st->switch_out_ts, st->last_cpu, ts);
            }
            st->switch_in_ts  = ts;
            st->switch_out_ts = 0;
            st->last_cpu      = cpu;
        }
    }

    return 0;
}