"""Figures from the saved CSVs only (runs.csv, summary/*.csv, fixtures/manifest.csv).

    python benchmarks/scripts/summarize.py --tier core
    python benchmarks/scripts/plot_figures.py --tier core

Writes figures/<tier>/figNN_<name>.{png,svg} and figures/<tier>/figures.md (what was drawn,
and which conditional figures were not, with the reason). Never plots invented values:
censored cells are drawn as "> T", missing cells are hatched and labelled.
"""

from __future__ import annotations

import argparse
import sys

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt  # noqa: E402
import numpy as np  # noqa: E402
import pandas as pd  # noqa: E402
from matplotlib.colors import LinearSegmentedColormap, LogNorm, Normalize, TwoSlopeNorm  # noqa: E402
from matplotlib.lines import Line2D  # noqa: E402
from matplotlib.patches import Patch  # noqa: E402

import bench_common as bc  # noqa: E402
import common as C  # noqa: E402

SEQ_CMAP = LinearSegmentedColormap.from_list("seq", C.SEQ)
DIV_CMAP = LinearSegmentedColormap.from_list("div", [C.DIV_LOW, C.DIV_MID, C.DIV_HIGH])
POP_NAME = {"A": "population A (random blanks)", "B": "population B (unique puzzles)",
            "AS": "ablation AS (solver-filled grids)"}


