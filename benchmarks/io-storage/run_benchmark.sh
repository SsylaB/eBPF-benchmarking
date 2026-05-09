#!/bin/bash
# Usage: sudo ./run_benchmark.sh [output.csv]

OUTPUT=${1:-"results/run_$(date +%Y%m%d_%H%M%S).csv"}
FIO_FILE="/mnt/disk/fio_test"          # doit correspondre à --filename dans fio
BASELINE_SECS=3                   # fenêtre baseline avant fio

mkdir -p results

echo "==> Démarrage du benchmark io-storage"
echo "==> Sortie : $OUTPUT"
echo "==> Fenêtre baseline : ${BASELINE_SECS}s"

# 1. Lance write_lat AVANT fio pour capturer le bruit de fond
./write_lat --baseline $BASELINE_SECS > "$OUTPUT" &
WLAT_PID=$!
echo "==> write_lat lancé (PID=$WLAT_PID), collecte baseline..."

# 2. Attend la fenêtre baseline
sleep $BASELINE_SECS

# 3. Lance fio
fio --name=write_4k --rw=write --bs=4k --size=512M  \
    --filename=$FIO_FILE --direct=1 --runtime=30 --time_based \
    --ioengine=sync --loops=999 \
    --output=results/fio_stats.json --output-format=json &


FIO_PID=$!
echo "==> fio lancé (PID=$FIO_PID)"

# Attend que fio soit vraiment démarré
sleep 1

# 4. Récupère le vrai PID fio — vérification AVANT d'écrire le fichier
REAL_FIO_PID=$(pgrep -n fio)
if [ -z "$REAL_FIO_PID" ]; then
    echo "ERREUR : impossible de trouver le PID de fio"
    kill -SIGINT $WLAT_PID
    exit 1
fi
echo "==> fio réel PID=$REAL_FIO_PID"

# 5. Notifie write_lat du PID fio → bascule phase=workload
echo $REAL_FIO_PID > /tmp/wlat_fio_pid
kill -USR1 $WLAT_PID
echo "==> Phase workload activée"

# 6. Attend que fio termine
wait $FIO_PID
echo "==> fio terminé"

# 7. Arrête write_lat proprement
kill -SIGINT $WLAT_PID
wait $WLAT_PID 2>/dev/null

echo "==> Collecte terminée"
echo "==> Lignes collectées : $(wc -l < "$OUTPUT")"
echo "==> Métriques : $(cut -d',' -f5 "$OUTPUT" | sort -u | tr '\n' ' ')"
echo "==> Phases    : $(cut -d',' -f8 "$OUTPUT" | sort -u | tr '\n' ' ')"

# Nettoyage
rm -f $FIO_FILE /tmp/wlat_fio_pid