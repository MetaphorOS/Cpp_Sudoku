"""Which solver is best in each (n, sparsity) cell: core tier, from summary/cells.csv only.

    python benchmarks/scripts/plot_winner.py

Rule per (population, op, n, sparsity) cell, over the measured runs (k <= 3, 180 s):
  1. more completed fixtures wins;
  2. if completions tie, the lower median time wins (censored "> T" medians cannot be ranked);
  3. a margin (runner-up median / winner median) under 1.1x is drawn as a tie.
Colour = winner; full colour when decisive (>= 2x, or more completions), light tint for 1.1-2x.
Writes figures/core/fig08_best_solver.{png,svg} and results/core/summary/best_solver.csv.
"""

from __future__ import annotations

import math
import sys

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt  # noqa: E402
import numpy as np  # noqa: E402
import pandas as pd  # noqa: E402
from matplotlib.colors import to_rgb  # noqa: E402
from matplotlib.patches import Patch, Rectangle  # noqa: E402

import bench_common as bc  # noqa: E402
import common as C  # noqa: E402

TIE_MARGIN, DECISIVE = 1.1, 2.0
INITIAL = {"naive": "N", "mrv": "M", "dlx": "D"}
TIE, NONE_SOLVED = "#d3d2cb", "#6e6d68"


def tint(hex_color: str, amount: float) -> tuple:
    c, s = np.array(to_rgb(hex_color)), np.array(to_rgb(C.SURFACE))
    return tuple(s + (c - s) * amount)


def decide(g: pd.DataFrame) -> dict:
    g = g[g["k_measured"] > 0]
    if g.empty:
        return {"verdict": "not_measured"}
    if (g["k_completed"] == 0).all():
        return {"verdict": "none_solved", "k": int(g["k_measured"].max())}
    g = g.assign(med=np.where(g["median_censored"], np.inf, g["median_s"]))
    g = g.sort_values(["k_completed", "med"], ascending=[False, True])
    best, rest = g.iloc[0], g.iloc[1:]
    out = {"winner": best["solver"], "k": int(best["k_measured"]), "best_completed": int(best["k_completed"]),
           "best_median_s": best["med"]}
    if rest.empty:
        return out | {"verdict": "only_one_measured", "margin": math.nan}
    second = rest.iloc[0]
    out |= {"runner_up": second["solver"], "second_completed": int(second["k_completed"]),
            "second_median_s": second["med"]}
    if best["k_completed"] > second["k_completed"]:
        return out | {"verdict": "more_completed", "margin": math.inf}
    if not np.isfinite(best["med"]):           # completions tie and every median is "> T"
        return out | {"verdict": "tie", "tied": [s for s in g["solver"]
                                                 if g.set_index("solver").at[s, "k_completed"] == best["k_completed"]],
                      "margin": math.nan}
    margin = second["med"] / best["med"]
    if margin < TIE_MARGIN:
        tied = [s for s, m, k in zip(g["solver"], g["med"], g["k_completed"])
                if k == best["k_completed"] and m / best["med"] < TIE_MARGIN]
        return out | {"verdict": "tie", "tied": tied, "margin": margin}
    return out | {"verdict": "faster", "margin": margin}


def table(cells: pd.DataFrame) -> pd.DataFrame:
    rows = []
    c = cells[cells["op"].isin(["first", "unique"])]
    for (pop, op, n, pct), g in c.groupby(["population", "op", "n", "target_pct"]):
        d = decide(g)
        if g["sampling"].eq("target_not_achieved").all():
            d = {"verdict": "target_not_achieved"}
        k_pruned = int(g["k_pruned"].fillna(0).max())
        rows.append({"population": pop, "op": op, "n": int(n), "target_pct": int(pct), "k_pruned": k_pruned,
                     **{k: (",".join(v) if isinstance(v, list) else v) for k, v in d.items()}})
    return pd.DataFrame(rows)


