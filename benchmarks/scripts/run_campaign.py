"""Runs one tier of the solver study, resumably (README sections 6-9).

Every job is one `sudoku_bench run` process: one fixture, one solver, one operation, one time
budget. The runner adds the tier, job id, concurrency and process wall time, enforces the
external watchdog, applies the prefix-pruning rule and appends each result to
results/<tier>/runs.csv as soon as it is known. Rerunning the same command skips every job
already recorded.

Tiers:
  pilot      seed 1 of every cell, 60 s, parallel (exploratory), with ~20 pruning-validation pairs
  core       seeds 1-3 of every cell, 180 s; serial queue for jobs the pilot shows under 10 s,
             parallel queue for the rest
  deep       extra seeds of the deep-sampling cells (fixtures with seed index >= 4), 180 s, serial
  calibrate  ~30 pilot jobs (first or unique) that took 0.1-60 s, run serially and then --workers at a time
  custom     jobs from --jobs-file (serial confirmation, stress, self-tests); name with --name

Examples:
  python benchmarks/scripts/run_campaign.py --tier pilot --dry-run
  python benchmarks/scripts/run_campaign.py --tier pilot
  python benchmarks/scripts/run_campaign.py --tier core --cap-cpu-hours 120
  python benchmarks/scripts/run_campaign.py --tier custom --name stress --jobs-file shortlist.csv
"""

from __future__ import annotations

import argparse
import concurrent.futures as cf
import ctypes
import os
import queue
import csv
import io
import random
import re
import statistics
import subprocess
import sys
import tempfile
import threading
import time
from pathlib import Path

import bench_common as bc

RUNNER_FIELDS = ["job_id", "tier", "fixture_id", "population", "n", "N", "empty", "target_pct", "actual_pct",
                 "seed_index", "solver", "op", "budget_s", "dlx_budget", "queue", "concurrency",
                 "validation_pair", "pruned_source", "phase", "reason", "started_utc", "process_wall_s",
                 "exit_code", "cpu"]
TIER_BUDGET = {"pilot": 60.0, "core": 180.0, "deep": 180.0, "calibrate": 60.0}

# Logical CPUs in pinning order. On Windows the two hardware threads of a core are adjacent
# (0,1 = core 0), so even numbers are one thread per physical core. CPU 0 (interrupts) comes
# last among them; odd numbers (the second thread of a core) are used only above 6 workers.
PIN_ORDER = [2, 4, 6, 8, 10, 0, 3, 5, 7, 9, 11, 1]


def pin(proc: subprocess.Popen, cpu: int | None) -> bool:
    """Restricts a started process to one logical CPU (Windows only; elsewhere a no-op).
    Returns False if the process had already exited (a sub-millisecond single-shot job),
    so there was nothing left to pin."""
    if cpu is None or os.name != "nt":
        return True
    ok = ctypes.windll.kernel32.SetProcessAffinityMask(ctypes.c_void_p(int(proc._handle)),
                                                       ctypes.c_size_t(1 << cpu))
    if not ok:
        if proc.poll() is not None:
            return False
        raise OSError(f"SetProcessAffinityMask failed for CPU {cpu}")
    return True
REPETITION_ALLOWANCE_S = 60.0   # repeated timings of completed runs (at most ~5 x 10 s)


# ---------------------------------------------------------------------------
# Job planning
# ---------------------------------------------------------------------------

def ops_for(fixture: dict) -> list[str]:
    ops = list(bc.OPS)
    if fixture["population"] == "A" and int(fixture["n"]) <= bc.ENUMERATE_MAX_N:
        ops.append("enumerate")
    return ops


def pilot_index(pilot_runs: list[dict]) -> dict:
    """(population, n, empty, solver, op) -> pilot row (seed 1)."""
    return {(r["population"], r["n"], r["empty"], r["solver"], r["op"]): r for r in pilot_runs}


def job_id(tier: str, fixture: str, solver: str, op: str, budget: float, dlx_budget: str, phase: str = "") -> str:
    return f"{tier}|{fixture}|{solver}|{op}|b{budget:g}|m{dlx_budget or 'default'}" + (f"|{phase}" if phase else "")


