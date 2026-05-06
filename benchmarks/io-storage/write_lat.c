#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <unistd.h>
#include <time.h>
#include <string.h>
#include <bpf/libbpf.h>
#include "write_lat.skel.h"
#include <pthread.h>

#define TASK_COMM_LEN 16

struct event {
    unsigned int       pid;
    unsigned long long latency_ns;
    char               comm[TASK_COMM_LEN];
};

static volatile int running = 1;
static int filter_pid = -1;  /* -1 = pas de filtre */

static void sig_handler(int sig) { running = 0; }

/* Lit CPU et mémoire pour un PID donné depuis /proc/{pid}/stat et /proc/{pid}/status */
static int read_proc_stats(int pid, unsigned long long *utime, unsigned long long *stime,
                            unsigned long long *vmrss_kb)
{
    char path[64];
    FILE *f;

    *utime = 0; *stime = 0; *vmrss_kb = 0;

    /* /proc/{pid}/stat — lit la ligne entière puis parse depuis la fin du comm */
    snprintf(path, sizeof(path), "/proc/%d/stat", pid);
    f = fopen(path, "r");
    if (!f) return -1;

    char buf[1024];
    if (!fgets(buf, sizeof(buf), f)) { fclose(f); return -1; }
    fclose(f);

    /* Trouve la dernière ')' — fin du champ comm */
    char *p = strrchr(buf, ')');
    if (!p) return -1;
    p += 2;  /* saute ') ' */

    /* Après comm : state(1) ppid(2) pgrp(3) session(4) tty(5) tpgid(6)
       flags(7) minflt(8) cminflt(9) majflt(10) cmajflt(11) utime(12) stime(13) */
    unsigned long long dummy;
    char state;
    sscanf(p, "%c %llu %llu %llu %llu %llu %llu %llu %llu %llu %llu %llu %llu",
           &state,
           &dummy, &dummy, &dummy, &dummy, &dummy, &dummy,
           &dummy, &dummy, &dummy, &dummy,
           utime, stime);

    /* /proc/{pid}/status — VmRSS */
    snprintf(path, sizeof(path), "/proc/%d/status", pid);
    f = fopen(path, "r");
    if (!f) return -1;
    char line[128];
    while (fgets(line, sizeof(line), f)) {
        if (sscanf(line, "VmRSS: %llu", vmrss_kb) == 1) break;
    }
    fclose(f);

    return 0;
}

