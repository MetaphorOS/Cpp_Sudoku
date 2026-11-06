"""Shared loading, validation and style for the analysis scripts (summarize.py, plot_figures.py).

Needs pandas, matplotlib and numpy (scripts/requirements.txt). The rules applied here are the
ones fixed in benchmarks/README.md:
- pruned_by_prefix and not_run are never observations: they are counted, never timed;
- a cell median is censored ("> T") when timed-out or rejected runs make up at least half
  of its measured runs (k = 2 with one timeout is undeterminable, so it is censored too);
- serial (concurrency 1) and parallel timings are never pooled;
- a serial confirmation outcome replaces the parallel outcome for completion.
"""

from __future__ import annotations

import math
from pathlib import Path

import numpy as np
import pandas as pd

import bench_common as bc

MEASURED = sorted(bc.MEASURED_STATUSES)
NOT_COMPLETED = {"timeout", "watchdog_killed", "memory_rejected", "unsupported", "error"}
CELL = ["population", "n", "target_pct", "empty"]
SOLVERS = list(bc.SOLVERS)
OPS = ["first", "unique"]
PAIRS = [("naive", "mrv"), ("mrv", "dlx"), ("naive", "dlx")]   # (reference, challenger)

# Reference palette, categorical slots 1-3 (validated all-pairs) + a marker per solver.
COLOR = {"naive": "#2a78d6", "mrv": "#eb6834", "dlx": "#1baf7a"}
MARKER = {"naive": "o", "mrv": "s", "dlx": "^"}
LABEL = {"naive": "naive", "mrv": "MRV", "dlx": "DLX"}
INK, INK2, MUTED, GRID, AXIS, SURFACE = "#0b0b0b", "#52514e", "#898781", "#e1e0d9", "#c3c2b7", "#fcfcfb"
SEQ = ["#cde2fb", "#b7d3f6", "#9ec5f4", "#86b6ef", "#6da7ec", "#5598e7", "#3987e5", "#2a78d6",
       "#256abf", "#1c5cab", "#184f95", "#104281", "#0d366b"]
DIV_LOW, DIV_MID, DIV_HIGH = "#e34948", "#f0efec", "#2a78d6"


def results_dir(tier: str) -> Path:
    return bc.RESULTS_DIR / tier


def load_manifest() -> pd.DataFrame:
    m = pd.read_csv(bc.FIXTURES_DIR / "manifest.csv")
    return m


def load_runs(tier: str) -> pd.DataFrame:
    """runs.csv of a tier, checked, with the serial confirmation overlaid for the core tier."""
    path = results_dir(tier) / "runs.csv"
    r = pd.read_csv(path)
    dup = r.duplicated("job_id")
    if dup.any():
        raise ValueError(f"{path}: {dup.sum()} duplicate job ids")
    if set(r["tier"]) != {tier}:
        raise ValueError(f"{path}: mixed tiers {set(r['tier'])}")
    bad = r[(r["status"] == "completed") & (r["solution_valid"] != "yes")]
    if len(bad):
        raise ValueError(f"{path}: {len(bad)} completed runs without a valid solution")
    if (r["status"] == "error").any():
        raise ValueError(f"{path}: error rows present")

    m = load_manifest().set_index("id")
    r["target_pct"] = r["fixture_id"].map(m["target_pct"]).astype(int)
    r["time_s"] = r["total_ns"] / 1e9
    r["completed"] = r["status"] == "completed"
    r["measured"] = r["status"].isin(MEASURED)
    r["parallel_status"] = r["status"]
    r["confirmed"] = ""

    conf_path = bc.RESULTS_DIR / "serial_confirm" / "runs.csv"
    if tier == "core" and conf_path.exists():
        c = pd.read_csv(conf_path).set_index(["fixture_id", "solver", "op"])
        key = list(zip(r["fixture_id"], r["solver"], r["op"]))
        for i, k in enumerate(key):
            if k in c.index and r.at[i, "concurrency"] != 1:
                row = c.loc[k]
                r.at[i, "confirmed"] = f"serial:{row['status']}"
                if row["status"] == "completed":   # serial outcome is authoritative for completion
                    for col in ("status", "total_ns", "nodes", "setup_ns", "search_ns", "concurrency"):
                        r.at[i, col] = row[col]
                    r.at[i, "time_s"] = row["total_ns"] / 1e9
                    r.at[i, "completed"] = True
    return r


def sampling_level(k: int, k_completed: int) -> str:
    if k_completed >= 100:
        return "tail"
    if k >= 20:
        return "distribution"
    if k >= 3:
        return "sampled"
    if k >= 1:
        return "exploratory"
    return "none"


def wilson(successes: int, n: int, z: float = 1.96) -> tuple[float, float]:
    if n == 0:
        return (math.nan, math.nan)
    p = successes / n
    d = 1 + z * z / n
    c = (p + z * z / (2 * n)) / d
    h = z * math.sqrt(p * (1 - p) / n + z * z / (4 * n * n)) / d
    return (max(0.0, c - h), min(1.0, c + h))


def censored_stats(times: np.ndarray, done: np.ndarray, budget: float) -> dict:
    """Median/IQR/max over measured runs; runs not completed count as > budget."""
    k = len(times)
    if k == 0:
        return {"median_s": math.nan, "median_censored": False, "q1_s": math.nan, "q3_s": math.nan,
                "max_completed_s": math.nan}
    vals = np.where(done, times, np.inf)
    vals.sort()
    censored = (~done).sum() * 2 >= k
    q = np.quantile(vals, [0.25, 0.5, 0.75], method="inverted_cdf") if k else [math.nan] * 3
    fin = lambda v: float(v) if np.isfinite(v) else math.nan
    return {"median_s": math.nan if censored else fin(q[1]), "median_censored": bool(censored),
            "q1_s": fin(q[0]), "q3_s": fin(q[2]),
            "max_completed_s": float(times[done].max()) if done.any() else math.nan}


def fmt_time(s: float) -> str:
    if not np.isfinite(s):
        return ""
    if s < 1e-6:
        return f"{s * 1e9:.0f}ns"
    if s < 1e-3:
        return f"{s * 1e6:.0f}µs"
    if s < 1:
        return f"{s * 1e3:.0f}ms"
    return f"{s:.0f}s" if s >= 10 else f"{s:.1f}s"


def style():
    import matplotlib as mpl
    mpl.rcParams.update({
        "figure.facecolor": SURFACE, "axes.facecolor": SURFACE, "savefig.facecolor": SURFACE,
        "axes.edgecolor": AXIS, "axes.labelcolor": INK2, "xtick.color": MUTED, "ytick.color": MUTED,
        "text.color": INK, "axes.titlecolor": INK, "axes.grid": True, "grid.color": GRID,
        "grid.linewidth": 0.6, "axes.spines.top": False, "axes.spines.right": False,
        "font.size": 9, "axes.titlesize": 10, "axes.titleweight": "bold", "legend.frameon": False,
        "lines.linewidth": 2, "lines.markersize": 6, "svg.hashsalt": "sudoku", "svg.fonttype": "none",
        "figure.dpi": 110, "savefig.dpi": 150, "hatch.color": "#c3c2b7", "hatch.linewidth": 0.6,
    })
