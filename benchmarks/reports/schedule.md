# Phase 10B: fixtures, pilot and core schedule

*2026-11-05. Everything here comes from saved files. Fixtures:
`benchmarks/fixtures/manifest.csv`. Pilot: `benchmarks/results/pilot/runs.csv`.
Calibration: `benchmarks/results/calibrate/runs.csv`. The core job list per cell:
[core_jobs.csv](core_jobs.csv).*

**Status:** the core campaign was approved with changes and started on 2026-11-05 at 14:04.
The final settings are in [section 9](#9-revision-approved-core-settings); sections 5–8 below
are the original proposal they replace.

---

## 1. Harness checks

| Check | Result |
|---|---|
| Self-test (`benchmarks/scripts/selftest.py`) | **Passed.** It covers: an n ≤ 3 pilot end to end, independent validation of every result, the watchdog kill, `memory_rejected`, a timeout followed by `pruned_by_prefix` linked to its source run, resuming without rerunning, batches of at least 50 ms, concurrency recorded, hash rejection of a modified fixture, and completion rates that exclude pruned and not-run entries |
| CTest after the 10B library changes | 5/5 (96 unit tests, 15 properties) |
| Pilot results validated | 706/706 completed runs passed the independent check, and none failed validation |
| Prefix property in real runs | In 339/339 pilot uniqueness runs, the node count at the first solution equals the node count of the separate first-solution run |
| Pruning validation pairs | 20/20 (naive 9, MRV 6, DLX 5): the uniqueness run **also timed out** wherever the first-solution run timed out. No counterexample, so pruning stays enabled for all three solvers |

### Instrumentation overhead (A/B test): it does **not** fully meet the ±3% criterion
The pre-10B binary was compared with the instrumented build, 101 runs per case.

| Case | Change |
|---|---|
| 7 of 9 solver/puzzle cases | within +2.1% |
| Classic 9×9, MRV | +7 to 9%, which is about +0.9 µs on a 13 µs solve |
| Hard 9×9, DLX | +6.7%, about +46 µs on 0.69 ms, the same in two rounds |

Removing the only per-node addition (tracking the maximum depth) did not change the DLX
number, and a clock read costs about 22 ns. The most likely cause is code or data layout, not
the instrumentation's work.

All three solvers are measured with the same instrumented binary. The pre-change binary is
not used anywhere, so the study's comparisons are unaffected; only the absolute times of
those two cases are a little higher. This is recorded as a known limitation.

## 2. Fixtures

| Population | Fixtures | Status | Cost |
|---|---|---|---|
| A (random blanks of pattern grids) | 300 (100 cells × seeds 1–3) | all `ok` | under 1 s in total |
| AS (ablation, solver-filled grids, n = 3, 4) | 14 | all `ok` | under 1 s |
| B (carved to an exact, unique empty count) | 300 | **160 `ok`, 140 `target_not_achieved`** | **24.3 CPU-hours** (5 workers, about 4.9 h wall over two sessions) |

There were no generation failures (`failures.log` is empty).

### Population B: highest target reached

| n | Targets reached (all 3 seeds) | Missed targets: sparsest unique puzzle achieved |
|---|---|---|
| 1–3 | all | — |
| 4 | 10–60% | 70% → 64.1% (`removal_order_exhausted`) |
| 5 | 10–50% | 60, 70% → about 57% |
| 6 | 10–40% | 50–70% → about 49% |
| 7 | 10–40% | 50–70% → about 43% |
| 8 | 10–30% | 40–70% → about 39% |
| 9 | 10–30% | 40–70% → about 35% |
| 10 | 10–20%; 30% for 1 of 3 seeds | 40–70% → about 30.5% |
| 11–15 | 10–20% | 30–70% → 27% (n = 11) down to 21% (n = 15) |

- Every missed target except those at n = 4 is `generation_budget_exhausted` (600 s).
- The sparsity reached depends on n, not on the target.
- Why carving is so expensive: [population_b_cost.md](population_b_cost.md).
- **Independent re-verification:** 154 of the 160 were confirmed by the second solver. The other 6
  (n = 7 at 40%, n = 9 at 30%, n = 10 at 30%) are `inconclusive`: DLX proved uniqueness, but
  MRV could not repeat the proof within 600 s. They remain in the dataset, labelled as such.
  They are also early evidence that MRV can be far slower than DLX on puzzles near the
  uniqueness boundary.

## 3. Pilot (60 s budget, seed 1, concurrency 4)

The pilot ran **889 jobs in 47 min wall time (3.1 process-hours)**. The dry-run had
estimated a worst case of 51.5 CPU-hours.

| Status | Jobs |
|---|---|
| `completed` | 706 |
| `timeout` | 181 |
| `pruned_by_prefix` | 140 (not runs; excluded from all statistics) |
| `memory_rejected` | 2: DLX on A n = 15 at 70% (estimate 273.6 MB, over the 256 MiB budget) |

**First-solution runs completed within 60 s:**

| | naive | MRV | DLX |
|---|---|---|---|
| A (100 cells) | 42 | 52 | 58 (+1 memory-rejected) |
| B (53 cells reached) | 42 | 52 | **53** |
| AS (14) | 13 | 14 | 14 |

**Outcome grid for first-solution runs on population A, seed 1.** Each solver's block is the
sparsity targets 10, 20, …, 70%. Symbols:

| Symbol | Meaning |
|---|---|
| `a` | under 1 ms |
| `b` | under 0.1 s |
| `c` | under 10 s |
| `d` | under 60 s |
| `T` | timeout |
| `M` | memory rejected |
| `·` | cell does not exist (n = 1) |

```
 n   naive     | mrv       | dlx
 1   a···a··   | a···a··   | a···a··
 4   aaaabTd   | aaaaadc   | aaaaaaa
 5   aaabTTT   | aaaaTTc   | aaaabbb
 6   aaaTTTT   | aaabTTT   | aaaaTTT
 7   aaTTTTT   | aaaTTTT   | aabbTTT
 8   aaTTTTT   | aabTTTT   | abbTTTT
10   acTTTTT   | aaTTTTT   | bbbTTTT
12   aTTTTTT   | abTTTTT   | bbTTTTT
15   bTTTTTT   | abTTTTT   | bcTTTTM
```

(n = 2, 3 are all `a`. n = 9, 11, 13 and 14 follow their neighbours.)

**Exploratory findings.** These rest on one fixture per cell and are not conclusions yet.
- On population A, **finding even the first solution times out for every solver at moderate
  sparsity on larger boards**:
  - from 40% empty at n = 6–8;
  - from 30% at n ≥ 9 for MRV;
  - from 30% at n ≥ 12 for DLX.
- Naive backtracking fails first: at n ≥ 11 it completes only the 10% cells.
- DLX completes more cells than MRV at n = 4–8. MRV is faster on easy large boards (`a` vs `b`):
  DLX's matrix construction costs milliseconds even when the search is trivial.
- On population B (low sparsity at large n), **DLX solves every reached cell within 10 s**
  for both operations. MRV misses one (the uniqueness run at n = 5, 50%). Naive times out from 20–30% at n ≥ 8.
- The ablation shows a possible grid-family bias. At n = 4, MRV is about 12× faster and naive
  about 3× faster on solver-filled (AS) grids than on pattern (A) grids, measured as the geometric mean over 14 cells.
  At n = 3, and for DLX at both sizes, the two grid sources agree (ratios of 0.83 to 1.14). This is
  one seed per cell, so it is flagged rather than concluded; see decision 4.

## 4. Parallel slowdown (calibration)

The calibration reran 29 pilot jobs that took 0.1–60 s (first-solution and uniqueness runs), first serially and then 4 at a time.
All 58 completed.

| | Parallel / serial time |
|---|---|
| All 29 pairs | geometric mean **1.14**, median 1.10, range 1.03–1.47 |
| naive (11) | 1.20 |
| MRV (8) | 1.18 |
| DLX (10) | **1.05** |

The pilot's own times, measured at concurrency 4, are 1.14× the serial times on the same jobs.

**Consequence: borderline rule s = 1.5.** This is above the worst pair observed. A parallel run
that timed out, or that completed in more than 600 / 1.5 = 400 s, is rerun serially before any
completion or latency claim is made about it.

Deviation: the protocol asked for about 30 pilot jobs of 1–60 s. Only 15 pilot runs fell in that range, because
pilot times are bimodal: milliseconds or a timeout. The window was widened to 0.1–60 s and now includes uniqueness runs
(new option `--calibration-min-s`).

## 5. Core campaign (600 s budget, seeds 1–3)

**Jobs.** There are 1,380 (fixture, solver) groups and **2,841 planned jobs**: first-solution runs,
uniqueness runs, and enumeration for A at n ≤ 2. They cover 300 A and 160 B fixtures. Of the
462 (cell, solver) rows, 459 have 3 fixtures each; the B cell n = 10 at 30% has 1 fixture for each of
its 3 solver rows. Missed B targets are `not_run (target_not_achieved)`.

| Queue | Groups | Jobs | Rule |
|---|---|---|---|
| serial (concurrency 1) | 885 | 1,851 | every pilot op of this cell and solver completed in under 10 s |
| parallel (4 workers) | 495 | 990 | everything else: 159 (cell, solver) rows whose pilot first-solution run timed out, 4 that completed only after 10 s, 1 memory-rejected row and 3 B rows without a pilot |

**Cost**, from `run_campaign.py --tier core --dry-run`:

| Queue | Worst case (every job runs to its limit) | If every first-solution run times out (uniqueness pruned) | Expected from the pilot |
|---|---|---|---|
| serial | 370 CPU-h | 177 CPU-h | **0.6 CPU-h** |
| parallel | 198 CPU-h | 99 CPU-h | **80.7 CPU-h** |
| **Total** | 568 CPU-h | 276 CPU-h | **about 81 CPU-h, about 21 h wall** |

The expected cost assumes that every job that timed out at 60 s in the pilot also uses the
full 600 s. That accounts for almost all of it: about 480 first-solution runs × 600 s on 4
workers. Any of those that now finish within 600 s make the campaign shorter.

## 6. Deep sampling (protocol §7)

These are the cells where **every solver and operation completed in the pilot**. From the pilot times:

| Rule | Population A | Population B |
|---|---|---|
| all runs under 0.1 s → 100 fixtures | 40 cells (all n ≤ 3; n = 4 up to 50%; n = 5 up to 40%; n = 6 up to 30%; n = 7–9 up to 20%; n ≥ 10 at 10%) | 39 cells (the same pattern, except n = 5 only up to 30%) |
| all runs under 10 s → 20 fixtures | 1 (n = 10 at 20%) | 2 (n = 5 at 40%, n = 7 at 30%) |

The list is saved in `benchmarks/results/pilot/deep_cells.json`.

| Item | Amount |
|---|---|
| Extra fixtures (seeds 4–20 or 4–100) | 7,714: A 3,897 and B 3,817 |
| Generating them | A takes milliseconds; B's low-sparsity cells carve in milliseconds to seconds, under 0.1 CPU-h in total |
| Running them (serial, 6–7 jobs per fixture, each fast job batched in at least 15 batches of at least 50 ms) | **about 8–12 CPU-h**: about 46,000 short processes at roughly 1 s each |

**This exceeds the 5 CPU-h cap in the plan.** The batching rule sets the cost, because every
job takes about 1 s however fast the solver is. Options are listed in decision 2.

## 7. Prerequisites still open

- **pandas, matplotlib and numpy for Phase 10C (graphs).** You need to install them yourself.
  pip failed SSL certificate verification earlier, and I will not bypass that check:

  ```bash
  python -m pip install pandas matplotlib numpy
  ```

## 8. Decisions needed

1. **Start the core campaign** as listed above: about 81 CPU-h, about 21 h wall, resumable. You can set
   a CPU cap with `--cap-cpu-hours`; jobs left unstarted are recorded as `not_run (budget cap)`, and the
   seed-major order keeps whatever is done spread evenly across cells.
2. **Deep sampling** (choose one):
   - (a) keep the 5 CPU-h cap and let round-robin ordering cover what it can;
   - (b) approve about 8–12 CPU-h for the full deep set;
   - (c) reduce the batches for deep runs from 15 to 5, about 4 CPU-h, at the cost of wider per-fixture confidence intervals.
3. **Population B coverage gap** (above about 20% empty at n ≥ 11): accept it and report it as a
   finding (my recommendation for this study), or approve work on a faster carver later.
4. **Grid-family bias:** run the AS ablation with 3 seeds in core, about 84 cheap jobs,
   to check the n = 4 effect.

## 9. Revision: approved core settings

These were decided at review, after the proposal above.

| Setting | Proposal | Approved |
|---|---|---|
| Core budget | 600 s | **180 s** for every core job, one censoring time for all cells |
| Deep sampling | 20 or 100 extra fixtures in the fast cells | **not run for now.** It can be added after 10C if a figure needs it. |
| Parallel workers | 4, unpinned | **6, each pinned to its own physical core** (logical CPUs 0, 2, 4, 6, 8, 10). Serial jobs are pinned to CPU 2. |
| Borderline serial rerun | timed out, or completed after 400 s | timed out in a cell where another seed or solver completed, or completed after 180 / 1.25 = **144 s**. At most 40 reruns. |

**Why 180 s.** Completed pilot runs thin out decade by decade above 0.1 s:

| Completion time | Runs |
|---|---|
| 0.1–1 s | 14 |
| 1–10 s | 9 |
| 10–60 s | 6 |

So going from 60 s to 600 s would probably turn only about 1–2% of the roughly 480
timed-out runs into completions. 180 s keeps a margin of 3× over the pilot at about 30% of
the cost. The 1800 s stress tier still covers the slow end for shortlisted cases.

**Why 6 pinned workers.** Calibration repeated the same 29 jobs, serially and then at each
worker count, all pinned:

| Workers | Parallel / serial: geometric mean | Median | Worst pair | naive | MRV | DLX |
|---|---|---|---|---|---|---|
| 4 | 1.045 | 1.056 | 1.16 | 1.02 | 1.06 | 1.06 |
| 5 | 1.097 | 1.066 | 1.27 | 1.12 | 1.13 | 1.05 |
| **6** | **1.060** | 1.088 | **1.15** | 1.00 | 1.09 | 1.11 |
| 8 (adds hyperthreads) | 1.285 | 1.106 | **2.07** | 1.43 | 1.36 | 1.09 |

- Pinning alone brought the 4-worker slowdown down from 1.14 (unpinned) to 1.045.
- The 5-worker figure being above the 6-worker one is measurement noise. Serial
  run-to-run variation is 1.3% at the median, though one short job varied by up to 35%.
- Above 6 workers, two jobs share a core and the slowdown doubles for some jobs. **6 is the
  healthy maximum on this CPU.**
- Pinned serial times match unpinned serial times (ratio 0.994), so serial timing is unaffected.

**Cost**, from the dry-run:

| Queue | Worst case | If every first-solution run times out | Expected |
|---|---|---|---|
| parallel | 82.5 CPU-h | 41.2 CPU-h | 24.3 CPU-h |
| serial | 154 CPU-h | 73.8 CPU-h | 0.6 CPU-h |
| **Expected wall time** | | | **about 4.7 h** |

**What is given up:**
- whether any run finishes between 180 s and 600 s;
- per-cell distribution figures and the fixed-cell "runtime vs candidates" figure, which
  needed deep sampling and would be listed as "not produced: insufficient data";
- p95 values.

Each cell still gets 3 independent seeds.

**Deferred:** benchmarking the human-style `LogicalSolver` alongside the three solvers. It
would need a new status, `stalled`, and a deadline added to the solver. It will be
revisited after 10C.

## 10. Core campaign outcome and serial confirmation

The core campaign ran from 2026-11-05 14:04 to 19:04: 5 h wall, 24.9 process-hours,
2,362 jobs executed. The other 479 were pruned. Every completed run passed independent
validation. There were no errors and no watchdog kills.

| Solver | Completed | Timeout | Pruned | Memory rejected |
|---|---:|---:|---:|---:|
| naive | 532 | 208 | 207 | 0 |
| mrv | 648 | 150 | 149 | 0 |
| dlx | 695 | 123 | 123 | 6 |
| **Total** | **1,875** | **481** | **479** | **6** |

The 6 memory rejections are DLX on `A-n15-e035438`, seeds 1–3, both ops: 70% empty at
n = 15, under the 256 MiB budget.

**Borderline candidates.** The longest parallel completion took 121.9 s, so no run completed
after 144 s. Of the parallel timeouts, 99 fell in a cell where another seed or another solver
completed. This is more than the cap of 40, so the selection was made in two parts
(`scripts/select_borderline.py`):
1. All 10 timeouts where the **same solver** completed on another seed of the same cell.
   These are the runs most likely to complete if rerun serially.
2. 30 of the remaining 89 (another solver completed), sampled with a fixed seed (7).

The job list is [serial_confirm_jobs.csv](serial_confirm_jobs.csv), and the results go to
`results/serial_confirm/`. The serial outcome decides completion. Both outcomes are kept.
The 59 unsampled candidates keep their parallel outcome, and the analysis labels them as
unconfirmed.

**Serial confirmation result.** All 40 reruns timed out at 180 s, so every parallel outcome
stands. That is 2.0 process-hours, 2026-11-05 19:04–21:05.
