#include "fixtures.hpp"

#include "sudoku/backtracking_solver.hpp"
#include "sudoku/constraints.hpp"
#include "sudoku/dlx_solver.hpp"
#include "sudoku/generator.hpp"
#include "sudoku/random.hpp"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <limits>
#include <numeric>
#include <optional>
#include <span>
#include <stdexcept>

namespace bench {

namespace {

using Clock = std::chrono::steady_clock;
using sudoku::Board;

std::string padded(int value, int width) {
    std::string text = std::to_string(value);
    return std::string(static_cast<std::size_t>(std::max(0, width - static_cast<int>(text.size()))), '0') + text;
}

// Seeds derived from the fixture's identity, so every fixture is reproducible on its own.
std::uint64_t gridSeedOf(const FixtureSpec& spec) {
    const std::string key = spec.population + "|" + std::to_string(spec.n) + "|" + std::to_string(spec.empty) + "|" +
                            std::to_string(spec.seedIndex);
    return splitmix64(fnv1a(key));
}
std::uint64_t orderSeedOf(std::uint64_t gridSeed, int attempt) {
    return splitmix64(gridSeed ^ (0x5bd1e995ULL * static_cast<std::uint64_t>(attempt + 1)));
}

// A complete grid from a randomized MRV fill with restarts (the property tests' method).
Board solverFilledGrid(int n, sudoku::Rng& rng) {
    const Board empty(n);
    const auto budget = static_cast<std::uint64_t>(10 * empty.cellCount());
    for (int attempt = 0; attempt < 10'000; ++attempt) {
        auto grid = sudoku::MRVBacktrackingSolver().run(empty, {.nodeLimit = budget, .rng = &rng}).firstSolution;
        if (grid) return *grid;
    }
    throw std::runtime_error("could not fill an empty grid");
}

std::vector<int> shuffledCells(int count, std::uint64_t seed) {
    std::vector<int> cells(static_cast<std::size_t>(count));
    std::iota(cells.begin(), cells.end(), 0);
    sudoku::Rng rng(seed);
    sudoku::shuffle(std::span(cells), rng);
    return cells;
}

struct Features {
    int minCandidates = 0;
    double meanCandidates = 0;
    int forcedCells = 0;
    std::uint64_t placements = 0;
    std::uint64_t dlxBytes = 0;
};

Features featuresOf(const Board& puzzle) {
    Features f;
    const auto constraints = sudoku::Constraints::fromBoard(puzzle);
    if (!constraints) throw std::logic_error("fixture has conflicting givens");
    int empty = 0;
    std::uint64_t total = 0;
    f.minCandidates = std::numeric_limits<int>::max();
    for (int i = 0; i < puzzle.cellCount(); ++i) {
        if (puzzle.at(i) != 0) continue;
        const int count = constraints->candidateCount(i / puzzle.size(), i % puzzle.size());
        ++empty;
        total += static_cast<std::uint64_t>(count);
        f.minCandidates = std::min(f.minCandidates, count);
        if (count == 1) ++f.forcedCells;
    }
    if (empty == 0) f.minCandidates = 0;
    f.meanCandidates = empty == 0 ? 0.0 : static_cast<double>(total) / empty;
    const auto estimate = sudoku::DLXSolver::estimate(puzzle);
    f.placements = estimate->placements;
    f.dlxBytes = estimate->bytes;
    return f;
}

// ---- Population B: carve a unique puzzle down to exactly `target` empty cells ----

struct CarveResult {
    Board puzzle;              // the target puzzle if reached, else the sparsest one reached
    bool reached = false;
    std::string reason;        // when not reached: generation_budget_exhausted | removal_order_exhausted
    int maxEmpty = 0;
    std::string lastProver = "none";  // which argument proved the last accepted removal
    int checks = 0;
    int checksAborted = 0;
    int forcedRemovals = 0;
    int attempts = 0;
};

CarveResult carve(const Board& grid, int target, std::uint64_t gridSeed, const CarveOptions& options) {
    const sudoku::MRVBacktrackingSolver mrv;
    const sudoku::DLXSolver dlx;
    const auto deadline = Clock::now() + options.budget;
    const auto remaining = [&] {
        const auto left = std::chrono::ceil<std::chrono::milliseconds>(deadline - Clock::now());
        return std::max(left, std::chrono::milliseconds{1});
    };

    CarveResult best{.puzzle = grid};
    bool outOfTime = false;
    for (int attempt = 0; attempt < options.attempts && !outOfTime; ++attempt) {
        ++best.attempts;
        Board puzzle = grid;
        auto constraints = *sudoku::Constraints::fromBoard(puzzle);
        int empty = 0;
        std::string lastProver = "none";
        for (const int cell : shuffledCells(grid.cellCount(), orderSeedOf(gridSeed, attempt))) {
            if (empty == target) break;
            if (Clock::now() >= deadline) {
                outOfTime = true;
                break;
            }
            const int row = cell / grid.size();
            const int col = cell % grid.size();
            const int value = puzzle.at(cell);
            puzzle.setAt(cell, 0);
            constraints.remove(row, col, value);

            bool accepted = false;
            if (constraints.candidateCount(row, col) == 1) {  // the other clues still force the value
                accepted = true;
                ++best.forcedRemovals;
                lastProver = "forced";
            } else {
                ++best.checks;
                const sudoku::SolveResult byMrv =
                    mrv.run(puzzle, {.solutionLimit = 2, .nodeLimit = options.checkNodes, .timeLimit = remaining()});
                if (byMrv.stopReason == sudoku::StopReason::TimeLimit) {
                    outOfTime = true;
                } else if (!byMrv.aborted) {
                    accepted = byMrv.solutionCount == 1;
                    if (accepted) lastProver = "mrv";
                } else {  // MRV hit its node budget: DLX gets the same budget
                    std::optional<sudoku::SolveResult> byDlx;
                    try {
                        byDlx = dlx.run(puzzle,
                                        {.solutionLimit = 2, .nodeLimit = options.checkNodes, .timeLimit = remaining()});
                    } catch (const std::length_error&) {
                        // over DLX's memory budget: counts as an aborted check
                    }
                    if (byDlx && byDlx->stopReason == sudoku::StopReason::TimeLimit) {
                        outOfTime = true;
                    } else if (byDlx && !byDlx->aborted) {
                        accepted = byDlx->solutionCount == 1;
                        if (accepted) lastProver = "dlx";
                    } else {
                        ++best.checksAborted;
                    }
                }
            }
            if (accepted) {
                ++empty;
            } else {
                puzzle.setAt(cell, value);
                constraints.place(row, col, value);
            }
            if (outOfTime) break;
        }
        if (empty > best.maxEmpty || attempt == 0) {
            best.maxEmpty = empty;
            best.puzzle = puzzle;
            best.lastProver = lastProver;
        }
        if (empty == target) {
            best.reached = true;
            return best;
        }
    }
    best.reason = outOfTime ? "generation_budget_exhausted" : "removal_order_exhausted";
    return best;
}

std::string directoryFor(const std::string& outRoot, const FixtureSpec& spec) {
    const std::string dir = outRoot + "/" + spec.population + "/n" + padded(spec.n, 2);
    std::filesystem::create_directories(dir);
    return dir;
}

}  // namespace

std::string fixtureId(const FixtureSpec& spec) {
    return spec.population + "-n" + padded(spec.n, 2) + "-e" + padded(spec.empty, 6) + "-s" + padded(spec.seedIndex, 3);
}

Row makeFixture(const FixtureSpec& spec, const std::string& outRoot, const CarveOptions& options) {
    const auto started = Clock::now();
    const Board probe(spec.n);  // validates the box size
    if (spec.empty < 0 || spec.empty > probe.cellCount()) throw std::invalid_argument("empty count out of range");
    const std::uint64_t gridSeed = gridSeedOf(spec);
    sudoku::Rng gridRng(gridSeed);

    Board grid = spec.population == "AS" ? solverFilledGrid(spec.n, gridRng) : sudoku::randomCompletedGrid(spec.n, gridRng);

    Row row;
    const std::string id = fixtureId(spec);
    row.add("id", id);
    row.add("population", spec.population);
    row.add("n", spec.n);
    row.add("N", grid.size());
    row.add("cells", grid.cellCount());

    Board puzzle = grid;
    std::string status = "ok";
    std::string reason;
    std::string verification;
    std::string verifiedIndependently = "n/a";
    int maxEmpty = spec.empty;
    CarveResult carved;
    double verifyMs = 0;

    if (spec.population == "A" || spec.population == "AS") {
        const auto cells = shuffledCells(grid.cellCount(), orderSeedOf(gridSeed, 0));
        for (int k = 0; k < spec.empty; ++k) puzzle.setAt(cells[static_cast<std::size_t>(k)], 0);
        verification = "blanked_from_complete_grid";
    } else if (spec.population == "B") {
        carved = carve(grid, spec.empty, gridSeed, options);
        puzzle = carved.puzzle;
        maxEmpty = carved.maxEmpty;
        verification = "carved;last_proof=" + carved.lastProver;
        if (!carved.reached) {
            status = "target_not_achieved";
            reason = carved.reason;
        } else {
            // Independent re-verification by the solver that did not make the last proof.
            const auto verifyStart = Clock::now();
            const sudoku::MRVBacktrackingSolver mrv;
            const sudoku::DLXSolver dlx;
            const sudoku::Solver& other = carved.lastProver == "dlx" ? static_cast<const sudoku::Solver&>(mrv)
                                                                     : static_cast<const sudoku::Solver&>(dlx);
            try {
                const auto check = other.run(puzzle, {.solutionLimit = 2, .timeLimit = options.verifyBudget});
                if (check.aborted) {
                    verifiedIndependently = std::string("inconclusive(") + std::string(other.name()) + ")";
                } else if (check.solutionCount == 1 && check.firstSolution == grid) {
                    verifiedIndependently = std::string("yes(") + std::string(other.name()) + ")";
                } else {
                    throw std::logic_error("independent re-verification disagrees: " +
                                           std::to_string(check.solutionCount) + " solutions");
                }
            } catch (const std::length_error&) {
                verifiedIndependently = "inconclusive(dlx memory budget)";
            }
            verifyMs = std::chrono::duration<double, std::milli>(Clock::now() - verifyStart).count();
        }
    } else {
        throw std::invalid_argument("unknown population " + spec.population);
    }

    const std::string dir = directoryFor(outRoot, spec);
    const std::string puzzleText = puzzle.toString() + "\n";
    const std::string solutionText = grid.toString() + "\n";
    std::string puzzleFile;
    std::string solutionFile;
    if (status == "ok") {
        puzzleFile = dir + "/" + id + ".txt";
        solutionFile = dir + "/" + id + ".solution.txt";
    } else {  // keep the sparsest unique puzzle reached, clearly labelled; it never replaces the target
        puzzleFile = dir + "/" + id + ".best.txt";
        solutionFile = dir + "/" + id + ".best.solution.txt";
    }
    writeFile(puzzleFile, puzzleText);
    writeFile(solutionFile, solutionText);

    const int empty = puzzle.cellCount() - puzzle.clueCount();
    const Features features = featuresOf(puzzle);
    row.add("clues", puzzle.clueCount());
    row.add("empty", empty);
    row.add("target_empty", spec.empty);
    row.add("target_pct", spec.targetPct);
    row.add("actual_pct", 100.0 * empty / puzzle.cellCount());
    row.add("seed_index", spec.seedIndex);
    row.add("grid_seed", hex(gridSeed));
    row.add("status", status);
    row.add("reason", reason);
    row.add("max_empty_achieved", maxEmpty);
    row.add("solution_status", spec.population == "B" ? "unique" : "at_least_one(source_grid)");
    row.add("verification", verification);
    row.add("verified_independently", verifiedIndependently);
    row.add("content_hash", hex(fnv1a(puzzleText)));
    row.add("solution_hash", hex(fnv1a(solutionText)));
    row.add("min_candidates", features.minCandidates);
    row.add("mean_candidates", features.meanCandidates);
    row.add("forced_cells", features.forcedCells);
    row.add("dlx_placements", features.placements);
    row.add("dlx_estimated_bytes", features.dlxBytes);
    row.add("checks", carved.checks);
    row.add("checks_aborted", carved.checksAborted);
    row.add("forced_removals", carved.forcedRemovals);
    row.add("attempts", carved.attempts);
    row.add("generation_ms", std::chrono::duration<double, std::milli>(Clock::now() - started).count() - verifyMs);
    row.add("verify_ms", verifyMs);
    row.add("puzzle_file", std::filesystem::relative(puzzleFile, outRoot).generic_string());
    row.add("solution_file", std::filesystem::relative(solutionFile, outRoot).generic_string());
    return row;
}

}  // namespace bench
