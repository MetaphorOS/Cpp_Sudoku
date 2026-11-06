"""Joins the counters pass (results/profile_counters) with serial core timings and derives
per-operation costs. Counts are deterministic, so they come from the counters build; times
come from the Release build's serial core runs (concurrency 1).

    python benchmarks/scripts/profile_counters.py
Writes results/profile_counters/derived.csv and prints per-n tables for the report.
"""
import sys
import pandas as pd

import bench_common as bc

sys.stdout.reconfigure(encoding="utf-8")
p = pd.read_csv(bc.RESULTS_DIR / "profile_counters" / "runs.csv")
r = pd.read_csv(bc.RESULTS_DIR / "core" / "runs.csv")
k = ["fixture_id", "solver", "op"]
m = p.merge(r[k + ["nodes", "total_ns", "concurrency", "status", "dlx_est_bytes"]], on=k, suffixes=("_c", ""))
assert len(m) == len(p) and (m["nodes_c"] == m["nodes"]).all() and (m["status_c"] == m["status"]).all(), \
    "counters build disagrees with the core run"
man = pd.read_csv(bc.FIXTURES_DIR / "manifest.csv").set_index("id")
m["pct"] = m["fixture_id"].map(man["target_pct"])
m["empty_cells"] = m["fixture_id"].map(man["empty"])
m = m[(m["concurrency"] == 1) & (m["nodes"] > 0)].copy()
m["ns_per_node"] = m["total_ns"] / m["nodes"]
m["cols_per_node"] = m["cnt_columns_inspected"] / m["nodes"]
m["links_per_node"] = m["cnt_link_updates"] / m["nodes"]
m["ns_per_link"] = m["total_ns"] / m["cnt_link_updates"].where(m["cnt_link_updates"] > 0)
m["scan_per_node"] = m["cnt_cells_scanned"] / m["nodes"]
m["cand_per_node"] = m["cnt_candidates"] / m["nodes"]
m["backtracks_per_node"] = m["cnt_placements"] / m["nodes"]
m.to_csv(bc.RESULTS_DIR / "profile_counters" / "derived.csv", index=False, float_format="%.6g")

first = m[m["op"] == "first"]
easy = first[(first["population"] == "A") & (first["pct"] == 10)]
pd.set_option("display.width", 200)
for s, cols in (("dlx", ["empty_cells", "ns_per_node", "cols_per_node", "links_per_node", "ns_per_link"]),
                ("mrv", ["empty_cells", "ns_per_node", "scan_per_node", "cand_per_node"]),
                ("naive", ["empty_cells", "ns_per_node", "cand_per_node"])):
    print(f"\n== {s}: population A, first, 10% empty (medians)")
    print(easy[easy["solver"] == s].groupby("n")[cols].median().round(3).to_string())
