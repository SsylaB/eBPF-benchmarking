#!/usr/bin/env python3
"""
analyze.py — stress-ng uprobe benchmark analysis.

CSV schema (from csw.c):
    timestamp_ns, pid, tid, cpu, event_type, duration_ns, comm

event_type: EXEC (work unit duration) | GAP (idle between units)
"""

import argparse, sys
from pathlib import Path
from typing import NamedTuple

import numpy as np
import pandas as pd
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
import matplotlib.ticker as ticker

class Metrics(NamedTuple):
    label: str
    n_cpu: int
    elapsed_s: float
    continuity: float
    n_exec: int
    n_gaps: int
    stall_freq: float
    avg_exec_us: float
    p50_exec_us: float
    p95_exec_us: float
    p99_exec_us: float
    p999_exec_us: float
    avg_gap_us: float
    p99_gap_us: float
    cpu_residency: dict

STYLE = {
    "figure.dpi": 150,
    "axes.spines.top": False,
    "axes.spines.right": False,
    "font.size": 11,
    "axes.labelsize": 12,
    "axes.titlesize": 13,
}

def apply_style(): plt.rcParams.update(STYLE)

def save(fig, outdir, name):
    p = outdir / f"{name}.png"
    fig.savefig(p, bbox_inches="tight")
    plt.close(fig)
    print(f"  → {p}")

def load_csv(path):
    df = pd.read_csv(path)
    df["timestamp_s"]  = df["timestamp_ns"] / 1e9
    df["duration_us"]  = df["duration_ns"]  / 1e3
    return df

def compute_metrics(df, label):
    exec_df = df[df["event_type"] == "EXEC"]
    gap_df  = df[df["event_type"] == "GAP"]

    elapsed_s = (df["timestamp_ns"].max() - df["timestamp_ns"].min()) / 1e9
    if elapsed_s <= 0: elapsed_s = 1e-9

    total_exec = exec_df["duration_ns"].sum()
    total_gap  = gap_df["duration_ns"].sum()
    total      = total_exec + total_gap
    continuity = total_exec / total if total else 0.0

    ex = exec_df["duration_ns"].values / 1e3
    gp = gap_df["duration_ns"].values  / 1e3

    def pct(arr, p): return float(np.percentile(arr, p)) if len(arr) else 0.0

    cpu_res = {}
    if len(exec_df):
        per_cpu = exec_df.groupby("cpu")["duration_ns"].sum()
        t = per_cpu.sum()
        for c, v in per_cpu.items():
            cpu_res[int(c)] = float(v) / float(t)

    try: n_cpu = int(label)
    except: n_cpu = 0

    return Metrics(
        label=label, n_cpu=n_cpu, elapsed_s=elapsed_s,
        continuity=continuity,
        n_exec=len(exec_df), n_gaps=len(gap_df),
        stall_freq=len(gap_df)/elapsed_s,
        avg_exec_us=float(ex.mean()) if len(ex) else 0.0,
        p50_exec_us=pct(ex,50), p95_exec_us=pct(ex,95),
        p99_exec_us=pct(ex,99), p999_exec_us=pct(ex,99.9),
        avg_gap_us=float(gp.mean()) if len(gp) else 0.0,
        p99_gap_us=pct(gp,99),
        cpu_residency=cpu_res,
    )

def print_report(m):
    print(f"\n=== {m.label} ===")
    print(f"  Elapsed          : {m.elapsed_s:.3f} s")
    print(f"  Continuity C     : {m.continuity:.4f}  ({m.continuity*100:.1f}%)")
    print(f"  Work units       : {m.n_exec}  |  Gaps: {m.n_gaps}  ({m.stall_freq:.1f}/s)")
    print(f"  Exec time (µs)   : avg={m.avg_exec_us:.1f}  p50={m.p50_exec_us:.1f}"
          f"  p95={m.p95_exec_us:.1f}  p99={m.p99_exec_us:.1f}  p99.9={m.p999_exec_us:.1f}")
    print(f"  Gap time  (µs)   : avg={m.avg_gap_us:.1f}  p99={m.p99_gap_us:.1f}")
    if m.cpu_residency:
        res = "  ".join(f"CPU{k}={v*100:.1f}%" for k,v in sorted(m.cpu_residency.items()))
        print(f"  CPU residency    : {res}")

