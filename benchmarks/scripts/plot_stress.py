"""Stress tier (1,800 s, k = 1 per shortlisted cell) set beside the core run of the same fixture.

    python benchmarks/scripts/plot_stress.py

The generic figures (plot_figures.py) assume k = 3 cells over the whole matrix; the stress tier
has one fixture on each of 25 shortlisted cells, so it gets its own table and figure instead.
Writes results/stress/summary/stress_vs_core.csv and figures/stress/{figS1_stress_frontier.*, figures.md}.
"""

from __future__ import annotations

import sys

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt  # noqa: E402
import numpy as np  # noqa: E402
import pandas as pd  # noqa: E402
from matplotlib.lines import Line2D  # noqa: E402

import bench_common as bc  # noqa: E402
import common as C  # noqa: E402


def table() -> pd.DataFrame:
    s = C.load_runs("stress")
    core = C.load_runs("core").set_index(["fixture_id", "solver", "op"])
    man = C.load_manifest().set_index("id")
    rows = []
    for _, r in s.iterrows():
        k = (r["fixture_id"], r["solver"], r["op"])
        c = core.loc[k] if k in core.index else None
        f = man.loc[r["fixture_id"]]
        rows.append({
            "fixture_id": r["fixture_id"], "population": r["population"], "n": r["n"],
            "target_pct": r["target_pct"], "fixture_status": f["status"],
            "fixture_empty": int(f["empty"]), "solver": r["solver"], "op": r["op"],
            "dlx_budget": r["dlx_budget"], "concurrency": r["concurrency"],
            "core_status": c["status"] if c is not None else "not_run_in_core",
            "core_nodes": float(c["nodes"]) if c is not None and c["status"] == "timeout" else np.nan,
            "core_max_depth": float(c["max_depth"]) if c is not None and c["status"] == "timeout" else np.nan,
            "stress_status": r["status"],
            "stress_time_s": r["time_s"] if r["status"] != "pruned_by_prefix" else np.nan,
            "stress_nodes": float(r["nodes"]) if r["status"] == "timeout" else np.nan,
            "stress_max_depth": float(r["max_depth"]) if r["status"] == "timeout" else np.nan,
            "stress_mem_peak_ws_delta": r["mem_peak_ws_delta"] if r["status"] == "timeout" else np.nan,
        })
    t = pd.DataFrame(rows)
    t["node_ratio"] = t["stress_nodes"] / t["core_nodes"]
    t["stress_depth_frac"] = t["stress_max_depth"] / t["fixture_empty"]
    t["core_depth_frac"] = t["core_max_depth"] / t["fixture_empty"]
    return t.sort_values(["population", "solver", "target_pct", "n", "fixture_id", "op"]).reset_index(drop=True)


