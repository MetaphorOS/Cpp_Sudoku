"""Per-cell summaries and paired speedups from a tier's runs.csv.

    python benchmarks/scripts/summarize.py --tier core

Writes results/<tier>/summary/{cells.csv, speedups.csv, status_counts.csv}. Rules: see common.py.
"""

from __future__ import annotations

import argparse
import math

import numpy as np
import pandas as pd

import common as C


def cell_table(r: pd.DataFrame, manifest: pd.DataFrame) -> pd.DataFrame:
    rows = []
    budget = float(r["budget_s"].max())
    for key, g in r.groupby(C.CELL + ["solver", "op"]):
        meas = g[g["measured"]]
        done = meas["completed"].to_numpy()
        st = C.censored_stats(meas["time_s"].to_numpy(), done, budget)
        k, kc = len(meas), int(done.sum())
        lo, hi = C.wilson(kc, k)
        conc = sorted(set(meas["concurrency"]))
        comp = meas[meas["completed"]]
        rows.append(dict(zip(C.CELL + ["solver", "op"], key)) | {
            "actual_pct": float(g["actual_pct"].iloc[0]),
            "k_measured": k, "k_completed": kc,
            "k_timeout": int(meas["status"].isin(["timeout", "watchdog_killed"]).sum()),
            "k_memory_rejected": int((meas["status"] == "memory_rejected").sum()),
            "k_pruned": int((g["status"] == "pruned_by_prefix").sum()),
            "k_confirmed_serial": int((g["confirmed"] != "").sum()),
            "concurrency": "+".join(map(str, conc)),
            "completion_rate": kc / k if k else math.nan, "wilson_lo": lo, "wilson_hi": hi,
            **st,
            "median_nodes": float(comp["nodes"].median()) if len(comp) else math.nan,
            "median_setup_s": float(comp["setup_ns"].median() / 1e9) if len(comp) else math.nan,
            "median_search_s": float(comp["search_ns"].median() / 1e9) if len(comp) else math.nan,
            "median_mem_peak_ws": float(meas.loc[meas["status"] != "memory_rejected",
                                                  "mem_peak_ws_delta"].median()),
            "sampling": C.sampling_level(k, kc),
            "budget_s": budget,
        })
    cells = pd.DataFrame(rows)

    # Planned cells with no ok fixture (population B targets not achieved) are listed explicitly.
    missing = []
    gen = manifest[manifest["population"].isin(set(r["population"]))]
    for key, g in gen.groupby(["population", "n", "target_pct", "target_empty"]):
        if (g["status"] == "ok").any():
            continue
        best = g["max_empty_achieved"].max()
        for solver in C.SOLVERS:
            for op in C.OPS:
                missing.append({"population": key[0], "n": key[1], "target_pct": key[2], "empty": key[3],
                                "solver": solver, "op": op, "k_measured": 0, "k_completed": 0,
                                "sampling": "target_not_achieved",
                                "note": f"sparsest unique puzzle reached: {int(best)} empty"})
    if missing:
        cells = pd.concat([cells, pd.DataFrame(missing)], ignore_index=True)
    return cells.sort_values(C.CELL + ["solver", "op"]).reset_index(drop=True)


def bootstrap_gm(ratios: np.ndarray, reps: int = 2000, seed: int = 1) -> tuple[float, float]:
    if len(ratios) < 2:
        return (math.nan, math.nan)
    rng = np.random.default_rng(seed)
    logs = np.log(ratios)
    means = logs[rng.integers(0, len(logs), (reps, len(logs)))].mean(axis=1)
    return (float(np.exp(np.quantile(means, 0.025))), float(np.exp(np.quantile(means, 0.975))))


def speedups(r: pd.DataFrame) -> pd.DataFrame:
    """Paired by fixture and op. ratio = t_reference / t_challenger (> 1: challenger faster).
    Geometric means use only pairs where both completed at concurrency 1."""
    m = r[r["measured"]]
    idx = m.set_index(["fixture_id", "solver", "op"])
    out = []
    budget = float(r["budget_s"].max())
    for ref, ch in C.PAIRS:
        a = idx.xs(ref, level="solver")
        b = idx.xs(ch, level="solver")
        j = a.join(b, suffixes=("_ref", "_ch"), how="inner").copy()
        j = j.reset_index()
        for key, g in j.groupby(["population_ref", "n_ref", "target_pct_ref", "empty_ref", "op"]):
            both = g[g["completed_ref"] & g["completed_ch"]]
            serial = both[(both["concurrency_ref"] == 1) & (both["concurrency_ch"] == 1)]
            ratios = (serial["time_s_ref"] / serial["time_s_ch"]).to_numpy()
            gm = float(np.exp(np.log(ratios).mean())) if len(ratios) else math.nan
            lo, hi = bootstrap_gm(ratios)
            ch_only = g[~g["completed_ref"] & g["completed_ch"]]
            ref_only = g[g["completed_ref"] & ~g["completed_ch"]]
            out.append({"population": key[0], "n": key[1], "target_pct": key[2], "empty": key[3], "op": key[4],
                        "reference": ref, "challenger": ch, "pairs": len(g),
                        "both_completed": len(both), "both_completed_serial": len(serial),
                        "geomean_ratio": gm, "boot_lo": lo, "boot_hi": hi,
                        "censored_wins_challenger": len(ch_only),
                        "min_bound_challenger": float((budget / ch_only["time_s_ch"]).min()) if len(ch_only) else math.nan,
                        "censored_wins_reference": len(ref_only),
                        "both_not_completed": int((~g["completed_ref"] & ~g["completed_ch"]).sum())})
    return pd.DataFrame(out)


def main() -> int:
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--tier", default="core")
    args = p.parse_args()
    r = C.load_runs(args.tier)
    manifest = C.load_manifest()
    out = C.results_dir(args.tier) / "summary"
    out.mkdir(exist_ok=True)
    cells = cell_table(r, manifest)
    cells.to_csv(out / "cells.csv", index=False, float_format="%.6g")
    sp = speedups(r)
    sp.to_csv(out / "speedups.csv", index=False, float_format="%.6g")
    counts = r.groupby(["population", "solver", "op", "status"]).size().unstack(fill_value=0)
    counts.to_csv(out / "status_counts.csv")
    print(f"{len(r)} runs -> {len(cells)} cell rows, {len(sp)} speedup rows in {out}")
    print(r["status"].value_counts().to_string())
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