# ---- Plots ----------------------------------------------------------------

def plot_exec_cdf(dfs, labels, outdir):
    apply_style()
    fig, ax = plt.subplots(figsize=(7, 4))
    for df, label in zip(dfs, labels):
        lat = df[df["event_type"]=="EXEC"]["duration_us"].values
        if not len(lat): continue
        s = np.sort(lat)
        ax.plot(s, np.arange(1,len(s)+1)/len(s), label=label, linewidth=1.5)
    ax.set_xscale("log")
    ax.set_xlabel("Work unit execution time (µs)")
    ax.set_ylabel("CDF")
    ax.set_title("Execution Time Distribution (per work unit)")
    ax.yaxis.set_major_formatter(ticker.PercentFormatter(xmax=1))
    ax.legend()
    save(fig, outdir, "exec_cdf")

def plot_gap_cdf(dfs, labels, outdir):
    apply_style()
    fig, ax = plt.subplots(figsize=(7, 4))
    for df, label in zip(dfs, labels):
        lat = df[df["event_type"]=="GAP"]["duration_us"].values
        if not len(lat): continue
        s = np.sort(lat)
        ax.plot(s, np.arange(1,len(s)+1)/len(s), label=label, linewidth=1.5)
    ax.set_xscale("log")
    ax.set_xlabel("Inter-unit gap (µs)")
    ax.set_ylabel("CDF")
    ax.set_title("Off-CPU Gap Distribution (between work units)")
    ax.yaxis.set_major_formatter(ticker.PercentFormatter(xmax=1))
    ax.axvline(x=1000, color="grey", linestyle="--", linewidth=0.8, alpha=0.6)
    ax.text(1100, 0.05, "1 ms", color="grey", fontsize=9)
    ax.legend()
    save(fig, outdir, "gap_cdf")

def plot_continuity_vs_cpu(metrics, outdir):
    if len(metrics) < 2: return
    apply_style()
    ms = sorted(metrics, key=lambda m: m.n_cpu)
    labels = [m.label for m in ms]
    vals   = [m.continuity * 100 for m in ms]
    fig, ax = plt.subplots(figsize=(6, 4))
    ax.plot(labels, vals, marker="o", linewidth=2, color="#2176AE")
    ax.fill_between(range(len(labels)), vals, alpha=0.12, color="#2176AE")
    ax.set_xlabel("CPU allocation"); ax.set_ylabel("Continuity C (%)")
    ax.set_title("Execution Continuity vs CPU Count")
    ax.set_ylim(0, 105)
    ax.axhline(y=100, color="green", linestyle="--", linewidth=0.8, alpha=0.5)
    save(fig, outdir, "continuity_vs_cpu")

def plot_p99_exec_vs_cpu(metrics, outdir):
    if len(metrics) < 2: return
    apply_style()
    ms = sorted(metrics, key=lambda m: m.n_cpu)
    labels = [m.label for m in ms]
    p99s   = [m.p99_exec_us for m in ms]
    fig, ax = plt.subplots(figsize=(6, 4))
    bars = ax.bar(labels, p99s, color="#E84855", alpha=0.85, edgecolor="white")
    ax.set_xlabel("CPU allocation"); ax.set_ylabel("p99 execution time (µs)")
    ax.set_title("p99 Work Unit Execution Time vs CPU Count")
    for bar, val in zip(bars, p99s):
        ax.text(bar.get_x()+bar.get_width()/2, val+max(p99s)*0.01,
                f"{val:.0f}", ha="center", va="bottom", fontsize=9)
    save(fig, outdir, "p99_exec_vs_cpu")

def plot_gap_rate_vs_cpu(metrics, outdir):
    if len(metrics) < 2: return
    apply_style()
    ms = sorted(metrics, key=lambda m: m.n_cpu)
    labels = [m.label for m in ms]; rates = [m.stall_freq for m in ms]
    fig, ax = plt.subplots(figsize=(6, 4))
    ax.plot(labels, rates, marker="s", linewidth=2, color="#FF8C00")
    ax.set_xlabel("CPU allocation"); ax.set_ylabel("Gap rate (gaps/s)")
    ax.set_title("Inter-Unit Gap Rate vs CPU Count")
    save(fig, outdir, "gap_rate_vs_cpu")

