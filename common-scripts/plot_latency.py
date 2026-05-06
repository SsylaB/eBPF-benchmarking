#!/usr/bin/env python3
"""
plot_latency.py — Visualise la distribution de latence d'ecriture dans le temps.
Usage: python3 plot_latency.py <fichier.csv> [--comm fio] [--out latency.png] [--ylim 20] [--bin 0.5]
"""

import argparse
import pandas as pd
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
import matplotlib.ticker as ticker
import os
import sys


def main():
    parser = argparse.ArgumentParser(description="Plot latency over time from write_lat CSV")
    parser.add_argument("csv", help="Fichier CSV genere par write_lat")
    parser.add_argument("--comm", default="fio", help="Filtrer par nom de processus (defaut: fio)")
    parser.add_argument("--out", default="latency_over_time.png", help="Fichier de sortie (defaut: latency_over_time.png)")
    parser.add_argument("--ylim", type=float, default=None, help="Limite max axe Y en us")
    parser.add_argument("--bin", type=float, default=0.5, dest="bin_size",
                        help="Taille des bins en secondes (defaut: 0.5)")
    args = parser.parse_args()

    # --- Chargement ---
    df = pd.read_csv(args.csv)
    df = df[df["metric"] == "latency_ns"].copy()

    if args.comm and args.comm != "all":
        df = df[df["comm"].str.strip() == args.comm].copy()

    if df.empty:
        print(f"[ERREUR] Aucune donnee latency_ns pour comm='{args.comm}' dans {args.csv}", file=sys.stderr)
        print(f"  comm disponibles : {pd.read_csv(args.csv)['comm'].str.strip().unique().tolist()}", file=sys.stderr)
        sys.exit(1)

    df["t_sec"]      = (df["timestamp_ns"] - df["timestamp_ns"].min()) / 1e9
    df["latency_us"] = df["value"] / 1e3

    # --- Percentiles globaux ---
    p50 = df["latency_us"].quantile(0.50)
    p95 = df["latency_us"].quantile(0.95)
    p99 = df["latency_us"].quantile(0.99)
    n   = len(df)

    # --- Resampling par bins ---
    df["t_bin"] = (df["t_sec"] // args.bin_size) * args.bin_size
    grouped  = df.groupby("t_bin")["latency_us"]
    bins     = grouped.median().index
    p50_ts   = grouped.quantile(0.50)
    p95_ts   = grouped.quantile(0.95)
    p99_ts   = grouped.quantile(0.99)
    pmin_ts  = grouped.min()

    # --- Tracé ---
    fig, ax = plt.subplots(figsize=(14, 5))

    ax.fill_between(bins, pmin_ts, p99_ts,
                    alpha=0.12, color="#4f98a3", label="min–p99")
    ax.fill_between(bins, pmin_ts, p95_ts,
                    alpha=0.25, color="#4f98a3", label="min–p95")

    ax.plot(bins, p50_ts, color="#01696f", linewidth=2,   label=f"p50 (mediane)")
    ax.plot(bins, p95_ts, color="#da7101", linewidth=1.5, linestyle="--", label="p95")
    ax.plot(bins, p99_ts, color="#a12c7b", linewidth=1.5, linestyle=":",  label="p99")

    # Limites axe Y
    if args.ylim:
        ax.set_ylim(0, args.ylim)
    else:
        ax.set_ylim(0, df["latency_us"].quantile(0.999) * 1.2)

    ax.set_xlabel("Temps (s)", fontsize=12)
    ax.set_ylabel("Latence (us)", fontsize=12)
    ax.set_title(
        f"Distribution latence vfs_write — {args.comm} (bins {args.bin_size}s)\n"
        f"p50={p50:.1f}us · p95={p95:.1f}us · p99={p99:.1f}us · n={n:,}",
        fontsize=13
    )
    ax.legend(loc="upper right", fontsize=10)
    ax.yaxis.set_major_formatter(ticker.FormatStrFormatter("%.0f"))
    ax.grid(True, alpha=0.2)
    fig.tight_layout()

    # --- Sauvegarde ---
    out_dir = os.path.dirname(args.out)
    if out_dir:
        os.makedirs(out_dir, exist_ok=True)

    fig.savefig(args.out, dpi=150)
    print(f"[OK] Graphe sauvegarde : {os.path.abspath(args.out)}")
    print(f"     n={n:,} events | p50={p50:.1f}us | p95={p95:.1f}us | p99={p99:.1f}us")


if __name__ == "__main__":
    main()