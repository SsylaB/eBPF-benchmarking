#!/usr/bin/env python3
"""
parse_results.py — Plot all metrics from write_lat CSV + fio JSON
Usage: python3 parse_results.py results/run_YYYYMMDD_HHMMSS.csv [results/fio_stats.json]
"""

import sys
import json
import pathlib
import pandas as pd
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
import matplotlib.patches as mpatches
import numpy as np

# ── Palette ──────────────────────────────────────────────────────────────────
C_BASELINE  = "#aecbcf"
C_WORKLOAD  = "#01696f"
C_P50       = "#01696f"
C_P95       = "#da7101"
C_P99       = "#a12c7b"
C_CPU_SYS   = "#006494"
C_CPU_WLAT  = "#7a39bb"
C_CPU_FIO   = "#437a22"
C_MEM_WLAT  = "#7a39bb"
C_MEM_FIO   = "#437a22"
C_IOPS      = "#006494"
C_BW        = "#da7101"

PHASE_ALPHA = {"baseline": 0.45, "workload": 1.0}

def shade_phases(ax, df_time, df_phase):
    """Draw a light background band for the baseline window."""
    bl = df_phase[df_phase == "baseline"]
    if bl.empty:
        return
    t0, t1 = df_time[bl.index[0]], df_time[bl.index[-1]]
    ax.axvspan(t0, t1, color=C_BASELINE, alpha=0.15, label="baseline window")

def rel_time(df):
    """Return seconds relative to first timestamp."""
    return (df["timestamp_ns"] - df["timestamp_ns"].iloc[0]) / 1e9

# ── Load CSV ─────────────────────────────────────────────────────────────────
if len(sys.argv) < 2:
    print("Usage: python3 parse_results.py <run.csv> [fio_stats.json]")
    sys.exit(1)

csv_path = pathlib.Path(sys.argv[1])
fio_path = pathlib.Path(sys.argv[2]) if len(sys.argv) >= 3 else csv_path.parent / "fio_stats.json"
out_dir  = csv_path.parent / "plots1"
out_dir.mkdir(exist_ok=True)

df = pd.read_csv(csv_path)
df.columns = df.columns.str.strip()

# Backwards-compat: if no 'phase' column, everything is workload
if "phase" not in df.columns:
    df["phase"] = "workload"

df["t"] = (df["timestamp_ns"] - df["timestamp_ns"].iloc[0]) / 1e9
BIN = 0.5   # seconds per bin for latency distribution

# ── Helper ───────────────────────────────────────────────────────────────────
def metric(name):
    return df[df["metric"] == name].copy()

def save(fig, name):
    p = out_dir / name
    fig.savefig(p, dpi=150, bbox_inches="tight")
    plt.close(fig)
    print(f"  Saved: {p}")

print(f"Loaded {len(df):,} rows from {csv_path}")
print(f"Metrics: {sorted(df['metric'].unique())}")
print(f"Phases:  {sorted(df['phase'].unique())}")

# ═══════════════════════════════════════════════════════════════════════════════
# PLOT 1 — Latency distribution over time (p50/p95/p99 + bands)
# ═══════════════════════════════════════════════════════════════════════════════
lat = metric("latency_ns").copy()
if not lat.empty:
    lat["lat_us"] = lat["value"] / 1e3
    lat["bin"]    = (lat["t"] / BIN).astype(int) * BIN

    grp = lat.groupby("bin")["lat_us"]
    stats = pd.DataFrame({
        "min":  grp.min(),
        "p50":  grp.median(),
        "p95":  grp.quantile(0.95),
        "p99":  grp.quantile(0.99),
        "max":  grp.max(),
    })

    global_p50 = lat["lat_us"].median()
    global_p95 = lat["lat_us"].quantile(0.95)
    global_p99 = lat["lat_us"].quantile(0.99)
    n          = len(lat)

    fig, ax = plt.subplots(figsize=(14, 4))
    shade_phases(ax, lat.groupby("bin")["t"].mean(), lat.groupby("bin")["phase"].first())

    ax.fill_between(stats.index, stats["min"], stats["p99"],
                    alpha=0.18, color=C_WORKLOAD, label="min–p99")
    ax.fill_between(stats.index, stats["min"], stats["p95"],
                    alpha=0.28, color=C_WORKLOAD, label="min–p95")
    ax.plot(stats.index, stats["p50"], color=C_P50,  lw=2,   label="p50 (médiane)")
    ax.plot(stats.index, stats["p95"], color=C_P95,  lw=1.5, ls="--", label="p95")
    ax.plot(stats.index, stats["p99"], color=C_P99,  lw=1.5, ls=":",  label="p99")

    ax.set_xlabel("Temps (s)")
    ax.set_ylabel("Latence (µs)")
    ax.set_title(
        f"Distribution latence vfs_write — fio (bins {BIN}s)\n"
        f"p50={global_p50:.1f}µs · p95={global_p95:.1f}µs · p99={global_p99:.1f}µs · n={n:,}"
    )
    ax.legend(loc="upper right", fontsize=8)
    ax.grid(axis="y", alpha=0.3)
    save(fig, "01_latency_over_time.png")

