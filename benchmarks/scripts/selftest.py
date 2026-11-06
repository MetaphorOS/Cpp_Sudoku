"""Self-test of the benchmark harness (Phase 10B), in a temporary directory.

Checks, end to end through the real tools:
  1. a tiny n <= 3 matrix runs and every result is completed and validated
  2. a job that ignores its internal limit is killed by the watchdog and classified so
  3. a DLX job over a tiny memory budget is classified memory_rejected
  4. a first-solution timeout prunes the uniqueness run (pruned_by_prefix, not an observation)
  5. rerunning a campaign executes nothing already recorded (resumability)
  6. a modified fixture is rejected by the hash check
  7. completion summaries never count pruned or not-run entries
  8. batched timings use batches of at least 50 ms

Usage: python benchmarks/scripts/selftest.py [--keep]
"""

from __future__ import annotations

import argparse
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

import bench_common as bc

SCRIPTS = Path(__file__).resolve().parent
failures: list[str] = []


def check(condition: bool, what: str) -> None:
    print(("  ok    " if condition else "  FAIL  ") + what)
    if not condition:
        failures.append(what)


def run(args: list[str]) -> subprocess.CompletedProcess:
    proc = subprocess.run([sys.executable, *args], capture_output=True, text=True)
    if proc.returncode != 0:
        print(proc.stdout, proc.stderr)
        raise SystemExit(f"command failed: {args}")
    return proc


def fnv1a(text: str) -> str:
    h = 0xcbf29ce484222325
    for byte in text.encode():
        h = ((h ^ byte) * 0x100000001b3) & 0xFFFFFFFFFFFFFFFF
    return f"{h:016x}"


def add_trap_fixture(fixtures: Path) -> str:
    """A valid but unsolvable 16x16 that only a full search can refute: row 16 holds 1..15, so
    its last cell needs 16, but column 16 already has 16 in row 1. MRV sees the dead cell at
    once; naive backtracking fills rows 1-15 first and practically never finishes."""
    board = [["."] * 16 for _ in range(16)]
    for c in range(15):
        board[15][c] = str(c + 1)
    board[0][15] = "16"
    text = " ".join(v for row in board for v in row) + chr(10)
    fid = "X-n04-trap"
    (fixtures / "X").mkdir(parents=True, exist_ok=True)
    (fixtures / "X" / f"{fid}.txt").write_text(text, newline="")
    manifest = bc.read_csv(fixtures / "manifest.csv")
    row = {key: "" for key in manifest[0]}
    row.update({"id": fid, "population": "X", "n": "4", "N": "16", "cells": "256", "empty": "240",
                "target_empty": "240", "target_pct": "94", "actual_pct": "93.75", "seed_index": "1", "status": "ok",
                "content_hash": fnv1a(text), "puzzle_file": f"X/{fid}.txt", "solution_file": f"X/{fid}.txt"})
    bc.write_csv(fixtures / "manifest.csv", manifest + [row], list(manifest[0].keys()))
    return fid


