# eBPF Benchmarking — Conventions du groupe

Ce document définit les conventions partagées pour tous les membres du groupe. **Lisez-le avant d'écrire la moindre ligne de code.** L'objectif est de s'assurer que nous pourrons comparer les résultats entre tous les domaines (I/O stockage, réseau, sécurité, IA) à la fin du projet.

***

## Structure du dépôt

Chaque personne travaille exclusivement dans son propre sous-répertoire. **Ne modifiez jamais de fichiers en dehors de votre répertoire.**

```
eBPF-benchmarking/
├── README.md
├── docs/
│   └── CONVENTIONS.md          ← ce fichier
├── benchmarks/
│   ├── io-storage/              
│   ├── network/                 
│   ├── security/                
│   ├── ml-perf-ai/              
│   └── workloads/              
└── common-scripts/              ← scripts d'analyse et de visualisation partagés (discuter avant de toucher)
```

Chaque sous-répertoire de `benchmarks/` doit contenir au minimum :
- Le programme eBPF côté noyau (`.bpf.c`)
- Le programme chargeur côté espace utilisateur (`.c`)
- Un `Makefile`
- Un dossier `results/` (les fichiers bruts sont ignorés par git, mais conservez un exemple)
- Un court `README.md` expliquant ce que fait votre benchmark

***

## Format de sortie du csv

Tous les benchmarks doivent produire un fichier CSV avec **exactement** ce schéma :

```
timestamp_ns,domain,library,workload,metric,value,comm
```

| Colonne        | Type | Description | Exemple |
|----------------|---|---|---|
| `timestamp_ns` | `uint64` | Temps noyau de l'événement (`bpf_ktime_get_ns()`) | `1746441600000000000` |
| `domain`       | `string` | Identifiant de votre domaine | `io-storage`, `network`, `security`, `ml-perf-ai` |
| `library`      | `string` | Bibliothèque eBPF utilisée | `libbpf`, `bcc`, `none` |
| `workload`     | `string` | Nom de la charge de travail exécutée | `write_4k_low`, `tcp_connect_high` |
| `metric`       | `string` | Ce qui est mesuré | `latency_ns`, `throughput_bps`, `cpu_pct` |
| `value`        | `float64` | La valeur mesurée | `3421.0` |
| `comm`         | `string` | Nom du processus qui a effectué l'écriture (`task_comm`) | `fio, dd, python` |

### Exemples de lignes

```csv
timestamp_ns,domain,library,workload,metric,value,comm
1746441600000000001,io-storage,libbpf,write_4k_low,latency_ns,3421.0,fio
1746441600000003000,network,libbpf,tcp_connect_high,latency_ns,812.0,iperf3
```

### Règles

- **Un événement par ligne.** Ne pas agréger dans le CSV — conservez les échantillons bruts. Les percentiles sont calculés en post-traitement.
- **Pas d'espaces dans les champs texte.** Utilisez des underscores : `write_4k` et non `write 4k`.
- **Toujours inclure une ligne d'en-tête.**
- **Encodage UTF-8, fins de ligne Unix (`\n`).**
- Le champ `comm` permet de filtrer les événements correspondant au workload ciblé et d'éliminer le bruit de fond du système. Toujours filtrer sur `comm` lors de l'analyse.

***

## Protocole de mesure de l'overhead

Chaque personne doit effectuer **deux séries** de sa charge de travail :

1. **Baseline** — charge en cours d'exécution, aucun programme eBPF attaché. Mettre `library = none`.
2. **Avec eBPF** — même charge, programme eBPF attaché. Mettre `library = libbpf` (ou `bcc` si vous y arrivez).

L'overhead est ensuite calculé ainsi :

```
overhead_pct = (moyenne_avec_ebpf - moyenne_baseline) / moyenne_baseline × 100
```

Cette formule sera appliquée uniformément par le script d'analyse commun afin que les résultats de chacun soient comparables.

### Niveaux d'intensité de la charge

Exécutez chaque charge à **trois niveaux d'intensité** et étiquetez-les de manière cohérente :

| Label | Description |
|---|---|
| `low` | Charge légère, peu d'opérations par seconde |
| `med` | Charge modérée et soutenue |
| `high` | Charge saturante (aussi élevée que votre machine le permet) |

Encodez l'intensité dans le champ `workload` : par ex. `write_4k_low`, `write_4k_med`, `write_4k_high`.

***

## Métriques à collecter

Chaque benchmark doit obligatoirement remonter :

| Nom de la métrique | Unité | 
|---|---|
| `latency_ns` | nanosecondes |
| `throughput_bps` | bytes/s ou ops/s |
| `cpu_pct` | pourcentage (0–100) |

Les métriques supplémentaires spécifiques à votre domaine sont les bienvenues — ajoutez simplement de nouvelles lignes avec une nouvelle valeur de `metric`.

***

## Workflow Git

- **Messages de commit** : utilisez le format `[domaine] description courte`, par ex. `[io-storage] ajout du kprobe d'entrée sur vfs_write`.
- **Ne commitez pas les fichiers de résultats bruts** (gros CSV). Ajoutez `results/*.csv` au `.gitignore`. Commitez uniquement un petit fichier exemple (`results/sample.csv`) pour référence.
- **Ne commitez pas les binaires compilés.**

### `.gitignore` suggéré (à placer dans votre sous-répertoire)

```
# Binaires compilés
*.o
*.skel.h
vmlinux.h
votre_programme

# Résultats (ne garder que les exemples)
results/*.csv
!results/sample.csv
```

***

## Environnement

Tous les benchmarks doivent être exécutés sur une **machine Linux bare-metal** (pas dans une VM si possible, car la virtualisation ajoute une latence I/O qui fausse les résultats). Si vous devez utiliser une VM, indiquez-le clairement dans le `README.md` de votre sous-répertoire.

Version noyau minimale : **5.15** (pour le support stable du ring buffer).

Enregistrez votre configuration dans vos résultats :

```bash
uname -r >> results/env.txt
lscpu >> results/env.txt
```

***