# ═══════════════════════════════════════════════════════════════════════════════
# PLOT 2 — Latency CDF (baseline vs workload)
# ═══════════════════════════════════════════════════════════════════════════════
if not lat.empty and "baseline" in lat["phase"].values:
    fig, ax = plt.subplots(figsize=(8, 5))
    for phase, color, ls in [("baseline", C_BASELINE, "--"), ("workload", C_WORKLOAD, "-")]:
        sub = lat[lat["phase"] == phase]["lat_us"]
        if sub.empty:
            continue
        sorted_v = np.sort(sub)
        cdf = np.arange(1, len(sorted_v) + 1) / len(sorted_v)
        ax.plot(sorted_v, cdf, color=color, ls=ls, lw=2, label=phase)
    ax.set_xlabel("Latence (µs)")
    ax.set_ylabel("CDF")
    ax.set_title("CDF latence vfs_write — baseline vs workload")
    ax.set_xlim(left=0)
    ax.legend()
    ax.grid(alpha=0.3)
    save(fig, "02_latency_cdf.png")

# ═══════════════════════════════════════════════════════════════════════════════
# PLOT 3 — CPU system global
# ═══════════════════════════════════════════════════════════════════════════════
cpu_sys = metric("cpu_system_pct")
if not cpu_sys.empty:
    fig, ax = plt.subplots(figsize=(14, 3))
    shade_phases(ax, cpu_sys["t"], cpu_sys["phase"])
    for phase in ["baseline", "workload"]:
        sub = cpu_sys[cpu_sys["phase"] == phase]
        if not sub.empty:
            ax.plot(sub["t"], sub["value"], color=C_CPU_SYS,
                    alpha=PHASE_ALPHA.get(phase, 1.0), lw=1.5)
    # Baseline mean as reference line
    bl_mean = cpu_sys[cpu_sys["phase"] == "baseline"]["value"].mean()
    if not np.isnan(bl_mean):
        ax.axhline(bl_mean, color=C_BASELINE, lw=1.2, ls="--",
                   label=f"moyenne baseline = {bl_mean:.1f}%")
    ax.set_xlabel("Temps (s)")
    ax.set_ylabel("CPU (%)")
    ax.set_title("CPU système global")
    ax.legend(fontsize=8)
    ax.grid(axis="y", alpha=0.3)
    save(fig, "03_cpu_system.png")

# ═══════════════════════════════════════════════════════════════════════════════
# PLOT 4 — CPU overhead de l'outil eBPF (write_lat lui-même)
# ═══════════════════════════════════════════════════════════════════════════════
cpu_wlat = metric("cpu_write_lat_pct")
if not cpu_wlat.empty:
    fig, ax = plt.subplots(figsize=(14, 3))
    shade_phases(ax, cpu_wlat["t"], cpu_wlat["phase"])
    ax.plot(cpu_wlat["t"], cpu_wlat["value"], color=C_CPU_WLAT, lw=1.5,
            label="overhead write_lat (libbpf)")
    bl_mean = cpu_wlat[cpu_wlat["phase"] == "baseline"]["value"].mean()
    if not np.isnan(bl_mean):
        ax.axhline(bl_mean, color=C_BASELINE, lw=1.2, ls="--",
                   label=f"moyenne baseline = {bl_mean:.2f}%")
    ax.set_xlabel("Temps (s)")
    ax.set_ylabel("CPU (%)")
    ax.set_title("Overhead CPU — outil eBPF (write_lat / libbpf)")
    ax.legend(fontsize=8)
    ax.grid(axis="y", alpha=0.3)
    save(fig, "04_cpu_ebpf_overhead.png")