def plan_groups(args, manifest: dict, pilot: dict) -> list[dict]:
    """A group is (fixture, solver) with its operations in order: first, unique[, enumerate]."""
    tier = args.tier
    groups = []
    if tier == "custom":
        for spec in bc.read_csv(Path(args.jobs_file)):
            fx = manifest[spec["fixture_id"]]
            groups.append({"fixture": fx, "solver": spec["solver"], "ops": spec["op"].split(";"),
                           "budget": float(spec["budget_s"]), "dlx_budget": spec.get("dlx_budget", ""),
                           "queue": spec.get("queue", "serial"), "phase": spec.get("phase", ""),
                           "reason": spec.get("reason", ""),
                           "no_internal_limit": spec.get("no_internal_limit", "") == "1"})
        return groups

    if tier == "calibrate":
        candidates = [r for r in pilot.values() if r["status"] == "completed"
                      and args.calibration_min_s <= float(r["cold_total_ns"]) / 1e9 <= 60.0]
        candidates.sort(key=lambda r: r["job_id"])
        random.Random(7).shuffle(candidates)
        chosen = candidates[: args.calibration_jobs]
        for phase, queue in (("serial", "serial"), ("parallel", "parallel")):
            for r in chosen:
                groups.append({"fixture": manifest[r["fixture_id"]], "solver": r["solver"], "ops": [r["op"]],
                               "budget": TIER_BUDGET["calibrate"], "dlx_budget": "", "queue": queue,
                               "phase": phase, "reason": "calibration", "no_internal_limit": False})
        return groups

    budget = args.budget_s or TIER_BUDGET[tier]
    for fx in manifest.values():
        if fx["status"] != "ok":
            continue
        seed = int(fx["seed_index"])
        population = fx["population"]
        if tier == "pilot" and not (seed == 1 and population in ("A", "AS", "B")):
            continue
        if tier == "core" and not (seed <= 3 and population in ("A", "B")):
            continue
        if tier == "deep" and not (seed >= 4 and population in ("A", "B")):
            continue
        for solver in bc.SOLVERS:
            ops = ops_for(fx)
            queue = "parallel"
            if tier == "deep":
                queue = "serial"
            elif tier == "core":
                # Serial (precise timing) when the pilot showed every op of this solver and cell under 10 s.
                rows = [pilot.get((population, fx["n"], fx["target_empty"], solver, op)) for op in ops]
                if all(r and r["status"] == "completed" and float(r["cold_total_ns"]) < args.serial_threshold_s * 1e9
                       for r in rows):
                    queue = "serial"
            groups.append({"fixture": fx, "solver": solver, "ops": ops, "budget": budget, "dlx_budget": "",
                           "queue": queue, "phase": "", "reason": "", "no_internal_limit": False})
    return groups


def order_groups(groups: list[dict]) -> list[dict]:
    """Seed-major round robin (seed 1 across the whole matrix first), a fixed shuffle within each
    seed, and the solver order rotated per fixture (Latin square)."""
    by_fixture: dict[str, list[dict]] = {}
    for g in groups:
        by_fixture.setdefault(g["fixture"]["id"] + "|" + g["phase"], []).append(g)
    keys = sorted(by_fixture)
    random.Random(20261104).shuffle(keys)
    keys.sort(key=lambda k: int(by_fixture[k][0]["fixture"]["seed_index"]))  # stable: keeps the shuffle
    ordered = []
    for i, key in enumerate(keys):
        gs = sorted(by_fixture[key], key=lambda g: bc.SOLVERS.index(g["solver"]) if g["solver"] in bc.SOLVERS else 0)
        rot = i % max(1, len(gs))
        ordered += gs[rot:] + gs[:rot]
    return ordered


# ---------------------------------------------------------------------------
# Execution
# ---------------------------------------------------------------------------

