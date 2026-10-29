#!/usr/bin/env python3
"""Make speedup charts from a benchmark CSV.

    python3 tools/plot.py results/results.csv results/

One PNG per workload: speedup vs threads, one line per policy, with the
best possible speedup on this machine's cores for reference. Also a chart for the par cutoff sweep.
"""
import csv
import os
import sys
from collections import defaultdict

try:
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt
except ImportError:
    sys.exit("matplotlib is needed: pip install matplotlib")

# Fixed colour per policy, so a policy keeps its colour in every chart.
POLICY_COLOR = {"static": "#2a78d6", "shared": "#eb6834", "stealing": "#1baf7a"}
POLICY_ORDER = ["static", "shared", "stealing"]
INK, MUTED, GRID = "#1f1f1e", "#6b6a63", "#e4e3dd"
CORES = os.cpu_count() or 1   # change if you plot results from another machine


def load(path):
    with open(path, newline="") as f:
        return list(csv.DictReader(f))


def style(ax, title, xlabel, ylabel):
    ax.set_title(title, color=INK, fontsize=12, loc="left")
    ax.set_xlabel(xlabel, color=MUTED)
    ax.set_ylabel(ylabel, color=MUTED)
    ax.grid(True, color=GRID, linewidth=0.8)
    ax.set_axisbelow(True)
    for side in ("top", "right"):
        ax.spines[side].set_visible(False)
    for side in ("left", "bottom"):
        ax.spines[side].set_color(GRID)
    ax.tick_params(colors=MUTED)


def plot_speedup(rows, out_dir):
    by_workload = defaultdict(lambda: defaultdict(list))
    for r in rows:
        if r["policy"] == "sequential":
            continue
        by_workload[r["workload"]][r["policy"]].append(
            (int(r["threads"]), float(r["speedup"])))

    made = []
    for workload, policies in by_workload.items():
        threads_seen = sorted({t for pts in policies.values() for t, _ in pts})
        if len(threads_seen) < 2:
            continue   # single-thread-count rows (size/cutoff sweeps) are plotted elsewhere
        fig, ax = plt.subplots(figsize=(6.4, 4))
        # Speedup can't beat the number of cores, so the reference line flattens there.
        ideal = [min(t, CORES) for t in threads_seen]
        ax.plot(threads_seen, ideal, linestyle="--", color=MUTED, linewidth=1.2,
                label=f"best possible on {CORES} cores")
        for policy in POLICY_ORDER:
            if policy not in policies:
                continue
            pts = sorted(policies[policy])
            xs, ys = [p[0] for p in pts], [p[1] for p in pts]
            ax.plot(xs, ys, marker="o", markersize=8, linewidth=2,
                    color=POLICY_COLOR[policy], label=policy)
        style(ax, f"{workload}: speedup vs threads", "threads", "speedup over sequential")
        ax.set_xticks(threads_seen)
        ax.set_ylim(bottom=0)
        ax.legend(frameon=False, fontsize=9)
        fig.tight_layout()
        path = os.path.join(out_dir, f"speedup_{workload}.png")
        fig.savefig(path, dpi=120)
        plt.close(fig)
        made.append(path)
    return made


def plot_cutoff(rows, out_dir):
    pts = defaultdict(list)
    for r in rows:
        name = r["workload"]
        if name.startswith("lang-cut") and r["policy"] != "sequential":
            pts[r["policy"]].append((int(name[len("lang-cut"):]), float(r["speedup"])))
    if not pts:
        return []
    fig, ax = plt.subplots(figsize=(6.4, 4))
    ax.axhline(1.0, color=MUTED, linestyle="--", linewidth=1.2, label="sequential")
    for policy in POLICY_ORDER:
        if policy in pts:
            data = sorted(pts[policy])
            ax.plot([d[0] for d in data], [d[1] for d in data], marker="o", markersize=8,
                    linewidth=2, color=POLICY_COLOR[policy], label=policy)
    style(ax, "PFL fib: how deep should par make tasks?", "par cutoff depth",
          "speedup over sequential")
    ax.set_xticks(sorted({d[0] for data in pts.values() for d in data}))
    ax.set_ylim(bottom=0)
    ax.legend(frameon=False, fontsize=9)
    fig.tight_layout()
    path = os.path.join(out_dir, "lang_cutoff.png")
    fig.savefig(path, dpi=120)
    plt.close(fig)
    return [path]


def main():
    if len(sys.argv) < 2:
        sys.exit("usage: plot.py results.csv [out_dir]")
    out_dir = sys.argv[2] if len(sys.argv) > 2 else "."
    os.makedirs(out_dir, exist_ok=True)
    rows = load(sys.argv[1])
    for path in plot_speedup(rows, out_dir) + plot_cutoff(rows, out_dir):
        print("wrote", path)


if __name__ == "__main__":
    main()