class Figures:
    def __init__(self, tier: str):
        self.tier = tier
        self.r = C.load_runs(tier)
        sdir = C.results_dir(tier) / "summary"
        self.cells = pd.read_csv(sdir / "cells.csv")
        self.sp = pd.read_csv(sdir / "speedups.csv")
        self.T = float(self.r["budget_s"].max())
        self.out = bc.BENCH_DIR / "figures" / tier
        self.out.mkdir(parents=True, exist_ok=True)
        self.log = []          # (name, produced?, note)
        self.pops = [p for p in ("A", "B") if p in set(self.r["population"])]

    def save(self, fig, name, note=""):
        fig.savefig(self.out / f"{name}.png", bbox_inches="tight")
        fig.savefig(self.out / f"{name}.svg", bbox_inches="tight", metadata={"Date": None})
        plt.close(fig)
        self.log.append((name, True, note))

    def skip(self, name, reason):
        self.log.append((name, False, reason))

    def footer(self, fig, text):
        fig.text(0.01, -0.01, text, ha="left", va="top", fontsize=7.5, color=C.INK2, wrap=True)

    def legend_solvers(self, fig, extra=(), y=1.0):
        h = [Line2D([], [], color=C.COLOR[s], marker=C.MARKER[s], label=C.LABEL[s]) for s in C.SOLVERS]
        fig.legend(handles=h + list(extra), loc="lower center", bbox_to_anchor=(0.5, y),
                   ncol=len(h) + len(extra), fontsize=8.5)

    # ---------------------------------------------------------------- per-cell series helpers
    def series(self, pop, op, solver, by, fixed: dict):
        c = self.cells[(self.cells["population"] == pop) & (self.cells["op"] == op) &
                       (self.cells["solver"] == solver) & (self.cells["k_measured"] > 0)]
        for k, v in fixed.items():
            c = c[c[k] == v]
        return c.sort_values(by)

    def runtime_panel(self, ax, pop, op, xcol, fixed, xlabel, xlog=False):
        offs = {"naive": -0.12, "mrv": 0.0, "dlx": 0.12} if not xlog else {s: 0 for s in C.SOLVERS}
        for s in C.SOLVERS:
            c = self.series(pop, op, s, xcol, fixed)
            if c.empty:
                continue
            x = c[xcol].to_numpy(float) + offs[s]
            med = c["median_s"].to_numpy(float)
            cens = c["median_censored"].to_numpy(bool)
            ax.plot(x, np.where(cens, np.nan, med), color=C.COLOR[s], marker=C.MARKER[s], ms=5, lw=1.6)
            lo, hi = c["q1_s"].to_numpy(float), c["q3_s"].to_numpy(float)
            hi = np.where(np.isnan(hi), self.T, hi)
            ok = ~cens
            ax.vlines(x[ok], lo[ok], hi[ok], color=C.COLOR[s], lw=1, alpha=0.6)
            if cens.any():
                ax.scatter(x[cens], np.full(cens.sum(), self.T * 1.6), marker=C.MARKER[s], s=30,
                           facecolors="none", edgecolors=C.COLOR[s], lw=1.3, zorder=3)
        ax.axhline(self.T, color=C.MUTED, lw=0.8, ls=":")
        ax.set_yscale("log")
        ax.set_ylim(5e-8, self.T * 3)
        ax.set_xlabel(xlabel)

    # ---------------------------------------------------------------- figure 1
    def fig01(self):
        pcts = [10, 20, 40]
        cols = [(p, o) for p in self.pops for o in C.OPS]
        fig, axes = plt.subplots(len(pcts), len(cols), figsize=(3.1 * len(cols), 2.5 * len(pcts)),
                                 sharex=True, sharey=True, squeeze=False)
        for i, pct in enumerate(pcts):
            for j, (pop, op) in enumerate(cols):
                ax = axes[i][j]
                self.runtime_panel(ax, pop, op, "n", {"target_pct": pct}, "n (board is n² × n²)" if i == len(pcts) - 1 else "")
                if i == 0:
                    ax.set_title(f"{pop} · {op}", loc="left")
                if j == 0:
                    ax.set_ylabel(f"{pct}% empty\nmedian time (s)")
                ax.set_xticks(range(1, 16, 2))
        cens = Line2D([], [], color=C.MUTED, marker="o", mfc="none", ls="", label="> T (median censored)")
        fig.subplots_adjust(top=0.88)
        self.legend_solvers(fig, [cens], y=0.905)
        fig.suptitle(f"Figure 1. Runtime vs board size — median of k ≤ 3 fixtures (bars: IQR), budget T = {self.T:.0f} s",
                     x=0.01, ha="left", y=0.96, fontsize=11, fontweight="bold")
        self.footer(fig, "Missing points: population B has no fixture at that sparsity (target not reached), or every uniqueness run was pruned. "
                    "Timing: serial runs (concurrency 1) except the few completions in the parallel queue; see figures.md.")
        self.save(fig, "fig01_runtime_vs_n")

    # ---------------------------------------------------------------- figure 2 (+ conditional nodes)
    def fig02(self, metric="time"):
        ns = [3, 4, 5, 6, 8, 10]
        for op in C.OPS:
            fig, axes = plt.subplots(1, len(ns), figsize=(2.6 * len(ns), 2.9), sharey=True, squeeze=False)
            for j, n in enumerate(ns):
                ax = axes[0][j]
                for pop, ls in (("A", "-"), ("B", "--")):
                    if pop not in self.pops:
                        continue
                    for s in C.SOLVERS:
                        c = self.series(pop, op, s, "actual_pct", {"n": n})
                        if c.empty:
                            continue
                        if metric == "time":
                            y = np.where(c["median_censored"], np.nan, c["median_s"])
                            ax.plot(c["actual_pct"], y, color=C.COLOR[s], marker=C.MARKER[s], ls=ls, ms=4.5, lw=1.5,
                                    mfc=C.COLOR[s] if pop == "A" else "white")
                            cz = c[c["median_censored"]]
                            ax.scatter(cz["actual_pct"], np.full(len(cz), self.T * 1.6), marker=C.MARKER[s], s=26,
                                       facecolors="none", edgecolors=C.COLOR[s], lw=1.2)
                        else:
                            ax.plot(c["actual_pct"], c["median_nodes"], color=C.COLOR[s], marker=C.MARKER[s], ls=ls,
                                    ms=4.5, lw=1.5, mfc=C.COLOR[s] if pop == "A" else "white")
                ax.set_yscale("log")
                ax.set_title(f"n = {n}  ({n * n}×{n * n})", loc="left")
                ax.set_xlabel("empty cells (%)")
                ax.set_xticks([10, 30, 50, 70])
                if metric == "time":
                    ax.axhline(self.T, color=C.MUTED, lw=0.8, ls=":")
                    ax.set_ylim(5e-7, self.T * 3)
            axes[0][0].set_ylabel("median time (s)" if metric == "time" else "median search nodes (completed runs)")
            extra = [Line2D([], [], color=C.INK2, ls="-", label="A"), Line2D([], [], color=C.INK2, ls="--", mfc="white",
                                                                              marker="o", label="B")]
            if metric == "time":
                extra.append(Line2D([], [], color=C.MUTED, marker="o", mfc="none", ls="", label="> T"))
            self.legend_solvers(fig, extra, y=1.02)
            if metric == "time":
                fig.suptitle(f"Figure 2. Runtime vs sparsity — op '{op}'", x=0.01, ha="left", y=1.16,
                             fontsize=11, fontweight="bold")
                self.save(fig, f"fig02_runtime_vs_sparsity_{op}")
            else:
                fig.suptitle(f"Search nodes vs sparsity — op '{op}'", x=0.01, ha="left", y=1.16, fontsize=11,
                             fontweight="bold")
                self.footer(fig, "Nodes are not comparable across engines: backtracking counts cell choices, DLX counts "
                            "constraint-column choices. Compare shapes, not levels.")
                self.save(fig, f"figC1_nodes_vs_sparsity_{op}")

    # ---------------------------------------------------------------- heatmap helper
    def grid(self, pop, op, solver, value):
        c = self.cells[(self.cells["population"] == pop) & (self.cells["op"] == op) & (self.cells["solver"] == solver)]
        return {(int(r.n), int(r.target_pct)): r for r in c.itertuples()}

    def heat_axes(self, ax, title):
        ax.set_xlim(-0.5, 6.5)
        ax.set_ylim(15.5, 0.5)
        ax.set_xticks(range(7), [f"{p}%" for p in bc.SPARSITY_TARGETS])
        ax.set_yticks(range(1, 16))
        ax.grid(False)
        ax.set_title(title, loc="left")
        for sp in ax.spines.values():
            sp.set_visible(False)

    def heat_cell(self, ax, x, y, color, text, hatch=None, txtcolor=None):
        ax.add_patch(plt.Rectangle((x - 0.5, y - 0.5), 1, 1, facecolor=color, edgecolor=C.SURFACE, lw=1.5,
                                   hatch=hatch))
        if text:
            if txtcolor is None:
                rgb = matplotlib.colors.to_rgb(color)
                txtcolor = "white" if (0.299 * rgb[0] + 0.587 * rgb[1] + 0.114 * rgb[2]) < 0.55 else C.INK
            ax.text(x, y, text, ha="center", va="center", fontsize=6.3 if len(text) <= 7 or "\n" in text else 5.4,
                    color=txtcolor)

    def special(self, ax, x, y, row) -> bool:
        """Draw non-numeric states; return True if drawn."""
        if row is None:
            return True
        if row.sampling == "target_not_achieved":
            self.heat_cell(ax, x, y, "#f0efec", "not gen.", hatch="////", txtcolor=C.MUTED)
            return True
        if row.k_measured == 0:
            self.heat_cell(ax, x, y, "#f0efec", "pruned", txtcolor=C.MUTED)
            return True
        return False

    # ---------------------------------------------------------------- figure 3
    def fig03(self):
        norm = LogNorm(1e-7, self.T)
        for pop in self.pops:
            fig, axes = plt.subplots(2, 3, figsize=(10.5, 11.5), squeeze=False)
            for i, op in enumerate(C.OPS):
                for j, s in enumerate(C.SOLVERS):
                    ax = axes[i][j]
                    self.heat_axes(ax, f"{C.LABEL[s]} · {op}")
                    g = self.grid(pop, op, s, "median_s")
                    for (n, pct), row in g.items():
                        x = bc.SPARSITY_TARGETS.index(pct)
                        if self.special(ax, x, n, row):
                            continue
                        mark = "" if row.k_measured >= 3 else f"\nk={int(row.k_measured)}"
                        if row.k_memory_rejected * 2 >= row.k_measured:
                            self.heat_cell(ax, x, n, "#52514e", "MEM" + mark)
                        elif row.median_censored:
                            self.heat_cell(ax, x, n, C.SEQ[-1], f"> T{mark}")
                        else:
                            par = "∥" if "6" in str(row.concurrency) else ""
                            self.heat_cell(ax, x, n, SEQ_CMAP(norm(max(row.median_s, 1e-7))),
                                           C.fmt_time(row.median_s) + par + mark)
                    if j == 0:
                        ax.set_ylabel("n")
            sm = plt.cm.ScalarMappable(norm=norm, cmap=SEQ_CMAP)
            cb = fig.colorbar(sm, ax=axes, orientation="horizontal", fraction=0.025, pad=0.05, aspect=50)
            cb.set_label("median time per run (s), log scale")
            cb.outline.set_visible(False)
            fig.suptitle(f"Figure 3. Median runtime heatmap — {POP_NAME[pop]}", x=0.01, ha="left", y=0.93,
                         fontsize=11, fontweight="bold")
            fig.text(0.01, 0.03, f"> T: at least half of the measured fixtures did not finish in {self.T:.0f} s.  "
                     "MEM: DLX matrix over the 256 MiB budget.  pruned: uniqueness run skipped because the first-solution "
                     "run timed out (not an observation).  not gen.: no unique puzzle reached that sparsity.  "
                     "∥: timed in the parallel queue (6 workers).  k = 3 unless shown.", fontsize=7.5, color=C.INK2,
                     wrap=True)
            self.save(fig, f"fig03_runtime_heatmap_{pop}")

    # ---------------------------------------------------------------- figure 5
    def fig05(self):
        norm = Normalize(0, 1)
        for pop in self.pops:
            fig, axes = plt.subplots(2, 3, figsize=(10.5, 11.5), squeeze=False)
            for i, op in enumerate(C.OPS):
                for j, s in enumerate(C.SOLVERS):
                    ax = axes[i][j]
                    self.heat_axes(ax, f"{C.LABEL[s]} · {op}")
                    for (n, pct), row in self.grid(pop, op, s, "completion_rate").items():
                        x = bc.SPARSITY_TARGETS.index(pct)
                        if self.special(ax, x, n, row):
                            continue
                        txt = f"{int(row.k_completed)}/{int(row.k_measured)}"
                        if row.k_memory_rejected:
                            txt += " M"
                        if row.k_pruned:
                            txt += f"\n+{int(row.k_pruned)}p"
                        self.heat_cell(ax, x, n, SEQ_CMAP(norm(row.completion_rate)), txt)
                    if j == 0:
                        ax.set_ylabel("n")
            sm = plt.cm.ScalarMappable(norm=norm, cmap=SEQ_CMAP)
            cb = fig.colorbar(sm, ax=axes, orientation="horizontal", fraction=0.025, pad=0.05, aspect=50)
            cb.set_label(f"completion rate within T = {self.T:.0f} s (completed / measured)")
            cb.outline.set_visible(False)
            fig.suptitle(f"Figure 5. Completion rate — {POP_NAME[pop]}", x=0.01, ha="left", y=0.93,
                         fontsize=11, fontweight="bold")
            fig.text(0.01, 0.03, "Cell text: completed / measured fixtures; M: includes DLX memory rejections; "
                     "+Np: N uniqueness runs pruned (not counted). With k = 3 the Wilson 95% interval is wide "
                     "(e.g. 2/3 → 0.21–0.94); intervals are in results/<tier>/summary/cells.csv.", fontsize=7.5,
                     color=C.INK2, wrap=True)
            self.save(fig, f"fig05_completion_{pop}")

    # ---------------------------------------------------------------- figure 4
    def fig04(self):
        for pop in self.pops:
            fig, axes = plt.subplots(2, 3, figsize=(10.5, 11.5), squeeze=False)
            norm = TwoSlopeNorm(vcenter=0, vmin=-10, vmax=10)
            for i, op in enumerate(C.OPS):
                for j, (ref, ch) in enumerate(C.PAIRS):
                    ax = axes[i][j]
                    self.heat_axes(ax, f"{C.LABEL[ch]} vs {C.LABEL[ref]} · {op}")
                    sp = self.sp[(self.sp["population"] == pop) & (self.sp["op"] == op) &
                                 (self.sp["reference"] == ref) & (self.sp["challenger"] == ch)]
                    g = {(int(r.n), int(r.target_pct)): r for r in sp.itertuples()}
                    cells = self.grid(pop, op, ch, "")
                    for (n, pct), crow in cells.items():
                        x = bc.SPARSITY_TARGETS.index(pct)
                        if crow.sampling == "target_not_achieved":
                            self.special(ax, x, n, crow)
                            continue
                        row = g.get((n, pct))
                        if row is None or row.pairs == 0:
                            self.heat_cell(ax, x, n, "#f0efec", "pruned", txtcolor=C.MUTED)
                            continue
                        cw, rw = int(row.censored_wins_challenger), int(row.censored_wins_reference)
                        if row.both_completed_serial > 0:
                            l2 = np.log2(row.geomean_ratio)
                            v = row.geomean_ratio
                            txt = f"{v:.2g}×" if v < 100 else f"{v:.0f}×"
                        else:
                            l2, txt = None, ""
                        extra = ([f"+{cw} win"] if cw else []) + ([f"−{rw} loss"] if rw else [])
                        txt = "\n".join([t for t in [txt] + extra if t]) or "–"
                        if l2 is None:
                            hatch = "xx" if row.both_not_completed == row.pairs else None
                            color = C.DIV_HIGH if cw and not rw else (C.DIV_LOW if rw and not cw else "#f0efec")
                            if hatch:
                                txt = "both>T"
                            self.heat_cell(ax, x, n, color if not hatch else "#e1e0d9", txt, hatch=hatch,
                                           txtcolor=None if not hatch else C.INK2)
                        else:
                            self.heat_cell(ax, x, n, DIV_CMAP(norm(np.clip(l2, -10, 10))), txt)
                    if j == 0:
                        ax.set_ylabel("n")
            sm = plt.cm.ScalarMappable(norm=norm, cmap=DIV_CMAP)
            cb = fig.colorbar(sm, ax=axes, orientation="horizontal", fraction=0.025, pad=0.05, aspect=50)
            cb.set_ticks([-10, -5, 0, 5, 10], labels=["≤1/1024×", "1/32×", "1×", "32×", "≥1024×"])
            cb.set_label("speedup of the first-named solver (geometric mean over fixtures where both completed, serial)."
                         "  Blue: first-named faster")
            cb.outline.set_visible(False)
            fig.suptitle(f"Figure 4. Paired speedups — {POP_NAME[pop]}", x=0.01, ha="left", y=0.93, fontsize=11,
                         fontweight="bold")
            fig.text(0.01, 0.03, "Number: geometric-mean ratio t(second) / t(first) over paired fixtures. "
                     f"+N win: fixtures where only the first-named solver finished within T (speedup ≥ T / its time, a bound). "
                     "−N loss: only the second-named solver finished. Solid blue/red without a number: censored wins/losses only. "
                     "both > T: undetermined.", fontsize=7.5, color=C.INK2, wrap=True)
            self.save(fig, f"fig04_speedups_{pop}")

    # ---------------------------------------------------------------- figure 6
    def fig06(self):
        r = self.r[(self.r["population"] == "A") & (self.r["op"] == "first")]
        man = C.load_manifest()
        man = man[(man["population"] == "A") & (man["status"] == "ok")]
        fig, axes = plt.subplots(1, 2, figsize=(10, 3.8), sharey=True)
        for ax, pct in zip(axes, (20, 70)):
            rr = r[r["target_pct"] == pct]
            for s in C.SOLVERS:
                m = rr[(rr["solver"] == s) & (rr["status"] != "memory_rejected") & rr["measured"]]
                med = m.groupby("n")["mem_peak_ws_delta"].median()
                ax.plot(med.index, med.to_numpy() / 2**20, color=C.COLOR[s], marker=C.MARKER[s], ms=5,
                        label=f"{C.LABEL[s]} measured")
            est = man[man["target_pct"] == pct].groupby("n")["dlx_estimated_bytes"].median()
            ax.plot(est.index, est.to_numpy() / 2**20, color=C.COLOR["dlx"], ls="--", lw=1.6, label="DLX estimate")
            rej = rr[rr["status"] == "memory_rejected"]
            if len(rej):
                ax.scatter(rej["n"].unique(), [256 * 1.5] * rej["n"].nunique(), marker="x", s=60, color=C.INK,
                           label="DLX rejected (MEM)", zorder=4)
            ax.axhline(256, color=C.INK2, lw=1, ls=":")
            ax.text(1.2, 256 * 1.25, "256 MiB budget", fontsize=7.5, color=C.INK2)
            ax.set_yscale("log")
            ax.set_xlabel("n")
            ax.set_title(f"population A · first · {pct}% empty", loc="left")
            ax.set_xticks(range(1, 16, 2))
        axes[0].set_ylabel("MiB (log)")
        axes[1].legend(fontsize=7.5, loc="lower right")
        fig.suptitle("Figure 6. Memory vs board size — measured process peak above baseline (solid) and DLX analytic "
                     "estimate (dashed)", x=0.01, ha="left", y=1.03, fontsize=11, fontweight="bold")
        self.footer(fig, "Measured: peak working set minus the baseline after loading the fixture (4 KiB pages; small "
                    "boards sit at the page-granularity floor). The estimate is the size DLX computes before allocating.")
        self.save(fig, "fig06_memory_vs_n")

    # ---------------------------------------------------------------- figure 7
    def fig07(self):
        r = self.r[(self.r["population"] == "A") & (self.r["completed"]) & (self.r["target_pct"] == 20)]
        ns = [2, 3, 5, 7, 9, 11, 13, 15]
        fig, axes = plt.subplots(1, 2, figsize=(10, 4.2), sharey=True)
        for ax, op in zip(axes, C.OPS):
            labels, ypos = [], []
            y = 0
            for n in ns:
                for s in C.SOLVERS:
                    m = r[(r["n"] == n) & (r["solver"] == s) & (r["op"] == op)]
                    if m.empty:
                        continue
                    setup, search = m["setup_ns"].median(), m["search_ns"].median()
                    tot = setup + search
                    ax.barh(y, setup / tot, color=C.COLOR[s], height=0.78)
                    ax.barh(y, search / tot, left=setup / tot, color=C.COLOR[s], alpha=0.35, height=0.78)
                    ax.text(1.01, y, C.fmt_time(m["time_s"].median()), va="center", fontsize=6.5, color=C.INK2)
                    labels.append(f"n={n} {C.LABEL[s]}")
                    ypos.append(y)
                    y += 1
                y += 0.6
            ax.set_yticks(ypos, labels, fontsize=6.5)
            ax.set_xlim(0, 1)
            ax.set_xlabel("share of in-solver time")
            ax.set_title(f"population A · {op} · 20% empty", loc="left")
            ax.grid(axis="y", visible=False)
        h = [Patch(color=C.INK2, label="setup (validation, constraint state, DLX matrix)"),
             Patch(color=C.INK2, alpha=0.35, label="search")]
        fig.legend(handles=h, loc="lower center", bbox_to_anchor=(0.5, 1.0), ncol=2, fontsize=8.5)
        fig.suptitle("Figure 7. Setup vs search time (medians of completed runs; total at right)", x=0.01, ha="left",
                     y=1.13, fontsize=11, fontweight="bold")
        self.save(fig, "fig07_setup_vs_search")

    # ---------------------------------------------------------------- conditional figures
    def figC_ecdf(self):
        groups = []
        for pop in self.pops:
            for op in C.OPS:
                for n in range(1, 16):
                    m = self.r[(self.r["population"] == pop) & (self.r["op"] == op) & (self.r["n"] == n) &
                               self.r["measured"]]
                    if all((m["solver"] == s).sum() >= 20 for s in C.SOLVERS):
                        groups.append((pop, op, n))
        if not groups:
            self.skip("figC3_fraction_solved", "no population/op/size group has ≥ 20 measured runs per solver")
            return
        cols = 4
        rows = -(-len(groups) // cols)
        fig, axes = plt.subplots(rows, cols, figsize=(3 * cols, 2.5 * rows), sharex=True, sharey=True, squeeze=False)
        for ax in axes.flat[len(groups):]:
            ax.set_visible(False)
        for ax, (pop, op, n) in zip(axes.flat, groups):
            m = self.r[(self.r["population"] == pop) & (self.r["op"] == op) & (self.r["n"] == n) & self.r["measured"]]
            for s in C.SOLVERS:
                ms = m[m["solver"] == s]
                t = np.sort(ms.loc[ms["completed"], "time_s"].to_numpy())
                k = len(ms)
                if len(t):
                    ax.step(np.concatenate([[1e-8], t, [self.T]]), np.concatenate([[0], np.arange(1, len(t) + 1) / k,
                                                                                    [len(t) / k]]),
                            where="post", color=C.COLOR[s], lw=1.6)
            ax.set_xscale("log")
            ax.set_xlim(1e-7, self.T)
            ax.set_ylim(0, 1.03)
            ax.set_title(f"{pop} · {op} · n = {n}  (k = {len(m) // 3})", loc="left", fontsize=9)
        for ax in axes[-1]:
            ax.set_xlabel("time t (s)")
        for ax in axes[:, 0]:
            ax.set_ylabel("fraction solved by t")
        fig.subplots_adjust(top=0.93, hspace=0.35)
        self.legend_solvers(fig, y=0.945)
        fig.suptitle(f"Fraction of runs solved by time t (all sparsities of one n pooled; censored at T = {self.T:.0f} s)",
                     x=0.01, ha="left", y=0.985, fontsize=11, fontweight="bold")
        self.footer(fig, "Each curve is the empirical CDF over measured runs; runs not finished by T keep the curve "
                    "below 1. Pruned uniqueness runs are excluded, so k can differ between solvers for 'unique'.")
        self.save(fig, "figC3_fraction_solved", note=f"{len(groups)} groups")

    def figC_time_nodes(self):
        m = self.r[self.r["completed"] & (self.r["concurrency"] == 1) & (self.r["nodes"] > 0)]
        span = np.log10(m["nodes"].max() / m["nodes"].min()) if len(m) else 0
        if span < 2:
            self.skip("figC4_time_vs_nodes", f"completed serial runs span only {span:.1f} decades of nodes")
            return
        fig, axes = plt.subplots(1, 3, figsize=(11, 3.6), sharey=True)
        for ax, s in zip(axes, C.SOLVERS):
            ms = m[m["solver"] == s]
            sc = ax.scatter(ms["nodes"], ms["time_s"], c=ms["n"], cmap=SEQ_CMAP, s=10, alpha=0.8, lw=0)
            ax.set_xscale("log")
            ax.set_yscale("log")
            ax.set_title(C.LABEL[s], loc="left")
            ax.set_xlabel("search nodes")
        axes[0].set_ylabel("time (s)")
        cb = fig.colorbar(sc, ax=axes, fraction=0.02, pad=0.01)
        cb.set_label("n")
        cb.outline.set_visible(False)
        fig.suptitle("Time vs search nodes (completed serial runs; colour = n)", x=0.01, ha="left", y=1.02,
                     fontsize=11, fontweight="bold")
        self.footer(fig, "Nodes are engine-specific; compare each panel's slope and spread, not levels across panels.")
        self.save(fig, "figC4_time_vs_nodes", note=f"node span {span:.1f} decades")

    def write_index(self):
        lines = [f"# Figures — tier `{self.tier}`", "",
                 "Generated by `scripts/plot_figures.py` from the CSVs only. Budget T = "
                 f"{self.T:.0f} s.", "", "| Figure | Produced | Note |", "|---|---|---|"]
        for name, ok, note in self.log:
            link = f"[{name}]({name}.png)" if ok else name
            lines.append(f"| {link} | {'yes' if ok else 'no'} | {note} |")
        lines += ["", "Conditional figures that need deep sampling (k ≥ 20 per fixed cell) are not produced: "
                  "runtime distributions per cell, and runtime vs mean initial candidates.",
                  "", "Most completed runs were timed serially (concurrency 1). Cells timed in the parallel queue "
                  "(6 pinned workers, calibrated slowdown 1.06× mean, 1.15× worst) are marked ∥ in Figure 3; "
                  "the latency comparisons in Figure 4 use serial pairs only."]
        (self.out / "figures.md").write_text("\n".join(lines) + "\n", encoding="utf-8")


def main() -> int:
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--tier", default="core")
    args = p.parse_args()
    sys.stdout.reconfigure(encoding="utf-8")
    C.style()
    f = Figures(args.tier)
    f.fig01()
    f.fig02("time")
    f.fig03()
    f.fig04()
    f.fig05()
    f.fig06()
    f.fig07()
    f.fig02("nodes")
    f.figC_ecdf()
    f.figC_time_nodes()
    f.skip("figC2_runtime_distributions", "needs k >= 20 fixtures per cell (deep sampling deferred); core has k ≤ 3")
    f.skip("figC5_runtime_vs_candidates", "needs k >= 20 fixtures in a fixed (n, sparsity) cell; core has k ≤ 3")
    f.write_index()
    for name, ok, note in f.log:
        print(("drawn   " if ok else "skipped ") + name + (f"  ({note})" if note else ""))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
