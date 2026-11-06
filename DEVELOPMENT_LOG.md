# Development Log

A step-by-step record of how this project was built: what each step asked for, what changed,
the decisions made, and the verified results. A new entry is added after every step.

Entries are in the order the work happened. The Step 10 solver study ran for hours at a time,
so Steps 11 and 12 (difficulty grading and its hardening) were done in between, and Step 10's
entries are interleaved with theirs. Each entry's **Current state** table was true when it was
written; the table below is the current one.

For how to use the project, see [README.md](README.md).

## Current state

| | |
|---|---|
| Build | CMake 4.5 + Ninja 1.12 + MSYS2 GCC 14.2, in `build-cmake/`; earlier builds archived (hidden) in `old_builds/` |
| Unit tests | 131 tests in 12 categories, all passing (~8 s) |
| Property tests | 18 properties, all passing (~30 s) |
| CTest | 7/7 passing; labels `unit`, `property`, `slow` |
| Grid sizes | any box size n ≥ 1 (1×1, 4×4, 9×9 default, … 100×100 tested); n⁴ cells must fit in an `int` |
| Solvers | `auto` (default for solve/count: MRV or DLX per puzzle), `mrv` (default for generate), `naive`, `dlx`, chosen with `--solver`; cross-solver correctness verified (Step 10A) |
| Extra builds | validate (DLX invariant checks + `_GLIBCXX_ASSERTIONS`), UBSan (trap mode), Debug, counters, gprof: archived in `old_builds/`; configure commands in [benchmarks/README.md](benchmarks/README.md) |
| Difficulty | Easy / Medium / Hard / Expert by human techniques (`rate`, `generate --difficulty`), Steps 11–12 |
| Step 10 | Solver study complete: protocol in [benchmarks/README.md](benchmarks/README.md), conclusions in [benchmarks/reports/performance.md](benchmarks/reports/performance.md) |
| Deferred | Benchmarking `LogicalSolver`; deep sampling; the grid-family follow-up; the solver optimisations in performance.md §5 |

Rebuild and run everything:

```bash
cmake -S . -B build-cmake -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build-cmake
ctest --test-dir build-cmake --output-on-failure
```

---

## Step 1: Phase 1 implementation

**Goal.** Build a modular C++20 Sudoku generator and solver for a technical interview take-home. It needed:
- n²×n² grids;
- a backtracking solver with MRV and bitmasks;
- solution counting with a limit;
- seeded generation of unique puzzles;
- a CLI, tests and a README.

**What was built.**
- **Board** ([board.hpp](include/sudoku/board.hpp)): grid storage, parsing, printing and validity.
- **Constraints** ([constraints.hpp](include/sudoku/constraints.hpp)): row, column and box bitmasks. They are shared by the validity check and the solver, so the rule is written once.
- **Solver interface** ([solver.hpp](include/sudoku/solver.hpp)): one `run(board, options)` method with a solution limit, a node limit and an optional random value order. This is the extension point for naive backtracking and Dancing Links later.
- **BacktrackingSolver:** MRV cell choice using `std::popcount`, and candidate iteration using `std::countr_zero`.
- **Generator:** fill an empty grid randomly, then remove clues in random order, keeping a removal only if the puzzle stays unique.
- **Portable randomness** ([random.hpp](include/sudoku/random.hpp)): a hand-written shuffle and bounded random draw, so a seed gives the same puzzle on every compiler.
- **CLI:** `generate`, `solve`, `count` and `show`.
- **Tests:** 20 unit tests in a small built-in harness, registered with CTest.

**Decisions.**
- Each uniqueness check gets a 20 000-node budget. Without it, five 16×16 puzzles did not finish in 280 s; with it, a 16×16 takes about 0.2 s. A check that runs out of budget keeps its clue, so a puzzle can never have two solutions.
- No external test framework, to avoid dependencies.

**Issues.**
- CMake was not installed. Installing it with `pacman` and `pip` failed SSL certificate checks (HTTPS appears to be intercepted on this machine). Certificate checking was not disabled. Instead, everything was compiled and tested directly with g++.
- Three tests failed at first because of a typo in the test data (row 5 of the classic puzzle), not a solver bug.

**Result.** 20/20 tests in 0.13 s. A typical 9×9 solves in about 470 search nodes (0.04 ms).

---

## Step 2: Test suite readability and coverage

**Goal.** Make the tests easy to review in an interview: descriptive names, useful failure messages, and fixes for brittle or wrong assertions.

**What changed.**
- **New harness features:**
  - `CHECK_EQ` prints expected and actual values;
  - `REQUIRE` stops the test;
  - `CONTEXT(...)` attaches the board size, seed or puzzle to failures;
  - `FAIL(...)` gives a custom message;
  - `CHECK_THROWS` also reports a wrong exception type.
- **Tests:** went from 20 to 45.

**Problems fixed in the old tests.**
- `nodes == 100` pinned an internal detail; it became `nodes <= 100`.
- Reproducibility was only checked within one process. Puzzles recorded for fixed seeds now pin the promise that a seed gives the same puzzle on every compiler.
- `'x'` was tested as an unknown character, but it is a valid letter meaning 33.
- One assertion was nonsense and was rewritten.

**New coverage.**
- Exact counts: an empty 4×4 has 288 grids, and 72 with one cell given.
- Puzzles are minimal: removing any single clue gives a second solution.
- A solver passed to the generator is actually used.
- The exact pretty-print format.
- The random helpers.

**Result.** 45/45 in 0.4 s. Failure output was moved to stdout so it stays in order with the PASS/FAIL lines.

---

## Step 3: CMake build verified, old builds removed

**Goal.** Build with CMake once it was installed.

**What changed.**
- **Tools:** CMake 4.5.0-rc2 and Ninja 1.12.1 (from `C:\msys64\mingw64\bin`) were used.
- **Build folder:** the build went into a fresh `build-ninja/` folder, because an existing MinGW Makefiles build could not switch generators.
- **Cleanup:** on request, the old `build/` (direct g++ output) and `build-cmake/` (MinGW Makefiles) folders were deleted.
- **README:** its build steps were updated to Ninja.

**Result.** The build had no warnings and CTest passed 4/4.

---

## Step 4: Readable test runner output

**Goal.** Group the output by category, use plain-English descriptions, show a clear summary with timing, and keep the default output concise with a verbose option.

**What changed.**
- **Test registration:** tests became `TEST("Category", "description")`, in 6 categories: Board, Parsing, Solver, Randomness, Generation and CLI.
- **Runner modes:**
  - concise by default (one line per category, then failures, then a summary);
  - `-v` lists every test with its duration;
  - a text filter;
  - `--list`.
- **Exit codes:** 0 when all pass, 1 on a failure or no match, 2 for a bad option.
- **CLI as a library:** the CLI moved from `main.cpp` into `app/cli.cpp` (the `sudoku_cli` library), with a function that takes streams as parameters. This made in-process CLI tests possible.

**Result.** 48/48 tests (3 new CLI tests) and 4/4 CTest.

---

## Step 5: `--trace` mode for the test runner

**Goal.** Show each test's inputs, expected and actual values, and every individual check, without changing the default or verbose output.

**What changed.**
- **`--trace`:** passing checks are recorded, and a `TRACE(label, ...)` macro adds detail. Its arguments are not even evaluated unless tracing, so normal runs pay nothing.
- **Sudoku helpers for traces:**
  - boards (drawn in full up to 9×9, shortened above that);
  - solver statistics;
  - labelled parse inputs (`checkParseRejects`).
- **`--full`:** removes the shortening and the 40-entries-per-test limit. Failures are never hidden.

**Issues.**
- The first build failed: the helpers were defined after their first use, and a helper named `quoted` clashed with `std::quoted`.
- The CTest pass shown right after that failed build had run the old binary. Both problems were fixed and the tests rerun.

**Result.** 48/48 and CTest 4/4. Default output is unchanged.

---

## Step 6: Property-based tests and algorithm tracing

**Goal.**
- A separate randomized suite checking Sudoku invariants over many seeds.
- Optional tracing of the solver and generator internals, available from the CLI.

### 6a. Property tests

