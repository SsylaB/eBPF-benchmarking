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

struct thread_state {
    u64 enter_ts;
    u64 exit_ts;
};

struct {
    __uint(type, BPF_MAP_TYPE_HASH);
    __uint(max_entries, 512);
    __type(key, u32);
    __type(value, struct thread_state);
} thread_states SEC(".maps");

#define EVT_EXEC 0
#define EVT_GAP  1

struct event {
    u64  timestamp_ns;
    u64  duration_ns;
    u32  pid;
    u32  tid;
    u32  cpu;
    u8   type;
    u8   pad[3];
    char comm[TASK_COMM_LEN];
};

struct {
    __uint(type, BPF_MAP_TYPE_RINGBUF);
    __uint(max_entries, 4 << 20);
} events SEC(".maps");

static __always_inline int tgid_matches(void)
{
    u32 key = 0;
    u32 *target = bpf_map_lookup_elem(&target_pid, &key);
    if (!target || *target == 0) return 0;
    u32 tgid = bpf_get_current_pid_tgid() >> 32;
    return tgid == *target;
}

static __always_inline void emit(u8 type, u32 tid, u32 cpu, u64 ts, u64 duration)
{
    struct event *e = bpf_ringbuf_reserve(&events, sizeof(*e), 0);
    if (!e) return;
    e->type = type;
    e->timestamp_ns = ts;
    e->duration_ns = duration;
    e->pid = bpf_get_current_pid_tgid() >> 32;
    e->tid = tid;
    e->cpu = cpu;
    bpf_get_current_comm(e->comm, sizeof(e->comm));
    bpf_ringbuf_submit(e, 0);
}

/* --- Hook the ENTRY of processCommand --- */
SEC("uprobe/redis_enter")
int redis_enter(struct pt_regs *ctx)
{
    if (!tgid_matches()) return 0;

    u32 tid = (u32)bpf_get_current_pid_tgid();
    u32 cpu = bpf_get_smp_processor_id();
    u64 ts  = bpf_ktime_get_ns();

    struct thread_state *st = bpf_map_lookup_elem(&thread_states, &tid);

    if (!st) {
        struct thread_state new_st = { .enter_ts = ts, .exit_ts = 0 };
        bpf_map_update_elem(&thread_states, &tid, &new_st, BPF_ANY);
    } else {
        if (st->exit_ts != 0) {
            emit(EVT_GAP, tid, cpu, ts, ts - st->exit_ts);
        }
        st->enter_ts = ts;
        st->exit_ts  = 0;
    }
    return 0;
}

/* --- Hook the RETURN of processCommand --- */
SEC("uretprobe/redis_exit")
int redis_exit(struct pt_regs *ctx)
{
    if (!tgid_matches()) return 0;

    u32 tid = (u32)bpf_get_current_pid_tgid();
    u32 cpu = bpf_get_smp_processor_id();
    u64 ts  = bpf_ktime_get_ns();

    struct thread_state *st = bpf_map_lookup_elem(&thread_states, &tid);

    if (!st || st->enter_ts == 0) return 0;

    emit(EVT_EXEC, tid, cpu, ts, ts - st->enter_ts);

    st->exit_ts  = ts;
    st->enter_ts = 0;
    return 0;
}