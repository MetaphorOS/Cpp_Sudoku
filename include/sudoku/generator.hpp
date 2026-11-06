#pragma once

#include "sudoku/board.hpp"
#include "sudoku/deadline.hpp"
#include "sudoku/difficulty.hpp"
#include "sudoku/random.hpp"
#include "sudoku/solver.hpp"

#include <chrono>
#include <cstdint>
#include <optional>

namespace sudoku {

class GeneratorObserver;  // trace.hpp

struct GeneratorOptions {
    int boxSize = 3;
    std::uint64_t seed = 0;
    // Budget for each uniqueness check. If a check exceeds it, the clue is kept,
    // which bounds generation time on large grids without ever risking a
    // non-unique puzzle. 0 means unbounded.
    std::uint64_t uniquenessNodeLimit = 20'000;
    // If set, receives the filled grid, every clue-removal attempt and the result (see
    // trace.hpp). Off by default; it never changes the generated puzzle.
    GeneratorObserver* observer = nullptr;
    // Wall-clock budget for the whole call; 0 means unbounded. It is checked between clue
    // removals, by each uniqueness search, and (with a difficulty target) before each logic
    // step of every rating and between attempts. It is not checked inside a single logic step,
    // while building a candidate grid, or while building the complete grid, so the call can
    // overrun by the longest of those (see README "Difficulty"). A puzzle cut short is still
    // unique (every removal was verified) but may not be minimal, and unlike the seed alone
    // the result then depends on machine speed. See Puzzle::stats and Puzzle::timeLimitReached.
    std::chrono::milliseconds timeLimit{0};
    // Used instead of timeLimit when bounded: an absolute deadline, shared with other work, or
    // in tests Deadline::afterChecks for a deterministic "time limit" (a check-counted deadline
    // does not bound the uniqueness search, which needs clock time).
    Deadline deadline{};
    // If set, generate a puzzle graded exactly this difficulty (see difficulty.hpp):
    //  - Easy/Medium/Hard: a clue is removed only if the target's techniques still solve the
    //    puzzle. Logic alone proves uniqueness, so no search is needed.
    //  - Expert: clues are removed as without a target (uniqueness by search).
    // The finished puzzle is graded by logic alone (carving proved it unique, so a puzzle the
    // techniques cannot finish is Expert; no guessing is needed). If it misses the target,
    // generation starts over with seed + 1, seed + 2, ... up to maxAttempts attempts, all within
    // timeLimit. If the time limit stops an attempt before it is graded, that attempt's grade is
    // unknown: see Puzzle::difficulty. Without a target the result is exactly as before.
    std::optional<Difficulty> difficulty{};
    // At least 1. Hard is rare: on 9x9 about 1 attempt in 160 reaches it.
    int maxAttempts = 5000;
};

// How each cell's removal attempt ended.
struct GenerationStats {
    int cellsTried = 0;           // removal attempts made: every cell, unless the time limit hit
    int removedBySearch = 0;      // the uniqueness check found exactly one solution
    int removedForced = 0;        // the remaining clues still force the value; no search needed
    int keptAmbiguous = 0;        // removing it would allow a second solution
    int keptUnverified = 0;       // the uniqueness check hit its node limit
    int removedByLogic = 0;       // difficulty target: the target's techniques still solve the puzzle
    int keptTooHard = 0;          // difficulty target: removing it needs harder techniques
    bool timeLimitReached = false;  // stopped before trying every cell

    // Every kept clue is necessary: no single one can be removed without losing uniqueness
    // (or, with a difficulty target, without exceeding the target).
    [[nodiscard]] bool minimal() const noexcept { return !timeLimitReached && keptUnverified == 0; }
};

struct Puzzle {
    Board puzzle;
    Board solution;
    // The seed that produced this puzzle. With a difficulty target this is the seed of the
    // successful attempt, so generating with it again needs only one attempt.
    std::uint64_t seed = 0;
    GenerationStats stats;
    // With a difficulty target: the exact grade of the returned puzzle. Empty without a target,
    // and also when the time limit ran out before any attempt was fully graded: the returned
    // puzzle is then the interrupted attempt (unique, maybe not minimal) and its grade unknown.
    std::optional<Difficulty> difficulty{};
    int attempts = 1;  // attempts made (with a target)
    // False if no attempt hit the target; the puzzle returned is then the hardest fully graded
    // one (an attempt interrupted by the time limit is discarded), or the interrupted one if
    // none was graded. Always unique.
    bool targetReached = true;
    // The call stopped on the time limit. Differs from stats.timeLimitReached (the returned
    // puzzle's own carving was cut short) when an earlier, complete attempt is returned.
    bool timeLimitReached = false;
};

// A random complete grid, built constructively in O(cells): the standard pattern
// value(r, c) = (n * (r mod n) + r / n + c) mod n^2, then a random symbol relabelling,
// random row order within and across bands, random column order within and across stacks,
// and an optional transpose. These moves preserve validity, so no search is needed. The
// grids are not uniformly distributed over all Sudoku grids: they are the random images of
// one pattern, which is enough variety to carve puzzles from.
[[nodiscard]] Board randomCompletedGrid(int boxSize, Rng& rng);

// Produces random puzzles with exactly one solution:
//   1. build a random complete grid (randomCompletedGrid),
//   2. visit cells in random order, removing each clue if the puzzle stays unique
//      (and, with a difficulty target, stays within it),
//   3. with a difficulty target, grade the result and retry with the next seed if it missed.
// Without a time limit and with no check hitting its node limit, the result is locally
// minimal: no remaining clue can be removed on its own (see GenerationStats::minimal).
// generate() is const and keeps all state local, so one Generator can serve
// several threads, each generating with its own seed.
class Generator {
public:
    Generator();  // uses MRVBacktrackingSolver
    explicit Generator(const Solver& solver) : solver_(&solver) {}  // solver must outlive *this

    [[nodiscard]] Puzzle generate(const GeneratorOptions& options) const;

private:
    const Solver* solver_;
};

}  // namespace sudoku
