"""Generates the benchmark fixtures once, in parallel and resumably (README section 4-5).

Each fixture is built by `sudoku_bench make-fixture` in its own process; its manifest row is
kept in fixtures/meta/<id>.csv, so an interrupted run resumes where it stopped. At the end all
rows are merged into fixtures/manifest.csv.

Examples:
  python benchmarks/scripts/make_fixtures.py --populations A,B --seeds 1-3
  python benchmarks/scripts/make_fixtures.py --populations AS --n 3-4 --seeds 1
  python benchmarks/scripts/make_fixtures.py --populations A,B --cells deep_cells.csv --seeds 4-20
  python benchmarks/scripts/make_fixtures.py --dry-run
"""

from __future__ import annotations

import argparse
import concurrent.futures as cf
import subprocess
import sys
import threading
import time
from pathlib import Path

import bench_common as bc


def parse_range(text: str) -> list[int]:
    out: list[int] = []
    for part in text.split(","):
        if "-" in part:
            lo, hi = part.split("-")
            out += range(int(lo), int(hi) + 1)
        else:
            out.append(int(part))
    return out


def specs(args) -> list[dict]:
    cells = bc.matrix_cells()
    if args.cells:
        wanted = {(int(r["n"]), int(r["empty"])) for r in bc.read_csv(Path(args.cells))}
        cells = [c for c in cells if (c["n"], c["empty"]) in wanted]
    ns = set(parse_range(args.n))
    out = []
    for population in args.populations.split(","):
        for cell in cells:
            if cell["n"] not in ns:
                continue
            if population == "AS" and cell["n"] not in (3, 4):
                continue  # the grid-source ablation is defined for n = 3, 4 only
            for seed in parse_range(args.seeds):
                out.append({"population": population, "n": cell["n"], "empty": cell["empty"],
                            "target_pct": cell["target_pct"], "seed_index": seed,
                            "id": bc.fixture_id(population, cell["n"], cell["empty"], seed)})
    # Expensive carvings first, so the slowest fixtures do not end up running alone at the end.
    out.sort(key=lambda s: (s["population"] != "B", -s["n"] * s["target_pct"], s["id"]))
    return out


def merge_manifest(out_dir: Path) -> int:
    rows, header = [], None
    for meta in sorted((out_dir / "meta").glob("*.csv")):
        data = bc.read_csv(meta)
        if data:
            rows.append(data[0])
            header = header or list(data[0].keys())
    order = {"A": 0, "AS": 1, "B": 2}
    rows.sort(key=lambda r: (order.get(r["population"], 9), int(r["n"]), int(r["target_empty"]),
                             int(r["seed_index"])))
    if header:
        bc.write_csv(out_dir / "manifest.csv", rows, header)
    return len(rows)


def main() -> int:
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--populations", default="A,B")
    p.add_argument("--n", default="1-15")
    p.add_argument("--seeds", default="1-3")
    p.add_argument("--cells", help="CSV with n,empty columns: restrict to these cells (deep sampling)")
    p.add_argument("--out", default=str(bc.FIXTURES_DIR))
    p.add_argument("--bench", default=str(bc.DEFAULT_BENCH))
    p.add_argument("--workers", type=int, default=5)
    p.add_argument("--budget-s", type=float, default=600)
    p.add_argument("--verify-budget-s", type=float, default=600)
    p.add_argument("--check-nodes", type=int, default=1_000_000)
    p.add_argument("--dry-run", action="store_true")
    args = p.parse_args()

    out_dir = Path(args.out)
    (out_dir / "meta").mkdir(parents=True, exist_ok=True)
    todo = [s for s in specs(args) if not (out_dir / "meta" / f"{s['id']}.csv").exists()]
    worst_b = sum(args.budget_s + args.verify_budget_s for s in todo if s["population"] == "B")
    print(f"{len(todo)} fixtures to build ({sum(s['population'] == 'B' for s in todo)} carved); "
          f"worst case {worst_b / 3600:.1f} CPU-h for carving + verification, "
          f"~{worst_b / 3600 / max(1, args.workers):.1f} h wall on {args.workers} workers")
    if args.dry_run:
        return 0

    lock = threading.Lock()
    failures = out_dir / "failures.log"
    done = 0
    started = time.time()

    def build(spec: dict) -> None:
        nonlocal done
        cmd = [args.bench, "make-fixture", "--population", spec["population"], "--n", str(spec["n"]),
               "--empty", str(spec["empty"]), "--target-pct", str(spec["target_pct"]),
               "--seed-index", str(spec["seed_index"]), "--out", str(out_dir),
               "--budget-s", str(args.budget_s), "--verify-budget-s", str(args.verify_budget_s),
               "--check-nodes", str(args.check_nodes)]
        timeout = args.budget_s + args.verify_budget_s + 300
        try:
            proc = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout)
            ok = proc.returncode == 0 and proc.stdout.count("\n") >= 2
            message = proc.stderr.strip()
        except subprocess.TimeoutExpired:
            ok, message = False, f"killed after {timeout} s"
        with lock:
            done += 1
            if ok:
                (out_dir / "meta" / f"{spec['id']}.csv").write_text(proc.stdout, encoding="utf-8")
                status = proc.stdout.splitlines()[1].split(",")[12]
            else:
                status = "FAILED"
                with failures.open("a", encoding="utf-8") as f:
                    f.write(f"{spec['id']}: {message}\n")
            print(f"[{done}/{len(todo)} {time.time() - started:7.0f}s] {spec['id']}: {status}", flush=True)

    bc.keep_awake(True)
    try:
        with cf.ThreadPoolExecutor(max_workers=args.workers) as pool:
            list(pool.map(build, todo))
    finally:
        bc.keep_awake(False)
    print(f"manifest: {merge_manifest(out_dir)} fixtures in {out_dir / 'manifest.csv'}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
