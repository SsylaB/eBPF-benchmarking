#!/usr/bin/env python3
"""
analyze.py — Off-CPU benchmark analysis
========================================
Reads one or more CSV files produced by csw, computes all metrics,
and generates publication-quality plots.

Usage:
    # Single run
    python3 analyze.py --csv results/run_2cpu.csv --label "2 CPUs"

    # Multi-run comparison (for continuity/p99/migration vs CPU-count plots)
    python3 analyze.py \
        --csv results/run_1cpu.csv  --label "1" \
        --csv results/run_2cpu.csv  --label "2" \
        --csv results/run_4cpu.csv  --label "4" \
        --csv results/run_8cpu.csv  --label "8" \
        --outdir plots/

CSV schema (produced by csw.c):
    timestamp_ns, pid, cpu, event_type, duration_ns, prev_cpu, comm
"""

import argparse
import sys
import os
from pathlib import Path
from typing import NamedTuple

import numpy as np
import pandas as pd
import matplotlib
matplotlib.use("Agg")   # headless — works on servers / CI
import matplotlib.pyplot as plt
import matplotlib.ticker as ticker

# ---------------------------------------------------------------------------
# Metrics container
# ---------------------------------------------------------------------------
class Metrics(NamedTuple):
    label: str
    n_cpu: int                  # CPU count label parsed from --label if numeric
    elapsed_s: float
    continuity: float           # C = T_on / (T_on + T_off)
    stall_freq: float           # stalls per second
    n_stalls: int
    avg_us: float
    p50_us: float
    p95_us: float
    p99_us: float
    p999_us: float
    n_migrations: int
    migration_rate: float       # per second
    cpu_residency: dict         # {cpu_id: fraction_of_oncpu_time}


# ---------------------------------------------------------------------------
# Core analysis
# ---------------------------------------------------------------------------
def load_csv(path: str) -> pd.DataFrame:
    df = pd.read_csv(path)
    required = {"timestamp_ns", "pid", "cpu", "event_type", "duration_ns"}
    missing = required - set(df.columns)
    if missing:
        raise ValueError(f"CSV missing columns: {missing}")
    df["timestamp_s"] = df["timestamp_ns"] / 1e9
    df["duration_us"] = df["duration_ns"] / 1e3
    return df


def compute_metrics(df: pd.DataFrame, label: str) -> Metrics:
    offcpu  = df[df["event_type"] == "OFFCPU"]
    oncpu   = df[df["event_type"] == "ONCPU"]
    migrate = df[df["event_type"] == "MIGRATE"]

    elapsed_s = (df["timestamp_ns"].max() - df["timestamp_ns"].min()) / 1e9
    if elapsed_s <= 0:
        elapsed_s = 1e-9

    total_offcpu_ns = offcpu["duration_ns"].sum()
    total_oncpu_ns  = oncpu["duration_ns"].sum()
    total_ns = total_offcpu_ns + total_oncpu_ns

    continuity = total_oncpu_ns / total_ns if total_ns > 0 else 0.0

    n_stalls = len(offcpu)
    stall_freq = n_stalls / elapsed_s

    lat = offcpu["duration_ns"].values / 1e3   # microseconds
    avg_us  = float(lat.mean())  if len(lat) > 0 else 0.0
    p50_us  = float(np.percentile(lat, 50))   if len(lat) > 0 else 0.0
    p95_us  = float(np.percentile(lat, 95))   if len(lat) > 0 else 0.0
    p99_us  = float(np.percentile(lat, 99))   if len(lat) > 0 else 0.0
    p999_us = float(np.percentile(lat, 99.9)) if len(lat) > 0 else 0.0

    n_migrations   = len(migrate)
    migration_rate = n_migrations / elapsed_s

    # CPU residency: fraction of on-CPU time per CPU core
    cpu_res = {}
    if len(oncpu) > 0 and total_oncpu_ns > 0:
        per_cpu = oncpu.groupby("cpu")["duration_ns"].sum()
        for cpu_id, ns in per_cpu.items():
            cpu_res[int(cpu_id)] = float(ns) / float(total_oncpu_ns)

    # Parse numeric label for axis ordering
    try:
        n_cpu = int(label)
    except ValueError:
        n_cpu = 0

    return Metrics(
        label=label,
        n_cpu=n_cpu,
        elapsed_s=elapsed_s,
        continuity=continuity,
        stall_freq=stall_freq,
        n_stalls=n_stalls,
        avg_us=avg_us,
        p50_us=p50_us,
        p95_us=p95_us,
        p99_us=p99_us,
        p999_us=p999_us,
        n_migrations=n_migrations,
        migration_rate=migration_rate,
        cpu_residency=cpu_res,
    )


