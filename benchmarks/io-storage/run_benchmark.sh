#!/bin/bash
# Usage: sudo ./run_benchmark.sh [output.csv]

OUTPUT=${1:-"results/run_$(date +%Y%m%d_%H%M%S).csv"}
FIO_FILE="/tmp/fio_benchmark_test"
FIO_SIZE="256M"

mkdir -p results

echo "==> Démarrage du benchmark io-storage"
echo "==> Sortie : $OUTPUT"

# Lance fio en arrière-plan
fio --name=write_4k --rw=write --bs=4k --size=4G --filename=/tmp/fio_test --direct=1 --runtime=30 --time_based --output=results/fio_stats.json --output-format=json &

FIO_PID=$!
echo "==> fio lancé (sudo PID=$FIO_PID)"

# Attend que fio soit vraiment démarré
sleep 1

# Récupère le vrai PID du processus fio (enfant de sudo)
REAL_FIO_PID=$(pgrep -n fio)
if [ -z "$REAL_FIO_PID" ]; then
    echo "ERREUR : impossible de trouver le PID de fio"
    exit 1
fi
echo "==> fio réel PID=$REAL_FIO_PID"

./write_lat $REAL_FIO_PID > "$OUTPUT" &
WLAT_PID=$!
echo "==> write_lat lancé (PID=$WLAT_PID)"

# Attend que fio termine
wait $FIO_PID
echo "==> fio terminé"

# Arrête write_lat proprement
kill -SIGINT $WLAT_PID
wait $WLAT_PID 2>/dev/null

echo "==> Collecte terminée"
echo "==> Lignes collectées : $(wc -l < "$OUTPUT")"
echo "==> Métriques : $(cut -d',' -f5 "$OUTPUT" | sort -u | tr '\n' ' ')"

# Nettoyage
rm -f $FIO_FILE