#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <unistd.h>
#include <time.h>
#include <bpf/libbpf.h>
#include "write_lat.skel.h"

#define TASK_COMM_LEN 16

struct event {
    unsigned int       pid;
    unsigned long long latency_ns;
    char               comm[TASK_COMM_LEN];
};

static volatile int running = 1;

static void sig_handler(int sig)
{
    running = 0;
}

static int handle_event(void *ctx, void *data, size_t sz)
{
    const struct event *e = data;
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

    /* CSV header */
    printf("timestamp_ns,domain,library,workload,metric,value,comm\n");

    /* Load and verify BPF application */
    skel = write_lat_bpf__open_and_load();
    if (!skel) {
        fprintf(stderr, "Erreur : impossible de charger le skeleton BPF\n");
        return 1;
    }

    /* Attach probes */
    err = write_lat_bpf__attach(skel);
    if (err) {
        fprintf(stderr, "Erreur : impossible d'attacher les probes BPF\n");
        goto cleanup;
    }

    /* Set up ring buffer */
    rb = ring_buffer__new(bpf_map__fd(skel->maps.events), handle_event, NULL, NULL);
    if (!rb) {
        fprintf(stderr, "Erreur : impossible de créer le ring buffer\n");
        err = 1;
        goto cleanup;
    }

    signal(SIGINT, sig_handler);
    signal(SIGTERM, sig_handler);

    fprintf(stderr, "Collecte en cours... Ctrl+C pour arrêter.\n");

    while (running) {
        err = ring_buffer__poll(rb, 100 /* timeout ms */);
        if (err == -EINTR) {
            err = 0;
            break;
        }
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