def print_report(m: Metrics):
    print(f"\n=== {m.label} ===")
    print(f"  Elapsed          : {m.elapsed_s:.3f} s")
    print(f"  Continuity (C)   : {m.continuity:.4f}  ({m.continuity*100:.1f}%)")
    print(f"  Stalls           : {m.n_stalls}  ({m.stall_freq:.1f}/s)")
    print(f"  Off-CPU lat (µs) : avg={m.avg_us:.1f}  p50={m.p50_us:.1f}  "
          f"p95={m.p95_us:.1f}  p99={m.p99_us:.1f}  p99.9={m.p999_us:.1f}")
    print(f"  Migrations       : {m.n_migrations}  ({m.migration_rate:.2f}/s)")
    if m.cpu_residency:
        res_str = "  ".join(f"CPU{k}={v*100:.1f}%" for k,v in sorted(m.cpu_residency.items()))
        print(f"  CPU residency    : {res_str}")


# ---------------------------------------------------------------------------
# Plotting helpers
# ---------------------------------------------------------------------------
STYLE = {
    "figure.dpi":       150,
    "axes.spines.top":  False,
    "axes.spines.right":False,
    "font.size":        11,
    "axes.labelsize":   12,
    "axes.titlesize":   13,
    "legend.fontsize":  10,
}

def apply_style():
    plt.rcParams.update(STYLE)


def save(fig, outdir: Path, name: str):
    path = outdir / f"{name}.png"
    fig.savefig(path, bbox_inches="tight")
    plt.close(fig)
    print(f"  → saved {path}")


# ---------------------------------------------------------------------------
# Plot 1 — Off-CPU latency CDF (single or overlaid runs)
# ---------------------------------------------------------------------------
def plot_cdf(dfs: list[pd.DataFrame], labels: list[str], outdir: Path):
    apply_style()
    fig, ax = plt.subplots(figsize=(7, 4))

    for df, label in zip(dfs, labels):
        lat = df[df["event_type"] == "OFFCPU"]["duration_us"].values
        if len(lat) == 0:
            continue
        sorted_lat = np.sort(lat)
        cdf = np.arange(1, len(sorted_lat)+1) / len(sorted_lat)
        ax.plot(sorted_lat, cdf, label=label, linewidth=1.5)

    ax.set_xscale("log")
    ax.set_xlabel("Off-CPU latency (µs)")
    ax.set_ylabel("CDF")
    ax.set_title("Off-CPU Latency Distribution")
    ax.yaxis.set_major_formatter(ticker.PercentFormatter(xmax=1))
    ax.axvline(x=1000, color="grey", linestyle="--", linewidth=0.8, alpha=0.6)
    ax.text(1100, 0.05, "1 ms", color="grey", fontsize=9)
    ax.legend()
    save(fig, outdir, "offcpu_cdf")


# ---------------------------------------------------------------------------
# Plot 2 — Continuity ratio vs CPU count
# ---------------------------------------------------------------------------
def plot_continuity_vs_cpu(metrics: list[Metrics], outdir: Path):
    if len(metrics) < 2:
        return
    apply_style()
    ms = sorted(metrics, key=lambda m: m.n_cpu)
    labels = [m.label for m in ms]
    vals   = [m.continuity * 100 for m in ms]

    fig, ax = plt.subplots(figsize=(6, 4))
    ax.plot(labels, vals, marker="o", linewidth=2, color="#2176AE")
    ax.fill_between(range(len(labels)), vals, alpha=0.12, color="#2176AE")
    ax.set_xlabel("CPU allocation")
    ax.set_ylabel("Continuity C (%)")
    ax.set_title("Execution Continuity vs CPU Count")
    ax.set_ylim(0, 105)
    ax.axhline(y=100, color="green", linestyle="--", linewidth=0.8, alpha=0.5)
    save(fig, outdir, "continuity_vs_cpu")


# ---------------------------------------------------------------------------
# Plot 3 — p99 latency vs CPU count
# ---------------------------------------------------------------------------
def plot_p99_vs_cpu(metrics: list[Metrics], outdir: Path):
    if len(metrics) < 2:
        return
    apply_style()
    ms = sorted(metrics, key=lambda m: m.n_cpu)
    labels = [m.label for m in ms]
    p99s   = [m.p99_us for m in ms]

    fig, ax = plt.subplots(figsize=(6, 4))
    bars = ax.bar(labels, p99s, color="#E84855", alpha=0.85, edgecolor="white")
    ax.set_xlabel("CPU allocation")
    ax.set_ylabel("p99 Off-CPU latency (µs)")
    ax.set_title("p99 Off-CPU Latency vs CPU Count")
    for bar, val in zip(bars, p99s):
        ax.text(bar.get_x() + bar.get_width()/2, val + max(p99s)*0.01,
                f"{val:.0f}", ha="center", va="bottom", fontsize=9)
    save(fig, outdir, "p99_vs_cpu")


# ---------------------------------------------------------------------------
# Plot 4 — Migration rate vs CPU count
# ---------------------------------------------------------------------------
def plot_migration_vs_cpu(metrics: list[Metrics], outdir: Path):
    if len(metrics) < 2:
        return
    apply_style()
    ms = sorted(metrics, key=lambda m: m.n_cpu)
    labels = [m.label for m in ms]
    rates  = [m.migration_rate for m in ms]

    fig, ax = plt.subplots(figsize=(6, 4))
    ax.plot(labels, rates, marker="s", linewidth=2, color="#FF8C00")
    ax.set_xlabel("CPU allocation")
    ax.set_ylabel("Migration rate (migrations/s)")
    ax.set_title("CPU Migration Rate vs CPU Count")
    save(fig, outdir, "migration_vs_cpu")


