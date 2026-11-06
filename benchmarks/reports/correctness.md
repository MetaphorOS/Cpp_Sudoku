# Phase 10A — correctness verification report

**Scope:** `NaiveBacktrackingSolver`, `MRVBacktrackingSolver` and `DLXSolver`, as implemented in Step 9.
**Date:** 2026-11-04. **Machine:** AMD Ryzen 5 7600X, Windows 11, MSYS2 GCC 14.2.

## Verdict

**The correctness gate passed.** Every check passes in four build configurations. No
failures or unexplained inconclusive cases remain.

All inconclusive results are cases where **naive backtracking** hit a node budget on
16×16 or 25×25 uniquely solvable puzzles. They are listed in
[Inconclusive cases](#inconclusive-cases); they are expected, and are a first sign of the
performance gap Step 10 will measure.

## Build configurations

| Build | Directory | Configuration | Unit tests | Property tests |
|---|---|---|---|---|
| Release | `build-ninja/` | `-O3`, the normal build | 95/95 (2.7 s) | 15/15 (21 s) |
| DLX validate | `build-validate/` | Debug, `-DSUDOKU_DLX_VALIDATE=ON`, `-D_GLIBCXX_ASSERTIONS` | 95/95 (19.5 s) | 15/15 (282 s) |
| UBSan | `build-ubsan/` | RelWithDebInfo, `-fsanitize=undefined -fsanitize-trap=undefined` | 95/95 (4.4 s) | 15/15 (50 s) |
| CTest (Release) | `build-ninja/` | unit + CLI + property | 5/5 | — |

**DLX validate build.** It checks the dancing-links structure at run time:
- at every search node, for matrices with at most 20,000 nodes;
- once after construction, for every matrix;
- after every exhaustive search, by comparing every link and column size with the
  post-construction snapshot.

A violation throws `std::logic_error`, which fails the test. The unit test
"DLX exhaustive searches leave the linked structure exactly as built" confirms in its trace
that validation was compiled in (`validation: enabled (SUDOKU_DLX_VALIDATE)`).
`_GLIBCXX_ASSERTIONS` adds bounds checks to every `std::vector` access.

**UBSan.** The trap mode was confirmed to work on this toolchain before it was relied on. A
deliberate signed-integer overflow compiled with the same flags stops with an illegal
instruction (exit 132), so "clean" here means no undefined behaviour was detected.

**ASan was not run.** MinGW GCC does not support it. It would be possible under WSL if
wanted.

## What was checked

### Unit tests: new "Solvers" category (14 tests, each run against all three solvers)

| Check | Inputs | Expectation |
|---|---|---|
| Known unique solutions | classic 9×9, a well-known hard 9×9, the recorded 4×4 (seed 5), a generated 16×16 (seed 39) | exactly 1 solution, `Exhausted`, not aborted; equal to the known solution, or for the hard 9×9, all three solvers agree; independently validated |
| Exact counts | empty 4×4; 4×4 with one given; the classic puzzle minus two clues | 288; 72; limit 2 → 2 with `SolutionLimit` |
| Valid but unsatisfiable puzzles | `12........3...4.`; the classic puzzle with one cell set to a wrong but non-conflicting candidate | 0 solutions, `Exhausted`, not aborted |
| Conflicting givens | two 5s in one row | `InvalidGivens`, 0 nodes |
| Completed boards | constructive grids, n = 1–4 | 1 solution, 0 nodes, `Exhausted` at limit 2, `SolutionLimit` at limit 1 |
| Empty boards | n = 1–4 | an independently valid grid |
| Solution-limit semantics | empty 4×4 with limits 1, 2, 0; a unique puzzle with limit 2 | `SolutionLimit`, `SolutionLimit`, `Exhausted` (288), `Exhausted` (1) |
| Node limits | exhaustive count of an empty 9×9, limits 1, 10, 100, 1,000, 10,000 | `NodeLimit`, `aborted`, `nodes == limit`, counts never decrease, any solution valid |
| Time limit | exhaustive count of an empty 9×9, 30 ms | `TimeLimit`, `aborted`, stops well under 2 s |
| Repeated invocation | hard 9×9, then an interrupted run, then the hard 9×9 again | identical count, nodes, backtracks and solution |
| RNG reproducibility | empty 4×4 and 9×9 with seeds 11, 11, 12 | the same seed gives the same grid and node count; different seeds give different grids; all valid |
| Prefix property | 6 puzzles (unique, multi-solution, empty, blanked, 16×16) | see below |
| DLX memory budget | classic 9×9, empty 4×4, complete 9×9, conflicting 4×4 | see below |
| DLX link restoration | exhaustive searches on 24 puzzles | counts match MRV; in the validate build, all invariants hold |

**Prefix property.** This property underpins the benchmark's `pruned_by_prefix` rule. For every
puzzle and solver:
- the limit-1 and limit-2 runs return the same first solution;
- the limit-2 run's nodes and backtracks are at least the limit-1 run's;
- under a node limit of half the limit-1 run's nodes, both runs stop at the same node with
  the same count.

**DLX memory budget.**
- `estimate()` gives: 51 empty cells, 204 columns, and `nodes = 1 + columns + 4 · placements`.
- A solver with budget exactly `estimate().bytes` solves the puzzle. With one byte less, it
  throws `std::length_error`, and the same instance then still solves a smaller puzzle.
- An empty 4×4 needs 64 placements, 64 columns and 321 nodes. A complete grid needs 0
  placements. Conflicting givens give no estimate.

**CLI test.**
- `--solver naive|mrv|dlx` works for `solve`, `count` and `generate`, and `--stats` names
  the right solver.
- Naive traces say "fixed row-major order"; DLX traces "choose cell r1c1".
- `--solver bogus` exits 2.
- A blank 144×144 grid with `--solver dlx` exits 3 with a memory-budget message.

### Property tests: three new cross-solver properties

| Property | Cases | Result |
|---|---|---|
| Each solver enumerates **exactly the oracle's set of solutions** on random 4×4 puzzles, conflicting givens included. Solutions are collected through observer `Solution` events and compared, as sets, with the brute-force list of all 288 valid 4×4 grids that agree with the givens. Duplicates are also checked. | 2,000 puzzles × 3 solvers | all pass |
| **Capped solution counts agree** across solvers on random 9×9 puzzles, 10–70% empty. Grids come from randomized solver fills, a different family from the generator's pattern grids. Counts are capped at 1,000. Each returned solution is independently validated. | 600 puzzles; 1,800 solver results | all agree; 0 inconclusive |
| **Uniqueness verdicts agree** on 16×16 and 25×25 puzzles: half are generated (must be unique), half are 35%-blanked grids. Node limit 2,000,000. | 40 puzzles at 16×16, 10 at 25×25 | all conclusive verdicts agree; see below |

The existing properties are unchanged and still pass: transforms, generation, the MRV oracle
count, and contradiction detection.

**Independent validation.** Every new test checks returned solutions with
`solutionProblem()` in `tests/sudoku_test_helpers.hpp`. This checker tests completeness,
permutations of each row, column and box, and kept givens directly. It does **not** use
`Constraints` or `Board::isValid`, the code the solvers themselves rely on.

## Inconclusive cases

| Property | Solver | Inconclusive | Of | Reason |
|---|---|---:|---:|---|
| Uniqueness verdicts, 16×16 | `backtracking-naive` | 20 | 40 | Node limit (2 M) on every generated, uniquely solvable puzzle |
| Uniqueness verdicts, 25×25 | `backtracking-naive` | 5 | 10 | Same: every generated puzzle |
| Capped 9×9 counts | any | 0 | 1,800 | — |

MRV and DLX were conclusive on every case.

An inconclusive run is never counted as a pass. It only means that solver's verdict was not
compared for that case.

## Observation (not a benchmark result)

While choosing fixtures for the unit tests, naive backtracking proved uniqueness of only 3 of
40 generated 16×16 puzzles (seeds 14, 29, 39) within 2 s. Seed 3 needed more than 20 s and
1.27 billion nodes. MRV and DLX take milliseconds on the same puzzles.

The unit tests therefore use seed 39, which naive handles in about 0.1 s. This is an informal
observation from a probe program. The benchmark campaign (10B/10C) will measure this
properly.

## Library changes made in 10A

- `DLXSolver::estimate(const Board&) -> std::optional<DlxEstimate>`: the previously internal
  size and memory estimate, now public. It is needed for the exact budget-boundary test, and
  later for the memory figure.
- `SUDOKU_DLX_VALIDATE` CMake option, and `DLXSolver::kValidates`: run-time invariant checks,
  off by default and absent from normal builds.
- No change to `Solver`, `SolveOptions`, `SolveResult`, or any solver's search behaviour.

## Limitations

- Per-node structural checks run only on matrices of at most 20,000 nodes (4×4, and sparse
  9×9 puzzles). Larger matrices are checked after construction and after exhaustive searches.
- The prefix property is tested on 6 puzzles × 3 solvers, plus one node-limit cut each. It is
  a structural property of the deterministic search, and the pilot will also validate it
  empirically at benchmark scale (about 20 pairs).
- ASan was not run: it is unavailable on MinGW.
