#!/usr/bin/env python3
"""Deterministic analysis of benchmark results and task traces.

    python3 tools/trace_report.py results/results.csv [trace_bfs-skewed.csv ...]

Every finding comes from fixed rules applied to measured numbers, and prints
the numbers it is based on. No AI is involved here; ai_explain.py builds on
this output.
"""
import csv
import os
import re
import sys
from collections import defaultdict

# Rule thresholds. Kept in one place so they are easy to question.
IDLE_PCT = 20.0          # workers idle more than this share of the run
IMBALANCE = 1.5          # busiest worker ran 1.5x the average number of tasks
DEEP_NESTING = 100       # tasks stacked inside waits on one thread
SEARCH_OVERHEAD = 1.2    # parallel chess searched 20% more nodes
STATIC_GAP = 1.25        # static 25% slower than work stealing


def load_results(path):
    with open(path, newline="") as f:
        rows = list(csv.DictReader(f))
    for r in rows:
        r["threads"] = int(r["threads"])
        for key in ("time_ms", "speedup", "efficiency", "idle_pct", "imbalance"):
            r[key] = float(r[key])
        r["max_depth"] = int(float(r.get("max_depth", 0) or 0))
        r["correct"] = r["correct"] == "1"
    return rows


def extra_value(row, key):
    m = re.search(key + r"=([0-9.]+)", row.get("extra", ""))
    return float(m.group(1)) if m else None


def analyse(rows, cores):
    """Returns a list of (workload, finding, evidence) tuples."""
    findings = []
    by_workload = defaultdict(list)
    for r in rows:
        by_workload[r["workload"]].append(r)

    for workload, wrows in by_workload.items():
        par = [r for r in wrows if r["policy"] != "sequential"]
        if not par:
            continue

        # Rows with more threads than cores are reported once below; their
        # idle time and imbalance are expected, so the per-row rules skip them.
        for r in par:
            tag = f"{r['policy']}, {r['threads']} threads"
            fits = r["threads"] <= cores
            if not r["correct"]:
                findings.append((workload, "WRONG ANSWER in a parallel run", tag))
            if fits and r["threads"] > 1 and r["speedup"] < 1.0:
                findings.append((workload, "slower than sequential",
                                 f"{tag}: speedup {r['speedup']:.2f}"))
            if fits and r["idle_pct"] > IDLE_PCT:
                findings.append((workload, "workers spent a lot of time idle",
                                 f"{tag}: idle {r['idle_pct']:.1f}%"))
            enough_tasks = int(r["tasks"]) >= 4 * r["threads"]
            if fits and r["threads"] > 1 and enough_tasks and r["imbalance"] > IMBALANCE:
                findings.append((workload, "tasks spread unevenly over workers",
                                 f"{tag}: busiest worker ran {r['imbalance']:.2f}x the average"))
            if r["max_depth"] > DEEP_NESTING:
                findings.append((workload, "deep task nesting inside waits (stack growth)",
                                 f"{tag}: depth {r['max_depth']}"))
            overhead = extra_value(r, "search_overhead")
            if overhead is not None and overhead > SEARCH_OVERHEAD:
                findings.append((workload, "parallel search visited extra nodes",
                                 f"{tag}: {overhead:.2f}x the sequential node count"))
            dups = extra_value(r, "duplicate_visits")
            if dups:
                findings.append((workload, "race: vertices claimed by two threads",
                                 f"{tag}: {int(dups)} duplicate visits"))

        over = [r for r in par if r["threads"] > cores]
        if over:
            best_fit = max((r["speedup"] for r in par if r["threads"] <= cores), default=0)
            best_over = max(r["speedup"] for r in over)
            if best_over <= best_fit * 1.05:
                findings.append((workload, "more threads than cores did not help",
                                 f"best speedup with <= {cores} threads {best_fit:.2f}, "
                                 f"with more {best_over:.2f}"))

        # Static vs work stealing at the same thread count
        by_key = {(r["policy"], r["threads"]): r for r in par}
        for (policy, threads), r in by_key.items():
            ws = by_key.get(("stealing", threads))
            if policy == "static" and ws and r["time_ms"] > STATIC_GAP * ws["time_ms"]:
                findings.append((workload, "static split slower than work stealing",
                                 f"{threads} threads: {r['time_ms']:.1f} ms vs {ws['time_ms']:.1f} ms"))
    return findings


