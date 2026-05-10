#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <time.h>
#include <bpf/libbpf.h>
#include "security.skel.h"

static int stop = 0;

void handle_sig(int sig) { stop = 1; }

// Callback appelée à chaque fois qu'un événement arrive
static int handle_event(void *ctx, void *data, size_t data_sz) {
    struct {
        uint32_t pid;
        char fname[256];
        uint32_t is_blocked;
    } *e = data;
    // On écrit directement dans le fichier CSV.
    FILE *f = fopen("audit_log.csv", "a");
    if (f) {
        time_t now = time(NULL);
        struct tm *t = localtime(&now);
        char time_str[64];
        strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S", t);

        const char *status = e->is_blocked ? "BLOCKED" : "AUDIT";
        fprintf(f, "%s,%d,%s,%s\n", time_str, e->pid, status, e->fname);
        fclose(f);
    }
    
    return 0;
}

int main(int argc, char **argv) {
    struct security_bpf *skel;
    struct ring_buffer *rb = NULL;
    int err;

    if (argc < 2) {
        printf("Usage : sudo %s <fichier1> <fichier2> ...\n", argv[0]);
        return 1;
    }

    skel = security_bpf__open_and_load();
    if (!skel) return 1;

    err = security_bpf__attach(skel);
    if (err) goto cleanup;

    // Initialisation du CSV (Création de l'en-tête si le fichier est nouveau)
    FILE *f = fopen("audit_log.csv", "a");
    if (f) {
        fseek(f, 0, SEEK_END);
        if (ftell(f) == 0) {
            fprintf(f, "Timestamp,PID,Status,File\n");
        }
        fclose(f);
    }

    uint32_t val = 1;
    for (int i = 1; i < argc; i++) {
        char target[256] = {};
        strncpy(target, argv[i], sizeof(target) - 1);
        bpf_map__update_elem(skel->maps.sensitive_files, &target, sizeof(target), &val, sizeof(val), BPF_ANY);
    }

    // Message unique au démarrage
    printf("Outil eBPF lancé en mode SILENCIEUX.\n");
    printf(" -> Les événements sont enregistrés dans 'audit_log.csv'.\n");
    printf(" -> Appuyez sur Ctrl+C pour arrêter l'outil.\n");

    rb = ring_buffer__new(bpf_map__fd(skel->maps.rb), handle_event, NULL, NULL);
    signal(SIGINT, handle_sig);

    while (!stop) {
        err = ring_buffer__poll(rb, 100);
        if (err == -EINTR) break;
    }

cleanup:
    ring_buffer__free(rb);
    security_bpf__destroy(skel);
    printf("\n Arrêt de l'outil de sécurité.\n");
    return 0;
}
