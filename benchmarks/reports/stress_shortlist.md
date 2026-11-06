# Stress tier shortlist (run 2026-11-05: 25/25 timeouts; see performance.md §4)

**Purpose:** find out whether the completion walls from 10C (analysis.md §3) are real, or
are only artifacts of the 180 s budget. Also find out whether DLX can solve the case it
refused for memory. Each case runs at **1,800 s**, 10× the core budget. Results are reported
**beside** the core results under tier `stress`, never merged into them.

Job file: [stress_jobs.csv](stress_jobs.csv), which holds 25 groups and 28 jobs at most.

## 1. Frontier cells (22 jobs)

**Selection rule:** for each (population, solver, sparsity), take the **first n at which all
3 seeds failed** in the core run, just past the last n with any completion. Run seed 1, op
`first`. The rule is mechanical, so cells are not picked by expected outcome.

| Population | Solver | Cells (sparsity → n) |
|---|---|---|
| A | DLX | 30% → 12, 40% → 8, 50% → 6, 60% → 6, 70% → 6 |
| A | MRV | 30% → 10, 40% → 7, 50% → 6, 60% → 5, 70% → 6 |
| A | naive | 20% → 11, 30% → 8, 40% → 6, 50% → 5, 60% → 5, 70% → 5 |
| B | MRV | 30% → 10, 40% → 7 |
| B | naive | 20% → 11, 30% → 8, 40% → 6, 50% → 5 |

B with DLX has no frontier: DLX completed every generated B fixture.

**How to read the results:**
- A completion between 180 s and 1,800 s shows that the wall is soft at that cell.
- A timeout is consistent with a hard wall, but does not prove one.
- With 1 fixture per cell, results are **exploratory** (k = 1).

## 2. DLX memory rejections (3 groups, up to 6 jobs)

- **Fixtures:** `A-n15-e035438`, seeds 1–3 (70% empty).
- **Ops:** `first`, then `unique`; the uniqueness run is pruned if the first-solution run
  times out.
- **Budget:** 8 GiB, as planned. The estimate is 261 MiB, so the matrix fits easily.
- Under the 1 GiB rule these run in the parallel queue, not alone.
- **Expectation:** from the core runs, DLX times out on every A cell at 70% from n ≥ 6. A
  timeout here is the likely outcome, but it answers the question.

## Cost

| | |
|---|---|
| Worst case | 14.9 CPU-h: every job runs to 1,800 s, uniqueness runs included |
| If every first-solution run times out | 13.3 CPU-h |
| Expected (dry-run) | 11.0 CPU-h, about **1.8 h wall** on 6 pinned workers; worst case about 2.5 h |
| Peak memory | about 300 MB for each DLX n = 15 job; well under the 31 GB RAM |

**Run command, after approval:**
```
python benchmarks/scripts/run_campaign.py --tier custom --name stress --jobs-file benchmarks/reports/stress_jobs.csv --workers 6
```
Completions slower than 1,800 / 1.25 = 1,440 s would be candidates for serial confirmation,
under the same rule as the core run.

## Not included

- **More seeds per frontier cell:** that would triple the cost, so it was left out.
- **Population B beyond the generated sparsity:** no fixtures exist there.
- **The `LogicalSolver`:** deferred.
