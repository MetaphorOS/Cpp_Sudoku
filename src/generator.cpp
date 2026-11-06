#include "sudoku/generator.hpp"

#include "sudoku/backtracking_solver.hpp"
#include "sudoku/constraints.hpp"
#include "sudoku/random.hpp"
#include "sudoku/trace.hpp"

#include <algorithm>
#include <numeric>
#include <optional>
#include <stdexcept>
#include <utility>
#include <vector>

namespace sudoku {

namespace {


const Solver& defaultSolver() {
    static const MRVBacktrackingSolver solver;  // stateless, so sharing it is thread-safe
    return solver;
}

// A permutation of 0..n*n-1 that keeps each block of n consecutive indices together: the blocks
// (bands or stacks) are shuffled, and so are the indices within each block.
std::vector<int> blockPermutation(int n, Rng& rng) {
    std::vector<int> blocks(static_cast<std::size_t>(n));
    std::iota(blocks.begin(), blocks.end(), 0);
    shuffle(std::span(blocks), rng);
    std::vector<int> permutation;
    permutation.reserve(static_cast<std::size_t>(n) * static_cast<std::size_t>(n));
    std::vector<int> within(static_cast<std::size_t>(n));
    for (const int block : blocks) {
        std::iota(within.begin(), within.end(), 0);
        shuffle(std::span(within), rng);
        for (const int offset : within) permutation.push_back(block * n + offset);
    }
    return permutation;
}


}  // namespace

Board randomCompletedGrid(int boxSize, Rng& rng) {
    Board grid(boxSize);  // validates the size first
    const int n = boxSize;
    const int size = grid.size();

    std::vector<int> symbols(static_cast<std::size_t>(size));
    std::iota(symbols.begin(), symbols.end(), 1);
    shuffle(std::span(symbols), rng);
    const std::vector<int> rows = blockPermutation(n, rng);
    const std::vector<int> cols = blockPermutation(n, rng);
    const bool transpose = uniformBelow(rng, 2) == 1;

    for (int r = 0; r < size; ++r) {
        for (int c = 0; c < size; ++c) {
            int sourceRow = rows[static_cast<std::size_t>(r)];
            int sourceCol = cols[static_cast<std::size_t>(c)];
            if (transpose) std::swap(sourceRow, sourceCol);
            // Each row is a shift of the first; the shift n * (r mod n) + r / n makes every
            // column and every box contain each value exactly once.
            const int pattern = ((sourceRow % n) * n + sourceRow / n + sourceCol) % size;
            grid.setAt(r * size + c, symbols[static_cast<std::size_t>(pattern)]);
        }
    }
    return grid;
}

Generator::Generator() : solver_(&defaultSolver()) {}

namespace {

struct Attempt {
    Board puzzle;
    Board solution;
    GenerationStats stats;
};

// One carving pass from the grid of `seed`: tries every cell once in random order and removes
// its clue if the puzzle stays unique and, with a target below Expert, still solvable by the
// target's techniques.
Attempt carve(const Solver& solver, const GeneratorOptions& options, std::uint64_t seed, const Deadline& deadline) {
    using Kind = GeneratorEvent::Kind;
    using Verdict = GeneratorEvent::Verdict;
    GeneratorObserver* const observer = options.observer;
    const std::optional<Difficulty> target = options.difficulty;
    const bool byLogic = target && *target != Difficulty::Expert;
    Rng rng(seed);

    const Board full = randomCompletedGrid(options.boxSize, rng);
    if (observer) {
        observer->onGeneratorEvent(
            {.kind = Kind::FilledGrid, .seed = seed, .clues = full.cellCount(), .board = &full, .target = target});
    }

    std::vector<int> order(static_cast<std::size_t>(full.cellCount()));
    std::iota(order.begin(), order.end(), 0);
    shuffle(std::span(order), rng);

    Board puzzle = full;
    Constraints constraints = *Constraints::fromBoard(full);  // tracks the current puzzle's clues
    GenerationStats stats;
    int clues = puzzle.cellCount();
    for (std::size_t i = 0; i < order.size(); ++i) {
        if (deadline.expired()) {
            stats.timeLimitReached = true;
            break;
        }
        const int cell = order[i];
        const int row = cell / puzzle.size();
        const int col = cell % puzzle.size();
        const int value = puzzle.at(cell);
        puzzle.setAt(cell, 0);
        constraints.remove(row, col, value);

        SolveResult check;
        Verdict verdict = Verdict::Forced;
        // If the other clues leave `value` as the cell's only candidate, every solution of the
        // new puzzle has `value` there, so it is still unique: no search needed. It is also a
        // naked single, so removing it never raises the difficulty.
        if (constraints.candidateCount(row, col) != 1) {
            if (byLogic) {
                const RatingStatus status = rate(puzzle, {.cap = *target, .deadline = deadline}).status;
                verdict = status == RatingStatus::Solved      ? Verdict::SolvedByLogic
                          : status == RatingStatus::TimeLimit ? Verdict::TimeLimit
                                                              : Verdict::TooHard;
            } else {
                check = solver.run(puzzle, {.solutionLimit = 2,
                                            .nodeLimit = options.uniquenessNodeLimit,
                                            .timeLimit = deadline.remaining()});
                if (check.stopReason == StopReason::TimeLimit) {
                    verdict = Verdict::TimeLimit;
                } else if (check.stopReason == StopReason::NodeLimit) {
                    verdict = Verdict::CheckAborted;
                } else {
                    verdict = check.solutionCount == 1 ? Verdict::Accepted : Verdict::MultipleSolutions;
                }
            }
        }

        const bool removed =
            verdict == Verdict::Accepted || verdict == Verdict::Forced || verdict == Verdict::SolvedByLogic;
        if (removed) {
            --clues;
        } else {
            puzzle.setAt(cell, value);
            constraints.place(row, col, value);
        }
        ++stats.cellsTried;
        switch (verdict) {
            case Verdict::Accepted: ++stats.removedBySearch; break;
            case Verdict::Forced: ++stats.removedForced; break;
            case Verdict::MultipleSolutions: ++stats.keptAmbiguous; break;
            case Verdict::CheckAborted: ++stats.keptUnverified; break;
            case Verdict::TimeLimit: stats.timeLimitReached = true; break;
            case Verdict::SolvedByLogic: ++stats.removedByLogic; break;
            case Verdict::TooHard: ++stats.keptTooHard; break;
        }
        if (observer) {
            observer->onGeneratorEvent({.kind = Kind::Removal,
                                        .seed = seed,
                                        .index = static_cast<int>(i) + 1,
                                        .total = static_cast<int>(order.size()),
                                        .row = row,
                                        .col = col,
                                        .value = value,
                                        .verdict = verdict,
                                        .solutions = check.solutionCount,
                                        .nodes = check.nodes,
                                        .nodeLimit = options.uniquenessNodeLimit,
                                        .clues = clues,
                                        .target = target});
        }
        if (verdict == Verdict::TimeLimit) break;
    }
    return Attempt{.puzzle = std::move(puzzle), .solution = full, .stats = stats};
}

// The exact grade of a puzzle known to have exactly one solution (carving proves it), by
// logic alone: Medium/Hard/Easy if the techniques up to Hard solve it, otherwise Expert, since
// a solvable puzzle that every technique stalls on needs guessing. Not valid for arbitrary
// boards: there AboveCap could also mean an unsolvable puzzle logic has not disproved, which is
// why rate() guesses. Empty if the deadline expired before the grade was known.
std::optional<Difficulty> gradeUnique(const Board& puzzle, const Deadline& deadline) {
    const Rating rating = rate(puzzle, {.cap = Difficulty::Hard, .deadline = deadline});
    switch (rating.status) {
        case RatingStatus::Solved: return rating.difficulty;
        case RatingStatus::AboveCap: return Difficulty::Expert;
        default: return std::nullopt;  // TimeLimit (NoSolution cannot happen for a unique puzzle)
    }
}

}  // namespace

Puzzle Generator::generate(const GeneratorOptions& options) const {
    using Kind = GeneratorEvent::Kind;
    GeneratorObserver* const observer = options.observer;
    const std::optional<Difficulty> target = options.difficulty;
    if (target && options.maxAttempts < 1) throw std::invalid_argument("maxAttempts must be at least 1");
    if (target && *target > hardestPossible(options.boxSize)) {
        const int n = options.boxSize * options.boxSize;
        throw std::invalid_argument("no " + std::to_string(n) + "x" + std::to_string(n) + " puzzle can be graded " +
                                    std::string(name(*target)) + ": singles solve every one, so the hardest level is " +
                                    std::string(name(hardestPossible(options.boxSize))));
    }

    const Deadline deadline = options.deadline.bounded() ? options.deadline : Deadline::after(options.timeLimit);

    Puzzle result;
    if (!target) {
        Attempt attempt = carve(*solver_, options, options.seed, deadline);
        result = Puzzle{.puzzle = std::move(attempt.puzzle),
                        .solution = std::move(attempt.solution),
                        .seed = options.seed,
                        .stats = attempt.stats,
                        .timeLimitReached = attempt.stats.timeLimitReached};
    } else {
        // Keep the attempt that hits the target, or else the hardest fully graded one (the first
        // on ties). An attempt the time limit interrupts has no known grade: it is kept only if
        // no attempt was graded before it.
        std::optional<Attempt> best;
        Difficulty bestGrade = Difficulty::Easy;
        std::uint64_t bestSeed = options.seed;
        std::optional<Attempt> interrupted;
        std::uint64_t interruptedSeed = options.seed;
        bool stoppedByTimeLimit = false;
        int attempts = 0;
        while (attempts < options.maxAttempts) {
            if (attempts > 0 && deadline.expired()) {
                stoppedByTimeLimit = true;
                break;
            }
            const std::uint64_t seed = options.seed + static_cast<std::uint64_t>(attempts);
            ++attempts;
            Attempt attempt = carve(*solver_, options, seed, deadline);
            const std::optional<Difficulty> grade =
                attempt.stats.timeLimitReached ? std::nullopt : gradeUnique(attempt.puzzle, deadline);
            if (observer) {
                observer->onGeneratorEvent({.kind = Kind::Rated,
                                            .seed = seed,
                                            .index = attempts,
                                            .total = options.maxAttempts,
                                            .clues = attempt.puzzle.clueCount(),
                                            .target = target,
                                            .difficulty = grade.value_or(Difficulty::Easy),
                                            .graded = grade.has_value()});
            }
            if (!grade) {  // the time limit ran out during this attempt
                stoppedByTimeLimit = true;
                if (!best) {
                    interrupted = std::move(attempt);
                    interruptedSeed = seed;
                }
                break;
            }
            if (!best || *grade == *target || *grade > bestGrade) {
                best = std::move(attempt);
                bestGrade = *grade;
                bestSeed = seed;
            }
            if (bestGrade == *target) break;
        }
        if (best) {
            result = Puzzle{.puzzle = std::move(best->puzzle),
                            .solution = std::move(best->solution),
                            .seed = bestSeed,
                            .stats = best->stats,
                            .difficulty = bestGrade,
                            .attempts = attempts,
                            .targetReached = bestGrade == *target,
                            .timeLimitReached = stoppedByTimeLimit};
        } else {
            result = Puzzle{.puzzle = std::move(interrupted->puzzle),
                            .solution = std::move(interrupted->solution),
                            .seed = interruptedSeed,
                            .stats = interrupted->stats,
                            .difficulty = std::nullopt,
                            .attempts = attempts,
                            .targetReached = false,
                            .timeLimitReached = true};
        }
    }

    if (observer) {
        observer->onGeneratorEvent({.kind = Kind::Finished,
                                    .seed = result.seed,
                                    .clues = result.puzzle.clueCount(),
                                    .timeLimitReached = result.stats.timeLimitReached,
                                    .board = &result.puzzle,
                                    .solution = &result.solution,
                                    .stats = &result.stats,
                                    .target = target,
                                    .difficulty = result.difficulty.value_or(Difficulty::Easy),
                                    .graded = result.difficulty.has_value(),
                                    .attempts = result.attempts,
                                    .targetReached = result.targetReached,
                                    .stoppedByTimeLimit = result.timeLimitReached});
    }
    return result;
}

}  // namespace sudoku