# ---------------------------------------------------------------------------
# Plot 5 — CPU residency heatmap
# ---------------------------------------------------------------------------
def plot_cpu_residency(df: pd.DataFrame, label: str, outdir: Path):
    oncpu = df[df["event_type"] == "ONCPU"]
    if len(oncpu) == 0:
        return

    per_cpu = oncpu.groupby("cpu")["duration_ns"].sum().sort_index()
    total   = per_cpu.sum()
    fracs   = per_cpu / total * 100

    apply_style()
    fig, ax = plt.subplots(figsize=(max(4, len(fracs) * 0.8), 3))
    bars = ax.bar([f"CPU{c}" for c in fracs.index], fracs.values,
                  color="#5C6BC0", alpha=0.85)
    ax.set_ylabel("Fraction of on-CPU time (%)")
    ax.set_title(f"CPU Residency — {label}")
    ax.set_ylim(0, 105)
    for bar, val in zip(bars, fracs.values):
        ax.text(bar.get_x() + bar.get_width()/2, val + 1,
                f"{val:.1f}%", ha="center", va="bottom", fontsize=8)
    name = f"cpu_residency_{label.replace(' ', '_')}"
    save(fig, outdir, name)


# ---------------------------------------------------------------------------
# Plot 6 — Timeline: continuity over time (rolling window)
# ---------------------------------------------------------------------------
def plot_timeline(df: pd.DataFrame, label: str, outdir: Path,
                  window_s: float = 1.0):
    apply_style()
    df_sorted = df.sort_values("timestamp_s")
    t_start = df_sorted["timestamp_s"].min()
    t_end   = df_sorted["timestamp_s"].max()

    bins = np.arange(t_start, t_end + window_s, window_s)
    continuities = []

    for i in range(len(bins) - 1):
        lo, hi = bins[i], bins[i+1]
        win = df_sorted[(df_sorted["timestamp_s"] >= lo) &
                        (df_sorted["timestamp_s"] < hi)]
        on  = win[win["event_type"] == "ONCPU"]["duration_ns"].sum()
        off = win[win["event_type"] == "OFFCPU"]["duration_ns"].sum()
        total = on + off
        continuities.append(on / total * 100 if total > 0 else 0.0)

    t_mid = (bins[:-1] + bins[1:]) / 2 - t_start
    fig, ax = plt.subplots(figsize=(9, 3))
    ax.plot(t_mid, continuities, linewidth=1.2, color="#2176AE")
    ax.fill_between(t_mid, continuities, alpha=0.15, color="#2176AE")
    ax.set_xlabel("Time (s)")
    ax.set_ylabel("Continuity C (%)")
    ax.set_title(f"Continuity Over Time — {label}  (window={window_s:.1f}s)")
    ax.set_ylim(0, 105)
    name = f"timeline_{label.replace(' ', '_')}"
    save(fig, outdir, name)


# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------
def parse_args():
    p = argparse.ArgumentParser(
        description="Analyze csw CSV output and generate plots.")
    p.add_argument("--csv",   action="append", default=[], metavar="FILE",
                   help="CSV file to load (repeatable for multi-run comparison)")
    p.add_argument("--label", action="append", default=[], metavar="LABEL",
                   help="Label for each CSV (same order as --csv)")
    p.add_argument("--outdir", default="plots", metavar="DIR",
                   help="Output directory for plots (default: plots/)")
    p.add_argument("--window", type=float, default=1.0,
                   help="Rolling window size in seconds for timeline plot")
    return p.parse_args()


def main():
    args = parse_args()

    if not args.csv:
        print("Error: provide at least one --csv FILE", file=sys.stderr)
        sys.exit(1)

    # Pad labels if fewer were given than CSVs
    labels = list(args.label)
    while len(labels) < len(args.csv):
        labels.append(Path(args.csv[len(labels)]).stem)

    outdir = Path(args.outdir)
    outdir.mkdir(parents=True, exist_ok=True)

    dfs      : list[pd.DataFrame] = []
    metrics_list: list[Metrics]   = []

    print(f"Loading {len(args.csv)} run(s)…")
    for csv_path, label in zip(args.csv, labels):
        print(f"  {csv_path}  [{label}]")
        df = load_csv(csv_path)
        dfs.append(df)
        m = compute_metrics(df, label)
        metrics_list.append(m)
        print_report(m)

    print(f"\nGenerating plots in {outdir}/")

    plot_cdf(dfs, labels, outdir)

    for df, label in zip(dfs, labels):
        plot_cpu_residency(df, label, outdir)
        plot_timeline(df, label, outdir, window_s=args.window)

    if len(metrics_list) >= 2:
        plot_continuity_vs_cpu(metrics_list, outdir)
        plot_p99_vs_cpu(metrics_list, outdir)
        plot_migration_vs_cpu(metrics_list, outdir)

    print("\nDone.")


if __name__ == "__main__":
    main()