**What changed.**
- **New suite:** [property_tests.cpp](tests/property_tests.cpp), a separate executable with the CTest labels `property` and `slow`.
- **10 properties:**
  - generation validity and uniqueness;
  - reproducibility;
  - Sudoku-preserving transforms (symbol relabelling, row/column swaps within bands and stacks, band/stack swaps, transpose) keep validity, uniqueness and solution counts;
  - duplicated and contradictory values;
  - 4×4 counts compared with an independent brute-force list of all 288 grids;
  - random 9×9 consistency;
  - parse round-trips for every size.
- **Settings:** `SUDOKU_PROPERTY_SCALE` scales the number of cases, and `SUDOKU_PROPERTY_SEED` sets the base seed. Each failure reports the property, grid, case seed, input and a command to reproduce it.
- **Shared helpers** moved to [sudoku_test_helpers.hpp](tests/sudoku_test_helpers.hpp).

**Bugs found.**
1. **36×36 grids could not be written as text.** The `1-9A-Z` alphabet has 35 symbols, so value 36 printed as `[`. The maximum grid became 25×25, with a regression test.
2. **16×16 generation could stall.** About 5% of seeds needed more than 5 million nodes just to fill the empty grid. The fill now restarts with fresh random choices after 10 × cell-count nodes, and the result still depends only on the seed. The property run fell from 213 s to 4 s, and a regression test was added.
   - Side effect: 9×9 and 4×4 puzzles are unchanged, but some 16×16 and 25×25 seeds now give different puzzles than earlier builds did.

### 6b. Algorithm tracing

**What changed.**
- **Observers** ([trace.hpp](include/sudoku/trace.hpp)): `SolverObserver` and `GeneratorObserver` receive plain event structs. `SolveOptions` and `GeneratorOptions` have an `observer` field, `nullptr` by default.
- **Solver events:** start, MRV selection, placement, dead end, backtrack, solution, and finish with the reason it stopped. `SolveResult` gained a `backtracks` count.
- **Generator events:** the filled grid, every removal attempt with its verdict, and the final puzzle and solution.
- **Readable printers** ([text_trace.hpp](include/sudoku/text_trace.hpp)) with an event limit. The summary line is always printed.
- **CLI flags:** `--trace-solver` (on `solve` and `count`), `--trace-generation` and `--trace-limit n`. Traces go to stderr, and stdout is unchanged.
- **File input:** the CLI can now read a puzzle from a file (`sudoku solve puzzle.txt`).

**Decision.** The first version checked for an observer at runtime, and benchmarks showed up to about 10% overhead with tracing off. The search became a template on `bool kTraced`. The untraced version has no tracing code, and the remaining difference is within run-to-run noise (about ±3%).

**Result.**
- 65/65 unit tests, including 10 tracing tests.
- 10/10 property properties in about 4 s, and a 5× run with a different seed also passed (21 s).
- CTest 5/5, no warnings.

---

## Step 7: This development log

**Goal.** A step-by-step record of the work so far, kept up to date after every future step.

**What changed.** Added this file and linked it from the README. No code changed.

---

## Step 8: Arbitrary grid sizes

**Goal.** Support any positive box size n instead of a fixed maximum:
- checked dimension arithmetic;
- scalable candidate bitsets;
- a number format for multi-digit values, compatible with existing inputs;
- constructive completed grids;
- explicit handling of resource and time limits;
- tests at 36×36, 64×64, 81×81 and 100×100;
- existing behaviour, tests, CLI and the 9×9 default all kept.

**What changed.**
- **Board** ([board.hpp](include/sudoku/board.hpp)):
  - Any n ≥ 1 is accepted. The only bound comes from the index type: n⁴ cells must fit in an `int` (n ≤ 215).
  - The check runs before allocating and throws `std::length_error`; `std::bad_alloc` means not enough memory.
  - A second text format, the **number format**: whitespace- or separator-separated numbers without leading zeros. Text is read as numbers only when it is n⁴ such numbers, each at most n². Otherwise it is read one character per cell, exactly as before.
  - `toString()` writes one character per cell up to 25×25 (unchanged output) and numbers from 36×36 up.
- **Constraints** ([constraints.hpp](include/sudoku/constraints.hpp)):
  - Bitsets of ⌈n²/64⌉ words per row, column and box.
  - The hot functions take a compile-time word count, so grids up to 64×64 keep the single-word fast path.
  - `candidates()` now returns a list of values (it was a mask).
- **Solver:**
  - Iterative depth-first search with an explicit frame stack. Recursion one level deep per empty cell could overflow the call stack on 100×100 grids.
  - Candidate values sit on a growable stack instead of a fixed 64-entry array.
  - New `SolveOptions::timeLimit` and `SolveResult::stopReason` (`Exhausted`, `SolutionLimit`, `NodeLimit`, `TimeLimit`, `InvalidGivens`).
- **Generator:**
  - `randomCompletedGrid` builds the complete grid in time proportional to the cell count: the pattern `(n·(r mod n) + ⌊r/n⌋ + c) mod n²`, then random symbol, row and column permutations and an optional transpose. This replaces the randomized solve and its restarts from step 6.
  - **Forced-clue shortcut:** if the remaining clues leave the removed value as the only candidate, uniqueness is guaranteed without a search.
  - `GeneratorOptions::timeLimit`, and `Puzzle::stats` (`GenerationStats`) recording how every removal ended, plus `minimal()`.
- **Tracing:** `SolverEvent::candidates` is a list of values. The finish reason is the shared `StopReason`. Generator verdicts gained `Forced` and `TimeLimit`. The fill-attempt fields were removed.
- **CLI:**
  - `--node-limit` and `--time-limit` on `solve` and `count`; `--time-limit` on `generate`.
  - New **exit code 3** when a limit cut the work short, or the grid is too large to represent or allocate.
  - Warnings when generation stops early or keeps clues it couldn't verify.

**Decisions.**
- **Behaviour changes, intended but worth knowing:**
  - Box size 1 (a 1×1 grid) is now valid, and the tests that rejected box sizes 1 and 6 changed accordingly.
  - Constructive grids change which puzzle a seed produces, so the two recorded puzzles (4×4 seed 5, 9×9 seed 42) were re-recorded.
- **Puzzle quality held:** 9×9 averages 24.2 clues with no unverified checks, and 16×16 averages 101 clues (about 103 before).
- **No speed change on small grids:** the iterative solver with multi-word bitsets benchmarked within noise of the previous solver (counting 4×4 grids, generating 9×9 puzzles, a 3-million-node search).

