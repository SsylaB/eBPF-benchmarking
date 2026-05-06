#!/usr/bin/env python3
"""
plot_latency.py — Visualise l'évolution de la latence d'écriture dans le temps.
Usage: python3 plot_latency.py <fichier.csv> [--comm fio] [--out latency.png] [--rolling 50]
"""

import argparse
import pandas as pd
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
import matplotlib.ticker as ticker
import sys

def main():
    parser = argparse.ArgumentParser(description="Plot latency over time from write_lat CSV")
    parser.add_argument("csv", help="Fichier CSV généré par write_lat")
    parser.add_argument("--comm", default="fio", help="Filtrer par nom de processus (défaut: fio)")
    parser.add_argument("--out", default="plots/latency_over_time.png", help="Fichier de sortie (défaut: latency_over_time.png)")
    parser.add_argument("--rolling", type=int, default=50, help="Fenêtre de moyenne mobile (défaut: 50)")
    parser.add_argument("--ylim", type=float, default=None, help="Limite max axe Y en µs")
    args = parser.parse_args()

    df = pd.read_csv(args.csv)
    df = df[(df["metric"] == "latency_ns") & (df["comm"].str.strip() == args.comm)].copy()

    if df.empty:
        print(f"Aucune donnée pour comm='{args.comm}' dans {args.csv}", file=sys.stderr)
        sys.exit(1)

    df["t_sec"]      = (df["timestamp_ns"] - df["timestamp_ns"].min()) / 1e9
    df["latency_us"] = df["value"] / 1e3
    df["rolling"]    = df["latency_us"].rolling(window=args.rolling, min_periods=1).mean()

    p50 = df["latency_us"].quantile(0.50)
    p95 = df["latency_us"].quantile(0.95)
    p99 = df["latency_us"].quantile(0.99)

    fig, ax = plt.subplots(figsize=(12, 5))

    # Points bruts
    ax.scatter(df["t_sec"], df["latency_us"],
               s=4, alpha=0.3, color="#4f98a3", label="Latence brute", zorder=2)

    # Moyenne mobile
    ax.plot(df["t_sec"], df["rolling"],
            color="#01696f", linewidth=2,
            label=f"Moy. mobile (n={args.rolling})", zorder=3)

    # Percentiles
    ax.axhline(p99, color="#a12c7b", linestyle="--", linewidth=1.2,
               label=f"p99 = {p99:.1f} µs")
    ax.axhline(p95, color="#da7101", linestyle=":",  linewidth=1.2,
               label=f"p95 = {p95:.1f} µs")

    ax.set_xlabel("Temps (s)", fontsize=12)
    ax.set_ylabel("Latence (µs)", fontsize=12)
    ax.set_title(
        f"Latence vfs_write — {args.comm}\n"
        f"p50={p50:.1f}µs · p95={p95:.1f}µs · p99={p99:.1f}µs · n={len(df)}",
        fontsize=13
    )
    ax.legend(loc="upper right", fontsize=10)
    ax.yaxis.set_major_formatter(ticker.FormatStrFormatter("%.0f"))
    ax.grid(True, alpha=0.2)
    if args.ylim:
        ax.set_ylim(0, args.ylim)
    else:
        ax.set_ylim(0, df["latency_us"].quantile(0.999) * 1.2)
    fig.tight_layout()

    fig.savefig(args.out, dpi=150)
    print(f"Graphe sauvegardé : {args.out}")
    print(f"  n={len(df)} events | p50={p50:.1f}µs | p95={p95:.1f}µs | p99={p99:.1f}µs")

if __name__ == "__main__":
    main()