static void *cpu_monitor(void *arg)
{
    struct timespec ts;
    long clk_tck = sysconf(_SC_CLK_TCK);  /* ticks par seconde (généralement 100) */

    /* Suivi pour le système global */
    unsigned long long sys_prev_total = 0, sys_prev_work = 0;

    /* Suivi par PID (write_lat lui-même + PID filtré si fourni) */
    unsigned long long self_prev_utime = 0, self_prev_stime = 0;
    unsigned long long filt_prev_utime = 0, filt_prev_stime = 0;

    int self_pid = getpid();

    while (running) {
        clock_gettime(CLOCK_REALTIME, &ts);
        unsigned long long now_ns = (unsigned long long)ts.tv_sec * 1000000000ULL + ts.tv_nsec;

        /* --- CPU système global (/proc/stat) --- */
        FILE *f = fopen("/proc/stat", "r");
        if (f) {
            unsigned long long user, nice, system, idle, iowait, irq, softirq;
            fscanf(f, "cpu %llu %llu %llu %llu %llu %llu %llu",
                   &user, &nice, &system, &idle, &iowait, &irq, &softirq);
            fclose(f);

            unsigned long long total = user+nice+system+idle+iowait+irq+softirq;
            unsigned long long work  = user+nice+system;

            if (sys_prev_total > 0 && (total - sys_prev_total) > 0) {
                double cpu_pct = 100.0 * (work - sys_prev_work) / (total - sys_prev_total);
                printf("%llu,io-storage,libbpf,write_4k,cpu_system_pct,%.2f,system\n",
                       now_ns, cpu_pct);
            }
            sys_prev_total = total;
            sys_prev_work  = work;
        }

        /* --- CPU + mémoire de write_lat lui-même (/proc/self/stat) --- */
        unsigned long long self_utime, self_stime, self_rss;
        read_proc_stats(self_pid, &self_utime, &self_stime, &self_rss);

        if (self_prev_utime > 0) {
            double self_cpu = 100.0 * ((self_utime + self_stime) -
                                       (self_prev_utime + self_prev_stime)) / clk_tck;
            printf("%llu,io-storage,libbpf,write_4k,cpu_write_lat_pct,%.2f,write_lat\n",
                   now_ns, self_cpu);
        }
        printf("%llu,io-storage,libbpf,write_4k,mem_write_lat_kb,%llu,write_lat\n",
               now_ns, self_rss);
        self_prev_utime = self_utime;
        self_prev_stime = self_stime;

        /* --- CPU + mémoire du PID filtré (ex: fio) --- */
        if (filter_pid > 0) {
            unsigned long long filt_utime, filt_stime, filt_rss;
            int ret = read_proc_stats(filter_pid, &filt_utime, &filt_stime, &filt_rss);

            if (ret < 0) {
                fprintf(stderr, "PID %d terminé, arrêt du suivi workload\n", filter_pid);
                filter_pid = -1;  // ← désactive le suivi pour ce PID
            } else {
                if (filt_prev_utime > 0) {
                    double filt_cpu = 100.0 * ((filt_utime + filt_stime) -
                                               (filt_prev_utime + filt_prev_stime)) / clk_tck;
                    printf("%llu,io-storage,libbpf,write_4k,cpu_workload_pct,%.2f,pid_%d\n",
                           now_ns, filt_cpu, filter_pid);
                }
                printf("%llu,io-storage,libbpf,write_4k,mem_workload_kb,%llu,pid_%d\n",
                       now_ns, filt_rss, filter_pid);
                filt_prev_utime = filt_utime;
                filt_prev_stime = filt_stime;
            }
        }

        usleep(200000);  // 200ms
    }
    return NULL;
}

static int handle_event(void *ctx, void *data, size_t sz)
{
    const struct event *e = data;

    /* Filtre par PID si demandé */
    if (filter_pid > 0 && (int)e->pid != filter_pid)
        return 0;

    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    unsigned long long now_ns = (unsigned long long)ts.tv_sec * 1000000000ULL + ts.tv_nsec;

    printf("%llu,io-storage,libbpf,write_4k,latency_ns,%llu,%s\n",
           now_ns, e->latency_ns, e->comm);

    return 0;
}

int main(int argc, char **argv)
{
    struct write_lat_bpf *skel;
    struct ring_buffer   *rb;
    int err;

    /* Argument optionnel : PID à isoler */
    if (argc >= 2) {
        filter_pid = atoi(argv[1]);
        fprintf(stderr, "Filtre activé : PID %d\n", filter_pid);
    }

    printf("timestamp_ns,domain,library,workload,metric,value,comm\n");

    skel = write_lat_bpf__open_and_load();
    if (!skel) {
        fprintf(stderr, "Erreur : impossible de charger le skeleton BPF\n");
        return 1;
    }

    err = write_lat_bpf__attach(skel);
    if (err) {
        fprintf(stderr, "Erreur : impossible d'attacher les probes BPF\n");
        goto cleanup;
    }

    rb = ring_buffer__new(bpf_map__fd(skel->maps.events), handle_event, NULL, NULL);
    if (!rb) {
        fprintf(stderr, "Erreur : impossible de créer le ring buffer\n");
        err = 1;
        goto cleanup;
    }

    signal(SIGINT, sig_handler);
    signal(SIGTERM, sig_handler);

    fprintf(stderr, "Collecte en cours... Ctrl+C pour arrêter.\n");

    pthread_t cpu_thread;
    pthread_create(&cpu_thread, NULL, cpu_monitor, NULL);

    while (running) {
        err = ring_buffer__poll(rb, 100);
        if (err == -EINTR) { err = 0; break; }
        if (err < 0) {
            fprintf(stderr, "Erreur lors du poll : %d\n", err);
            break;
        }
    }

    ring_buffer__free(rb);

cleanup:
    write_lat_bpf__destroy(skel);
    return err < 0 ? 1 : 0;
}