**Issues found along the way.**
- **Round-trip noise:** the property test put separator characters *inside* multi-digit numbers. That part of the test was fixed. The parser also now treats `|`, `-` and `+` as cell separators in the number format, so `10|11` reads as two cells.
- **Slow test:** "unsupported box sizes are rejected" silently generated a whole 49×49 puzzle (12 s), because box size 7 became valid. It now tests 0, negative and unrepresentable sizes.
- **My own test mistakes:** two errors in the new word-boundary test (64 is both the boundary value and 64×64's largest value) were fixed.

**Measured limits** (recorded in the README):
- Completed grids take microseconds even at 100×100.
- With 20% of cells blank, every size up to 100×100 solves in about 1 ms or less. At 30%, 81×81 and 100×100 exceed 2 million nodes, and at 60% even 25×25 didn't finish in 20 s. Plain MRV backtracking without stronger propagation is heavy-tailed.
- Full generation takes about 0.9 s at 25×25 and 4 s at 36×36, with many checks hitting the node limit. 64×64 and up needs `--time-limit`.

**Result.**
- 80/80 unit tests (15 new, including a "Large grids" category).
- 12/12 property properties (2 new, and 36×36–100×100 added to 4 others), with 0 inconclusive cases.
- CTest 5/5, no warnings.

## Step 9: Naive backtracking and DLX solvers

**Asked for.**
- Rename `BacktrackingSolver` to `MRVBacktrackingSolver`.
- Add `NaiveBacktrackingSolver`, which reuses the MRV search and only changes how the next cell is chosen.
- Add `DLXSolver`: Knuth's Algorithm X on Dancing Links, with index-based storage, checked sizing and a memory budget.
- Add `--solver mrv|naive|dlx` to `solve`, `count` and `generate`.
- Build, run the existing tests and do basic smoke checks. No new test suites or benchmarks: those come after review.

**Changes.**
- `include/sudoku/backtracking_solver.hpp`, `src/backtracking_solver.cpp`:
  - `Search<kWords, kTraced, kMrv>` holds the whole search.
  - With `kMrv`, `select()` scans for the fewest candidates, as before. Without it, `select()` takes `empty_[depth]`, which stays in row-major order because nothing reorders it.
  - Everything else is shared: `Constraints` bitsets, candidate push, the frame stack, place and undo, limits, rng and tracing.
  - The filenames are unchanged because the files now hold both solvers.
- `include/sudoku/dlx_solver.hpp`, `src/dlx_solver.cpp` (new):
  - **Matrix:** one column per constraint the givens leave open (4 per empty cell) and one row per legal placement, taken from `Constraints::appendCandidates`.
  - **Storage:** flat `int` arrays (`left`, `right`, `up`, `down`, `column`), plus a size per column and a cell and value per row.
  - **Search:** standard Knuth cover and uncover; uncover is the exact reverse. Column choice scans every open column, stops at once on an empty one, and otherwise takes the smallest. The search loop is iterative, with one frame per covered column.
  - **Limits:** sizes are computed with saturating 64-bit arithmetic before anything is allocated. A run is refused with `std::length_error` (CLI exit 3) if it needs more than `INT_MAX` nodes or more than the memory budget: 256 MiB by default, configurable through the constructor.
- `trace.hpp`, `text_trace.cpp`:
  - `SolverEvent` gains `selection` (`Mrv`, `FixedOrder` or `Constraint`), `constraint` and `options`. The defaults leave MRV traces unchanged.
  - Naive traces say "next empty cell in fixed row-major order"; DLX traces print lines like `choose row 3 needs a 5: 2 options`.
- `app/cli.cpp`: `--solver` on `solve`, `count` and `generate`. The default is `mrv`; an unknown name exits with 2.
- `CMakeLists.txt`: adds `src/dlx_solver.cpp`.

**Unchanged.** The `Solver` interface, `SolveOptions`, `SolveResult` and the MRV solver's behaviour. Its node counts and traces are identical, and all existing tests pass unmodified apart from the rename.

**Observed.**
- With seed 42, `naive` generates a different 9×9 puzzle than `mrv` and `dlx`: 6 uniqueness checks hit the 20,000-node limit, so those clues stay. The puzzle is still unique and reproducible.
- On a hard 9×9 puzzle:

  | Solver | Nodes | Time |
  |---|---|---|
  | `mrv` | 23,714 | 1.3 ms |
  | `dlx` | 2,719 | 0.7 ms |
  | `naive` | 2,068,780 | 33 ms |

**Result.**
- Build with no warnings; CTest 5/5 (80 unit tests, 12 properties).
- Smoke checks for each solver:
  - the classic and 4×4 puzzles solve correctly;
  - a count with limit 2 gives 1 and 2 as expected;
  - an empty 4×4 has 288 solutions;
  - conflicting givens and an unsolvable puzzle exit 1;
  - the node limit exits 3;
  - the time limit is reported;
  - rng fills are valid and deterministic;
  - generation is unique and reproducible per solver.
- Exact counts agreed across all three solvers on 80 sparse 4×4 and 9×9 puzzles.
- DLX solved 20%-blank 36×36, 64×64 and 100×100 grids in ≤ 12 ms. A blank 144×144 grid is refused by the memory budget.

## Step 10A: Solver study protocol and correctness verification

**Asked for.** Plan a reproducible study of the three solvers covering n = 1..15 and 10–70%
empty cells: correctness checks, benchmarks, graphs and profiling, in four reviewable phases
(10A–10D). Write the experiment down in a README. Then carry out Phase 10A, rigorous
correctness verification, and stop for review.

**Protocol.** [benchmarks/README.md](benchmarks/README.md) records the full experiment design
before any measurement exists:
- the research questions;
- the workload matrix (100 cells per population);
- the two puzzle populations;
- fixture rules, operations and the prefix-pruning rule;
- tiers and budgets, with worst-case costs;
- measurement and status definitions;
- the statistics rules, the figures, and the threats to validity.

Later deviations go in its changelog.

**Changes.**
- `DLXSolver::estimate(board)`: the matrix size and memory estimate, now public.
- CMake option `SUDOKU_DLX_VALIDATE`: DLX checks the header list, column lists, sizes and
  row rings after construction. On small matrices it repeats the check at every search
  node, and after exhaustive searches it requires the links to match the post-construction
  snapshot exactly.
- `tests/sudoku_test_helpers.hpp`: `allSolvers()`, and an independent solution checker,
  `solutionProblem()`, that does not use `Constraints`.
- `tests/test_main.cpp`: 14 "Solvers" tests run against all three solvers, plus one CLI test
  for `--solver`.
- `tests/property_tests.cpp`: three new cross-solver properties:
  - oracle solution *sets* on 4×4;
  - capped counts on 9×9;
  - uniqueness verdicts on 16×16 and 25×25.

**Result.**
- Unit tests: 95/95 in Release, DLX-validate and UBSan builds.
- Property tests: 15/15 in Release, DLX-validate and UBSan builds.
- CTest: 5/5.
- The only inconclusive cases are naive backtracking hitting its node limit on the
  generated 16×16 and 25×25 puzzles (25 runs); MRV and DLX were conclusive on all.
- Full report: [benchmarks/reports/correctness.md](benchmarks/reports/correctness.md).

**Observed.** Naive backtracking proved only 3 of 40 generated 16×16 puzzles unique within
2 s, so the unit tests use one of those (seed 39).


## Step 10B (in progress): note on population B generation cost

Carving population B was paused after 80 of 300 fixtures (n = 7–15, high-sparsity targets).
Every one of them used the full 600 s budget and stopped at the same sparsity whatever its
target: from 42.7% empty at n = 7 down to 21.0% at n = 15.

**Why.** Every clue removal needs a full solver run to prove uniqueness, so the cost grows
roughly with the square of the cell count even when the proofs are easy. On top of that,
proofs explode near the minimal-clue frontier: at n = 7, 13% of checks hit the
1-million-node limit.

Write-up: [benchmarks/reports/population_b_cost.md](benchmarks/reports/population_b_cost.md).
Resuming carving skips the fixtures already built.


## Step 11A: Human logical solver

**Asked for.** Human-aware difficulty in four tiers (Easy: naked and hidden singles; Medium:
+ locked candidates and naked pairs; Hard: + X-Wing; Expert: controlled backtracking when
logic stalls), built in three reviewable phases. 11A is the logical solver alone: candidate
state, the five deduction techniques and an explainable trace, with each rule tested on its
own. No grading and no generator changes yet.

**Changes.**
- New `include/sudoku/logical_solver.hpp` / `src/logical_solver.cpp`:
  - `CandidateGrid`: the board plus an explicit candidate bitmask per empty cell, seeded from
    `Constraints`. It needs its own masks because pair, locked-candidate and X-Wing
    eliminations can't be stored in `Constraints`' per-unit bitsets. The unit tables are
    shared between copies (ready for 11B's guessing). It offers `place`, `eliminate`,
    `apply(Deduction)` and `contradiction()`: an empty cell with no candidates, a repeated
    value, or a missing value with no place left in a unit.
  - Pure finders `findNakedSingle`, `findHiddenSingle`, `findLockedCandidates` (pointing,
    then claiming), `findNakedPair` and `findXWing` (row base, then column base). Each
    returns one `Deduction` that changes the grid, scanning in a fixed order, and none
    applies it. They work for any box size.
  - `Deduction`: technique, pattern cells, values, the units involved in a documented order,
    the placement or the sorted eliminations.
  - `LogicalSolver`: always applies the easiest available technique and starts again from
    the easiest after each step. It stops at Solved, Stalled or Contradiction.
    `LogicalOptions::hardest` caps the techniques it may use (11B grades with this).
    `recordSteps` keeps every deduction, and an optional `LogicalObserver` sees each one.
- `TextLogicalTracer` and `explain(deduction, size)` in `text_trace.*`. Example line:
  `[logic] X-Wing on 7: rows 2,6 / columns 3,8; remove 7 from r1c3, r1c8, ...`. The summary
  line always prints the outcome and the number of steps per technique.
- Tests: 10 unit tests in category `Logic`:
  - each finder on a candidate grid carved by hand, checked for its exact
    placement/eliminations, plus a near miss that must find nothing (two places, two rows,
    mismatched pair, different column pairs, a third X-Wing position);
  - pointing along a row and along a column, claiming, and both X-Wing orientations;
  - the classic puzzle solved by singles alone;
  - contradiction from conflicting givens and from a dead cell;
  - Inkala's puzzle stalls, and every placement and elimination agrees with MRV's unique
    solution;
  - the tracer's output format and event budget.

  One property test checks soundness on generated 4×4, 9×9 and 16×16 puzzles: no placement
  or elimination ever contradicts the unique solution.

**Result.**
- Unit tests 106/106 and property tests 16/16, in the Release and UBSan builds.
- CTest `-L unit`: 4/4.
- `sudoku_bench` was not relinked, because the Step 10B carving processes were running it.

**Observed.** Over generated 9×9 puzzles (seeds 0–299), logic alone solved 198 and stalled
on 102. Techniques used, by number of puzzles: locked candidates 138, naked pair 48,
X-Wing 9. So with this technique set, unconstrained generation gives Expert about a third of
the time, while Hard (X-Wing needed, then solved) is at most about 3%. 11C's retry budget has to allow for
that.


## Step 11B: Difficulty grading

**Asked for.** Grade puzzles Easy, Medium, Hard or Expert by the lowest sufficient tier, with
controlled guessing for Expert, and test the grader against known puzzles. (11B and 11C were
done back to back at the user's request, with no review stop in between.)

**Changes.**
- New `include/sudoku/difficulty.hpp` / `src/difficulty.cpp`:
  - `Difficulty`, plus `tierOf(Technique)`, `hardestTechnique(Difficulty)`, `name` and a
    case-insensitive `parseDifficulty`.
  - `rate(board, RateOptions)` returns a `Rating`: status, grade, solution, steps per
    technique, logic steps before the first guess, and the number of guesses.
  - Grade = tier of the hardest technique the easiest-first solver used. This is the lowest
    sufficient tier because:
    - techniques only remove candidates, so each tier's closure is fixed;
    - a harder technique runs only after every easier one has stalled.
  - **Controlled guessing** (Expert): if logic stalls, take the empty cell with the fewest
    candidates and try each candidate on a copy of the `CandidateGrid`. Run full logic
    again, recursing on further stalls. It stops at the first solution, and
    `RateOptions::guessLimit` (default 100,000 tried values) bounds it, giving `GuessLimit`.
    Grading does not check uniqueness.
  - `RateOptions::cap`: only the cap's techniques are used and nothing is guessed. A puzzle
    they can't finish is `AboveCap`. This is the cheap "is it at most T?" query for 11C.
- Tests:
  - Five unit tests in category `Difficulty`, on 8 fixtures (2 per tier), all checked unique
    with MRV:
    - Easy: the classic puzzle, and one that needs hidden singles;
    - Medium: one needing locked candidates, one needing a naked pair;
    - Hard: two needing an X-Wing, found by capped carving (below);
    - Expert: Inkala's puzzle and a generated one.
  - Each fixture is checked for its grade, its solution, the technique it must use, and
    guesses (none below Expert, some at Expert).
  - "Lowest sufficient": solving with one tier lower stalls, and with the grade's own tier
    solves.
  - Cap behaviour, a guess limit of 1, contradictions, and the empty 4×4 grid (Expert,
    solved by guessing, not unique).
  - Property test on generated 4×4 and 9×9 puzzles:
    - `rate()` finds the unique solution;
    - guesses happen exactly when the grade is Expert;
    - capping at the grade solves, and capping one tier lower is `AboveCap`.

**Result.** Unit tests 111/111 in the Release and UBSan builds. The new property test
passes in both builds.

**Observed: Hard is rare.**
- Unconstrained generation, 300 9×9 seeds: Easy 153, Medium 45, Hard 0, Expert 102.
- Every puzzle that needed an X-Wing went on to stall anyway.
- Capped carving prototype (remove a clue only while the puzzle stays at most Hard), 6,000
  attempts: Easy 4,370, Medium 1,611, Hard 19 (0.3%). About 6.4 ms per attempt.
- Removing a clue never lowers the grade, since a tier's closure can only shrink. So a
  carve ends Hard only if some single removal lands exactly on Hard. These grids almost
  always jump from Medium straight past it.
- Consequence for 11C: about 300 attempts on average for Hard. With the planned 1,000-attempt
  default, about 5% of Hard requests would fail, so the default is raised to 5,000.


## Step 11C: Difficulty-targeted generation and CLI

**Asked for.** Put the grader into clue removal, bound the generation attempts, add
`--difficulty` and a `rate` command to the CLI, and check uniqueness independently.

**Changes.**
- `GeneratorOptions` gains `difficulty` and `maxAttempts` (default 5000, raised from the
  planned 1000 after the 11B measurement). `Puzzle` gains `difficulty`, `attempts` and
  `targetReached`. `GenerationStats` gains `removedByLogic` and `keptTooHard`.
- `Generator::generate` now runs one carving pass per attempt:
  - **Easy/Medium/Hard target:** a clue is removed only if `rate(puzzle, {.cap = T})` still
    solves the puzzle. Logic proves uniqueness, so these targets never run a backtracking
    search. A forced removal (the cell's only candidate) is still accepted without a check,
    because it is a naked single and can't raise the grade.
  - **Expert target:** the existing search check.
  - **Retries:** the finished puzzle is graded. On a miss, the next attempt uses seed + 1,
    up to `maxAttempts`. `timeLimit` covers all attempts.
  - **Result:** `Puzzle::seed` is the successful attempt's seed, so it reproduces in one
    attempt. On a total miss the hardest puzzle found is returned, with
    `targetReached = false`.
  - **No target:** the code path is unchanged. Compared against the previous build
    (`build-validate`, 4 Nov) with `--show-solution` for sizes 2–4 and seeds 0–40: all 123
    outputs identical, and so is a full `--trace-generation` at seed 9. The Step 10B fixtures
    therefore stay reproducible.
- Events: new `Rated` kind (one per attempt), new verdicts `SolvedByLogic` and `TooHard`. The
  Finished event carries the stats, target, grade, attempts and `targetReached`. The text
  tracer now builds its summary from the stats instead of counting removals, so a summary
  covers only the returned attempt. Untargeted output is unchanged.
- CLI:
  - `generate --difficulty easy|medium|hard|expert [--attempts k]`. stderr adds
    `difficulty: X, attempts: N`. A missed target prints the hardest puzzle found with a
    warning and exits 3.
  - New `rate [puzzle] [--guess-limit k] [--trace-logic]` prints the level, the steps per
    technique and the guesses. It exits 1 for no solution and 3 at the guess limit. For
    Expert it notes how far logic got, and checks uniqueness with MRV, warning if the puzzle
    has more than one solution.
- Tests:
  - **Unit:**
    - each level on a fixed seed is graded exactly that and unique by MRV and DLX. Hard uses
      seed 549, which hits on the first attempt;
    - the result is deterministic, and the reported seed reproduces it in one attempt;
    - stats show logic-only removals for levels below Expert;
    - untargeted generation stays ungraded; a missed target (Hard, 3 attempts) returns a
      graded unique puzzle; `maxAttempts = 0` throws;
    - generator trace text, and the CLI paths and exit codes.
  - **CTest:** `cli_generate_difficulty` and `cli_rate`.
  - **Property:** generated at T ⇒ graded T and unique by MRV and DLX: 4×4 easy ×200, and
    9×9 easy, medium and expert ×30 each, hard ×3.
- README: new "Difficulty" section, CLI examples, exit code 3 cases, architecture entries
  and test lists. Extension point 1 is marked done.

**Result.**
- Unit tests 115/115, property tests 18/18, CTest `-L unit` 6/6, in both the Release and
  UBSan builds.
- `sudoku_bench` was again not relinked: the Step 10B carving processes still hold it.

**Observed.**
- `generate --seed 1 --difficulty X` on 9×9:

  | Level | Attempts | Time |
  |---|---|---|
  | easy | 1 | 53 ms |
  | medium | 3 | 61 ms |
  | hard | 549 | 2.8 s |
  | expert | 5 | 50 ms |

  Process start dominates the short runs.
- One planned check didn't hold up. The property test first required *all three* solvers to
  confirm uniqueness, and naive backtracking hit a 5M-node limit on an Easy 9×9 puzzle
  (seed 20261121). Human difficulty and naive-search difficulty are unrelated, so
  uniqueness is checked with MRV and DLX, as the plan specified.

## Step 10B: Benchmark dataset, harness, pilot and schedule

**Built.**
- Timing fields in `SolveResult`: setup and search time, time and nodes to the first solution,
  and maximum depth. The deadline now starts at `run()` entry.
- A counters-only build option, `SUDOKU_SOLVER_COUNTERS`.
- `sudoku_bench`, which builds fixtures and runs one validated, timed job per process.
- Scripts: `make_fixtures.py`, `run_campaign.py` (pilot, core, deep, calibrate and custom tiers,
  watchdog, pruning, resume, dry-run costs) and `selftest.py`.

**Data.**
- 614 fixtures:
  - 300 in population A;
  - 14 in the ablation set (AS);
  - 300 in population B: 160 reached their target and 140 are `target_not_achieved`. Carving
    cost 24.3 CPU-h.
- Pilot: 889 jobs in 47 min. 706 completed, 181 timed out, 140 were pruned and 2 were
  memory-rejected.
- All 20 pruning-validation pairs agreed, so pruning stays enabled.
- Calibration: running 4 at a time is 1.14× slower than serial (geometric mean); DLX slows only 1.05×.

**Result.**
- [benchmarks/reports/schedule.md](benchmarks/reports/schedule.md): the core campaign is
  2,841 jobs, about 81 CPU-h expected and about 21 h wall. Deep sampling needs about 8–12 CPU-h,
  over the planned 5 CPU-h cap.
- The A/B overhead check missed ±3% in 2 of 9 cases; this is recorded as a limitation.
- The core campaign has not been started; it waits for review.

**Current state**

| Item | State |
|---|---|
| Unit tests | 96 |
| Property tests | 15 |
| Builds | Release, validate and UBSan pass |
| Step 10 | 10A and 10B done; core campaign, 10C and 10D pending approval |
| Prerequisite for 10C | pandas, matplotlib and numpy, to be installed by the user |

## Step 10C (started): core campaign at 180 s on 6 pinned workers

Decided at the 10B review:
- **Budget:** core 180 s instead of 600 s. Pilot completions thin out decade by decade above
  0.1 s, so 600 s would add only about 1–2% of completions.
- **Deep sampling:** deferred.
- **Logical solver:** benchmarking `LogicalSolver` is deferred until after 10C.

**Changes.** `run_campaign.py` now pins each job to one logical CPU (`SetProcessAffinityMask`):
- serial jobs run on CPU 2;
- parallel workers use the even CPUs, one per physical core;
- a `cpu` column records where each job ran;
- `--no-pin` turns pinning off.

**Calibration.** Pinned slowdown is 1.045× at 4 workers, 1.06× at 6, and 1.29× at 8 (the
worst pair at 8 is 2.07×, from hyperthreads), so the campaign uses 6 workers.

**Status.** The self-test passes. The core campaign (2,841 jobs, about 4.7 h expected) started
2026-11-05 14:04. The graphs still need pandas and matplotlib: the pip install was blocked by
Avast's HTTPS scanning, and the user will decide how to resolve that.

## Step 10C (in progress): core campaign finished, serial confirmation running

**Core campaign.** It ran 2026-11-05 14:04–19:04: 2,841 jobs (2,362 executed), 24.9 process-hours.

| Status | Count |
|---|---:|
| Completed | 1,875 |
| Timeout | 481 |
| Pruned | 479 |
| Memory rejected (DLX at n = 15, 70% empty) | 6 |

There were no errors and no watchdog kills, and every completion validated. Completions by
solver: naive 532, MRV 648, DLX 695.

**Serial confirmation.**
- No parallel run completed after 144 s; the longest took 121.9 s.
- 99 parallel timeouts fell in a cell where another seed or solver completed. This is more
  than the cap of 40.
- Rerun serially: all 10 where the same solver completed on another seed, plus 30 sampled
  with a fixed seed (`benchmarks/scripts/select_borderline.py`,
  `benchmarks/reports/serial_confirm_jobs.csv`).
- About 2 h, in `results/serial_confirm/`.
- Recorded in schedule.md §10 and the README changelog.

**Current state**

| Item | State |
|---|---|
| Unit tests | 96 |
| Property tests | 15 |
| Step 10 | 10A and 10B done. 10C: core campaign done, serial confirmation running, figures and analysis pending. 10D pending. |
| Blocker | pandas and matplotlib are not installed: Avast's HTTPS scanning blocks pip |

**Update.** The user added Avast exclusions for PyPI. pandas 3.1.1 and matplotlib 3.11.2 are
now installed, alongside numpy 2.5.2, and pinned in `benchmarks/scripts/requirements.txt`.
The figures are no longer blocked.

## Step 11D: 4×4 grids can only be Easy

**Finding.** Asking for Medium, Hard or Expert on a 4×4 grid used up all 5,000 attempts and
returned an Easy puzzle with exit 3. An exhaustive check (a scratch program, not kept in the
project) explains why: all 288 grids × 65 536 clue subsets, 13 579 680 uniquely solvable
puzzles, and singles solve every one. Higher levels on 4×4 are impossible, not just rare.

**Change.**
- `hardestPossible(boxSize)` in `difficulty.hpp`: Easy for box size ≤ 2, Expert otherwise.
- `Generator::generate` throws `invalid_argument` when the target is above it, e.g.
  "no 4x4 puzzle can be graded medium: singles solve every one, so the hardest level is easy".
  The CLI prints it as an error and exits 2.
- 16×16 and 25×25 were checked by hand and reach every level, so nothing changes there.

**Tests.** New unit test (rejection on 1×1 and 4×4, Easy still works in one attempt) and a CLI
check of the message. Unit 116/116, property 18/18, CTest unit 6/6, in Release and UBSan.

## Step 10C: summaries, figures and preliminary analysis

**Serial confirmation.** All 40 borderline reruns timed out again when run alone, so the
parallel outcomes stand.

**New scripts** (pandas, matplotlib, numpy):
- `benchmarks/scripts/common.py` loads runs and checks them: duplicates, a single tier, valid
  solutions, no errors. It overlays the serial confirmation, holds the censoring and Wilson
  rules, and sets the plot style (reference palette, validated slots 1–3, plus a marker per
  solver).
- `summarize.py` writes `results/<tier>/summary/{cells,speedups,status_counts}.csv`. Missing
  population-B targets are listed explicitly.
- `plot_figures.py` draws priority figures 1–7 and conditional figures C1, C3 and C4, with
  `figures.md` saying what was drawn and what was not. C2 and C5 are not produced, because
  they need k ≥ 20 per cell.
- Figures regenerate byte-identically from the CSVs.

**Preliminary findings** ([benchmarks/reports/analysis.md](benchmarks/reports/analysis.md)):
- On easy puzzles, MRV and naive are about even. DLX is slower everywhere at 10% empty, by a
  median of 28× and up to 90× at n = 13–15. Its time per node grows with the board area.
- On hard puzzles, DLX completes the most (A 174/300, B 160/160). It never fails where MRV
  completes, and it reaches 2,400× over MRV at n = 5, 70%.
- On population A, no solver finishes at 30% or more empty from n ≥ 12, 40% from n ≥ 8, or
  50% from n ≥ 6.
- DLX needs over 256 MiB at n = 15, 70% empty.

**Current state**

| Item | State |
|---|---|
| Unit tests | 96 |
| Property tests | 15 |
| Step 10 | 10A, 10B and 10C done, waiting for review. 10D (profiling, stress, conclusions) is next. |
| Deferred | Benchmarking `LogicalSolver`; deep sampling |

## Step 10D (in progress): profiling, and the stress shortlist

**Tooling.**
- New `build-counters/` (operation counters) and `build-gprof/`. The gprof build needs
  `-fno-pie` and `-no-pie`, or MinGW gprof records no samples; `gprof -l` gives line-level
  attribution.
- `run_campaign.py` gains `--timing single`, and tolerates jobs that exit before they can be
  pinned. The self-test passes.
- `scripts/profile_counters.py` joins the counts with the serial Release timings.

**Results** ([benchmarks/reports/profiling.md](benchmarks/reports/profiling.md)):
- All 1,875 core completions were replayed in the counters build, and every node count
  matched.
- **H1, confirmed:** DLX's `chooseColumn()` scans every open column (2 × the empty cells per
  node on average) at about 1.3 ns each. This is 77–94% of DLX time, and about 90–97% of
  gprof samples.
- **H2, confirmed:** DLX wins hard puzzles through a 10–67,000× smaller tree, despite
  6–9× costlier nodes.
- **H3, rejected:** per-column and per-link costs are flat from 0.3 to 174 MB matrices, so
  no cache effect is visible.
- **H4, supported:** MRV's scan stops early when a forced cell exists, and is a full scan on
  hard boards (about 97% of gprof samples).
- Optimisation candidates are documented, not applied. The top one is an early exit on a
  size-1 column in DLX.

**Stress tier.** [stress_shortlist.md](benchmarks/reports/stress_shortlist.md) lists:
- 22 frontier cells chosen by rule, plus the 3 DLX memory-rejected fixtures;
- 1,800 s per job, 8 GiB DLX budget;
- worst case 14.9 CPU-h, expected 11.0 (about 1.8 h on 6 workers).

It has **not been run**; it waits for approval.

**Current state**

| Item | State |
|---|---|
| Unit tests | 96 |
| Property tests | 15 |
| Step 10 | 10A–10C done. 10D: profiling done, stress waiting for approval, final `performance.md` pending. |
| Deferred | Benchmarking `LogicalSolver`; deep sampling |

## Step 12: review findings for the difficulty feature (before any fix)

An external review raised six criticisms. Each one was checked against the Step 11 code.
"Impact" is what can actually happen today; it is updated with measurements as each phase
lands.

| # | Finding | Who can hit it | Actual impact today | Severity |
|---|---|---|---|---|
| 1 | Grading ignores the time limit. `carve()` checks the deadline only between removals; its `rate(puzzle, {.cap})` call and the final `rate(attempt.puzzle)` in `generate()` take no deadline, and `LogicalSolver::solve` has none. | `generate --difficulty X --time-limit T`, `GeneratorOptions{.difficulty, .timeLimit}`, any `rate()` caller. Untargeted generation and the solver benchmarks do not grade and are unaffected. | 9×9: negligible (grading takes milliseconds). Large grids: after the deadline the final grade of an Expert attempt may guess up to 100 000 times, each a full logic solve, so it can overrun by minutes to hours. The 49×49+ timing run needs an outer `timeout` for this reason. | High: the documented `timeLimit` contract is broken |
| 2 | `Board::setAt` stores any value; `Constraints::fromBoard` then indexes bitsets with it unchecked. | Only library code calling `setAt` with a bad value. Every in-tree caller writes valid values; CLI input goes through `Board::parse`/`set`, which check the range. | No reachable path through the CLI or generator. For a library user: undefined behaviour (out-of-bounds access) in `isValid`, the solvers, `CandidateGrid::fromBoard` and `rate()`, not a clean error. | Medium: latent, misuse only |
| 3 | `guess()` in `difficulty.cpp` recurses; each level holds a grid copy. | `rate()` / `sudoku rate` on sparse or ambiguous large boards (the generator stops guessing in 12A). | Not seen in practice: generated Expert puzzles guess only a few levels deep. Near-empty large boards can go thousands deep against a 1 MB Windows main-thread stack, and each level holds a grid copy (about 160 KB on 100×100). | Medium: possible crash on adversarial input |
| 4 | Every completed grid is a transformed copy of one pattern grid. | Every generated puzzle. | Not a correctness bug. The tier rates (Hard ≈ 1 in 160 on 9×9) describe this grid family only. Already documented in `generator.hpp`. | Low; deferred as a separate enhancement |
| 5 | `tests/test_main.cpp` is 2 063 lines; a quarter is difficulty tests. | Maintainers. | Maintenance only. | Low |
| 6 | Docs drift: `generator.hpp` says `timeLimit` "covers all attempts" (false, see #1); README "Extension points (future phases)" lists done work; README tier rates are about 2× off the measured ones. | Readers. | The `timeLimit` comment hides #1. | Medium for that comment, low otherwise |

**Plan:** 12A fixes #1, 12B fixes #2 and #3, 12C splits the difficulty tests out and fixes
the docs. Each fix starts with a test that reproduces the problem on the current code.
#4 is deferred. Untargeted generation stays byte-identical throughout.

## Step 12A: the time limit now covers grading

**Reproduction first (pre-12A binary).** `generate --difficulty X --time-limit T` on 25×25,
36×36 and 49×49 ended 0.13–0.18 s past the limit. The overrun was the grading of the
interrupted puzzle after the deadline. Grading a generated puzzle needs only 2–6 guesses and
about 0.1 s even on 36×36, so the generator's overrun was small in practice; the
"minutes to hours" estimate in the findings was too pessimistic for generation. The
unbounded case is `rate` itself: an empty 36×36 board took 6 s and 1 036 guesses, and only
`--guess-limit` could stop it. Impact for #1 is revised to: generation overran by
sub-second amounts; `rate()` could not be bounded in time at all.

**Change.**
- New `Deadline` (`include/sudoku/deadline.hpp`): a clock deadline, never, or (testing only)
  "expires after N checks", which makes time-limit paths deterministic in tests.
- `LogicalOptions::deadline`, checked before each step → `LogicalStatus::TimeLimit`.
- `RateOptions::deadline`, passed to every logic pass and checked before each guess →
  `RatingStatus::TimeLimit`. `Rating::difficulty` is now `std::optional`: empty for
  NoSolution and TimeLimit, so no grade is ever reported for an unfinished rating.
- Generator:
  - grades attempts without guessing (internal `gradeUnique`: logic capped at Hard; a stall
    means Expert because carving proved the puzzle unique);
  - passes the deadline into every per-removal rating (a timed-out rating keeps the clue
    and stops carving, `Verdict::TimeLimit`);
  - an interrupted attempt has no grade: the best graded attempt is returned if there is one,
    otherwise the interrupted puzzle with `Puzzle::difficulty` empty;
  - new `Puzzle::timeLimitReached` (the call stopped on the limit), separate from
    `stats.timeLimitReached` (the returned puzzle's carving was cut short);
  - `GeneratorOptions::deadline` as an alternative to `timeLimit`.
- Trace: `Rated` and `Finished` events carry `graded` / `stoppedByTimeLimit`; "grade unknown
  (time limit)" and "time limit reached, returning the hardest graded puzzle".
- CLI: `generate` prints `difficulty: unknown` and a matching warning (exit 3);
  `rate --time-limit` (exit 3); the uniqueness check after an Expert `rate` shares the budget.

**What the deadline cannot interrupt (measured, Release):** one technique scan that finds
nothing is the longest unit: about 5 ms on 49×49 and 30 ms on 100×100. Building a candidate
grid takes at most 1.2 ms on 100×100; the complete grid under 0.1 ms. That is well under the
100 ms threshold, so no checks were added inside the finders; README "Difficulty" documents
the check points and these figures.

**After 12A.** `generate --size 7 --difficulty hard --time-limit 5` returns in 5.10 s with
`difficulty: unknown`; `rate` on an empty 36×36 with `--time-limit 1` stops at 1.06 s, exit 3.

**Unchanged output.** Targeted generation (92 runs: 9×9 seeds 1–20 and 16×16 seeds 1–3, all
levels) is byte-identical to the pre-12A binary, so grading without guessing gives the same
grades. Untargeted generation (sizes 2–4, seeds 0–40, plus a full trace) is identical to
`build-validate`.

**Tests.** Six new unit tests: an expired deadline in the logic solver and the grader
(clock and check-counted), a deadline that expires during guessing, generation with no
graded attempt (unique by MRV and DLX), generation where a graded attempt is followed by an
interrupted one (found by bisecting the check count; exact grade kept), a loose 25×25
200 ms timing check, and the CLI. The property test now also checks that the generator's
guess-free grade equals `rate()`'s on 650 puzzles. Unit 122/122, property 18/18, CTest unit
6/6 in Release (`build-step12`), UBSan and Debug (`build-debug`, asserts on).

**Side issue found.** The earlier large-grid timing run had two copies of its script
running at once (stopping the first background task did not stop its child script). Its
49×49 and 64×64 rows were measured under that contention and are discarded; the sweep is
rerun on the 12A binary.

## Step 12B: malformed boards and iterative guessing

**Reproduction first (unfixed code, UBSan build).** A test that stores out-of-range values
with `Board::setAt` (size + 1, -1, `INT_MAX`, `INT_MIN`, on 4×4, 9×9 and 81×81) and then
calls `isValid`, the three solvers, `CandidateGrid::fromBoard` and `rate()` crashed with a
segmentation fault (exit 139). A test calling `CandidateGrid::place` with a bad cell or value
died silently (exit 127, memory corruption). Measured impact unchanged from the findings: no
CLI or generator path reaches it, but a library caller got memory corruption instead of an error.

**Recursion (#3), measured.** Rating empty boards, which drive the deepest guessing:

| Board | Guesses | Max depth | Peak memory | Old recursive build |
|---|---|---|---|---|
| 25×25 | 474 | 462 | 8 MB | fine |
| 36×36 | 1 036 | 1 029 | 21 MB | fine |
| 49×49 | 1 996 | 1 967 | 59 MB | fine |
| 64×64 | 3 772 | 3 490 | 170 MB | fine |

The old recursive version did not overflow the 1 MB main-thread stack at any of these sizes:
its frames were small. So #3 was a latent risk (deeper boards, bigger frames in Debug builds),
not an observed crash. What grows in practice is memory, one grid copy per level, which the
iterative version keeps and now reports.

**Change.**
- `Constraints::fromBoard` returns empty for any value outside 0..size. That single check makes
  `isValid` false, the solvers report `InvalidGivens`, and `CandidateGrid::fromBoard`, the
  logic solver and `rate()` report a contradiction / no solution.
- `Board::setAt` stays unchecked for values, documented as such; `at`/`setAt` assert the index.
  No value assert, so tests can build malformed boards in every build type.
- `CandidateGrid::place` is checked: `std::out_of_range` for a bad cell or value,
  `std::logic_error` for a filled cell. `has`/`eliminate`/the mask accessors stay unchecked hot
  paths with documented preconditions and asserts (active in `build-debug`).
- `guess()` is a loop over an explicit stack of frames (grid, cell, candidates, next index),
  in the same depth-first order. `Rating::maxGuessDepth` is new.

**Unchanged results.** Guess counts, steps and solutions of the two Expert fixtures were pinned
from the recursive version (33 / 346 / 0 and 4 / 90 / 22) and still match. Targeted generation
(92 runs) and untargeted generation (123 runs) are byte-identical to before.

**Tests.** Three new unit tests: malformed boards, `place` checks, pinned guessing results, plus
deep guessing on an empty 25×25 board. Unit 126/126, property 18/18, CTest unit 6/6 in Release,
UBSan and Debug.

## Step 12C: difficulty tests in their own file; docs brought up to date

**Tests split.** The logic, difficulty and difficulty-generation tests moved from
`tests/test_main.cpp` (2 299 → 1 503 lines) to `tests/test_difficulty.cpp` (812 lines), built
into the same `sudoku_tests` executable. `CliResult`/`runCli` moved to
`tests/cli_test_helpers.hpp` (the property tests do not link the CLI) and the classic puzzle
constants to `tests/sudoku_test_helpers.hpp`. The sorted `--list` output is identical before
and after (126 tests). The test framework itself is unchanged: replacing it was out of scope.

**Docs.**
- README "Extension points (future phases)" → "Status and possible extensions": done work
  links to its section; only undone work (more techniques, grid diversity, fuzzing, parallel
  generation) is listed as possible.
- Difficulty section: measured tier rates (Easy 1 attempt, Medium 2.6, Expert 2.9, Hard 161
  on 9×9), where the time limit is checked and what it cannot interrupt, and a "Known
  limitations" list (grid copies while guessing, one grid family, five techniques).
- Test counts and lists, exit-code text and the CLI usage text updated; `generator.hpp`
  comments fixed in 12A.

**Deferred: grid diversity (#4)** stays a separate enhancement; the README lists it with the
constraints from the review (total budget, deadline, fallback, deterministic RNG, reuse of the
randomized MRV solver).

## Step 12D: generation time by grid size and level

A single-process sweep on the 12A binary (`generate --difficulty L --time-limit 600`,
4×4 to 100×100; a level that hits the 10-minute limit is not run at larger sizes). Raw rows
in `benchmarks/reports/difficulty_generation_times.csv`; the median table is in README
"Difficulty". Summary: about 0.1 s for every level on 9×9 except Hard (1.1 s median); roughly
tenfold per size step from 16×16 up; Expert hit the limit at 49×49, Medium and Hard at 64×64,
Easy at 81×81, so nothing ran at 100×100. An old-vs-new check on five slow points showed 12A
changed generation time by under 2%. `build-ninja` was rebuilt afterwards (126/126, CTest 6/6).
The chart of this sweep is saved as `benchmarks/reports/difficulty_generation_times.html`
(standalone page, light and dark mode, with the median table), linked from README "Difficulty".
A static PNG of the same chart is `benchmarks/reports/difficulty_generation_times.png`, drawn
from the CSV by `benchmarks/scripts/plot_difficulty_times.py` (matplotlib).

## Step 10D: stress tier and final performance report

**Stress tier** (approved; 1,800 s, 6 pinned workers, 12.5 process-hours, inside the 14.9 CPU-h
worst case):
- **25 runs executed, 25 timeouts, 0 completions**; 3 uniqueness runs pruned behind them.
  No frontier cell went soft between 180 s and 1,800 s, so nothing needed serial confirmation.
- **Search progress:** each run explored 6–13× the nodes of its 180 s core run (median 8.7×),
  but the deepest level reached barely moved. Of the 21 runs with a core counterpart, 18
  gained less than 0.08 of the empty cells.
- **DLX at n = 15, 70%, 8 GiB budget:** the matrix built in about 0.1 s with a peak of about
  251 MiB (estimate 261 MiB). All 3 seeds then timed out, so the shipped 256 MiB budget cost
  no completion.
- **Deviation:** the B/MRV/30% job ran on a `target_not_achieved` fixture, so it used that
  fixture's sparsest reached puzzle (29.2% empty, `.best.txt`). It is labelled
  "(best, 29.2%)" and recorded in the benchmarks README changelog.

**New files:**
- `benchmarks/scripts/plot_stress.py`, which writes `results/stress/summary/stress_vs_core.csv`
  and `figures/stress/figS1_stress_frontier`. Its output is byte-identical when regenerated.
- [benchmarks/reports/performance.md](benchmarks/reports/performance.md), the final report.

**Conclusions** (performance.md):
- **MRV stays the default:** it ties naive on easy puzzles and wins as soon as backtracking
  starts.
- **DLX pays off only on hard puzzles,** where its smaller tree outweighs its per-node column
  scan.
- **Adaptive selection is not worth building yet.** The first step would be the DLX early exit
  on a size-1 column; then re-measure.
- **Uncertainty correction:** the pilot ablation's possible grid-family bias (n = 4, k = 1) was
  never followed up, and is now listed among the uncertainties.

**Current state**

| Item | State |
|---|---|
| Step 10 | 10A–10D complete; final report `benchmarks/reports/performance.md` |
| Deferred | Benchmarking `LogicalSolver`; deep sampling; the AS grid-family follow-up; optimisation candidates 1–5 (documented only) |

## Step 10D addendum: best-solver heatmap

**What was added.** A map of which solver is best in each (n, sparsity) cell, drawn from the
core cell summaries:
- `benchmarks/scripts/plot_winner.py` writes `figures/core/fig08_best_solver.{png,svg}` and
  `results/core/summary/best_solver.csv`, and is linked from `performance.md` §1;
- there are four panels: population A and B, each for the first-solution and uniqueness ops;
- the output is byte-identical when regenerated.

**Rule.** The solver that completed the most puzzles wins, then the one with the lowest median
time. A margin under 1.1× is a tie.

**What the map shows:**
- naive and MRV tie on small and easy boards;
- MRV wins 20% empty from n ≥ 6 and 10% empty at large n;
- DLX wins the hard band (n = 4–5 at 50–70%, n = 7–11 at 30–40%);
- nothing solves population A beyond that band.

**Current state:** unchanged from the previous entry. Step 10 is complete.

## Step 13: automatic MRV/DLX selection, and one build directory

**`AutoSolver`** (`include/sudoku/auto_solver.hpp`, `src/auto_solver.cpp`):
- **What it does:** picks MRV or DLX per puzzle from two facts read off the board before
  solving: the box size, and the share of empty cells. It then runs that solver unchanged.
- **Thresholds:** DLX from 55% empty at n = 4, 45% at n = 5, 35% at n = 6–8, and 25% at n ≥ 9.
  MRV is always used at n ≤ 3.
  - They are the midpoints between the measured crossovers in the Step 10 best-solver map
    (`figures/core/fig08_best_solver.png`).
  - n ≥ 12 is extrapolated: no solver finished at 30% there.
- **Fallback:** MRV whenever DLX's matrix would exceed its memory budget, or the givens
  conflict.
- **Check against the 645 core puzzles that MRV or DLX finished:**

  | Strategy | Solved | Geometric-mean slowdown vs the faster solver | Worst slowdown |
  |---|---:|---:|---:|
  | auto | 645 | 1.015× | 4.2× |
  | always MRV | 621 | 1.37× | 277,778× |
  | always DLX | 645 | 7.0× | 144× |

  The thresholds were fitted to this same data, so this is in-sample.

**CLI:**
- `--solver auto` is the new default for `solve` and `count`, and `--stats` prints e.g.
  `solver: auto (dlx)`.
- `generate` keeps `mrv` as its default, so seeded output is unchanged; it also accepts `auto`.

**Tests:**
- New: 4 AutoSolver tests (thresholds; choice at the exact empty count; identical results to the
  chosen solver; memory and conflicting-givens fallback) and 1 CLI test.
- Changed: the existing node-limit CLI test now pins `--solver mrv`. Auto would pick DLX for its
  100×100, 30%-empty board, and DLX solves it within the node limit.
- Results: unit 131/131, property 18/18, CTest 7/7, in Release.

**Builds:**
- The main build is now `build-cmake/` (Release, tests and benchmarks).
- `benchmarks/scripts/bench_common.py` points at it, and the READMEs are updated.
- The other eight builds (`build-ninja`, `-counters`, `-debug`, `-gprof`, `-review`, `-step12`,
  `-ubsan`, `-validate`) moved, unchanged, to the hidden folder `old_builds/`.
  - Their CMake caches hold the old absolute paths, so to rebuild one, configure a fresh
    directory with the documented commands.
  - A test run in `build-review` was stopped first, at the user's request.

**Current state**

| Item | State |
|---|---|
| Unit tests | 131 |
| Property tests | 18 |
| Default solver | `auto` for solve/count; `mrv` for generate |
| Build | `build-cmake/`; old builds in `old_builds/` (hidden) |

## Step 14: documentation sweep

The READMEs and this log were brought in line with the finished project.

**README.md:**
- **Algorithms:** new subsections on naive backtracking, DLX, and auto selection (its threshold
  table and how it was checked).
- **Architecture:** now lists `auto_solver.hpp`, `deadline.hpp`, `test_difficulty.cpp`,
  `cli_test_helpers.hpp` and `benchmarks/`.
- **Solver study:** now summarises the conclusions rather than only Phase 10A.
- **Smaller fixes:**
  - test timings updated (CTest about 40 s, unit about 8 s, property about 30 s);
  - the MRV performance table is labelled as measured before DLX and auto existed;
  - the build notes cover `old_builds/`;
  - the possible extensions now include the profiling's solver optimisations and a
    `LogicalSolver` benchmark.

**benchmarks/README.md:**
- the status line notes that Step 13 built `AutoSolver` from the study;
- the tools table and the reproduce section list the 10C/10D scripts;
- the builds section gives configure flags for the archived validate and UBSan builds;
- the population-A bias entry states the pilot finding and that the follow-up was not run.

**Consistency fix.** The stress report called the B/MRV/30% run a "B\*" fixture, but the
protocol keeps missed targets as `.best.txt` files, not as B\* fixtures. The run is now
described as the fixture's sparsest reached puzzle and labelled "(best, 29.2%)" in
`figS1_stress_frontier`; the figure was redrawn.

**This log:**
- a note that Step 10 entries are interleaved with Steps 11–12, and that each entry's state
  table reflects its own time;
- the top table gains Difficulty and Deferred rows, and its Extra builds row now points to
  `old_builds/`;
- the second "Step 12" heading is now "Step 12D".

All relative links and anchors in the READMEs, this log and performance.md resolve (checked by
script).

**Current state:** unchanged from Step 13.


## Step 15: code-level explanation filled in

`code_level_explanation.md` was checked against the code by script:
- its test catalog matches all 149 registered tests (131 unit, 18 property);
- every source file, script and CMake option is covered, and almost every function and type is
  named.

Six detail gaps were then filled:
- **Editor setting (§2):** `compile_commands.json` is not produced unless the build is configured
  with `-DCMAKE_EXPORT_COMPILE_COMMANDS=ON`.
- **DLX cost model (§7, new subsection):**
  - the `chooseColumn()` scan stops only at an empty column, and takes 77–94% of DLX time;
  - hidden-single branching makes DLX's trees smaller on hard puzzles;
  - the matrix needs about 92 bytes per placement.
- **AutoSolver (§8):**
  - the evidence for the thresholds, and that it is in-sample;
  - the constructor's memory budget;
  - how a caller finds out which engine ran (`choose()`; `SolveResult` does not record it);
  - the memory estimate being worked out twice when DLX is picked.
- **`run_campaign.py` (§16):** a table of every command-line option.
- **End-to-end benchmark flow (§18):** extended past the core figures to serial confirmation,
  profiling, the stress tier and the best-solver map.
- **Data and reports (§17):**
  - `build-cmake/` and the hidden `old_builds/` archive replace the old `build-*` row;
  - each written report and what it concludes is named.

All links and backticked paths in the four documents resolve (checked by script). No code changed.

## Step 16: folder guide in the benchmarks README

`benchmarks/README.md` gains a **Folder guide** section ahead of the protocol, linked from its
contents list. It has three parts:
- **Where to start:** a table that maps each question to the file that answers it.
- **Annotated tree:** every folder and script, with its purpose and its size where useful:
  - 300 A and 300 B fixtures, 140 of the B ones only as `.best.txt`;
  - 14 AS fixtures;
  - nine campaign result folders, with job counts;
  - which figures each tier has;
  - every report, by phase.
- **Conventions:** the four standard files in a results folder, what each `summary/` holds, and
  how fixture IDs, populations, solvers and ops are named.

Every path in the guide was checked to exist, and all links resolve. Only documentation changed.

## Step 17: `.gitignore`

A new root `.gitignore` keeps these out of version control:
- all build output: `build-*/` (including `build-cmake/`), the hidden `old_builds/` archive, CMake caches, `Testing/`, and compiled objects and binaries;
- Python clutter: `requirements.txt`, `__pycache__/` and virtual environments;
- editor folders: `.vscode/`, `.idea/` and `.vs/`;
- OS files.

The benchmark fixtures (48 MB), results, figures and reports stay tracked, because they are the project's measured record. `git status` now lists only source, docs and `benchmarks/`.