class Campaign:
    def __init__(self, args, out_dir: Path, bench_fields: list[str]):
        self.args = args
        self.out_dir = out_dir
        self.fields = RUNNER_FIELDS + bench_fields
        self.runs_path = out_dir / "runs.csv"
        self.lock = threading.Lock()
        self.done = {r["job_id"]: r for r in bc.read_csv(self.runs_path)}
        self.spent_s = 0.0
        self.executed = 0
        self.validation_pairs_left = args.validation_pairs if args.tier == "pilot" else 0
        self.stop = False
        self.serial_cpu = PIN_ORDER[0] if args.pin else None
        if self.runs_path.exists():
            with self.runs_path.open(newline="", encoding="utf-8") as f:
                existing = next(csv.reader(f), [])
            if existing and existing != self.fields:
                raise SystemExit(f"{self.runs_path} has different columns; use a new results directory")

    def log(self, text: str) -> None:
        line = time.strftime("%Y-%m-%d %H:%M:%S ") + text
        print(line, flush=True)
        with (self.out_dir / "campaign.log").open("a", encoding="utf-8") as f:
            f.write(line + "\n")

    def record(self, row: dict) -> None:
        with self.lock:
            new_file = not self.runs_path.exists()
            with self.runs_path.open("a", newline="", encoding="utf-8") as f:
                w = csv.DictWriter(f, fieldnames=self.fields, extrasaction="ignore")
                if new_file:
                    w.writeheader()
                w.writerow(row)
                f.flush()
            self.done[row["job_id"]] = row

    def base_row(self, g: dict, op: str, concurrency: int) -> dict:
        fx = g["fixture"]
        return {"job_id": job_id(self.args.name, fx["id"], g["solver"], op, g["budget"], g["dlx_budget"], g["phase"]),
                "tier": self.args.name, "fixture_id": fx["id"], "population": fx["population"], "n": fx["n"],
                "N": fx["N"], "empty": fx["target_empty"], "target_pct": fx["target_pct"],
                "actual_pct": fx["actual_pct"], "seed_index": fx["seed_index"], "solver": g["solver"], "op": op,
                "budget_s": f"{g['budget']:g}", "dlx_budget": g["dlx_budget"], "queue": g["queue"],
                "concurrency": concurrency, "validation_pair": 0, "pruned_source": "", "phase": g["phase"],
                "reason": g["reason"]}

    def run_job(self, g: dict, op: str, row: dict, cpu: int | None) -> dict:
        fx = g["fixture"]
        fixtures_dir = Path(self.args.fixtures_dir)
        cmd = [str(self.args.bench), "run", "--header", "--puzzle", str(fixtures_dir / fx["puzzle_file"]),
               "--solution", str(fixtures_dir / fx["solution_file"]), "--population", fx["population"],
               "--solver", g["solver"], "--op", op, "--time-limit-s", f"{g['budget']:g}",
               "--expect-hash", fx["content_hash"]]
        if g["dlx_budget"]:
            cmd += ["--dlx-budget", g["dlx_budget"]]
        if g["no_internal_limit"]:
            cmd.append("--no-internal-limit")
        if self.args.tier in ("pilot", "calibrate") or self.args.timing == "single":
            cmd += ["--timing", "single"]
        watchdog = g["budget"] + self.args.grace_s + REPETITION_ALLOWANCE_S
        row["started_utc"] = time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime())
        row["cpu"] = "" if cpu is None else cpu
        start = time.perf_counter()
        proc = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        try:
            if not pin(proc, cpu):
                row["cpu"] = "exited-before-pin"
            stdout, stderr = proc.communicate(timeout=watchdog)
            row["exit_code"] = proc.returncode
            if proc.returncode == 0:
                bench = next(csv.DictReader(io.StringIO(stdout)))
                row.update(bench)
            else:
                row["status"] = "error"
                row["message"] = stderr.strip().replace(",", ";")[:300]
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.communicate()
            row["status"] = "watchdog_killed"
            row["exit_code"] = ""
            row["message"] = f"killed by the watchdog after {watchdog:g} s"
        row["process_wall_s"] = f"{time.perf_counter() - start:.3f}"
        return row

    def run_group(self, g: dict, concurrency: int, cpus: "queue.Queue | None" = None) -> None:
        """Runs one group's operations in order. `cpus` hands out pinned CPUs (one per worker);
        without it the group runs on the serial CPU."""
        cpu = cpus.get() if cpus else self.serial_cpu
        try:
            self._run_group(g, concurrency, cpu)
        finally:
            if cpus:
                cpus.put(cpu)

    def _run_group(self, g: dict, concurrency: int, cpu: int | None) -> None:
        first_row = None
        for op in g["ops"]:
            row = self.base_row(g, op, concurrency)
            if row["job_id"] in self.done:
                if op == "first":
                    first_row = self.done[row["job_id"]]
                continue
            if self.stop:
                return
            if op != "first" and first_row and first_row.get("status") in bc.TIMED_OUT:
                take_pair = False
                with self.lock:
                    if op == "unique" and self.validation_pairs_left > 0:
                        self.validation_pairs_left -= 1
                        take_pair = True
                if not take_pair:
                    row.update({"status": "pruned_by_prefix", "pruned_source": first_row["job_id"],
                                "message": "first-solution run timed out; not executed (not an observation)"})
                    self.record(row)
                    continue
                row["validation_pair"] = 1
            row = self.run_job(g, op, row, cpu)
            self.record(row)
            with self.lock:
                self.executed += 1
                self.spent_s += float(row["process_wall_s"])
                if self.args.cap_cpu_hours and self.spent_s > self.args.cap_cpu_hours * 3600:
                    self.stop = True
            self.log(f"{row['job_id']}: {row.get('status')} "
                     f"({float(row.get('cold_total_ns') or 0) / 1e9:.3f} s solver, {row['process_wall_s']} s process)")
            if op == "first":
                first_row = row


