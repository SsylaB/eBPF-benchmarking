#include "vmlinux.h"
#include <bpf/bpf_helpers.h>
#include <bpf/bpf_tracing.h>

char _license[] SEC("license") = "GPL";

#define EACCES 13 // Code d'erreur "Permission Denied"

// --- STRUCTURES ---
struct event {
    u32 pid;
    char fname[256];
    u32 is_blocked;
};

struct my_syscall_trace_enter {
    u64 pad;
    int __syscall_nr;
    u32 pad2;
    u64 args[6];
};

// --- MAPS ---

// 1. Fichiers à protéger
struct {
    __uint(type, BPF_MAP_TYPE_HASH);
    __uint(max_entries, 1024);
    __type(key, char[256]);
    __type(value, u32);
} sensitive_files SEC(".maps");

// 2. Taint Mode (Le carnet des PID infectés)
struct {
    __uint(type, BPF_MAP_TYPE_HASH);
    __uint(max_entries, 10240);
    __type(key, u32); // PID
    __type(value, u32); // 1 = Infecté
} tainted_pids SEC(".maps");

// 3.  Le Pont entre Tracepoint et LSM
// On stocke temporairement ce que le Thread essaie d'ouvrir
struct {
    __uint(type, BPF_MAP_TYPE_HASH);
    __uint(max_entries, 10240);
    __type(key, u32); // TID (Thread ID)
    __type(value, char[256]); // Nom du fichier
} active_opens SEC(".maps");

struct {
    __uint(type, BPF_MAP_TYPE_RINGBUF);
    __uint(max_entries, 256 * 1024);
} rb SEC(".maps");


// --- 1. Le Hook Réseau ---
SEC("tp/syscalls/sys_enter_connect")
int taint_on_connect(void *ctx) {
    u32 pid = bpf_get_current_pid_tgid() >> 32;
    u32 tainted = 1;
    bpf_map_update_elem(&tainted_pids, &pid, &tainted, BPF_ANY);
    return 0;
}


// --- 2. Les Tracepoints ---
static __always_inline int handle_sys_open(const char *filename) {
    char fname[256] = {};
    u64 pid_tgid = bpf_get_current_pid_tgid();
    u32 tid = (u32)pid_tgid; // On identifie le Thread précis

    if (bpf_probe_read_user_str(&fname, sizeof(fname), filename) <= 0)
        return 0;

    // Si on essaie d'ouvrir un fichier sensible, on prévient le LSM
    if (bpf_map_lookup_elem(&sensitive_files, &fname)) {
        bpf_map_update_elem(&active_opens, &tid, &fname, BPF_ANY);
    }
    return 0;
}

SEC("tp/syscalls/sys_enter_open")
int handle_open(struct my_syscall_trace_enter *ctx) {
    return handle_sys_open((const char *)ctx->args[0]);
}
SEC("tp/syscalls/sys_enter_openat")
int handle_openat(struct my_syscall_trace_enter *ctx) {
    return handle_sys_open((const char *)ctx->args[1]);
}
SEC("tp/syscalls/sys_enter_openat2")
int handle_openat2(struct my_syscall_trace_enter *ctx) {
    return handle_sys_open((const char *)ctx->args[1]);
}


// --- 3. Le Hook LSM ---
SEC("lsm/file_open")
int BPF_PROG(restrict_file_open, struct file *file) {
    u64 pid_tgid = bpf_get_current_pid_tgid();
    u32 pid = pid_tgid >> 32;
    u32 tid = (u32)pid_tgid;

    // 1. Ce Thread a-t-il été signalé par l'Observateur juste avant ?
    char *fname = bpf_map_lookup_elem(&active_opens, &tid);
    if (!fname) {
        return 0; // Non, on laisse passer
    }

    // 2. Le processus complet est-il infecté (Tainted) ?
    u32 *is_tainted = bpf_map_lookup_elem(&tainted_pids, &pid);

    // 3. On envoie l'alerte à l'utilisateur
    struct event *e = bpf_ringbuf_reserve(&rb, sizeof(*e), 0);
    if (e) {
        e->pid = pid;
        e->is_blocked = (is_tainted != NULL) ? 1 : 0;
        bpf_probe_read_kernel_str(e->fname, sizeof(e->fname), fname);
        bpf_ringbuf_submit(e, 0);
    }

    // 4. On nettoie la map temporaire
    bpf_map_delete_elem(&active_opens, &tid);

    // 5. LA SENTENCE !
    if (is_tainted) {
        return -EACCES; // Blocage !
    }

    return 0; // Sains, on laisse passer
}
