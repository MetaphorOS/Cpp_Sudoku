# Sudoku Generator & Solver (C++20)

A small, dependency-free library and CLI that solves and generates n²×n² Sudoku for any box
size n ≥ 1: 1×1, 4×4, 9×9 (the default), 16×16, and up through 36×36, 64×64, 81×81 and 100×100.
Generated puzzles always have exactly one solution and can be reproduced from a seed.

The design is generalized. Nothing assumes a maximum size, and dimensions are computed with
overflow checks. It does **not** promise practical running times for arbitrarily large or
hard puzzles: searches can be bounded with node and time limits, and a stopped search is
always reported (see [Limits and performance](#limits-and-performance)).

[DEVELOPMENT_LOG.md](DEVELOPMENT_LOG.md) records step by step how the project was built.
[code_level_explanation.md](code_level_explanation.md) walks through the code file by file.
[benchmarks/README.md](benchmarks/README.md) is the solver study (Step 10).

## Build

Requires CMake ≥ 3.20, Ninja and a C++20 compiler (GCC 11+, Clang 14+, MSVC 19.30+).
Verified with CMake 4.5, Ninja 1.12 and MSYS2 GCC 14.2.

```bash
cmake -S . -B build-cmake -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build-cmake
ctest --test-dir build-cmake --output-on-failure            # everything (~40 s)
ctest --test-dir build-cmake -L unit --output-on-failure    # unit + CLI tests (~8 s)
ctest --test-dir build-cmake -L property --output-on-failure  # property tests (~30 s)
```

With MSYS2/MinGW on Windows, put `C:\msys64\mingw64\bin` on PATH so the executables
find the MinGW runtime DLLs. `build-cmake/` is the one working build; builds from earlier
steps are archived, hidden, in `old_builds/` (their CMake caches point at their old paths, so
configure a fresh directory to rebuild one).

Without CMake (e.g. a bare MinGW/GCC toolchain):

```bash
g++ -std=c++20 -O2 -Iinclude src/*.cpp app/cli.cpp app/main.cpp -o sudoku
g++ -std=c++20 -O2 -Iinclude -Iapp src/*.cpp app/cli.cpp tests/test_main.cpp tests/test_difficulty.cpp -o sudoku_tests
g++ -std=c++20 -O2 -Iinclude src/*.cpp tests/property_tests.cpp -o sudoku_property_tests
```

## CLI

```bash
sudoku generate --size 3 --seed 42 --pretty     # 9x9; seed + clue count go to stderr
sudoku generate --size 4 --show-solution        # 16x16, random seed
sudoku generate --size 6 --time-limit 2         # 36x36 in the number format, at most ~2 s
sudoku solve "53..7....6..195....98....6.8...6...34..8.3..17...2...6.6....28....419..5....8..79"
sudoku generate --seed 42 --pretty | sudoku solve --stats   # pretty output parses back
sudoku count ................ --limit 0         # all 288 4x4 grids
sudoku count puzzle.txt --limit 0 --node-limit 1000000      # bounded: exit 3 if cut short
sudoku show  "12........3...4."
sudoku solve puzzle.txt --trace-solver          # puzzle from a file; algorithm trace on stderr
sudoku generate --seed 42 --trace-generation --trace-limit 20
sudoku solve puzzle.txt --solver dlx --stats     # --solver auto (default) | mrv | naive | dlx
sudoku generate --difficulty medium --pretty   # graded by human techniques (see Difficulty)
sudoku rate puzzle.txt --trace-logic             # grade a puzzle and explain each deduction
```

`--solver` works with `solve`, `count` and `generate` (which uses it for its uniqueness
checks). `auto`, the default for `solve` and `count`, picks `mrv` or `dlx` per puzzle (see
[Algorithms](#algorithms)); `--stats` shows which one ran, e.g. `solver: auto (dlx)`.
`generate` keeps `mrv` as its default, so a seed still prints the same puzzle. `mrv` and
`naive` are the same backtracking search, branching on the cell with the fewest candidates or
on the next empty cell in row-major order; `dlx` is Knuth's Algorithm X on Dancing Links. DLX refuses (exit 3) a puzzle whose exact-cover matrix would need more than
its 256 MiB memory budget, e.g. a blank 144×144 grid.

**Puzzle formats.** Both formats are row-major, use `.` or `0` for an empty cell, and ignore
the box separators `| - +`. `parse` tells them apart by the cell count; the grid size is
inferred from it.

| Format | Cells | Grid sizes | Written by `toString()` for |
|---|---|---|---|
| One character per cell | `1-9` then `A-Z` (16×16 uses `1-9A-G`); whitespace ignored | any, values up to 35 | grids up to 25×25 |
| Numbers | decimal numbers without leading zeros, separated by whitespace or `| - +` | any | grids from 36×36 |

Inputs from earlier versions read exactly as before. Text is taken as numbers only when it is
n⁴ well-formed numbers, each at most n². The no-leading-zeros rule makes sure lines of digits
such as `0000…` in a 16×16 stay one character per cell. A puzzle argument that names an
existing file is read from that file.

**Exit codes:**
- `0`: ok.
- `1`: no solution.
- `2`: usage or parse error.
- `3`: a resource limit. A `--node-limit` or `--time-limit` stopped the search before it finished (the output says what is incomplete), or the grid is too large to represent (n⁴ cells must fit in an `int`) or to allocate. `generate --difficulty` also exits 3 when no attempt reached the level (the hardest graded puzzle is still printed, or the level is `unknown` if time ran out before any was graded), and `rate` when guessing hit `--guess-limit` or `--time-limit`.

## Algorithm tracing

`--trace-solver` (on `solve` and `count`) and `--trace-generation` (on `generate`)
print what the algorithms do, one line per decision. The trace goes to **stderr**, so
stdout, and anything piped from it, is unchanged. `--trace-limit n` caps the number of
event lines (default 1000; `0` = no limit). The closing summary is always printed,
together with how many events were left out.

```
$ sudoku count "1..............." --limit 2 --trace-solver
[solve] start: 4x4, 1 given, 15 empty cells
[d0  ] select r1c2: candidates {2,3,4} (MRV scanned 15 of 15 open cells)
[d0  ] try    r1c2 = 2
[d1  ] select r1c3: candidates {3,4} (MRV scanned 14 of 14 open cells)
[d1  ] try    r1c3 = 3
[d2  ] select r1c4: candidates {4} (forced; MRV scanned 1 of 13 open cells, stopped early)
...
[d15 ] solution #1 found after 15 nodes, 0 backtracks
[d14 ] undo   r4c4 = 1 (backtrack #1)
...
[solve] finished: stopped at the solution limit (2); 2 solutions, 18 search nodes, 4 backtracks

$ sudoku solve "12.. .... ..3. ..4." --trace-solver
[solve] start: 4x4, 4 givens, 12 empty cells
[d0  ] dead   r1c3: no candidates left
[solve] finished: search space exhausted, so the count is exact; 0 solutions, 1 search node, 0 backtracks
no solution
```

Each solver line shows:
- **`select`:** the cell MRV picked (`r`ow/`c`olumn, 1-based), its candidates, and how many open cells MRV scanned. It stops early at a forced cell.
- **`try` / `undo`:** placing a value, and taking it back to try something else (a backtrack).
- **`dead`:** a cell with no candidates left, so this branch fails.
- **`solution #k`:** a complete grid was found.
- **Summary line:** why the search ended (solution limit, node limit, time limit, search space exhausted or conflicting givens) and the node and backtrack counts.

`solve` asks for up to two solutions so it can warn about non-unique puzzles. Its trace therefore continues after solution #1, and the undos that follow are the search proving there is no second solution.

```
$ sudoku generate --size 2 --seed 5 --trace-generation
[generate] seed 5, 4x4: random complete grid (base pattern + random symbol, row and column permutations):
    3 4 | 2 1
    ...
[generate] removal 1/16: r1c4 = 1 -> removed, the other clues still force it (no search needed); 15 clues
...
[generate] removal 7/16: r2c1 = 1 -> kept, removing it allows a second solution (check: 10 nodes); 10 clues
...
[generate] removal 9/16: r3c4 = 2 -> removed, puzzle still unique (check: 8 nodes); 8 clues
...
[generate] finished: 4 clues left; 12 removed (9 forced, 3 checked by search), 4 kept (4 would allow a second solution, 0 uniqueness checks hit the node limit)
  puzzle:
    . . | . .
    1 2 | . .
    ...
  solution:
    ...
```

**How it works.** `trace.hpp` defines plain event structs and two one-method observer
interfaces, `SolverObserver` and `GeneratorObserver`. `SolveOptions::observer` and
`GeneratorOptions::observer` are `nullptr` by default. `text_trace.hpp` provides the
readable printers used by the CLI (`TextSolverTracer`, `TextGeneratorTracer`), which also
apply the event limit. Library users can plug in their own observer instead, as the
tests do to record events.

The solver's search is a class template on `bool kTraced`. The untraced instantiation
contains no tracing code, so normal solving costs the same as without tracing. A benchmark
with no observer showed no difference beyond run-to-run noise (about ±3%). A first version
with a runtime `if (observer)` check cost up to about 10% on the raw search.

## Architecture

```
include/sudoku/
  board.hpp               Board: any n^2 x n^2 grid; geometry, parsing (2 formats), printing, validity
  constraints.hpp         Constraints: row/column/box bitsets of used values, any width
  solver.hpp              Solver interface + SolveOptions / SolveResult / StopReason
  backtracking_solver.hpp MRVBacktrackingSolver and NaiveBacktrackingSolver: one iterative bitset search
  dlx_solver.hpp          DLXSolver: Algorithm X on Dancing Links, iterative, memory-budgeted
  auto_solver.hpp         AutoSolver: picks MRV or DLX per puzzle (box size, share of empty cells)
  logical_solver.hpp      CandidateGrid, the five human techniques, LogicalSolver (explainable steps)
  difficulty.hpp          Difficulty levels and rate(): lowest sufficient tier, controlled guessing
  generator.hpp           Generator: constructive complete grid + clue removal (+ difficulty target)
  random.hpp              portable seeded shuffle
  deadline.hpp            Deadline: one time budget shared by generation, grading and searches
  trace.hpp               optional observer interfaces and event structs
  text_trace.hpp          observers that print events as readable text (with an event limit)
src/                      implementations
app/cli.hpp, cli.cpp      CLI commands (argument parsing and I/O only), writing to given streams
app/main.cpp              forwards argv and the standard streams to the CLI
tests/
  test_framework.hpp      dependency-free test harness
  sudoku_test_helpers.hpp helpers shared by both test suites
  cli_test_helpers.hpp    runs CLI commands in-process for the unit tests
  test_main.cpp           unit tests: board, parsing, solvers, generation, tracing, CLI (CTest label "unit")
  test_difficulty.cpp     unit tests: logic techniques, grading, difficulty-targeted generation
  property_tests.cpp      randomized invariant tests (CTest labels "property", "slow")
benchmarks/               the solver study: sudoku_bench tool, fixtures, scripts, results, figures, reports
```

The core library (`sudoku_core`) is separate from the CLI and tests, so future
benchmarks, fuzzers or a GUI can link it directly. The CLI is a small library
too (`sudoku_cli`), so the tests can run commands in-process and check their
output and exit codes.

- **Board** is a plain value type (`std::vector<int>`, compared with `==`). It
  knows the geometry and the text formats, and holds no solver state.
  - Rows, columns, values and cell indices are `int`. The only size bound is that the n⁴ cells fit in an `int`: n ≤ 215, about 2.1 billion cells, far more than fits in memory.
  - The constructor checks this before allocating. It throws `std::length_error` if the grid can't be represented; `std::bad_alloc` means there isn't enough memory.
- **Constraints** keeps one bitset per row, column and box, as `⌈n²/64⌉` 64-bit words. The
  candidates of a cell are `full & ~(row | col | box)`, word by word. `Board::isValid()` and
  the solver both use it, so the Sudoku rule is written down in exactly one place.
  - Grids up to 64×64 need one word. The hot functions take a compile-time word count, so that path compiles to the same single-mask code as a fixed 64-bit design.
  - Larger grids use the run-time width.
- **Solver** is an abstract interface with a single `run(board, options)`.
  `solve`, `countSolutions` and `hasUniqueSolution` are thin wrappers over it.
  Implementations keep their search state inside `run`, so they are stateless
  and safe to share between threads.
- **AutoSolver** is a `Solver` too: it looks at the board, picks `MRVBacktrackingSolver` or
  `DLXSolver`, and returns that solver's result unchanged (see [Algorithms](#algorithms)).
- **Generator** depends only on the `Solver` interface (MRV backtracking by default), plus
  `rate()` when a difficulty target is set.
- **LogicalSolver** keeps its own `CandidateGrid`: one candidate bitmask per cell. Pair,
  locked-candidate and X-Wing eliminations remove single candidates, which `Constraints`'
  per-unit bitsets cannot store.

## Algorithms

**Solver: backtracking + MRV + bitsets.** The solver keeps a list of the empty
cells. At each node it scans the remaining cells for the one with the fewest
candidates (`std::popcount` over the bitset words). It stops scanning early at 0 (dead end)
or 1 (forced move). Then it tries each candidate (`std::countr_zero` over set bits),
updating the bitsets in O(1) on place and undo. Choosing the most constrained cell
first keeps the branching factor small: typical 9×9 puzzles take a few hundred
nodes and solve in well under a millisecond.

The depth-first search runs on an **explicit stack** instead of recursion. A 100×100 grid can
have thousands of empty cells, and one call-stack frame per cell could overflow the stack.
The iterative version makes the same decisions in the same order, with the same events and
counts, as the earlier recursive one.

**Naive backtracking** is the same engine with one change: it branches on the next empty cell
in row-major order instead of the most constrained one. It exists as a baseline for the
solver study.

**DLX: Knuth's Algorithm X on Dancing Links.** The puzzle becomes an exact-cover matrix: one
row per legal placement (cell, value), one column per open constraint (each cell filled; each
row, column and box holding each value). The search repeatedly branches on the column with
the fewest remaining rows. Because that includes "this value has only one place left in this
row" (a hidden single), DLX's search tree on hard puzzles can be thousands of times smaller
than MRV's. In exchange, each node costs more: choosing a column scans every open column. The
matrix needs about 92 bytes per placement, so DLX has a memory budget (256 MiB by default) and
refuses, with exit 3, a puzzle that would exceed it.

**Auto selection** (`AutoSolver`, the CLI default for `solve` and `count`). The solver study
found that MRV wins easy and medium puzzles at every size, and DLX wins hard ones, with the
crossover moving to lower sparsity as the grid grows. `AutoSolver` reads two facts off the
board before solving, the box size n and the share of empty cells, and picks DLX at or above
the threshold, MRV below it:

| Box size n (grid) | DLX from |
|---|---|
| 1–3 (up to 9×9) | never (MRV always) |
| 4 (16×16) | 55% empty |
| 5 (25×25) | 45% empty |
| 6–8 (36×36 to 64×64) | 35% empty |
| 9 and up (81×81 and larger) | 25% empty (from n = 12 an extrapolation) |

If DLX's matrix would not fit its memory budget, or the givens conflict, it uses MRV. On the
study's 645 benchmark puzzles that either solver finished, this rule solved all of them and
was never more than 4.2× slower than the better solver for that puzzle (always-MRV missed 24);
the thresholds were fitted to that same data. `generate` keeps MRV as its default so that a
seed prints the same puzzle as before.

`SolveOptions` controls the search:
- `solutionLimit`: stop after k solutions (0 = all). Uniqueness checks use 2.
- `nodeLimit`: abort after N nodes. Deterministic.
- `timeLimit`: abort after this much wall-clock time. It is checked every 256 nodes, so it may overshoot slightly, and the result depends on machine speed.
- `rng`: shuffle the candidate order.
- `observer`: optional algorithm tracing (see above).

`SolveResult::stopReason` says why the search ended: `Exhausted` (the count is exact),
`SolutionLimit`, `NodeLimit`, `TimeLimit` or `InvalidGivens`. `aborted` is set for the two
limits, when the count is only a lower bound. `nodes` and `backtracks` are machine-independent
cost metrics.

**Completed grids are constructive** (`randomCompletedGrid`). No search is needed:
1. Start from the standard pattern `value(r, c) = (n·(r mod n) + ⌊r/n⌋ + c) mod n²`. Every row, column and box contains each value once.
2. Apply random Sudoku-preserving moves: a symbol relabelling, row order within and across bands, column order within and across stacks, and an optional transpose.

This takes time proportional to the cell count: microseconds even at 100×100. Earlier
versions solved an empty grid in random order instead, which was heavy-tailed (some 16×16
seeds needed millions of nodes). The grids are not uniformly distributed over all Sudoku
grids, only over the images of one pattern under those moves, which is ample variety for
carving puzzles.

**Generator.**
1. Build a random completed grid as above.
2. Visit every cell in random order and try to remove its clue:
   - If the remaining clues leave the removed value as the cell's **only candidate**, every solution must put it back, so uniqueness holds without a search. This happens for most early removals.
   - Otherwise run `run(puzzle, {solutionLimit = 2})`. If the count isn't exactly 1, put the clue back.
3. Without a time limit, and when no check hits its node limit, the result is unique and *locally minimal*: removing any single remaining clue would create a second solution. `Puzzle::stats` records how every attempt ended, and `stats.minimal()` says whether this guarantee holds.

Each uniqueness check gets a node budget (`uniquenessNodeLimit`, default 20 000), because on
sparse large grids a check can explode. If a check exceeds it, the clue is conservatively
kept: the time is bounded, the puzzle can never become non-unique, and the cost is extra
clues. `GeneratorOptions::timeLimit` bounds the whole generation the same way. Clue removal
stops when time runs out, so the puzzle is still unique but may not be minimal. The CLI
prints a warning in both cases.

**Reproducibility.** `std::mt19937_64` is fully specified by the standard, but
`std::shuffle` and `std::uniform_int_distribution` are implementation-defined.
`random.hpp` therefore implements Fisher–Yates with rejection sampling, so a seed
gives the same puzzle on GCC, Clang and MSVC. The node budget is deterministic
too, so it doesn't affect reproducibility; a **time** limit does.

## Difficulty

Clue count is a poor measure of difficulty. Puzzles are graded instead by the techniques
a person needs to solve them:

| Level | Techniques | What it takes |
|---|---|---|
| Easy | naked singles, hidden singles | every placement follows from basic elimination |
| Medium | Easy + locked candidates, naked pairs | candidates must be eliminated first |
| Hard | Medium + X-Wing | a more advanced pattern |
| Expert | controlled backtracking when logic stalls | the techniques above cannot finish it |

**Techniques** (`logical_solver.hpp`; all work for any box size):
- **Naked single:** a cell has one candidate left.
- **Hidden single:** a value has one possible cell left in a row, column or box.
- **Locked candidates:**
  - pointing: a value's candidates in a box all lie in one row or column, so the value is
    removed from the rest of that line;
  - claiming: a value's candidates in a row or column all lie in one box, so it is removed
    from the rest of the box.
- **Naked pair:** two cells of a unit hold the same two candidates, so those values are
  removed from the unit's other cells.
- **X-Wing:** a value's candidates in two rows lie in the same two columns (or the transposed
  form), so it is removed from the rest of those columns.

**Grading** (`difficulty.hpp`).
- `LogicalSolver` always applies the easiest technique available, and starts again from the
  easiest after every step.
- Techniques only remove candidates, so a harder one is used only once every easier one has
  stalled. The tier of the hardest technique used is therefore the lowest tier that solves
  the puzzle.
- If the full set stalls, the puzzle is Expert. `rate()` then guesses in a controlled way:
  it takes the cell with the fewest candidates, tries each one on a copy and reruns full
  logic, recursing on further stalls. `RateOptions::guessLimit` bounds the guessing.
- Each `Deduction` records the pattern and what it proved. `TextLogicalTracer`
  (`--trace-logic`) prints them, e.g.
  `X-Wing on 7: rows 2,6 / columns 3,8; remove 7 from r1c3, r1c8, ...`.

**Generating for a level** (`GeneratorOptions::difficulty`, `--difficulty`):
- **Easy, Medium, Hard:** a clue is removed only if the level's techniques still solve the
  puzzle (`rate` with `cap`). Every deduction is sound, so this also proves uniqueness: no
  backtracking search is needed.
- **Expert:** clues are removed as usual (uniqueness by search).
- **Retries:** the finished puzzle is graded. If it missed the level, generation retries with
  `seed + 1, seed + 2, ...` up to `maxAttempts` (default 5000). The echoed seed reproduces the
  result in one attempt.
- **Grading a generated puzzle needs no guessing.** Carving proves the puzzle unique, so
  logic capped at Hard decides the grade: solved means Easy/Medium/Hard, stalled means Expert.
  (On an arbitrary board a stall could also mean "no solution", which is why `rate()` guesses.)
- **No match:** the hardest graded puzzle is returned with `targetReached = false`.
- **Time limit** (`--time-limit`, `GeneratorOptions::timeLimit`): one budget for the whole call.
  - **Where it is checked:** between clue removals, inside each uniqueness search, before every
    logic step of every rating, before every guess (`rate`), and between attempts.
  - **What it cannot interrupt:** one technique scan, building a candidate grid, or building
    the complete grid. Measured worst cases: about 5 ms on 49×49 and 30 ms on 100×100 (a full
    scan that finds nothing), under 2 ms to build a candidate grid. The overrun is bounded
    by these, plus process start-up for the CLI.
  - **An attempt the limit interrupts has no grade.** If an earlier attempt was graded, the
    best graded one is returned with its exact grade, and `Puzzle::timeLimitReached` is set.
    If none was, the interrupted puzzle is returned (unique, maybe not minimal) with
    `Puzzle::difficulty` empty, and the CLI prints `difficulty: unknown`. Both exit 3.
  - **`rate --time-limit`** stops grading the same way (exit 3), and `RateOptions::deadline`
    does it in the library.
- **Without `--difficulty`:** generation is byte-for-byte unchanged.

On 9×9 (carving about 5 ms per attempt; measured over 30 seeds per level, 20 for Hard):
- Easy reached the level on the first attempt every time; Medium in 2.6 attempts on
  average (at most 7); Expert in 2.9 (at most 6). Each takes about 0.1 s in total.
- Hard is rare: 161 attempts on average (at most 524), 0.9 s on average and up to 2.8 s.
  Removing a clue never lowers the grade, and these grids usually jump from Medium straight
  past Hard.

The techniques work on any grid size, but the tier frequencies above are for 9×9. On large
grids each check is slower, since a logic pass scans the whole grid per step. Median time of
`sudoku generate --difficulty L --time-limit 600` (Release build, seeds 1000, 2000, ...; 10
seeds on 9×9, 5 on 16×16, 3 on 25×25, 1 above; about 0.05 s of each is program start-up):

| Grid | Easy | Medium | Hard | Expert |
|---|---|---|---|---|
| 4×4 | 0.09 s | – | – | – |
| 9×9 | 0.09 s | 0.10 s | 1.1 s (0.2–4.1) | 0.09 s |
| 16×16 | 0.21 s | 0.34 s | 3.5 s (1.3–8.4) | 0.32 s |
| 25×25 | 2.1 s | 4.8 s | 5.0 s | 25 s (16–35) |
| 36×36 | 17 s | 36 s | 73 s | 61 s |
| 49×49 | 1.8 min | 4.2 min | 4.1 min | hit the 10-min limit |
| 64×64 | 7.9 min | hit the limit | hit the limit | not run |
| 81×81 | hit the limit | not run | not run | not run |

Chart: [interactive](benchmarks/reports/difficulty_generation_times.html) or [PNG](benchmarks/reports/difficulty_generation_times.png) (`python benchmarks/scripts/plot_difficulty_times.py` redraws it from the CSV). Time grows roughly tenfold per size step from 16×16 up. Expert stops first because on large
grids many uniqueness checks hit their node budget, so clues are kept and the attempts keep
grading easier than Expert.

**4×4 is always Easy.** An exhaustive check graded every uniquely solvable 4×4 puzzle
(13 579 680 of them, over all 288 grids): singles solve every one. `hardestPossible(boxSize)`
records this, and asking for a higher level on a 4×4 (or 1×1) grid is rejected at once
(`invalid_argument`; the CLI exits 2) instead of spending every attempt.

**Known limitations.**
- **Guessing copies grids.** `rate()` guesses depth-first with an explicit stack (no
  recursion, so no call-stack growth), but each level keeps one candidate-grid copy.
  `Rating::maxGuessDepth` reports the depth. An empty 36×36 board goes 1 029 levels deep
  and peaks at 21 MB; generated Expert puzzles need only a few levels.
- **One grid family.** Every completed grid is a transformed copy of one pattern grid (see
  `randomCompletedGrid`). The tier rates above describe puzzles carved from that family, not
  Sudoku in general.
- **Five techniques.** A puzzle that needs, say, a Swordfish is graded Expert.

## Limits and performance

Measured with MRV on the development machine (MSYS2 GCC 14.2, `-O2`), before the DLX and
auto solvers existed:

| Grid | Completed grid | Solve, 20% blank | Solve, 30% blank | Full generation |
|---|---|---|---|---|
| 9×9 | — | — | — | ~0.5 ms, avg 24 clues |
| 16×16 | — | — | — | ~0.2 s, avg 101 clues |
| 25×25 | < 0.1 ms | < 1 ms | < 1 ms | ~0.9 s; 268 of 625 checks hit the node limit |
| 36×36 | < 0.1 ms | < 1 ms | < 1 ms | ~4 s; 669 of 1296 checks hit the node limit |
| 64×64 | < 0.1 ms | < 1 ms | ~2 ms | not finished in 10 s: use `--time-limit` |
| 81×81 | < 0.1 ms | < 1 ms | over 2M nodes | not finished in 10 s: use `--time-limit` |
| 100×100 | < 0.1 ms | ~1 ms | over 2M nodes | not finished in 10 s: use `--time-limit` |

Plain MRV backtracking has no stronger propagation, such as hidden singles. On grids with
many blanks it can make a wrong early guess and then search an enormous subtree. With 60%
of cells blank, even 25×25 did not finish in 20 s. DLX, which `auto` picks for exactly those
puzzles, goes further, but not without limit: in the solver study no solver finished randomly
blanked grids at 30% empty or more from 144×144, 40% from 64×64, or 50% from 36×36, even with
30 minutes per puzzle ([performance.md](benchmarks/reports/performance.md)). That is why
solving and generation have explicit node and time limits, and why stopping is always
reported, never silent.

## Tests

`tests/test_framework.hpp` is a small, dependency-free harness:
- `TEST("Category", "description")` registers a test under a category, with a plain-English description;
- `CHECK` and `REQUIRE` (stops the current test);
- `CHECK_EQ` prints expected and actual values, and boards print as their grid string. For long values it also points at the first differing character;
- `CHECK_THROWS` also reports when the wrong exception type is thrown;
- `FAIL(...)` reports a custom message from a helper function;
- `CONTEXT(...)` attaches details such as the grid size, seed and generated puzzle to any failure in scope;
- `TRACE(label, ...)` records extra detail, such as boards and solver statistics, that only `--trace` shows. Its arguments aren't evaluated in other modes, so normal runs pay nothing for it.

By default the runner prints one line per category, then any failures in full, then a summary:

```
Running 80 tests in 8 categories

  PASS  Board        12/12 passed  (0.2 ms)
  PASS  Parsing      14/14 passed  (1.5 ms)
  FAIL  Generation   10/11 passed  (636.9 ms)
  ...

Failures:

  FAIL  Generation: 16x16 puzzles have exactly one solution (2 seeds)
        test_main.cpp:85
          CHECK_EQ(result.solutionCount, 1u)
            expected: 1
            actual:   2
          with grid: 16x16 (box 4), seed: 7
          with puzzle: G1.2..9.... (103 clues)

------------------------------------------------------------
FAILED  79 passed, 1 failed, 80 total  (2.02 s)
```

| Command | Effect |
|---|---|
| `sudoku_tests` | concise run |
| `sudoku_tests -v` | every test with its duration, failures shown inline |
| `sudoku_tests solver` | only tests whose "Category: description" contains the text (case-insensitive) |
| `sudoku_tests --list` | list tests without running them |
| `sudoku_tests -t "large grids:"` | trace a category: every check with its result, plus inputs, boards and solver statistics |
| `sudoku_tests -t "classic 9x9"` | trace a single test by part of its description |
| `sudoku_tests -t --full 16x16` | trace with whole grids and no per-test entry limit |

`--trace` shows each test's checks in order:
- **Passing checks:** each one is listed, with expected and actual values for comparisons.
- **Inputs:** parse inputs are shown with the error that was expected and the message actually thrown.
- **Generation:** the seed, generated puzzle, completed solution and uniqueness check.
- **Solving:** the input puzzle, the solution and the solver statistics (solutions found and search nodes).

A context line such as `[grid: 4x4 (box 2), seed: 3]` appears whenever the context changes. Grids up to 9×9 are drawn in full. Larger grids, long values and long inputs are shortened, and each test shows at most 40 entries; failures are never hidden. `--full` lifts these limits.

Solving tests that call `solve()` get their statistics from a second solver run that happens only under `--trace`. The solver itself has no logging.

The exit code is 0 only when every selected test passes. It is 1 if any fail or none match, and 2 for a bad option. CTest relies on this.

`sudoku_tests` has 131 tests in twelve categories (the Solvers category, cross-solver checks from Step 9/10A, and the AutoSolver category, which checks the thresholds, the per-puzzle choice, identical results to the chosen solver and the memory fallback, are not itemised here). The logic, difficulty and difficulty-generation tests live in `tests/test_difficulty.cpp`, the rest in `tests/test_main.cpp`; both register into one runner:
- **Board:**
  - geometry and range checks for box sizes 1 to 10;
  - box size 0 or negative rejected;
  - sizes whose n⁴ cells overflow an `int` rejected with `std::length_error` before allocating;
  - row, column and box conflicts.
- **Parsing:**
  - empty markers, size inference (1×1 to 36×36), separators, letters up to `P` (25);
  - the number format: multi-digit values, small grids agreeing with the one-character format, too-large values, and leading zeros keeping digit lines in the one-character format;
  - output format per size;
  - each kind of invalid input;
  - round-trips with every value of 25×25, 36×36, 64×64 and 100×100 grids.
- **Solver:**
  - the known 9×9 solution; given cells preserved; empty grids up to 25×25;
  - dead ends and conflicting givens;
  - exact counts (288 grids for an empty 4×4, 72 completions with one given);
  - solution and node limits; randomized order reproducible per seed.
- **Randomness:** `uniformBelow` stays in range and covers it; `shuffle` produces a permutation.
- **Generation:**
  - uniqueness for 4×4, 9×9 and 16×16 across several seeds;
  - local minimality, with an unbounded check budget;
  - reproducibility;
  - recorded puzzles for fixed seeds, which pin the promise that a seed gives the same puzzle on every compiler;
  - an injected `Solver` actually being used;
  - formerly slow 16×16 seeds;
  - 1×1 generation;
  - invalid or unrepresentable sizes rejected.
- **Large grids:**
  - constructive grids valid for box sizes 1 to 10;
  - bitsets across word boundaries (64×64 in 1 word, 81×81 and 100×100 in 2);
  - solving 20%-blank 36×36, 64×64, 81×81 and 100×100 puzzles;
  - a node limit and a time limit stopping a search and reporting why;
  - time-limited 64×64 and 100×100 generation staying unique and reporting that it stopped;
  - the 1×1 solve.
- **Tracing:**
  - results are identical with and without an observer;
  - events account for every node, placement, backtrack and solution;
  - dead ends are reported at the right cell;
  - the finish reason is correct;
  - the generator reports one removal per cell, with consistent verdicts and stats;
  - the text tracer respects the event limit (and `0` = no limit);
  - tracing is off by default.
- **CLI:**
  - `generate`, `solve` and malformed input, run in-process, checking stdout, stderr and the exit code;
  - the trace flags write only to stderr and leave stdout identical;
  - `--trace-limit` validation;
  - reading a puzzle from a file;
  - 36×36 generation with `--time-limit` and its warning;
  - exit code 3 for `--node-limit` stops and for grids too large to represent;
  - invalid limits rejected.
  - `--solver` for each solver, `auto` as the default for `solve` and `count` (and `--stats` naming the solver it picked), `generate` keeping `mrv`.
  - `generate --difficulty` / `--attempts` and `rate`: output, the missed-target warning and exit 3, guess limit, unknown levels, `--time-limit` (unknown grade; `rate` stopped).
- **Logic:**
  - each technique finder on a hand-carved candidate grid: exact placement or eliminations, plus a near miss that must find nothing;
  - pointing and claiming, both X-Wing orientations;
  - the classic puzzle solved by singles; contradictions; Inkala's puzzle stalls with every deduction consistent with its solution;
  - the `TextLogicalTracer` format and event budget.
- **Difficulty:**
  - two fixture puzzles per level, each graded correctly with the right solution and technique;
  - lowest sufficient tier: one tier lower stalls, the grade's tier solves;
  - `cap`, the guess limit, contradictions, and an ambiguous grid;
  - deadlines: an expired deadline stops the logic solver and `rate()` with no grade, also part-way through guessing;
  - guess counts and steps of the Expert fixtures pinned; deep guessing on an empty 25×25 board.
  - Generation (category Generation): each level on a fixed seed is graded exactly that and unique by MRV and DLX; a missed target returns the hardest puzzle found; levels above Easy are rejected on 4×4 and 1×1; time limit before any attempt is graded (unique, grade unknown) and after a graded attempt (exact grade kept), made deterministic with `Deadline::afterChecks`.
- **Malformed boards** (category Board): values outside 0..size stored with `setAt` are rejected by `isValid`, all three solvers, `CandidateGrid::fromBoard` and `rate()`; `CandidateGrid::place` checks its arguments.

Comparisons between different seeds use fixed seeds, so they are deterministic rather than flaky.
CTest also runs the real `sudoku` executable for five CLI cases (including `generate --difficulty hard` and `rate`), to cover `main()` and argv handling.

### Property tests

`tests/property_tests.cpp` checks invariants over many seeded random inputs. It is a
separate executable labelled `property` and `slow` in CTest. At the default scale it runs
18 property tests (about 12 300 cases) in about 30 s (Release build):

| Category | Property | Sizes (cases) |
|---|---|---|
| Generation | puzzle valid, clues ⊆ solution, exactly one solution, equal to the generator's | 4×4 (1000), 9×9 (200), 16×16 (10), 25×25 (3), 36×36 (1) |
| Generation | same seed ⇒ same puzzle and solution | 4×4 (300), 9×9 (50) |
| Generation | constructive completed grids are complete and valid | 1×1 to 100×100 (1413) |
| Transforms | transformed complete grids stay complete and valid, and a conflict stays a conflict | 4×4 to 100×100 (1585) |
| Transforms | a transformed unique puzzle stays unique, and its solution is the transformed solution | 4×4 (300), 9×9 (100), 16×16 (5) |
| Transforms | transforms preserve the solution count of ambiguous puzzles | 4×4 (500), 9×9 (100) |
| Solver | a duplicated value ⇒ invalid, 0 solutions, no search | 4×4 to 100×100 (855) |
| Solver | a wrong but locally allowed value in a unique puzzle ⇒ 0 solutions | 4×4 (300), 9×9 (100), 16×16 (5) |
| Solver | counts on random (also conflicting) 4×4 givens equal a brute-force oracle of all 288 grids | 4×4 (2000) |
| Solver | answers on random 9×9 givens are self-consistent | 9×9 (500) |
| Solver | 20%-blank grids solve to completions of their givens | 25×25 to 100×100 (90) |
| Parsing | `toString`, `toPrettyString`, lower case and random whitespace/separator noise all parse back | 4×4 to 100×100 (1278) |
| Logic | no placement or elimination by any technique contradicts the unique solution | 4×4 (500), 9×9 (150), 16×16 (5) |
| Difficulty | `rate()` finds the unique solution; guesses ⇔ Expert; capping at the grade solves, one tier lower is `AboveCap`; the generator's guess-free grade (cap Hard, stall = Expert) equals `rate()`'s | 4×4 (500), 9×9 (150) |
| Difficulty | puzzles generated for a level are graded exactly that and unique by MRV and DLX | 4×4 easy (200); 9×9 easy, medium, expert (30 each), hard (3) |

Notes:
- Transforms combine a symbol permutation, row swaps within bands, band swaps, column swaps within stacks, stack swaps and an optional transpose.
- Up to 16×16, the complete grids in the transform and duplicate properties come from a randomized solve, not from the library's constructive builder, so those properties don't only see grids the library built itself.

```bash
sudoku_property_tests                                   # default: 1x cases, base seed 20261103
SUDOKU_PROPERTY_SCALE=0.1 sudoku_property_tests         # quick smoke run
SUDOKU_PROPERTY_SCALE=10 SUDOKU_PROPERTY_SEED=1 sudoku_property_tests   # soak with other seeds
sudoku_property_tests -t "transform" --full             # see every case's inputs
```

Case *i* of each property uses seed `base + i`. A property stops at its first failing
case, and the failure lists:
- the property, grid size and case seed;
- the input puzzle, and the transform where one was applied;
- a `sudoku generate --size n --seed s` command for generation properties;
- a `SUDOKU_PROPERTY_SEED=<case seed>` hint that reruns that case first.

## Limitations

- **No practical-time promise for large or hard puzzles.** See the table in [Limits and performance](#limits-and-performance). Use node and time limits; exit code 3 and `StopReason` report a cut-short search.
- **Large puzzles are often not minimal.** From 25×25 up, many uniqueness checks hit their node budget, so the clue is kept, and from 64×64 up full generation needs a time limit. `Puzzle::stats` and the CLI warnings say so.
- **The size bound is the index type.** n⁴ cells must fit in an `int`, so n ≤ 215. Memory runs out long before that.
- **Inconclusive property cases.** Some 16×16 contradiction proofs, and large-grid solves, are capped at a node limit; a case that hits the cap is counted as inconclusive rather than failed. The trace (`-t --full`) reports how many. None occurred at the default scale.
- **Traces can be long.** One line is printed per search decision, so traces of hard searches grow quickly; use `--trace-limit`. Values are printed as numbers (`= 12`), not as their letters (`C`).
- **Backtracks after the first solution.** `SolveResult::backtracks` counts every placement undone so the search could continue. When the solution limit is above 1, that includes the unwinding after a solution is found.

## Solver study

A reproducible comparison of the three solvers: box sizes 1–15, 10–70% empty, two puzzle
populations, about 2,800 benchmark runs, plus profiling and a 30-minute stress tier.
[benchmarks/README.md](benchmarks/README.md) holds the protocol and the results index;
the conclusions are in [benchmarks/reports/performance.md](benchmarks/reports/performance.md):
- **MRV** is fastest or tied on easy and medium puzzles at every size.
- **DLX** is 28× slower in the median on easy puzzles, because choosing a column scans every
  open constraint at every node, but it wins hard puzzles through a far smaller search tree.
  It finished the most hard runs and every generated unique puzzle.
- **Naive** keeps up only while the search barely backtracks.
- Past a sparsity wall that moves lower as the grid grows, no solver finishes, even with
  30 minutes per puzzle.

[fig08_best_solver.png](benchmarks/figures/core/fig08_best_solver.png) maps the best solver
for each grid size and sparsity; `AutoSolver` (see [Algorithms](#algorithms)) is built from it.
Optimisation ideas from the profiling (for example, stopping DLX's column scan at a column with
one row) are documented in performance.md §5 and not implemented.

## Status and possible extensions

**Done** (the sections describe the current code):
- three interchangeable solvers (`--solver`) with cross-solver tests: [Algorithms](#algorithms), [Tests](#tests);
- automatic MRV/DLX selection per puzzle, the default for `solve` and `count`: [Algorithms](#algorithms);
- human-technique difficulty grading and targeted generation: [Difficulty](#difficulty);
- a reproducible solver benchmark and its conclusions: [Solver study](#solver-study);
- the test suites also ran under an undefined-behaviour-sanitizer build (`-fsanitize=undefined`)
  and a Debug build with assertions; those builds are archived in `old_builds/`.

**Possible extensions** (not implemented):
1. **More techniques:** hidden pairs, naked triples, Swordfish, chains. Each is a new finder
   in `logical_solver.cpp` and a tier.
2. **Grid diversity:** completed grids from the randomized MRV solver (`SolveOptions::rng`)
   instead of one pattern family, opt-in, with a total budget (node limit per try, a maximum
   number of restarts, the generation deadline) and a fallback to the pattern grid. It would
   change seeded outputs and tier rates, so it needs its own evaluation.
3. **Fuzzing:** `Board::parse` is a natural fuzz target.
4. **Faster solvers,** from the solver study's profiling (performance.md §5): stop DLX's
   column scan at a column with one row, keep columns bucketed by size, and give MRV incremental
   candidate counts or hidden-single detection. Each needs its own before/after benchmark, and
   the `AutoSolver` thresholds would need re-measuring afterwards.
5. **Benchmark the `LogicalSolver`** with the same harness (deferred during Step 10).
6. **Parallel generation.** `Generator::generate` is `const`, uses no globals,
   and seeds its own RNG. A thread pool can call it concurrently with seeds
   `base + i` and get the same results as a serial run.