def bench_columns(bench: Path) -> list[str]:
    """The bench's CSV columns, from a trivial 1x1 run."""
    with tempfile.TemporaryDirectory() as tmp:
        puzzle = Path(tmp) / "one.txt"
        puzzle.write_text(".\n")
        out = subprocess.run([str(bench), "run", "--header", "--puzzle", str(puzzle), "--population", "A",
                              "--solver", "mrv", "--op", "first", "--time-limit-s", "1", "--timing", "single"],
                             capture_output=True, text=True, check=True).stdout
    return out.splitlines()[0].split(",")


# ---------------------------------------------------------------------------
# Dry run: cost estimates (README section 7)
# ---------------------------------------------------------------------------

def estimate(groups: list[dict], pilot: dict, campaign_done: dict, args) -> None:
    rows = {}
    for g in groups:
        q = g["queue"]
        r = rows.setdefault(q, {"groups": 0, "jobs": 0, "worst": 0.0, "all_first_timeout": 0.0, "expected": 0.0,
                                "no_pilot": 0})
        r["groups"] += 1
        r["jobs"] += len(g["ops"])
        overhead = args.grace_s + REPETITION_ALLOWANCE_S
        r["worst"] += len(g["ops"]) * (g["budget"] + overhead)
        r["all_first_timeout"] += g["budget"] + overhead
        fx = g["fixture"]
        exp = 0.0
        first_timed_out = False
        for op in g["ops"]:
            p = pilot.get((fx["population"], fx["n"], fx["target_empty"], g["solver"], op))
            if op != "first" and first_timed_out:
                continue  # pruned
            if p is None or p["status"] == "pruned_by_prefix":
                exp += g["budget"]
                r["no_pilot"] += 1
                if op == "first":
                    first_timed_out = True
            elif p["status"] == "completed":
                t = float(p["cold_total_ns"]) / 1e9
                reps = 1.0 if t < 0.01 else (min(5 * t, 30.0) if t <= 10 else 0.0)
                exp += t + reps + 0.05
            elif p["status"] == "memory_rejected":
                exp += 0.1
            else:  # timed out in the pilot: assume it times out here too
                exp += g["budget"]
                if op == "first":
                    first_timed_out = True
        r["expected"] += exp
    print(f"{'queue':10}{'groups':>8}{'jobs':>8}{'worst CPU-h':>13}{'all-first-T/O CPU-h':>21}{'expected CPU-h':>16}")
    for q, r in rows.items():
        print(f"{q:10}{r['groups']:8}{r['jobs']:8}{r['worst'] / 3600:13.1f}{r['all_first_timeout'] / 3600:21.1f}"
              f"{r['expected'] / 3600:16.1f}")
    serial = rows.get("serial", {}).get("expected", 0.0)
    parallel = rows.get("parallel", {}).get("expected", 0.0)
    print(f"expected wall time: {(serial + parallel / args.workers) / 3600:.1f} h "
          f"(serial queue alone, then {args.workers} workers); {len(campaign_done)} jobs already recorded")
    if not pilot:
        print("no pilot results: 'expected' assumes every job uses its full budget")


# ---------------------------------------------------------------------------

