# Security Benchmark - eBPF File Access Control

## 📋 Description

Ce benchmark mesure l'**overhead de performance** des hooks eBPF utilisés pour l'audit et le blocage d'accès aux fichiers sensibles.

### 3 scénarios testés:

1. **Sans eBPF** (Baseline)
   - Accès normal aux fichiers système
   
2. **Avec eBPF - Mode Audit**
   - Tous les accès aux fichiers sensibles sont loggés
   - Aucun blocage, impact en lecture seulement
   
3. **Avec eBPF - Mode Prévention**
   - Les accès aux fichiers sensibles sont bloqués pour les processus "taintés"
   - Un processus est considéré tainte s'il a établi une connexion réseau

---

## 🏗️ Architecture eBPF

### Composants Kernel (security.bpf.c)

| Hook | Rôle |
|------|------|
| `tp/syscalls/sys_enter_connect` | Marque les processus connectés au réseau (Taint Mode) |
| `tp/syscalls/sys_enter_open*` | Tracepoints pour tracer l'ouverture des fichiers sensibles |
| `lsm/file_open` | Hook LSM qui bloque les accès (si processus tainte) |

### Maps eBPF

| Map | Type | Objectif |
|-----|------|----------|
| `sensitive_files` | HASH | Liste des fichiers à protéger |
| `tainted_pids` | HASH | PID marqués comme "infectés" (connectés au réseau) |
| `active_opens` | HASH | État transitoire entre tracepoint et LSM |
| `rb` | RINGBUF | Envoi d'événements vers userspace |

### Composants Userspace (security.c)

- Charge et attache le programme eBPF
- Initialise la map des fichiers sensibles
- Reçoit les événements via ring buffer
- Enregistre les accès dans `audit_log.csv`

---

## 🚀 Utilisation

### Compilation

```bash
cd benchmarks/security
make clean && make
```

Cela génère:
- `security.bpf.o` - Objet eBPF compilé
- `security.skel.h` - Skeleton généré par bpftool
- `security` - Exécutable final

### Exécution du benchmark

```bash
# Test des 3 scénarios
sudo python3 benchmark.py Sans_eBPF
sudo python3 benchmark.py Avec_eBPF_Audit
sudo python3 benchmark.py Avec_eBPF_Bloque
```

Cela génère `benchmark_results.csv` avec les résultats.

### Visualisation des résultats

```bash
python3 plot_results.py
```

Génère `benchmark_results.png` avec 2 graphiques:
- Latence absolue par appel
- Overhead (%) par rapport à la baseline

---

## 📊 Résultats typiques

| Scénario | Latence (ms) | Overhead |
|----------|------------|----------|
| Sans eBPF | 0.0052 | Baseline |
| Avec Audit | 0.0091 | +75% |
| Avec Prévention | 0.0050 | -6% |

---

## 📦 Dépendances

### Kernel
- Linux 5.15+ (pour LSM BPF)
- bpftool (pour générer le skeleton)

### Userspace
```bash
sudo apt install clang llvm libbpf-dev libelf-dev
pip install pandas matplotlib
```

---

## 📝 Fichiers du répertoire

| Fichier | Type | Description |
|---------|------|-------------|
| `security.bpf.c` | Source | Programme eBPF kernel |
| `security.c` | Source | Programme userspace chargeur |
| `Makefile` | Build | Instructions de compilation |
| `benchmark.py` | Test | Script de benchmark |
| `plot_results.py` | Analyse | Génération de graphiques |
| `audit_log.csv` | Output | Logs des accès enregistrés |
| `benchmark_results.csv` | Output | Résultats des benchmarks |

---

## 🔧 Troubleshooting

**Erreur: "command not found: bpftool"**
```bash
sudo apt install linux-tools-generic
```

**Erreur: "LSM BPF not available"**
```bash
# Vérifier que le kernel support LSM BPF (5.15+)
uname -r
grep CONFIG_BPF_LSM /boot/config-$(uname -r)
```

**Permission denied sur vmlinux.h**
```bash
# Générer vmlinux.h localement
bpftool btf dump file /sys/kernel/btf/vmlinux format c > vmlinux.h
```

---

## 📚 Références

- [eBPF & LSM Documentation](https://www.kernel.org/doc/html/latest/bpf/index.html)
- [libbpf GitHub](https://github.com/libbpf/libbpf)
- [BPF Security Hooks](https://lwn.net/Articles/709232/)