def main() -> int:
    p = argparse.ArgumentParser()
    p.add_argument("--keep", action="store_true", help="keep the temporary directory")
    args = p.parse_args()
    tmp = Path(tempfile.mkdtemp(prefix="sudoku_selftest_"))
    fixtures, results = tmp / "fixtures", tmp / "results"
    common = ["--fixtures-dir", str(fixtures), "--results-dir", str(results)]
    try:
        print(f"self-test in {tmp}")
        run([str(SCRIPTS / "make_fixtures.py"), "--populations", "A,B", "--n", "1-3", "--seeds", "1",
             "--out", str(fixtures), "--budget-s", "60", "--verify-budget-s", "60"])
        hard = add_trap_fixture(fixtures)
        manifest = bc.load_manifest(fixtures)

        print("1. tiny matrix (pilot rules, n <= 3)")
        run([str(SCRIPTS / "run_campaign.py"), "--tier", "pilot", *common, "--only=-n0[1-3]-"])
        rows = bc.read_csv(results / "pilot" / "runs.csv")
        check(len(rows) > 0 and all(r["status"] == "completed" for r in rows),
              f"{len(rows)} pilot jobs on n <= 3, all completed")
        check(all(r["solution_valid"] == "yes" for r in rows), "every result independently validated")
        check({r["op"] for r in rows} == {"first", "unique", "enumerate"}, "first, unique and enumerate ops ran")

        print("2-4. watchdog, memory rejection, pruning")
        small = next(fid for fid, f in manifest.items() if f["n"] == "3" and f["population"] == "A")
        jobs = tmp / "jobs.csv"
        bc.write_csv(jobs, [
            {"fixture_id": hard, "solver": "naive", "op": "enumerate", "budget_s": 1, "no_internal_limit": 1,
             "reason": "watchdog"},
            {"fixture_id": small, "solver": "dlx", "op": "first", "budget_s": 10, "dlx_budget": 1000,
             "reason": "memory"},
            {"fixture_id": hard, "solver": "naive", "op": "first;unique", "budget_s": 0.2, "reason": "prune"},
        ], ["fixture_id", "solver", "op", "budget_s", "dlx_budget", "queue", "phase", "reason", "no_internal_limit"])
        run([str(SCRIPTS / "run_campaign.py"), "--tier", "custom", "--name", "special", "--jobs-file", str(jobs),
             "--grace-s", "2", *common])
        special = {r["reason"] + ":" + r["op"]: r for r in bc.read_csv(results / "special" / "runs.csv")}
        check(special["watchdog:enumerate"]["status"] == "watchdog_killed", "watchdog kills a runaway job")
        check(special["memory:first"]["status"] == "memory_rejected", "tiny DLX budget gives memory_rejected")
        check(special["prune:first"]["status"] == "timeout", "first-solution run times out at 0.2 s")
        check(special["prune:unique"]["status"] == "pruned_by_prefix" and
              special["prune:unique"]["pruned_source"] == special["prune:first"]["job_id"],
              "its uniqueness run is pruned and linked to the timed-out run")

        print("5. resumability")
        before = len(bc.read_csv(results / "pilot" / "runs.csv"))
        out = run([str(SCRIPTS / "run_campaign.py"), "--tier", "pilot", *common, "--only=-n0[1-3]-"]).stdout
        after = len(bc.read_csv(results / "pilot" / "runs.csv"))
        check(after == before and "end: 0 jobs executed" in out, "a rerun executes no recorded job")

        print("8. batch timing")
        timed = next(fid for fid, f in manifest.items() if f["n"] == "3" and f["population"] == "B")
        bc.write_csv(jobs, [{"fixture_id": timed, "solver": s, "op": "unique", "budget_s": 10} for s in bc.SOLVERS],
                     ["fixture_id", "solver", "op", "budget_s"])
        run([str(SCRIPTS / "run_campaign.py"), "--tier", "custom", "--name", "timing", "--jobs-file", str(jobs),
             *common])
        timing = bc.read_csv(results / "timing" / "runs.csv")
        batched = [r for r in timing if r["timing_kind"] == "batch"]
        check(len(batched) == 3 and all(float(r["batch_min_ms"]) >= 50 for r in batched),
              "fast jobs are batched, every batch >= 50 ms "
              + str([(r["solver"], r["batch_size"], r["batch_min_ms"]) for r in batched]))
        check(all(r["concurrency"] == "1" for r in timing), "serial queue records concurrency 1")

        print("6. fixture hash check")
        target = fixtures / manifest[small]["puzzle_file"]
        text = target.read_text()
        first_digit = next(i for i, ch in enumerate(text) if ch.isdigit())
        target.write_text(text[:first_digit] + "." + text[first_digit + 1:])
        bc.write_csv(jobs, [{"fixture_id": small, "solver": "mrv", "op": "first", "budget_s": 10}],
                     ["fixture_id", "solver", "op", "budget_s"])
        run([str(SCRIPTS / "run_campaign.py"), "--tier", "custom", "--name", "tampered", "--jobs-file", str(jobs),
             *common])
        tampered = bc.read_csv(results / "tampered" / "runs.csv")[0]
        check(tampered["status"] == "error" and "hash mismatch" in tampered["message"],
              "a modified fixture is rejected")

        print("7. summaries exclude pruned and not-run entries")
        summary = bc.completion_summary([{"status": s} for s in
                                         ("completed", "timeout", "pruned_by_prefix", "not_run", "completed")])
        check(summary["measured"] == 3 and summary["completed"] == 2 and summary["pruned"] == 1
              and summary["not_run"] == 1 and abs(summary["completion_rate"] - 2 / 3) < 1e-12,
              "completion rate = 2/3 (pruned and not-run excluded)")

    finally:
        if args.keep:
            print(f"kept {tmp}")
        else:
            shutil.rmtree(tmp, ignore_errors=True)

    print("SELF-TEST " + ("PASSED" if not failures else f"FAILED ({len(failures)})"))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