def best_configs(rows):
    best = {}
    for r in rows:
        if r["policy"] == "sequential":
            continue
        cur = best.get(r["workload"])
        if cur is None or r["speedup"] > cur["speedup"]:
            best[r["workload"]] = r
    return best


def trace_timeline(path, buckets=40):
    """ASCII picture of how busy each worker was over time, from an event trace."""
    starts = defaultdict(list)
    spans = defaultdict(list)
    steals = defaultdict(int)
    t_end = 0.0
    with open(path, newline="") as f:
        for row in csv.DictReader(f):
            w, t = int(row["worker"]), float(row["time_us"])
            t_end = max(t_end, t)
            if row["event"] == "start":
                starts[w].append(t)
            elif row["event"] == "end" and starts[w]:
                spans[w].append((starts[w].pop(), t))
            elif row["event"] == "steal":
                steals[w] += 1
    if not spans:
        return "  (no task events in trace)"
    t0 = min(s for ws in spans.values() for s, _ in ws)
    width = max(t_end - t0, 1e-9) / buckets
    shades = " .:-=+*#%@"   # more ink = busier
    lines = [f"  {os.path.basename(path)}: {t_end - t0:.0f} us, one column = {width:.0f} us"]
    for w in sorted(spans):
        busy = [0.0] * buckets
        # Outer spans only: nested tasks overlap their parent.
        merged = []
        for s, e in sorted(spans[w]):
            if merged and s <= merged[-1][1]:
                merged[-1][1] = max(merged[-1][1], e)
            else:
                merged.append([s, e])
        for s, e in merged:
            for b in range(buckets):
                lo, hi = t0 + b * width, t0 + (b + 1) * width
                busy[b] += max(0.0, min(e, hi) - max(s, lo))
        bar = "".join(shades[min(9, int(9.99 * x / width))] for x in busy)
        lines.append(f"  worker {w:2d} |{bar}| tasks={len(spans[w]):6d} steals={steals[w]}")
    return "\n".join(lines)


def build_report(results_path, trace_paths=(), cores=None):
    cores = cores or os.cpu_count() or 1
    rows = load_results(results_path)
    out = [f"PARAFORGE REPORT (measured, rule based)  - {len(rows)} result rows, {cores} cores", ""]

    out.append("Best configuration per workload:")
    for workload, r in sorted(best_configs(rows).items()):
        out.append(f"  {workload:22s} {r['policy']:9s} {r['threads']:2d} threads  "
                   f"speedup {r['speedup']:5.2f}  efficiency {r['efficiency']:4.2f}")
    out.append("")

    findings = analyse(rows, cores)
    out.append(f"Findings ({len(findings)}):")
    if not findings:
        out.append("  none of the rules fired")
    grouped = defaultdict(list)
    for workload, what, evidence in findings:
        grouped[(workload, what)].append(evidence)
    for (workload, what), evidence in sorted(grouped.items()):
        out.append(f"  [{workload}] {what}")
        for e in evidence[:4]:
            out.append(f"      - {e}")
        if len(evidence) > 4:
            out.append(f"      - ... {len(evidence) - 4} more")

    for path in trace_paths:
        out.append("")
        out.append("Trace timeline:")
        out.append(trace_timeline(path))
    return "\n".join(out)


def main():
    if len(sys.argv) < 2:
        sys.exit("usage: trace_report.py results.csv [trace.csv ...]")
    print(build_report(sys.argv[1], sys.argv[2:]))


if __name__ == "__main__":
    main()
