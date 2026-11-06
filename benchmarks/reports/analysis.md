# Phase 10C — preliminary analysis (core campaign, 180 s)

*Preliminary: written for the 10C review. It covers the core tier only: 3 fixtures per cell,
budget T = 180 s, no deep sampling. Every number below comes from
`results/core/summary/*.csv` or the figures in `figures/core/`; [figures.md](../figures/core/figures.md)
lists what was drawn and what was not. 10D (profiling, stress) has not started, so
explanations of **why** something happens are labelled as hypotheses.*

## Data

| Item | Value |
|---|---|
| Jobs | 2,841: 1,875 completed, 481 timeout, 6 memory-rejected, 479 pruned (not observations) |
| Validation | Every completion matched the independent checker; no errors and no watchdog kills |
| Serial confirmation | 40 of 99 borderline parallel timeouts rerun alone; **all 40 timed out again** (schedule.md §10) |
| Timing | 1,837 of the 1,875 completions were timed serially (concurrency 1); the other 38 come from the parallel queue and are marked ∥ in Figure 3 |
| Sampling level | k = 3 per cell ("sampled"): median, IQR, maximum and completion counts only, no p95, wide intervals |
| Reproducibility | Deleting and regenerating `figures/core/` gives byte-identical files |

Population B exists only where carving reached the target ([population_b_cost.md](population_b_cost.md)):
every target at n ≤ 3, falling to 20% at n ≥ 11. Cells without a fixture are shown as
"not gen." and are **not** timeouts.

## Findings

### 1. Easy puzzles: MRV is fastest, naive is close, DLX pays a large fixed cost

At 10–20% empty, all three solvers finish everything, and the order is consistent
([Figure 1](../figures/core/fig01_runtime_vs_n.png), top two rows;
[Figure 4](../figures/core/fig04_speedups_A.png), left two columns):
- **MRV vs naive:** about even. The median paired ratio at 10% is 0.97× on A and 0.96× on B,
  with a range of 0.84–33×. MRV pulls ahead only where naive starts to struggle, for example
  at n = 15 (33×) and at 20% from n ≥ 7.
- **DLX vs MRV:** DLX is **slower** at every cell of 10% empty, by a median of 28× on A and B
  (ratios 0.01–0.54). At n = 13–15 it is about 70–90× slower: 67 ms against 0.9 ms at
  n = 15, A, 10%.
- Time per search node (A, 10%, first; serial):

  | n | naive | MRV | DLX |
  |---:|---:|---:|---:|
  | 3 | 0.09 µs | 0.09 µs | 0.22 µs |
  | 9 | 0.04 µs | 0.05 µs | 2.0 µs |
  | 15 | 0.02 µs | 0.18 µs | 13 µs |

  DLX's cost per node grows roughly with the board area, while the backtracking solvers'
  stays flat ([time vs nodes](../figures/core/figC4_time_vs_nodes.png)). Setup is only
  about 2% of DLX's time at n = 15 ([Figure 7](../figures/core/fig07_setup_vs_search.png)),
  so matrix construction is not the main cost.
  **Hypothesis for 10D:** the per-node column choice scans every remaining column.

### 2. Hard puzzles: DLX completes the most and never loses a completion to MRV

Where puzzles get hard (the boundary moves to lower sparsity as n grows):
- **Completion, first solution** ([Figure 5](../figures/core/fig05_completion_A.png)):

  | Population | naive | MRV | DLX |
  |---|---:|---:|---:|
  | A | 126/300 | 157/300 | **174/300** |
  | B | 127/160 | 154/160 | **160/160** |

- **DLX vs MRV, paired:** 23 fixtures where only DLX finished, and **0** where only MRV
  finished. Where both finished in the hard region, DLX is 4.5× faster (A, n = 6, 40%),
  60× (A, n = 4, 70%) and 2,400× (A, n = 5, 70%).
- **MRV vs naive:** 59 fixtures where only MRV finished, and 1 where only naive finished
  (A, n = 4, 70%). Where both finished, MRV reaches 14,600× (A, n = 10, 20%).
- **Population B is fully solved by DLX:** every generated unique puzzle, both ops, up to
  n = 15. MRV misses 6 first-solution runs (B at n = 7, 40% and n = 9–10, 30%), and naive
  misses 33.

### 3. A hard wall for every solver on population A

On population A, **no solver** finishes at 30% empty or more from n ≥ 12, at 40% or more from
n ≥ 8, or at 50% or more from n ≥ 6, within 180 s
([Figure 3](../figures/core/fig03_runtime_heatmap_A.png)).
- The [fraction-solved curves](../figures/core/figC3_fraction_solved.png) plateau well below
  1 from n = 6. That means the runs that do finish are fast, and the rest do not finish at
  all: there is no long tail approaching T. This matches the pilot's bimodal timings and the
  decision to cut the budget from 600 s.