def plot_timeline(df, label, outdir, window_s=1.0):
    apply_style()
    df = df.sort_values("timestamp_s")
    t0, t1 = df["timestamp_s"].min(), df["timestamp_s"].max()
    bins = np.arange(t0, t1 + window_s, window_s)
    vals = []
    for lo, hi in zip(bins[:-1], bins[1:]):
        w = df[(df["timestamp_s"] >= lo) & (df["timestamp_s"] < hi)]
        ex = w[w["event_type"]=="EXEC"]["duration_ns"].sum()
        gp = w[w["event_type"]=="GAP"]["duration_ns"].sum()
        tot = ex + gp
        vals.append(ex/tot*100 if tot else 0.0)
    t_mid = (bins[:-1]+bins[1:])/2 - t0
    fig, ax = plt.subplots(figsize=(9, 3))
    ax.plot(t_mid, vals, linewidth=1.2, color="#2176AE")
    ax.fill_between(t_mid, vals, alpha=0.15, color="#2176AE")
    ax.set_xlabel("Time (s)"); ax.set_ylabel("Continuity C (%)")
    ax.set_title(f"Continuity Over Time — {label}  (window={window_s:.1f}s)")
    ax.set_ylim(0, 105)
    save(fig, outdir, f"timeline_{label.replace(' ','_')}")

def plot_cpu_residency(df, label, outdir):
    exec_df = df[df["event_type"]=="EXEC"]
    if not len(exec_df): return
    per_cpu = exec_df.groupby("cpu")["duration_ns"].sum().sort_index()
    fracs = per_cpu / per_cpu.sum() * 100
    apply_style()
    fig, ax = plt.subplots(figsize=(max(4, len(fracs)*0.8), 3))
    bars = ax.bar([f"CPU{c}" for c in fracs.index], fracs.values, color="#5C6BC0", alpha=0.85)
    ax.set_ylabel("Fraction of exec time (%)"); ax.set_title(f"CPU Residency — {label}")
    ax.set_ylim(0, 105)
    for bar, val in zip(bars, fracs.values):
        ax.text(bar.get_x()+bar.get_width()/2, val+1, f"{val:.1f}%",
                ha="center", va="bottom", fontsize=8)
    save(fig, outdir, f"cpu_residency_{label.replace(' ','_')}")

# ---- CLI ------------------------------------------------------------------

def main():
    p = argparse.ArgumentParser()
    p.add_argument("--csv",    action="append", default=[], metavar="FILE")
    p.add_argument("--label",  action="append", default=[], metavar="LABEL")
    p.add_argument("--outdir", default="plots")
    p.add_argument("--window", type=float, default=1.0)
    args = p.parse_args()

    if not args.csv:
        print("Error: provide at least one --csv FILE", file=sys.stderr); sys.exit(1)

    labels = list(args.label)
    while len(labels) < len(args.csv):
        labels.append(Path(args.csv[len(labels)]).stem)

    outdir = Path(args.outdir); outdir.mkdir(parents=True, exist_ok=True)
    dfs, metrics_list = [], []

    for path, label in zip(args.csv, labels):
        print(f"Loading {path}  [{label}]")
        df = load_csv(path); dfs.append(df)
        m  = compute_metrics(df, label); metrics_list.append(m)
        print_report(m)

    print(f"\nGenerating plots in {outdir}/")
    plot_exec_cdf(dfs, labels, outdir)
    plot_gap_cdf(dfs, labels, outdir)
    for df, label in zip(dfs, labels):
        plot_timeline(df, label, outdir, args.window)
        plot_cpu_residency(df, label, outdir)
    if len(metrics_list) >= 2:
        plot_continuity_vs_cpu(metrics_list, outdir)
        plot_p99_exec_vs_cpu(metrics_list, outdir)
        plot_gap_rate_vs_cpu(metrics_list, outdir)
    print("Done.")

if __name__ == "__main__":
    main()