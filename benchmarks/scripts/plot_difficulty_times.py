"""Plots difficulty-targeted generation time by grid size and level.

Reads benchmarks/reports/difficulty_generation_times.csv (one row per run, from the Step 12
sweep) and writes benchmarks/reports/difficulty_generation_times.png: the median time per
(grid size, level) on a log scale, with runs that hit the 10-minute limit marked by a cross.

    python benchmarks/scripts/plot_difficulty_times.py
"""

from pathlib import Path
import statistics

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt  # noqa: E402
import pandas as pd  # noqa: E402

REPORTS = Path(__file__).resolve().parent.parent / "reports"
LIMIT_S = 600
LEVELS = [  # name, colour (validated categorical slots 1-4), dash pattern, marker
    ("easy", "#2a78d6", (0, ()), "o"),
    ("medium", "#eb6834", (0, (6, 4)), "s"),
    ("hard", "#1baf7a", (0, (2, 3)), "^"),
    ("expert", "#eda100", (0, (10, 3, 2, 3)), "D"),
]
INK, MUTED, GRID, AXIS = "#0b0b0b", "#898781", "#e1e0d9", "#c3c2b7"


def main() -> None:
    runs = pd.read_csv(REPORTS / "difficulty_generation_times.csv")
    runs = runs[runs["stopped"] != "skipped"].copy()
    runs["s"] = runs["ms"] / 1000.0
    boxes = sorted(runs["box"].unique())
    x_of = {b: i for i, b in enumerate(boxes)}

    fig, ax = plt.subplots(figsize=(9, 5.2), dpi=200)
    for name, colour, dash, marker in LEVELS:
        xs, ys, hits = [], [], []
        for box in boxes:
            cell = runs[(runs["box"] == box) & (runs["level"] == name)]
            if cell.empty:
                continue
            if (cell["stopped"] == "yes").any():
                # Levels that hit the limit at the same size are nudged apart so both crosses show.
                same = [n for n, *_ in LEVELS
                        if (runs[(runs["box"] == box) & (runs["level"] == n)]["stopped"] == "yes").any()]
                nudge = 0.0 if len(same) == 1 else (same.index(name) - (len(same) - 1) / 2) * 0.14
                hits.append(x_of[box] + nudge)
                xs.append(x_of[box] + nudge)
                ys.append(LIMIT_S)
                break  # larger sizes were not run for this level
            xs.append(x_of[box])
            ys.append(statistics.median(cell["s"]))
        n_ok = len(xs) - len(hits)
        ax.plot(xs[:n_ok], ys[:n_ok], color=colour, linestyle=dash, linewidth=2, marker=marker,
                markersize=6, markeredgecolor="white", markeredgewidth=1.2, label=name.capitalize(),
                solid_capstyle="round", dash_capstyle="round")
        if hits:  # dotted segment into the cross at the limit
            ax.plot(xs[n_ok - 1:], ys[n_ok - 1:], color=colour, linestyle=(0, (2, 4)), linewidth=2)
            ax.plot([hits[0]], [LIMIT_S], color=colour, marker="x", markersize=9, markeredgewidth=2.5,
                    linestyle="none")

    ax.axhline(LIMIT_S, color=MUTED, linewidth=1, linestyle=(0, (4, 4)))
    ax.plot([], [], color=MUTED, marker="x", linestyle="none", markersize=8, markeredgewidth=2,
            label="hit 10-min limit")

    ax.set_yscale("log")
    ax.set_ylim(0.05, 1000)
    ticks = [0.1, 1, 10, 60, 600]
    ax.set_yticks(ticks, ["0.1 s", "1 s", "10 s", "1 min", "10 min"])
    ax.minorticks_off()
    ax.set_xticks(range(len(boxes)), [f"{b * b}x{b * b}" for b in boxes])
    ax.set_xlabel("Grid size", color=MUTED)
    ax.set_ylabel("Median time (log scale)", color=MUTED)
    ax.tick_params(colors=MUTED, length=0)
    ax.grid(axis="y", color=GRID, linewidth=1)
    for side in ("top", "right", "left"):
        ax.spines[side].set_visible(False)
    ax.spines["bottom"].set_color(AXIS)
    ax.set_axisbelow(True)

    fig.suptitle("Difficulty-targeted generation: time by grid size and level", x=0.06, ha="left",
                 fontsize=13, color=INK)
    ax.legend(loc="upper left", ncol=5, frameon=False, fontsize=9, labelcolor="#52514e",
              bbox_to_anchor=(0, 1.08), handlelength=2.6)
    fig.text(0.06, 0.015,
             "sudoku generate --difficulty L --time-limit 600, Release build. Median of 10 seeds (9x9), "
             "5 (16x16), 3 (25x25), 1 (36x36 and up);\n4x4 is Easy only. Crosses hit the 10-minute limit; "
             "larger sizes were not run for that level. About 0.05 s of each time is program start-up.",
             fontsize=7.5, color=MUTED, ha="left", va="bottom")
    fig.subplots_adjust(left=0.1, right=0.97, top=0.86, bottom=0.2)
    out = REPORTS / "difficulty_generation_times.png"
    fig.savefig(out, facecolor="white")
    print(out)


if __name__ == "__main__":
    main()