# ═══════════════════════════════════════════════════════════════════════════════
# PLOT 5 — CPU fio (workload)
# ═══════════════════════════════════════════════════════════════════════════════
cpu_fio = metric("cpu_workload_pct")
if not cpu_fio.empty:
    fig, ax = plt.subplots(figsize=(14, 3))
    shade_phases(ax, cpu_fio["t"], cpu_fio["phase"])
    ax.plot(cpu_fio["t"], cpu_fio["value"], color=C_CPU_FIO, lw=1.5, label="CPU fio")
    ax.set_xlabel("Temps (s)")
    ax.set_ylabel("CPU (%)")
    ax.set_title("CPU fio (workload)")
    ax.legend(fontsize=8)
    ax.grid(axis="y", alpha=0.3)
    save(fig, "05_cpu_fio.png")

# ═══════════════════════════════════════════════════════════════════════════════
# PLOT 6 — Mémoire RSS (write_lat + fio)
# ═══════════════════════════════════════════════════════════════════════════════
mem_wlat = metric("mem_write_lat_kb")
mem_fio  = metric("mem_workload_kb")
if not mem_wlat.empty or not mem_fio.empty:
    fig, ax = plt.subplots(figsize=(14, 3))
    if not mem_wlat.empty:
        shade_phases(ax, mem_wlat["t"], mem_wlat["phase"])
        ax.plot(mem_wlat["t"], mem_wlat["value"] / 1024,
                color=C_MEM_WLAT, lw=1.5, label="write_lat RSS (MB)")
    if not mem_fio.empty:
        ax.plot(mem_fio["t"], mem_fio["value"] / 1024,
                color=C_MEM_FIO, lw=1.5, label="fio RSS (MB)")
    ax.set_xlabel("Temps (s)")
    ax.set_ylabel("Mémoire RSS (MB)")
    ax.set_title("Empreinte mémoire — write_lat vs fio")
    ax.legend(fontsize=8)
    ax.grid(axis="y", alpha=0.3)
    save(fig, "06_memory_rss.png")

# ═══════════════════════════════════════════════════════════════════════════════
# PLOT 7 & 8 — Throughput + IOPS depuis fio JSON
# ═══════════════════════════════════════════════════════════════════════════════
if fio_path.exists():
    with open(fio_path) as f:
        fio = json.load(f)

    job = fio["jobs"][0]
    wr  = job["write"]

    # fio clat histogram (completion latency)
    if "clat_ns" in wr and "percentile" in wr["clat_ns"]:
        perc = wr["clat_ns"]["percentile"]
        labels = list(perc.keys())
        values = [v / 1e3 for v in perc.values()]  # ns → µs

        fig, ax = plt.subplots(figsize=(8, 4))
        ax.bar(labels, values, color=C_WORKLOAD, alpha=0.8)
        ax.set_xlabel("Percentile")
        ax.set_ylabel("Latence completion (µs)")
        ax.set_title("fio clat percentiles (write_4k, direct=1)")
        ax.grid(axis="y", alpha=0.3)
        plt.xticks(rotation=45, ha="right", fontsize=8)
        save(fig, "07_fio_clat_percentiles.png")

    # Throughput & IOPS summary bar
    bw_mb   = wr.get("bw",   0) / 1024  # KB/s → MB/s
    iops    = wr.get("iops", 0)
    lat_avg = wr.get("lat_ns", {}).get("mean", 0) / 1e3  # ns → µs

    fig, axes = plt.subplots(1, 3, figsize=(10, 4))
    for ax, val, label, color, unit in zip(
        axes,
        [bw_mb, iops, lat_avg],
        ["Throughput", "IOPS", "Lat. moyenne"],
        [C_BW, C_IOPS, C_P50],
        ["MB/s", "ops/s", "µs"]
    ):
        ax.bar([label], [val], color=color, alpha=0.85)
        ax.set_ylabel(unit)
        ax.set_title(f"{val:.1f} {unit}")
        ax.grid(axis="y", alpha=0.3)
    fig.suptitle("fio — write_4k sequential (résumé)", y=1.02)
    save(fig, "08_fio_summary.png")
    print(f"  fio: {bw_mb:.1f} MB/s | {iops:.0f} IOPS | lat_avg={lat_avg:.1f}µs")
else:
    print(f"  (fio JSON non trouvé : {fio_path} — plots 7/8 ignorés)")

print(f"\nDone. Plots in: {out_dir}/")