def draw_cell(ax, x, y, r):
    v = r["verdict"]
    txt, color, hatch, ink = "", C.SURFACE, None, C.INK
    if v == "target_not_achieved":
        hatch, txt, ink = "////", "not gen.", C.MUTED
    elif v == "not_measured":
        hatch, txt, ink = "....", "pruned", C.MUTED
    elif v == "none_solved":
        color, txt, ink = NONE_SOLVED, "none", "#ffffff"
    elif v == "tie":
        color = TIE
        tied = str(r["tied"]).split(",")
        txt = "≈".join(INITIAL[s] for s in C.SOLVERS if s in tied)
    else:
        w = r["winner"]
        decisive = v in ("more_completed", "only_one_measured") or r["margin"] >= DECISIVE
        color = C.COLOR[w] if decisive else tint(C.COLOR[w], 0.38)
        ink = "#ffffff" if decisive else C.INK
        if v == "more_completed":
            txt = f"{int(r['best_completed'])}/{int(r['k'])}"
        elif v == "only_one_measured":
            txt = f"{int(r['best_completed'])}/{int(r['k'])} only"
        elif v == "faster":
            m = r["margin"]
            txt = f"{m / 1000:.0f}k×" if m >= 1000 else f"{m:.0f}×" if m >= 10 else f"{m:.1f}×"
    ax.add_patch(Rectangle((x - 0.5, y - 0.5), 1, 1, facecolor=color, edgecolor=C.SURFACE, lw=1.5,
                           hatch=hatch))
    if txt:
        ax.text(x, y, txt, ha="center", va="center", fontsize=6.3, color=ink)


def figure(t: pd.DataFrame, out) -> None:
    pcts = sorted(t["target_pct"].unique())
    pcts = [p for p in pcts if p >= 10]
    ns = list(range(1, 16))
    fig, axes = plt.subplots(1, 4, figsize=(15, 6.2), sharey=True)
    for ax, (pop, op) in zip(axes, [("A", "first"), ("A", "unique"), ("B", "first"), ("B", "unique")]):
        sub = t[(t["population"] == pop) & (t["op"] == op)]
        for _, r in sub.iterrows():
            if r["target_pct"] in pcts:
                draw_cell(ax, pcts.index(r["target_pct"]), r["n"], r)
        ax.set_xlim(-0.5, len(pcts) - 0.5)
        ax.set_ylim(0.5, 15.5)
        ax.set_xticks(range(len(pcts)), [f"{p}%" for p in pcts])
        ax.set_yticks(ns, [f"{n} ({n * n}×{n * n})" for n in ns])
        ax.grid(False)
        ax.set_xlabel("empty cells")
        name = {"A": "A: random blanks", "B": "B: unique puzzles"}[pop]
        ax.set_title(f"{name} — {'first solution' if op == 'first' else 'uniqueness check'}", fontsize=9.5)
        for s in ax.spines.values():
            s.set_visible(False)
        ax.tick_params(length=0)
    axes[0].set_ylabel("box size n (board)")
    h = [Patch(color=C.COLOR[s], label=f"{C.LABEL[s]} best (≥ 2× or more solved)") for s in C.SOLVERS]
    h += [Patch(color=tint(C.COLOR[s], 0.38), label=f"{C.LABEL[s]} 1.1–2×") for s in C.SOLVERS]
    h += [Patch(color=TIE, label="tie (< 1.1×)"), Patch(color=NONE_SOLVED, label="no solver finished in 180 s"),
          Patch(facecolor=C.SURFACE, hatch="////", edgecolor=C.AXIS, label="puzzle not generated"),
          Patch(facecolor=C.SURFACE, hatch="....", edgecolor=C.AXIS, label="uniqueness run pruned")]
    fig.legend(handles=h, loc="lower center", bbox_to_anchor=(0.5, 1.0), ncol=5, fontsize=8)
    fig.text(0.01, -0.01,
             "Core tier, 180 s budget, k ≤ 3 puzzles per cell (sampled): treat single cells as indicative. "
             "Winner = most puzzles solved, then lowest median time. Cell text: the winner's speed margin over "
             "the runner-up (runner-up median ÷ winner median), or 'x/3' when the winner solved more puzzles than any "
             "other solver ('only' = the other solvers' uniqueness runs were pruned); N/M/D = naive/MRV/DLX tied. n = 1 has only 2 distinct puzzles and is drawn at the "
             "nearest sparsity column. Medians mostly from serial (single-process) timings.",
             ha="left", va="top", fontsize=7.5, color=C.INK2, wrap=True)
    fig.tight_layout()
    for ext in ("png", "svg"):
        kw = {"metadata": {"Date": None}} if ext == "svg" else {}
        fig.savefig(out / f"fig08_best_solver.{ext}", bbox_inches="tight", **kw)
    plt.close(fig)


def main() -> int:
    sys.stdout.reconfigure(encoding="utf-8")
    C.style()
    sdir = C.results_dir("core") / "summary"
    t = table(pd.read_csv(sdir / "cells.csv"))
    t.to_csv(sdir / "best_solver.csv", index=False, float_format="%.6g")
    figure(t, bc.BENCH_DIR / "figures" / "core")
    for (pop, op), g in t.groupby(["population", "op"]):
        print(pop, op, g["verdict"].value_counts().to_dict(), g["winner"].value_counts().to_dict())
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
