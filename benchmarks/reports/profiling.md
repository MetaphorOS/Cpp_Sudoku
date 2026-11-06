# Phase 10D — profiling

*This explains the 10C findings ([analysis.md](analysis.md)) using operation counts and
sampling profiles. Each claim cites its data. No solver code was changed: optimisation
candidates are listed but not applied.*

## Method

| Tool | What it gives | Data |
|---|---|---|
| **Counters build** (`build-counters/`, `-DSUDOKU_SOLVER_COUNTERS=ON`) | Exact operation counts per run: columns inspected, link updates, covers, cells scanned, candidate computations, placements | All 1,875 core completions replayed (`results/profile_counters/`). Node counts matched the Release core runs in **every** case, so the counters build follows exactly the same search |
| **Cost model** (`scripts/profile_counters.py`) | Counts from the counters build, divided into the **serial Release** core times | `results/profile_counters/derived.csv` |
| **DLX cache test** | Throughput on hard DLX searches whose matrices range from 0.1 MB to 174 MB (L2 = 1 MB per core, L3 = 32 MB). Population A, seed 1, 10 s each, serial, counters build | `results/profile_cache/runs.csv` (43 runs) |
| **gprof, line level** (`build-gprof/`: `-O3 -pg -g -fno-pie`, linked `-no-pie`) | Where samples land, in the optimised code | `results/profile_gprof/*/lines.txt`: 5 jobs |

**Notes on the tools:**
- MinGW gprof recorded **no samples** until the build used non-PIE code and linking. With
  that fixed, everything at -O3 is inlined into `run()`, so only the line-level profile
  (`gprof -l`) is informative.
- Samples labelled `vector::operator[]` were mapped back to their surrounding loop through
  their addresses (`objdump -l`).
- Absolute times from the counters build and the gprof build are inflated, by the counter
  increments and by `-pg` respectively. They are used only for proportions and trends.

## H1. DLX's fixed cost per node is a linear scan over all open columns — **confirmed**

**Claim:** on easy boards, DLX is 1–2 orders of magnitude slower than MRV (analysis.md §1)
because `chooseColumn()` inspects every open column at every node.

**Evidence for:**
- **Counters:** on population A at 10% empty, DLX inspects exactly
  `2 × empty cells` columns per node at every n. That is the average number of open columns,
  since columns start at 4 × the empty cells and close as cells fill:

  | n | empty cells | columns inspected per node | link updates per node | ns per node (Release) |
  |---:|---:|---:|---:|---:|
  | 3 | 8 | 18 | 3.0 | 215 |
  | 6 | 130 | 262 | 3.4 | 474 |
  | 9 | 656 | 1,314 | 3.7 | 1,978 |
  | 12 | 2,074 | 4,150 | 4.0 | 5,499 |
  | 15 | 5,063 | 10,128 | 4.4 | 13,213 |

  Link work per node is constant at about 3–4 updates. Only the scan grows.
- **Cost model:** least squares over 414 serial DLX completions gives **1.31 ns per column
  inspected**. On that model the scan is **77–94%** of DLX time in every
  (population, sparsity) group.
- **gprof:** the `chooseColumn` loop (dlx_solver.cpp:325–329) accounts for **about 90%** of
  samples on a hard search (A-n08-e002458, 10 s) and **about 97%** on an easy n = 15 board.
  `cover()` and `uncover()` together take about 3%.
- **The equal-tree case isolates per-node cost.** On A-n15-e010125 (20% empty), DLX and MRV
  both search **exactly 10,125 nodes** (no backtracking), yet DLX takes 270 ms and MRV
  74 ms.

**Evidence against:** none found.

**Why the scan has no early exit:** it stops only on a column of size 0. Any column of size
1 (a forced placement) is as good as it gets, but the loop keeps scanning.

## H2. DLX wins hard puzzles through a smaller search tree, not cheaper nodes — **confirmed**

**Claim:** DLX's completions and speedups in the hard region come from exploring a smaller
tree.

**Evidence for:**
- On every fixture where DLX beat MRV (first op, serial, both completed):
  - MRV's tree was **10× to 67,000× larger** (median node ratio among these 9 fixtures: 25×);
  - each DLX node cost **6–9× more**.
- The speedup is the product of the two: for example A-n04-e000179-s001 is 67,138× fewer
  nodes ÷ 8× the per-node cost ≈ 8,400× faster.
- Over all paired fixtures the median node ratio is **1.0**: most completed fixtures need no
  backtracking at all, so DLX's per-node cost decides and it loses.
- **Mechanism:** DLX branches on the most constrained of **all four constraint kinds**
  (cell, row-value, column-value, box-value), so it sees "this value has only one place left
  in this row/column/box" (hidden singles). MRV branches only on cells (cell-value counts).
  This is a known property of the exact-cover formulation; the counters show its effect
  (tree size) without measuring it directly.

**Evidence against:** none. This is a mechanism argument plus tree-size measurements, not an
ablation: no MRV variant with hidden singles was built.

## H3. DLX slows down once its matrix leaves the cache — **rejected** (for this range)