- The serial confirmation agrees: none of the 40 borderline timeouts finished when run alone.
- Population A and population B behave alike wherever both exist. Completion rates are equal
  in 158 of the 162 shared (cell, solver) pairs; B is higher in 3 and A in 1, each by one or
  two fixtures out of 3. B simply does not exist past the wall, so this campaign cannot say
  whether unique puzzles there would be easier or harder.

### 4. Uniqueness costs about the same as finding the first solution here

Where the uniqueness op was run, its median is close to the first-solution median for every
solver ([Figure 2](../figures/core/fig02_runtime_vs_sparsity_unique.png) against
[the first-op version](../figures/core/fig02_runtime_vs_sparsity_first.png)):
- at 10% empty, the ratio of uniqueness median to first-solution median across 90
  (population, n, solver) cells is 1.05 at the median, 1.01–1.11 for the IQR, and 3.2 at
  most;
- completion is 98–100% among executed runs.

This is partly selection, not a property of the solvers: uniqueness runs were pruned
wherever the first-solution run timed out (479 runs), so only fixtures that were already
easy were measured.

### 5. Memory

DLX's measured peak tracks its analytic estimate from about n = 6 upwards
([Figure 6](../figures/core/fig06_memory_vs_n.png)):
- at 70% empty it is about 175 MiB at n = 14 and over 256 MiB at n = 15, which gives the
  6 `memory_rejected` runs (A, n = 15, 70%, both ops, all seeds);
- at 20% empty it is about 5 MiB at n = 15.

Naive and MRV stay under 2 MiB above baseline at every size. Below n ≈ 5, every measurement
sits at the page-granularity floor of about 40 KiB.

## What this suggests (for the 10D conclusions, not yet conclusions)

- **MRV as the default** is supported for typical use: it is fastest or close on easy and
  medium puzzles at every size, and it is never far behind naive.
- **DLX pays off** only once a puzzle is hard enough for the search tree, not the fixed
  per-node cost, to dominate: small n at high sparsity, or unique puzzles near the
  generation frontier. On easy large boards it is 1–2 orders of magnitude slower.
- That split is a reason to consider adaptive selection later. It is **not** evidence that
  adaptive selection pays off yet: the crossover lies in a narrow band, and it is sampled
  with k = 3.

## Limits of this evidence

- k = 3 per cell. A completion rate of 2/3 has a Wilson 95% interval of 0.21–0.94, and
  medians rest on 1–3 values.
- Anything that would finish between 180 s and 600 s is invisible. The serial confirmation
  and the fraction-solved plateaus suggest it is rare, but neither proves it.
- Population B is missing exactly in the high-sparsity, large-n region where the solvers
  differ most.
- 59 borderline timeouts were not confirmed serially. They keep their parallel outcome
  (calibrated slowdown 1.06× mean, 1.15× worst).
- Not produced (k < 20 per cell): per-cell runtime distributions, and runtime vs initial
  candidate count.
- Node counts are not comparable across engines.

## Figures

| # | Figure |
|---|---|
| 1 | [Runtime vs n](../figures/core/fig01_runtime_vs_n.png) |
| 2 | Runtime vs sparsity: [first](../figures/core/fig02_runtime_vs_sparsity_first.png), [unique](../figures/core/fig02_runtime_vs_sparsity_unique.png) |
| 3 | Runtime heatmaps: [A](../figures/core/fig03_runtime_heatmap_A.png), [B](../figures/core/fig03_runtime_heatmap_B.png) |
| 4 | Paired speedups: [A](../figures/core/fig04_speedups_A.png), [B](../figures/core/fig04_speedups_B.png) |
| 5 | Completion rates: [A](../figures/core/fig05_completion_A.png), [B](../figures/core/fig05_completion_B.png) |
| 6 | [Memory vs n](../figures/core/fig06_memory_vs_n.png) |
| 7 | [Setup vs search](../figures/core/fig07_setup_vs_search.png) |
| C1 | Nodes vs sparsity: [first](../figures/core/figC1_nodes_vs_sparsity_first.png), [unique](../figures/core/figC1_nodes_vs_sparsity_unique.png) |
| C3 | [Fraction solved by time t](../figures/core/figC3_fraction_solved.png) |
| C4 | [Time vs nodes](../figures/core/figC4_time_vs_nodes.png) |

The pilot figures (k = 1, exploratory) are in `figures/pilot/`.

## Reproduce

```
python -m pip install -r benchmarks/scripts/requirements.txt
python benchmarks/scripts/summarize.py --tier core
python benchmarks/scripts/plot_figures.py --tier core
```
