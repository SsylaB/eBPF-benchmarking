#include "vmlinux.h"
#include <bpf/bpf_helpers.h>
#include <bpf/bpf_tracing.h>

#define TASK_COMM_LEN 16

struct event {
	u32  pid;
	u64  latency_ns;
	char comm[TASK_COMM_LEN];
};

struct {
	__uint(type, BPF_MAP_TYPE_HASH);
	__uint(max_entries, 8192);
	__type(key, u32);
	__type(value, u64);
} start SEC(".maps");

struct {
	__uint(type, BPF_MAP_TYPE_RINGBUF);
	__uint(max_entries, 256 * 1024);
} events SEC(".maps");

SEC("kprobe/vfs_write")
int BPF_KPROBE(vfs_write_entry)
{
	u32 tid = (u32)bpf_get_current_pid_tgid();
	u64 ts  = bpf_ktime_get_ns();
	bpf_map_update_elem(&start, &tid, &ts, BPF_ANY);
	return 0;
}

SEC("kretprobe/vfs_write")
int BPF_KRETPROBE(vfs_write_exit)
{
	u32  tid = (u32)bpf_get_current_pid_tgid();
	u64 *tsp = bpf_map_lookup_elem(&start, &tid);
	if (!tsp) return 0;

	u64 latency = bpf_ktime_get_ns() - *tsp;
	bpf_map_delete_elem(&start, &tid);

	struct event *e = bpf_ringbuf_reserve(&events, sizeof(*e), 0);
	if (!e) return 0;

	e->pid        = (u32)(bpf_get_current_pid_tgid() >> 32);
	e->latency_ns = latency;
	bpf_get_current_comm(&e->comm, sizeof(e->comm));
	bpf_ringbuf_submit(e, 0);
	return 0;
}

char LICENSE[] SEC("license") = "Dual BSD/GPL";