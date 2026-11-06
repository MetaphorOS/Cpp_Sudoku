# Solver study: naive backtracking vs MRV backtracking vs Dancing Links

This document is the **experiment protocol** for Step 10. It was written before any benchmark
code or measurement existed, so the results can be judged against a design fixed in
advance. Any later change to the protocol goes in the [changelog](#15-changelog) with its
date and reason. Changes are never applied silently.

**Status:** Step 10 complete (2026-11-06). Final conclusions: [reports/performance.md](reports/performance.md).
Every phase and its report is listed in the [results index](#14-results-index).

**After the study.** Step 10 itself changed no solver code (see Out of scope, below). Step 13 then
used its results to build `AutoSolver`, which picks MRV or DLX per puzzle from the box size and
share of empty cells; it is the CLI default for `solve` and `count`. The thresholds come from the
best-solver map ([figures/core/fig08_best_solver.png](figures/core/fig08_best_solver.png)); see
the main [README](../README.md#algorithms).

## Folder guide

**Where to start**

| If you want… | Open |
|---|---|
| The conclusions (which solver when, and why) | [reports/performance.md](reports/performance.md) |
| One picture of which solver wins where | [figures/core/fig08_best_solver.png](figures/core/fig08_best_solver.png) |
| The core results, finding by finding | [reports/analysis.md](reports/analysis.md), with charts in `figures/core/` |
| Why the solvers behave that way (counters, gprof) | [reports/profiling.md](reports/profiling.md) |
| How the study was designed and run | this file, sections 1–13 |
| The raw numbers | `results/<run>/runs.csv`, one row per job |
| To rerun or extend something | section 12 here, and the "Reproduce" block at the end of each report |

**What is in each folder** (about 2,000 files, 72 MB, almost all of it fixtures and figures)

```
benchmarks/
├── README.md            this file: folder guide, protocol, results index, changelog
├── CMakeLists.txt       builds sudoku_bench into build-cmake/ next to sudoku.exe
├── *_stdout.log         console output of each campaign run, named after its results/ folder
│
├── src/                 the C++ benchmark tool, sudoku_bench (5 files)
│   ├── bench_main.cpp     commands: run (time one job, print one CSV row), make-fixture, env
│   ├── fixtures.*         building puzzles: population A blanking, B carving, AS grids, features
│   └── common.*           hashing, seeds, memory sampling, the independent solution checker, CSV rows
│
├── scripts/             Python that drives and analyses everything (13 files)
│   ├── bench_common.py    shared paths, the n × sparsity matrix, status rules (standard library only)
│   ├── make_fixtures.py   generates the fixtures, in parallel and resumably
│   ├── run_campaign.py    runs a tier: plans jobs, queues, pinning, watchdog, pruning, resume
│   ├── selftest.py        end-to-end test of the harness in a temporary folder
│   ├── select_borderline.py  picks parallel timeouts to rerun serially
│   ├── common.py          analysis helpers (pandas): loading runs, censored medians, colours
│   ├── summarize.py       per-cell summaries and paired speedups → results/<tier>/summary/
│   ├── plot_figures.py    figures 1–7 and C1–C4 for a tier
│   ├── plot_winner.py     figure 8, best solver per cell (the basis for AutoSolver)
│   ├── plot_stress.py     stress tier vs core, same puzzle
│   ├── profile_counters.py   cost per node / column / link from the counters build
│   ├── plot_difficulty_times.py  Step 12 chart, not part of the solver study
│   └── requirements.txt   pinned pandas, matplotlib, numpy (only needed for analysis)
│
├── fixtures/            the saved puzzles every run reads (generated once, hash-checked)
│   ├── manifest.csv       one row per fixture: sizes, seeds, features, status, hash
│   ├── meta/              the same rows, one file per fixture, written as each is built
│   ├── A/nNN/             300 randomly blanked grids: <id>.txt + <id>.solution.txt
│   ├── B/nNN/             300 carved unique puzzles; 140 missed their target and keep
│   │                      <id>.best.txt instead (the sparsest unique puzzle reached)
│   └── AS/                14 pilot-only grids from a randomized solver (grid-bias check)
│
├── results/             raw measurements, one folder per campaign run
│   ├── pilot/             60 s, 1 puzzle per cell: the first map of the matrix (1,029 jobs)
│   ├── calibrate/         parallel-slowdown check, 4 unpinned workers
│   ├── calibrate_w4…w8/   the same with pinned workers (4, 5, 6, 8): chose 6 workers
│   ├── core/              the main dataset: 180 s, 3 puzzles per cell (2,841 jobs)
│   ├── serial_confirm/    40 borderline core timeouts rerun alone (all timed out again)
│   ├── profile_counters/  every core completion replayed in the counters build (+ derived.csv)
│   ├── profile_cache/     DLX throughput vs matrix size (cache test, 43 runs)
│   ├── profile_gprof/     line-level gprof profiles of 5 representative jobs
│   └── stress/            1,800 s reruns of 25 shortlisted cells (all timed out)
│
├── figures/             charts drawn from results/ only, PNG + SVG, each folder with a figures.md index
│   ├── pilot/             figures 1–7 and C1 for the pilot (exploratory)
│   ├── core/              figures 1–8 and C1, C3, C4 for the core tier
│   └── stress/            figS1: stress vs core
│
└── reports/             the written analysis, plus the job lists the scripts consumed
    ├── correctness.md     10A: cross-solver correctness and invariant checks
    ├── schedule.md        10B: pilot findings, calibration, the approved core plan
    ├── population_b_cost.md  why unique puzzles run out at high sparsity (only up to about 20% at n ≥ 11)
    ├── analysis.md        10C: core results
    ├── profiling.md       10D: why the solvers behave as they do
    ├── stress_shortlist.md   10D: which cells got 1,800 s, and why
    ├── performance.md     final conclusions
    ├── *_jobs.csv         exact job lists for core, serial_confirm, profile_* and stress
    └── difficulty_generation_times.*  Step 12 timing sweep (not part of the solver study)
```

**Inside a results folder.** Every campaign folder except `profile_gprof/` has the same four files:
- `plan.csv`: every job the run planned, so jobs that never ran can be listed;
- `runs.csv`: one row per job, appended as each finishes (status, times, nodes, memory, counters);
- `environment.jsonl`: machine, compiler and source hash at each start;
- `campaign.log`: progress log.

`summary/` (pilot, core, stress) holds the derived per-cell tables `cells.csv`,
`speedups.csv` and `status_counts.csv`; core also has `best_solver.csv` and stress
`stress_vs_core.csv`. Each `profile_gprof/<job>/` holds that job's `row.csv`, the raw
`gmon.out`, and `flat.txt`, `graph.txt` and `lines.txt` from gprof.

**Naming.**
- **Fixture IDs:** `A-n05-e000313-s001` = population A, box size 5 (a 25×25 board), 313 empty cells, seed 1.
- **Populations:** A = random blanks, which may have many solutions; B = carved unique puzzles; AS = pilot ablation grids.
- **Solvers** in result rows: `naive`, `mrv`, `dlx`.
- **Ops:** `first` (one solution), `unique` (look for a second), `enumerate` (count all; n ≤ 2 only).

Contents:
- [Folder guide](#folder-guide)
1. [Purpose](#1-purpose)
2. [The three solvers](#2-the-three-solvers)
3. [Workload matrix](#3-workload-matrix)
4. [Puzzle populations](#4-puzzle-populations)
5. [Fixtures](#5-fixtures)
6. [Operations](#6-operations)
7. [Execution tiers, sampling and cost](#7-execution-tiers-sampling-and-cost)
8. [Measurements](#8-measurements)
9. [Run statuses, limits and concurrency](#9-run-statuses-limits-and-concurrency)
10. [Statistics](#10-statistics)
11. [Figures](#11-figures)
12. [How to reproduce](#12-how-to-reproduce)
13. [Threats to validity](#13-threats-to-validity)
14. [Results index](#14-results-index)
15. [Changelog](#15-changelog)

---

## 1. Purpose

The study measures how three exact Sudoku solvers behave as the grid grows and as puzzles
get sparser, on generalized n²×n² boards for every box size n = 1..15.

**Algorithm questions**
- How much does MRV improve on naive backtracking, and when is naive competitive?
- When does DLX beat MRV? When do its matrix construction and memory cost outweigh its search
  benefits?
- How does runtime scale with grid size and with sparsity, and do the three solvers scale
  differently?
- How does sparsity affect the size of the search tree? How much does puzzle structure
  matter beyond the number of clues?
- How do finding a first solution and proving uniqueness differ?
- Which solvers have heavy-tailed runtimes, and where do time and memory limits become
  binding?

**Engineering questions** (answered from the data; nothing gets implemented in Step 10)
- Should MRV remain the default solver?
- Does DLX give meaningful benefits for particular workloads?
- Do grid size or puzzle features justify different algorithms, and would automatic
  solver selection be worth building later?
- Which optimizations deserve investigation, and which complexities are not justified?

**Out of scope:** new solving algorithms, adaptive solver selection, difficulty-aware
generation (Step 11), new board representations, GUI work, and unrelated optimizations.
Profiling may suggest improvements; they are documented, not implemented.

## 2. The three solvers

All three implement `Solver::run(const Board&, const SolveOptions&) -> SolveResult`, with the
same solution limit, node limit, time limit, RNG and observer options.

| Solver | Name | Branches on | Representation |
|---|---|---|---|
| `NaiveBacktrackingSolver` | `backtracking-naive` | the next empty cell in row-major order | bitset candidates per row, column and box |
| `MRVBacktrackingSolver` | `backtracking-mrv` | the empty cell with the fewest candidates | same engine and bitsets as naive |
| `DLXSolver` | `dlx` | the open constraint with the fewest placements | exact-cover matrix in index-based dancing links |

**Naive vs MRV** is a clean ablation. The two share one engine
(`Search<kWords, kTraced, kMrv>` in `src/backtracking_solver.cpp`) and differ only in
`select()`.

**DLX vs MRV** differ in two ways at once: the representation, and the branching rule.
DLX may branch on "row 3 needs a 5", not only on a cell.

**What a "node" means.** Backtracking counts one node per cell choice. DLX counts one
node per constraint choice. Node counts are therefore **not comparable across engines**,
and runtime is the primary metric.

## 3. Workload matrix

**Box sizes:** every n = 1..15. N = n² is the side length, and N² is the number of cells.

**Sparsity targets:** p ∈ {10, 20, 30, 40, 50, 60, 70}% empty cells.

**Exact empty count:** `empty = round_half_up(p · N² / 100)`. Duplicate counts within one n
are removed, and the actual sparsity is recorded.

**Cells:** n = 1 gives 2 distinct (n, empty) cells, and every other n gives 7. That is
**100 cells per population**.

| n | Board | Cells | Empty cells per target (10% … 70%) |
|---:|---|---:|---|
| 1 | 1×1 | 1 | 0, 1 (the 1×1 grid is degenerate: a pure-overhead point) |
| 2 | 4×4 | 16 | 2, 3, 5, 6, 8, 10, 11 |
| 3 | 9×9 | 81 | 8, 16, 24, 32, 41, 49, 57 |
| 4 | 16×16 | 256 | 26, 51, 77, 102, 128, 154, 179 |
| 5 | 25×25 | 625 | 63, 125, 188, 250, 313, 375, 438 |
| 6 | 36×36 | 1,296 | 130, 259, 389, 518, 648, 778, 907 |
| 7 | 49×49 | 2,401 | 240, 480, 720, 960, 1,201, 1,441, 1,681 |
| 8 | 64×64 | 4,096 | 410, 819, 1,229, 1,638, 2,048, 2,458, 2,867 |
| 9 | 81×81 | 6,561 | 656, 1,312, 1,968, 2,624, 3,281, 3,937, 4,593 |
| 10 | 100×100 | 10,000 | 1,000, 2,000, 3,000, 4,000, 5,000, 6,000, 7,000 |
| 11 | 121×121 | 14,641 | 1,464, 2,928, 4,392, 5,856, 7,321, 8,785, 10,249 |
| 12 | 144×144 | 20,736 | 2,074, 4,147, 6,221, 8,294, 10,368, 12,442, 14,515 |
| 13 | 169×169 | 28,561 | 2,856, 5,712, 8,568, 11,424, 14,281, 17,137, 19,993 |
| 14 | 196×196 | 38,416 | 3,842, 7,683, 11,525, 15,366, 19,208, 23,050, 26,891 |
| 15 | 225×225 | 50,625 | 5,063, 10,125, 15,188, 20,250, 25,313, 30,375, 35,438 |

`bench_common.matrix_cells()` in `scripts/` defines this matrix, and every script uses it.

**Coverage means a recorded outcome, not completion.** Earlier measurements (Step 8)
showed MRV exceeding 2 million nodes on 30%-empty 81×81 grids, and running longer than 20 s on
60%-empty 25×25 grids. Many high-n, high-sparsity cells are therefore expected to time out
for the backtracking solvers, and DLX will refuse some of them for memory. Every planned
cell still gets a **recorded outcome** (§9), and the completion-rate heatmaps are the
evidence for that region. A useful finding does not require every case to complete.

**Sparsity is not difficulty.** A 70%-empty randomly blanked board usually has many
solutions, so finding the first one can be easy. Hard puzzles cluster where puzzles approach
uniqueness. So every fixture also records structural features (§5), and the analysis can
separate structure from clue count.

## 4. Puzzle populations

The study uses two separate populations. The analysis never mixes them.

### A. Randomly blanked complete grids
1. Build a complete grid with `randomCompletedGrid(n, rng(gridSeed))`.
2. Choose **exactly** `empty` cells uniformly (a seeded shuffle of the cell indices,
   taking the first k) and clear them.
3. Store the source grid as a known solution.

These puzzles may have many solutions. They allow controlled study of sparsity.

**Known bias.** `randomCompletedGrid` gives random images (relabelling, row and column
permutations, transpose) of **one** pattern grid, not uniformly random grids. A pilot-only
ablation at n = 3 and 4 builds a second set from randomized solver fills, the method of
`randomFullGrid` in `tests/property_tests.cpp`, and compares the two.

### B. Verified uniquely solvable puzzles
Benchmark code only; the library generator is not changed.
1. Start from a complete grid and take a seeded random removal order.
2. Remove clues until exactly `empty` cells are empty. A removal is accepted when:
   1. the remaining clues still force the value (the generator's existing argument), or
   2. an MRV uniqueness check (solution limit 2, node budget of 10⁶) proves the puzzle unique, or
   3. the MRV check aborted, and a DLX check proves uniqueness within the same budget.

   If both checks abort, the clue stays.
3. If the removal order runs out before the target is reached, the procedure retries with a
   new sub-seed: up to 3 attempts and 600 s per fixture in total.
4. **Proof of uniqueness:** by induction over the accepted removals.
5. **Independent re-verification:** the solver that did *not* make the last proof re-checks
   the finished puzzle, with a 600 s budget. The outcome is recorded as
   `verified_independently = yes | inconclusive`.

**Targets that are not reached.** If a target cannot be reached within the budget, the
fixture is recorded as `target_not_achieved`, together with:
- the reason: `generation_budget_exhausted` or `removal_order_exhausted`;
- the most empty cells achieved;
- the attempts made, checks run, checks aborted, and generation time.

The sparsest unique puzzle reached is kept as a separately labelled file
(`<id>.best.txt`), but it never replaces the target and is not part of the matrix.

A target is called `impossible` only when that is proven, e.g. by a minimum-clue bound.
None of the planned targets is known to be impossible: 4×4 puzzles can have at most 12 empty
cells, and 9×9 puzzles at most 64.

**Known bias.** Carving uses two of the solvers under study, so population B leans toward
puzzles that MRV or DLX can prove unique within budget.

## 5. Fixtures

Fixtures are generated **once**, saved, and kept. Every solver and every tier reads the same
files. Nothing is regenerated per solver, and the runner checks each file's hash before
using it.

```
benchmarks/fixtures/
  manifest.csv                            one row per fixture (merged from meta/)
  meta/<id>.csv                           each fixture's manifest row, written as it is built
  A/n05/A-n05-e000313-s001.txt            the puzzle (project text format)
  A/n05/A-n05-e000313-s001.solution.txt   a known solution (A) or the unique solution (B)
  AS/...                                  ablation grids (n = 3, 4; seed 1)
  B/...                                   carved unique puzzles; missed targets keep <id>.best.txt
```

**Manifest columns:**
- **Identity:** `id`, `population`, `n`, `N`, `cells`, `clues`, `empty`, `target_pct`,
  `actual_pct`, `seed_index`, `grid_seed`, `blank_or_carve_seed`.
- **Solution:** `solution_status`, `verification`, `verified_independently`.
- **Integrity:** `content_hash` (FNV-1a 64).
- **Structural features:** `min_candidates`, `mean_candidates`, `forced_cells`,
  `dlx_placements`, `dlx_estimated_bytes`.
- **Generation:** `checks`, `checks_aborted`, `attempts`, `generation_ms`.
- **Status:** `ok` or `target_not_achieved`, with `reason` and `max_empty_achieved`.

**Seeds** are pre-assigned indices 1, 2, 3, … and are never chosen by outcome. Each one maps to
generator seeds through a documented `splitmix64(hash(population, n, empty, seed_index))`.

## 6. Operations

| Operation | Solution limit | Question answered |
|---|---|---|
| `first` | 1 | Find one solution |
| `unique` | 2 | Find two solutions, or prove there is only one (unique puzzles force an exhaustive search after the first solution) |
| `enumerate` | 0 | Count all solutions; population A, n ≤ 2 only |

**The prefix property.** Without RNG, a solver's search order is deterministic, and the
solution limit only changes where it stops. So a `unique` run performs exactly the `first`
run's work and then continues. Phase 10A tests this.

The property is about **work**, not wall-clock time. Two separate executions can differ in
timing, so it is used only to **skip** runs:
- If a fixture's `first` run timed out, its `unique` run is not executed. It gets the status
  `pruned_by_prefix`, linked to the source run.
- A pruned run is not an observation. It is **excluded from all statistics** and counted
  separately.
- **Validation:** the pilot executes about 20 such pairs anyway. If any `unique` run completes
  where `first` timed out, pruning is dropped for that solver, and the counterexample is
  reported.
- Every executed `unique` run also records time-to-first-solution, as a cross-check against
  the separately measured `first` run.

## 7. Execution tiers, sampling and cost

**Tiers**

| Tier | Budget per run | Fixtures per cell | Purpose |
|---|---|---|---|
| Pilot | 60 s | 1 (seed 1) | Find fast, slow, variable and memory-bound regions; estimate campaign cost. Results are exploratory and stored separately. |
| Core | 180 s (600 s in the original protocol; see changelog) | 3 (seeds 1–3) for every cell, solver and population | The primary dataset |
| Core deep | 180 s; deferred (see changelog) | 20, or 100 where every solver takes < 0.1 s | Distributions and tails, in cells that are cheap to sample. Serial only, capped at 5 CPU-hours. |
| Serial confirmation | 180 s | selected | Rerun borderline parallel results and representative long comparisons serially. At most 40 runs. |
| Stress | 1800 s | shortlist only | Run after reviewing the core results, on a shortlist you approve. DLX memory rejections are rerun with an 8 GiB budget, each alone. |

**Sampling order ("sentinel first").** Seed 1 runs across the whole matrix, then seed 2,
then seed 3, cycling round-robin over cells. If a CPU cap stops a campaign, the missing runs
are spread evenly. A timeout on seed 1 **never** cancels seeds 2 and 3.

**Sampling levels.** Every statistic and figure is labelled with k, the number of fixtures
measured in its cell.

| k | Level | What is reported |
|---|---|---|
| 1 | exploratory | the single result; no completion rate |
| 3–19 | sampled | median, IQR, maximum, completion count, Wilson confidence interval (wide, and shown) |
| 20–99 | distribution | adds boxplots and bootstrap confidence intervals |
| ≥ 100 completed | tail | adds p95 |

**Worst-case cost.** This assumes every executed run hits its limit, and is computed from the
number of possible runs.

| Component | Runs | Limit each | Worst case |
|---|---:|---:|---:|
| Population B carving (300 fixtures, all attempts) | 300 | 600 s | 50 CPU-h |
| Population B independent re-verification | 300 | 600 s | 50 CPU-h |
| Pilot: 200 fixtures × 3 solvers × 2 ops, plus 84 ablation runs and 20 validation pairs | ~1,300 | 60 s | ~22 CPU-h |
| Core: 600 fixtures × 3 solvers × 2 ops, plus enumeration for n ≤ 2 | ~3,650 | 600 s | ~608 CPU-h |
| Core deep | capped | — | 5 CPU-h |
| Repetitions (runs under 10 s only) | — | ≤ ~10 s per run | in the dry-run total |
| Serial confirmation | ≤ 40 | 600 s | 6.7 CPU-h |
| Stress (example: 40 cases) | shortlist | 1800 s | 20 CPU-h |

Population A generation takes milliseconds.

**Expected cost.** It is far lower, because most cells finish in milliseconds and pruning
removes most `unique` runs in the timeout region. A rough pre-pilot guess for the core tier is
75–150 CPU-hours (20–40 hours of wall-clock time on 4 workers). No firm number is quoted
before the pilot.

`run_campaign.py --dry-run` reports three figures for every component:
- the worst case from the number of possible runs;
- the worst case after pruning;
- the expected cost, extrapolated from pilot times.

The core campaign starts only after the schedule review, which can also set a CPU cap.

## 8. Measurements

**Primary metric: total time.** It is measured with `steady_clock` around
`solver.run(board, options)` inside the benchmark process.
- **Included:** validity checks, constraint setup, DLX matrix construction, allocation,
  search and result copying.
- **Excluded:** parsing and loading the fixture.

**Diagnostic split** (measured inside the solver):
- **setup:** from `run()` entry to the first search iteration;
- **search:** from there until return.

**Also recorded for every run:**
- time to first solution, nodes at first solution;
- nodes, backtracks, maximum depth;
- solutions found, stop reason, status;
- the process wall time seen by the runner, as a separate column.

**Repetitions.** A first (cold) invocation is always recorded separately. Its duration
then decides the method:

| Single-run time | Method |
|---|---|
| under 10 ms | **Batched.** Double a batch size b until one timed batch of b back-to-back `run()` calls lasts at least 50 ms. That is at least 500× the measured clock resolution, and b has no upper limit (about 10⁶ for n = 1). Time 15 such batches; per-invocation time = batch time / b. These are warm-cache latencies, labelled as such, and every invocation includes its own setup. |
| 10 ms – 10 s | Individual runs, repeated until 5 runs or 30 s have accumulated, whichever comes first |
| over 10 s | A single run |

A fixture's time is the median of its batches or repetitions. Two kinds of variation are
reported separately:
- repeat noise: the coefficient of variation of one fixture across its repetitions;
- between-puzzle variation: IQR and spread across the fixtures of one cell.

**Memory**
- **Measured:** the process's peak working set and peak commit after the run, read
  through `GetProcessMemoryInfo`, minus a baseline taken after the fixture is loaded. This
  is approximate, with 4 KB page granularity.
- **Estimated:** `DLXSolver::estimate(board)` for DLX, and a formula for the backtracking
  solvers.

Figures always label which kind of memory number they show, and never mix them.

**Diagnostic counters** come from a separate build (`-DSUDOKU_SOLVER_COUNTERS=ON`, in
`build-counters/`), so the timed build stays unchanged.
- **Backtracking:** candidate computations, cells scanned by MRV, placements.
- **DLX:** columns inspected, covers, uncovers, rows examined, link updates.
- **Allocations:** count and bytes, from a replaced `operator new` in that build only.

**Overhead check.** The added timing fields cost a few clock reads per run and one compare per
node. Before the pilot, an A/B comparison against the pre-change binary must show a
difference within run-to-run noise (about ±3%).

## 9. Run statuses, limits and concurrency

| Status | Meaning |
|---|---|
| `completed` | finished within the budget, with a validated result |
| `timeout` | the internal deadline (measured from `run()` entry) was reached |
| `watchdog_killed` | the process exceeded the budget plus 120 s (60 s grace + 60 s allowance for repeated timings) |
| `memory_rejected` | DLX refused the puzzle because its memory estimate exceeded the budget |
| `unsupported` | the workload cannot be run (none expected for n ≤ 15) |
| `error` | crash, non-zero exit, or **a result that fails validation** (this halts the campaign) |
| `pruned_by_prefix` | not run (§6); excluded from statistics |
| `not_run` | planned but not executed: a CPU cap, an unfinished campaign, or a `target_not_achieved` fixture |

**Validation of results.** Every completed result is checked by an independent checker
that does not use the solvers' `Constraints` code:
- the grid is complete;
- every row, column and box is a permutation;
- the givens are kept;
- for population B, the solution equals the stored one and the count is 1.

**Limits.** The runner's external watchdog is the primary deadline: it kills the process
at budget + 120 s (60 s grace plus a 60 s allowance for the repeated timings of runs that finish under
10 s). That also covers DLX matrix construction, which cannot be interrupted.
The solver's internal time limit, set to the same budget, stays on as a secondary
safeguard: it ends normal timeouts cleanly and records how far the search got. Both solvers
start their deadline at `run()` entry, so initialization counts against the budget. The runner
keeps the machine awake for its own process (`SetThreadExecutionState`); no system settings
are changed.

**Concurrency.** Every result row records `concurrency`, `tier` and `budget_s`.
- **Serial queue** (concurrency 1): runs expected to take under 10 s, deep sampling, and
  serial confirmations. **Only serial runs are used for latency figures and speedup
  ratios.**
- **Parallel queue** (4 workers): long, timeout-prone runs. They supply completion data and
  exploration. Their times are shown only in figures labelled "parallel (concurrency 4)".
- **Calibration:** about 30 runs of 1–60 s execute at concurrency 1 and 4, to measure the
  slowdown factor s.
- **Borderline parallel runs** are rerun serially: those that timed out, or completed in
  more than budget / s. The serial outcome decides completion, and both outcomes are kept.
- **No pooling:** serial and parallel timings are never combined.
- **High-memory DLX runs** (estimated above 1 GiB) always run alone.

## 10. Statistics

- **Medians with timeouts.** If more than half of a cell's runs timed out, the median is
  reported as "> budget". It is never computed from the completed runs alone.
- **Tails.** p95 is reported only where at least 100 fixtures completed. Elsewhere the
  report gives the median, IQR, maximum observed and completion count.
- **Completion rate:** completed / measured runs, with a Wilson 95% confidence interval and
  k. Pruned and not-run entries are excluded from the denominator.
- **Paired speedups** compare the same fixture and operation, from serial runs. For example,
  DLX speedup vs MRV = t_MRV / t_DLX, and values above 1 mean DLX is faster.

  | Outcome of the pair | Reported as |
  |---|---|
  | Both completed | geometric mean of the ratios, with a bootstrap 95% CI that resamples fixtures |
  | One timed out | a bound (≥ budget / t), counted as a censored win and shown separately |
  | Both timed out | undetermined |

- **Censored runtime distributions.** All runs in a tier share one budget, so the
  Kaplan–Meier estimator reduces to the empirical CDF up to the budget. "Fraction of fixtures
  solved by time t" curves are therefore exact without survival libraries. Pruned runs are
  not censored observations and are excluded.
- **Environment record**, stored per tier in `environment.json`:
  - CPU, RAM, OS;
  - compiler version, build type and flags;
  - source-tree hash (the project is not a git repository);
  - date, budgets, worker count.

## 11. Figures

All figures are generated by scripts from the saved CSV files, as PNG and SVG, with
consistent solver colours.
- Missing or censored cells are hatched or marked; values are never invented.
- Every panel shows k and its sampling level.
- Pruned and not-run counts are shown, and are never drawn as timeouts.

**Priority figures**, always produced:
1. Runtime vs n (1–15), per population × operation × selected sparsity; log scale.
2. Runtime vs sparsity, for selected n (3, 4, 5, 6, 8, 10).
3. A runtime heatmap per solver (n × sparsity, log colour scale). Cells are marked as
   timed out, memory-rejected, `target_not_achieved`, not run, or exploratory.
4. Paired speedups (MRV vs naive, DLX vs MRV, DLX vs naive), with censored wins shown
   separately.
5. Completion-rate heatmaps per solver, with core and stress kept apart.
6. Memory vs n, measured vs estimated, with the 256 MiB budget line and rejections marked.
7. Setup vs search time for representative workloads.

**Conditional figures,** produced only when their data condition holds. Otherwise the
analysis lists them as "not produced: insufficient data (k = …)".

| Figure | Data condition |
|---|---|
| Search nodes vs sparsity | Same cells as figure 2; the caption notes nodes are not comparable across engines |
| Runtime distributions (box and strip plots) | Cells with k ≥ 20 |
| Fraction solved by time t | Groups with ≥ 20 measured runs per solver |
| Time vs nodes, log–log | Completed serial runs spanning at least 2 orders of magnitude in node count |
| Runtime vs mean initial candidates | Fixed (n, sparsity) cells with k ≥ 20 |

**Tools** (Phase 10B):

| Tool | Role |
|---|---|
| `build-cmake/sudoku_bench` | `make-fixture` builds one fixture; `run` executes one job in its own process and prints one CSV row; `env` prints build information |
| `scripts/make_fixtures.py` | builds fixtures in parallel and resumably, then merges `manifest.csv` |
| `scripts/run_campaign.py` | plans and runs a tier: queues, watchdog, pruning, resume, `--dry-run` cost estimates |
| `scripts/selftest.py` | end-to-end self-test of the harness in a temporary directory |
| `scripts/bench_common.py` | the matrix, status rules and environment capture shared by the scripts |
| `scripts/summarize.py`, `scripts/plot_figures.py` | per-cell summaries and figures 1–7 and C1–C4 per tier (10C) |
| `scripts/select_borderline.py` | picks parallel timeouts for serial confirmation |
| `scripts/profile_counters.py` | joins counters-build operation counts with serial timings (10D) |
| `scripts/plot_stress.py` | stress tier vs core, same fixture (10D) |
| `scripts/plot_winner.py` | best solver per (n, sparsity) cell, figure 8 |

**Results layout** (`benchmarks/results/<tier>/`):
- `plan.csv`: every planned job, for deriving `not_run`.
- `runs.csv`: one appended row per job, holding runner fields plus `sudoku_bench` fields. The
  per-repetition detail is summarised in the row (median, quartiles, minimum, maximum, sample
  count, batch size).
- `environment.jsonl`: one environment record per campaign start.
- `campaign.log`.

## 12. How to reproduce

The commands below rebuild the study from scratch. Fixtures are already saved in
`fixtures/`, and the results in `results/`; rerunning a tier resumes and skips runs it already
has.

**Prerequisites**
- MSYS2 GCC 14 (`C:\msys64\mingw64\bin` on PATH)
- CMake ≥ 3.20 and Ninja
- Python 3.12
- For plotting only: `pandas`, `matplotlib`, `numpy`, installed from
  `benchmarks/scripts/requirements.txt`

```bash
cmake -S . -B build-cmake -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build-cmake
```

```bash
python benchmarks/scripts/selftest.py
```

```bash
python benchmarks/scripts/make_fixtures.py --populations A --seeds 1-3
```

```bash
python benchmarks/scripts/make_fixtures.py --populations AS --n 3-4 --seeds 1
```

```bash
python benchmarks/scripts/make_fixtures.py --populations B --seeds 1-3
```

```bash
python benchmarks/scripts/run_campaign.py --tier pilot --dry-run
```

```bash
python benchmarks/scripts/run_campaign.py --tier pilot
```

```bash
python benchmarks/scripts/run_campaign.py --tier core
```

```bash
python benchmarks/scripts/summarize.py --tier core
```

```bash
python benchmarks/scripts/plot_figures.py --tier core
```

Rerunning `run_campaign.py` resumes where it stopped: runs already present in `runs.csv`
are skipped.

**Analysis after the core tier** (10C/10D):

```bash
python benchmarks/scripts/plot_winner.py        # best solver per cell (fig08) and best_solver.csv
python benchmarks/scripts/profile_counters.py   # cost model from the counters build
python benchmarks/scripts/summarize.py --tier stress
python benchmarks/scripts/plot_stress.py        # stress vs core (figS1) and stress_vs_core.csv
```

The profiling and stress commands, including how to build the counters and gprof binaries,
are listed in [reports/profiling.md](reports/profiling.md) and
[reports/performance.md](reports/performance.md).

**Builds.** `build-cmake/` is the normal Release build, with all tests and `sudoku_bench`.
During Step 10 it was called `build-ninja/`; that and the special-purpose builds used in the
study are archived, hidden, in `old_builds/`. Their CMake caches point at their old paths, so
to rebuild one, configure a fresh directory:
- **validate** (10A): `-DCMAKE_BUILD_TYPE=Debug -DSUDOKU_DLX_VALIDATE=ON -DCMAKE_CXX_FLAGS=-D_GLIBCXX_ASSERTIONS`;
  runs the DLX structural invariant checks.
- **UBSan** (10A): `-DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_CXX_FLAGS="-fsanitize=undefined -fsanitize-trap=undefined -fno-sanitize-recover=all"`.
- **counters** and **gprof** (10D): see [reports/profiling.md](reports/profiling.md).

## 13. Threats to validity

- **Timing noise:**
  - CPU boost and thermal behaviour, and Windows background activity.
  - Warm caches during batched timing (labelled as such).
  - Contention in parallel runs, measured and reported, and never pooled with serial
    timings.
- **Sample sizes:** most cells have k = 3. Their statistics are labelled "sampled", and
  strong tail claims are made only at k ≥ 100.
- **Population A** comes from one pattern-grid family. The pilot's n = 3, 4 ablation (one
  seed per cell) flagged a possible bias at n = 4 for the backtracking solvers, not for DLX;
  the 3-seed follow-up was not run (see [performance.md](reports/performance.md) §6).
- **Population B** is carved with MRV and DLX checks, so it leans toward puzzles those
  solvers can prove unique. Unreached targets are reported, never substituted.
- **Node counts** mean different things in DLX and backtracking.
- **Measured memory** is a process-level approximation with page granularity, shown beside
  the analytic estimates.
- **Versioning:** results are tied to a hash of the source tree, because the project is not
  a git repository.
- **Single machine:** an AMD Ryzen 5 7600X (6 cores, 12 threads, 1 MB L2 per core, 32 MB L3)
  with 31 GB RAM, running Windows 11. Results are relative comparisons on this machine.

## 14. Results index

| Phase | Deliverable | Status |
|---|---|---|
| 10A | [Correctness report](reports/correctness.md) | done (2026-11-04) |
| 10B | Fixtures, harness, pilot, [schedule](reports/schedule.md), [population B cost note](reports/population_b_cost.md) | done (2026-11-05) |
| 10C | Core campaign, figures, [analysis](reports/analysis.md) | done: [analysis.md](reports/analysis.md), figures in `figures/core/` and `figures/pilot/` |
| 10D | Profiling, [stress shortlist](reports/stress_shortlist.md), [final report](reports/performance.md) | done (2026-11-06): [profiling.md](reports/profiling.md); stress tier 25/25 timeouts at 1,800 s (`figures/stress/`); [performance.md](reports/performance.md) |

## 15. Changelog

| Date | Change | Reason |
|---|---|---|
| 2026-11-04 | Protocol written | Step 10 plan approved |
| 2026-11-04 | Tooling as built: fixtures come from `make_fixtures.py` + `sudoku_bench make-fixture`, not a `plan`/`fixtures` subcommand; repetition detail is summarised in each `runs.csv` row instead of a separate `reps.csv`; environment records go to `environment.jsonl` | Simpler; the measured quantities are unchanged |
| 2026-11-04 | Watchdog limit = budget + 60 s grace + 60 s repetition allowance | Completed runs under 10 s are repeated in the same process, and that time must not trigger the watchdog |
| 2026-11-04 | The sparsest unique puzzle of a missed population-B target is kept as `<id>.best.txt` rather than as a `B*` fixture | It is not part of the matrix, so it needs no ID of its own |
| 2026-11-04 | The ablation population is labelled `AS` (solver-filled grids) | Needs a label of its own in the manifest |
| 2026-11-05 | Instrumentation A/B check: 7 of 9 cases within +2.1%, but classic 9×9 MRV +7–9% (≈ +0.9 µs) and hard 9×9 DLX +6.7% | Outside the ±3% criterion; likely code layout. All solvers use the same instrumented binary, so comparisons are unaffected; recorded as a limitation |
| 2026-11-05 | Calibration uses pilot jobs of 0.1–60 s, first-solution and uniqueness runs alike (`--calibration-min-s`), instead of 1–60 s first-solution runs only | Pilot times are bimodal; only 15 runs fell in 1–60 s |
| 2026-11-05 | Borderline rule uses s = 1.5 (measured slowdown 1.03–1.47, geometric mean 1.14) | Calibration result |
| 2026-11-05 | Full deep sampling costs about 8–12 CPU-h, over the 5 CPU-h cap | Batching makes each fast job about 1 s; decision left to review |
| 2026-11-05 | Core and deep budget changed from 600 s to 180 s; deep sampling deferred | Review decision: about 1–2% of timeouts would convert between 60 and 600 s, and wall time drops from about 21 h to about 5 h |
| 2026-11-05 | Jobs are pinned to one logical CPU each (`--no-pin` disables this); the parallel queue uses 6 workers on separate physical cores; a `cpu` column records the pinning | Calibration: pinned slowdown is 1.05–1.10 at 4–6 workers, versus 1.14 unpinned at 4 and up to 2.07 with hyperthreads |
| 2026-11-05 | Borderline rule uses s = 1.25 (rerun after 144 s at a 180 s budget) | Pinned calibration: the worst pair at 6 workers is 1.15 |
| 2026-11-05 | Serial confirmation capped at 40 of 99 borderline candidates: all 10 where the same solver completed on another seed, plus 30 sampled (seed 7) from the rest | 99 candidates exceed the cap of 40; the unsampled 59 keep their parallel outcome, labelled unconfirmed |
| 2026-11-05 | Fraction-solved curves pool all sparsities of one n; a median with exactly half its runs not completed is reported as censored | The plan defined groups by population/op/size; a two-run cell with one timeout has no determinable median |
| 2026-11-05 | `run_campaign.py --timing single` for counters and profiling passes; a job that exits before it can be pinned is recorded as `cpu = exited-before-pin` instead of aborting the run | Sub-millisecond single-shot jobs finished before `SetProcessAffinityMask` ran |
| 2026-11-05 | gprof builds use `-fno-pie` and link with `-no-pie`; line-level profiles (`gprof -l`) | MinGW gprof collected no samples from PIE binaries, and -O3 inlines all solver code into `run()` |
| 2026-11-05 | The stress tier uses frontier cells chosen by rule (first all-fail n per population, solver and sparsity), seed 1, 1,800 s; DLX memory reruns use the parallel queue | The estimate is 261 MiB, under the 1 GiB run-alone threshold |
| 2026-11-06 | The B/MRV/30% stress job ran on `B-n10-e003000-s001`, a `target_not_achieved` fixture, so `sudoku_bench` used its sparsest reached puzzle (`.best.txt`, 2,923 empty, 29.2%); it is reported as a near-target run, not as the 30% cell | The selection rule took seed 1 mechanically without checking fixture status; the cell's only on-target fixture (s003) had already timed out in core |
| 2026-11-06 | Stress results get their own table and figure (`scripts/plot_stress.py`, `figS1_stress_frontier`) instead of the generic figures 1–7 | One fixture on each of 25 shortlisted cells and no completions; the generic figures need k ≥ 3 across the matrix |