def figure(t: pd.DataFrame, out) -> None:
    ex = t[t["stress_status"] == "timeout"].copy()
    fig, (a1, a2) = plt.subplots(1, 2, figsize=(11, 4.6), gridspec_kw={"width_ratios": [1, 1.25]})

    # Panel 1: nodes explored at 180 s (core) vs 1,800 s (stress), same fixture.
    p = ex[ex["core_nodes"].notna()]
    for s in C.SOLVERS:
        q = p[p["solver"] == s]
        a1.scatter(q["core_nodes"], q["stress_nodes"], color=C.COLOR[s], marker=C.MARKER[s], s=46,
                   edgecolors=C.SURFACE, linewidths=1.2, zorder=3)
    lim = np.array([3e6, 3e11])
    a1.plot(lim, lim * 10, color=C.MUTED, lw=1, ls="--")
    a1.text(4e6, 3e10, "dashed: 10× the core nodes\n(same rate for 10× the time)", color=C.INK2, fontsize=7.5)
    a1.set_xscale("log"); a1.set_yscale("log"); a1.set_xlim(*lim); a1.set_ylim(lim[0] * 3, lim[1] * 3)
    a1.set_xlabel("nodes explored in 180 s (core, timed out)")
    a1.set_ylabel("nodes explored in 1,800 s (stress, timed out)")
    a1.set_title("Search keeps its rate but does not finish")

    # Panel 2: deepest level reached as a fraction of the empty cells.
    ex = ex.sort_values(["solver", "population", "target_pct", "n"]).reset_index(drop=True)
    y = np.arange(len(ex))[::-1]
    for i, r in ex.iterrows():
        col = C.COLOR[r["solver"]]
        if not np.isnan(r["core_depth_frac"]):
            a2.plot([r["core_depth_frac"], r["stress_depth_frac"]], [y[i]] * 2, color=col, lw=1.2, alpha=0.6)
            a2.scatter(r["core_depth_frac"], y[i], marker=C.MARKER[r["solver"]], s=30, facecolors="none",
                       edgecolors=col, lw=1.2, zorder=3)
        a2.scatter(r["stress_depth_frac"], y[i], marker=C.MARKER[r["solver"]], s=36, color=col,
                   edgecolors=C.SURFACE, linewidths=1, zorder=4)
    labels = []
    for _, r in ex.iterrows():
        tag = f"{r['population']} n={r['n']} {r['target_pct']}%"
        if r["fixture_status"] != "ok":
            tag += " (best, 29.2%)"
        if r["core_status"] == "memory_rejected":
            tag += f" s{r['fixture_id'][-1]} (8 GiB)"
        labels.append(tag)
    a2.set_yticks(y, labels, fontsize=7)
    a2.set_xlim(0, 1.02)
    a2.set_xlabel("deepest search level reached / empty cells (1 = a full assignment)")
    a2.set_title("How deep the search got")
    a2.grid(axis="y", visible=False)

    h = [Line2D([], [], color=C.COLOR[s], marker=C.MARKER[s], ls="", label=C.LABEL[s]) for s in C.SOLVERS]
    h += [Line2D([], [], color=C.INK2, marker="o", ls="", mfc="none", label="core, 180 s"),
          Line2D([], [], color=C.INK2, marker="o", ls="", label="stress, 1,800 s")]
    fig.legend(handles=h, loc="lower center", bbox_to_anchor=(0.5, 1.0), ncol=len(h), fontsize=8.5)
    fig.text(0.01, -0.01,
             "Stress tier: 25 executed runs, 1 fixture per cell (exploratory, k = 1), all timed out at 1,800 s; "
             "3 uniqueness runs pruned. Core and stress runs both ran in the parallel queue (6 pinned workers). "
             "DLX n = 15 runs were memory-rejected in core (256 MiB) and have no core point. "
             "'best, 29.2%' = carving stopped short of the 30% target; the sparsest puzzle reached was run. "
             "Node counts are not comparable across engines.",
             ha="left", va="top", fontsize=7.5, color=C.INK2, wrap=True)
    fig.tight_layout()
    for ext in ("png", "svg"):
        kw = {"metadata": {"Date": None}} if ext == "svg" else {}
        fig.savefig(out / f"figS1_stress_frontier.{ext}", bbox_inches="tight", **kw)
    plt.close(fig)


def main() -> int:
    sys.stdout.reconfigure(encoding="utf-8")
    C.style()
    t = table()
    sdir = C.results_dir("stress") / "summary"
    sdir.mkdir(exist_ok=True)
    t.to_csv(sdir / "stress_vs_core.csv", index=False, float_format="%.6g")
    out = bc.BENCH_DIR / "figures" / "stress"
    out.mkdir(parents=True, exist_ok=True)
    figure(t, out)
    (out / "figures.md").write_text(
        "# Figures — tier `stress`\n\nGenerated by `scripts/plot_stress.py` from the CSVs only. Budget T = 1,800 s, "
        "k = 1 per shortlisted cell (exploratory).\n\n| Figure | Produced | Note |\n|---|---|---|\n"
        "| [figS1_stress_frontier](figS1_stress_frontier.png) | yes | nodes and search depth, core (180 s) vs stress "
        "(1,800 s), same fixture |\n"
        "| fig01–fig07, figC* | no | need k ≥ 3 cells across the matrix; the stress tier has 1 fixture on each of "
        "25 shortlisted cells, and no completions |\n", encoding="utf-8")
    ex = t[t["stress_status"] == "timeout"]
    print(f"{len(t)} rows; {len(ex)} executed; statuses {t['stress_status'].value_counts().to_dict()}")
    print(f"node ratio stress/core: median {ex['node_ratio'].median():.2f}, "
          f"range {ex['node_ratio'].min():.2f}-{ex['node_ratio'].max():.2f}")
    print(f"depth fraction (stress): median {ex['stress_depth_frac'].median():.2f}, "
          f"range {ex['stress_depth_frac'].min():.2f}-{ex['stress_depth_frac'].max():.2f}")
    print(f"depth gain stress-core: max {(ex['stress_depth_frac'] - ex['core_depth_frac']).max():.3f}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