**Claim:** the cost per link update rises when the matrix outgrows L2 (1 MB) and then L3
(32 MB).

**Evidence against:** per-column and per-link costs, fitted within each matrix-size band,
are flat:

| Matrix size | Runs | ns per column inspected | ns per link update |
|---|---:|---:|---:|
| < 1 MB (L2) | 3 | 1.49 | 2.9 |
| 1–8 MB | 14 | 1.50 | 3.3 |
| 8–32 MB (L3) | 9 | 1.50 | 4.2 |
| > 32 MB (DRAM), up to 174 MB | 12 | 1.51 | 3.3 |

The joint fit over all sizes gives a size term of +0.005 ns per column per doubling of matrix
size, which is effectively zero.

**Interpretation:**
- The scan walks the header list: `right_` and `size_` for each column, about 8 bytes per
  open column, which is under 1 MB even at n = 15.
- `cover()` touches only rows in the chosen column's neighbourhood.
- The hot working set therefore stays in cache while the full matrix does not.

**Caveat:** this was measured in the counters build, where costs read about 15% higher than
Release (1.50 against 1.31 ns per column). The trend is what matters.

## H4. MRV's selection cost is cheap when forced cells exist, and a full scan when not — **supported**

**Evidence:**
- **Easy boards:** MRV scans 1–11 cells per node (10% empty), because the scan stops at the
  first cell with one candidate. Its cost per node stays at 43–180 ns up to n = 15. This is
  why MRV beats DLX on easy boards.
- **Hard boards:** where no forced cell exists, the scan covers every open cell.
  - gprof on A-n08-e002458 (hard, timeout) puts **about 97%** of samples in the selection
    scan (`candidateCount`, `box`, popcount, in `select`).
  - The core timeouts show about 3.6 µs per node for MRV there (5.0 × 10⁷ nodes in 180 s),
    against about 15 ns for naive (1.2 × 10¹⁰ nodes).
- MRV therefore pays the same kind of linear scan as DLX, but only on hard boards. There it
  is outweighed by its smaller tree compared with naive.

**Evidence against:** no counters exist for MRV timeouts (the counters pass replayed
completions only), so "full scan on hard boards" rests on gprof plus the per-node time.

## H5. Naive's cost is bookkeeping, and its weakness is tree size — **supported**

- Naive's cost per node falls slightly with n (89 ns → 24 ns at 10% empty). Its samples are
  spread over `place`, `box`, `setAt` and `candidateCount`, with no hot loop.
- Every one of its losses comes from tree size: it fails to complete in 59 fixtures that MRV
  finishes (analysis.md §2).

## Optimisation candidates (documented only, not applied)

| # | Candidate | Expected effect | Basis |
|---|---|---|---|
| 1 | DLX: return early from `chooseColumn()` on a size-1 column | Removes most of the scan on easy boards, where forced columns are common. It picks the same column as today, unless a size-0 column (a dead end) lies further down the list. The full scan detects that dead end at once, while the early exit branches first and finds it one level deeper, so trees can grow slightly | H1. Needs node counts compared before and after |
| 2 | DLX: keep open columns bucketed by size, giving O(1) selection of the minimum | Removes the scan entirely; adds upkeep to `cover()`/`uncover()` | H1, H3: link updates are cheap and cache-resident |
| 3 | MRV: track candidate counts incrementally, or keep a priority structure of cells | Removes the full scan on hard boards | H4 |
| 4 | MRV: add hidden-single detection (value-in-unit counts) | Smaller trees, closer to DLX on hard boards | H2 |
| 5 | Adaptive selection between solvers | Only worthwhile if 1–4 leave a gap. Candidate 1 alone may remove DLX's easy-board penalty | analysis.md; H1/H2 |

## Reproduce

```
cmake -S . -B build-counters -G Ninja -DCMAKE_BUILD_TYPE=Release -DSUDOKU_SOLVER_COUNTERS=ON -DSUDOKU_BUILD_TESTS=OFF
cmake --build build-counters --target sudoku_bench
python benchmarks/scripts/run_campaign.py --tier custom --name profile_counters --jobs-file benchmarks/reports/profile_counters_jobs.csv --bench build-counters/sudoku_bench.exe --timing single --workers 6
python benchmarks/scripts/run_campaign.py --tier custom --name profile_cache --jobs-file benchmarks/reports/profile_cache_jobs.csv --bench build-counters/sudoku_bench.exe --timing single --workers 1
python benchmarks/scripts/profile_counters.py
cmake -S . -B build-gprof -G Ninja -DCMAKE_BUILD_TYPE=Release -DSUDOKU_BUILD_TESTS=OFF -DCMAKE_CXX_FLAGS="-pg -g -fno-omit-frame-pointer -fno-pie" -DCMAKE_EXE_LINKER_FLAGS="-pg -no-pie"
```
For gprof: run `build-gprof/sudoku_bench.exe run ... --time-limit-s 10` in an empty
directory, then `gprof -b -l -p build-gprof/sudoku_bench.exe gmon.out`.