def main() -> int:
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--tier", required=True, choices=["pilot", "core", "deep", "calibrate", "custom"])
    p.add_argument("--name", help="results subdirectory and tier label (default: the tier)")
    p.add_argument("--jobs-file", help="custom tier: CSV with fixture_id,solver,op,budget_s[,dlx_budget,queue,"
                                       "phase,reason,no_internal_limit]")
    p.add_argument("--fixtures-dir", default=str(bc.FIXTURES_DIR))
    p.add_argument("--results-dir", default=str(bc.RESULTS_DIR))
    p.add_argument("--pilot-dir", help="pilot results directory (default: <results-dir>/pilot)")
    p.add_argument("--bench", default=str(bc.DEFAULT_BENCH))
    p.add_argument("--workers", type=int, default=4)
    p.add_argument("--budget-s", type=float, help="override the tier budget")
    p.add_argument("--grace-s", type=float, default=60.0, help="watchdog grace beyond the budget")
    p.add_argument("--serial-threshold-s", type=float, default=10.0)
    p.add_argument("--validation-pairs", type=int, default=20)
    p.add_argument("--no-pin", dest="pin", action="store_false",
                   help="do not pin each job to its own logical CPU (pinning is Windows only)")
    p.add_argument("--timing", choices=["auto", "single"], default="auto",
                   help="single: one timed run per job (counters and profiling passes); pilot and calibrate always use it")
    p.add_argument("--calibration-jobs", type=int, default=30)
    p.add_argument("--calibration-min-s", type=float, default=0.1,
                   help="shortest pilot time (s) eligible for calibration")
    p.add_argument("--cap-cpu-hours", type=float, default=0.0, help="stop starting new jobs after this much")
    p.add_argument("--only", help="regex on job ids")
    p.add_argument("--dry-run", action="store_true")
    args = p.parse_args()
    args.name = args.name or args.tier
    if args.tier == "custom" and not args.jobs_file:
        p.error("--tier custom needs --jobs-file")
    args.bench = Path(args.bench)

    manifest = bc.load_manifest(Path(args.fixtures_dir))
    if not manifest:
        raise SystemExit(f"no manifest in {args.fixtures_dir}; run make_fixtures.py first")
    out_dir = Path(args.results_dir) / args.name
    out_dir.mkdir(parents=True, exist_ok=True)
    pilot_dir = Path(args.pilot_dir) if args.pilot_dir else Path(args.results_dir) / "pilot"
    pilot = pilot_index([r for r in bc.read_csv(pilot_dir / "runs.csv") if r["tier"] == "pilot"])

    groups = order_groups(plan_groups(args, manifest, pilot))
    if args.only:
        rx = re.compile(args.only)
        groups = [g for g in groups if any(rx.search(job_id(args.name, g["fixture"]["id"], g["solver"], op,
                                                            g["budget"], g["dlx_budget"], g["phase"]))
                                           for op in g["ops"])]
    done = {r["job_id"] for r in bc.read_csv(out_dir / "runs.csv")}

    # The full planned job list: analysis derives not_run = planned minus recorded.
    plan_rows = [{"job_id": job_id(args.name, g["fixture"]["id"], g["solver"], op, g["budget"], g["dlx_budget"],
                                   g["phase"]),
                  "fixture_id": g["fixture"]["id"], "solver": g["solver"], "op": op, "budget_s": g["budget"],
                  "queue": g["queue"], "phase": g["phase"]} for g in groups for op in g["ops"]]
    print(f"tier {args.name}: {len(groups)} groups, {len(plan_rows)} planned jobs, "
          f"{sum(r['job_id'] in done for r in plan_rows)} already recorded")
    estimate(groups, pilot, done, args)
    if args.dry_run:
        return 0
    bc.write_csv(out_dir / "plan.csv", plan_rows, list(plan_rows[0].keys()) if plan_rows else ["job_id"])

    bc.append_json_line(out_dir / "environment.jsonl", bc.environment(args.bench, {
        "tier": args.name, "workers": args.workers, "pinned": args.pin, "grace_s": args.grace_s,
        "planned_jobs": len(plan_rows), "fixtures_manifest_sha256": bc.sha256_file(Path(args.fixtures_dir) / "manifest.csv"),
    }))
    campaign = Campaign(args, out_dir, bench_columns(args.bench))
    campaign.log(f"start: {len(plan_rows)} planned jobs, {len(done)} already recorded")
    bc.keep_awake(True)
    try:
        serial = [g for g in groups if g["queue"] == "serial"]
        parallel = [g for g in groups if g["queue"] != "serial"]
        for g in serial:                   # alone on the machine: concurrency 1
            campaign.run_group(g, 1)
        if parallel:
            cpus = None
            if args.pin:
                cpus = queue.Queue()
                for cpu in PIN_ORDER[:args.workers]:
                    cpus.put(cpu)
            with cf.ThreadPoolExecutor(max_workers=args.workers) as pool:
                list(pool.map(lambda g: campaign.run_group(g, args.workers, cpus), parallel))
    finally:
        bc.keep_awake(False)
    campaign.log(f"end: {campaign.executed} jobs executed, {campaign.spent_s / 3600:.2f} process-hours"
                 + (" (stopped at the CPU cap)" if campaign.stop else ""))
    return 0


if __name__ == "__main__":
    sys.exit(main())
