// Property-based tests: check Sudoku invariants over many seeded random inputs.
//
// Slower than the unit tests, so it is a separate executable with the CTest labels
// "property" and "slow". Configure it with environment variables:
//   SUDOKU_PROPERTY_SCALE  multiplies every iteration count (default 1; e.g. 0.1 for a quick
//                          smoke run, 10 for a soak run)
//   SUDOKU_PROPERTY_SEED   base seed (default 20261103); case i of each property uses base + i
//
// Each property stops at its first failing case. The failure reports the property, grid size,
// case seed and input. Setting SUDOKU_PROPERTY_SEED to that case seed reruns it as case 0.

#include "test_framework.hpp"

#include "sudoku_test_helpers.hpp"

#include "sudoku/backtracking_solver.hpp"
#include "sudoku/constraints.hpp"
#include "sudoku/generator.hpp"
#include "sudoku/logical_solver.hpp"
#include "sudoku/difficulty.hpp"
#include "sudoku/dlx_solver.hpp"
#include "sudoku/random.hpp"
#include "sudoku/trace.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <map>
#include <numeric>
#include <optional>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

using namespace sudoku;
using namespace sudoku_test;

namespace {

// ===========================================================================
// Configuration and the property loop
// ===========================================================================

struct Config {
    double scale = 1.0;
    std::uint64_t baseSeed = 20261103;
};

Config& config() {
    static Config c;
    return c;
}

// Reads the environment variables; prints an error and returns false if one is malformed.
bool loadConfig() {
    if (const char* text = std::getenv("SUDOKU_PROPERTY_SCALE"); text != nullptr && *text != '\0') {
        char* end = nullptr;
        const double scale = std::strtod(text, &end);
        if (*end != '\0' || !(scale > 0)) {
            std::cerr << "SUDOKU_PROPERTY_SCALE must be a positive number, got \"" << text << "\"\n";
            return false;
        }
        config().scale = scale;
    }
    if (const char* text = std::getenv("SUDOKU_PROPERTY_SEED"); text != nullptr && *text != '\0') {
        char* end = nullptr;
        const unsigned long long seed = std::strtoull(text, &end, 10);
        if (*end != '\0') {
            std::cerr << "SUDOKU_PROPERTY_SEED must be an unsigned integer, got \"" << text << "\"\n";
            return false;
        }
        config().baseSeed = seed;
    }
    return true;
}

int iterations(int baseCount) {
    return std::max(1, static_cast<int>(std::lround(baseCount * config().scale)));
}

// Runs `check(seed)` for the scaled number of cases, with seeds base, base + 1, ... Failures
// inside `check` carry the property, grid and case seed via CONTEXT. The loop stops at the first
// failing case so that one bug doesn't flood the output.
template <typename Check>
void forAll(std::string_view property, int boxSize, int baseCount, Check check) {
    const int count = iterations(baseCount);
    for (int i = 0; i < count; ++i) {
        const std::uint64_t seed = config().baseSeed + static_cast<std::uint64_t>(i);
        bool failed = false;
        {
            CONTEXT("property: ", property, " | grid: ", gridName(boxSize), " | case seed: ", seed);
            const std::size_t before = mini_test::failuresSoFar();
            check(seed);
            failed = mini_test::failuresSoFar() > before;
        }
        if (failed) {
            FAIL(property, " (", gridName(boxSize), "): stopped at failing case ", i + 1, " of ", count,
                 "; rerun it first with SUDOKU_PROPERTY_SEED=", seed);
            return;
        }
    }
    TRACE(gridName(boxSize), count, " cases passed (seeds ", config().baseSeed, "..",
          config().baseSeed + static_cast<std::uint64_t>(count) - 1, ")");
}

// Separate stream for decisions made by the test itself (which cell to modify, which transform
// to apply), so they don't correlate with the generator's own use of the same seed.
Rng testRng(std::uint64_t seed) { return Rng(seed ^ 0x9E3779B97F4A7C15ULL); }

int randomBelow(Rng& rng, int bound) { return static_cast<int>(uniformBelow(rng, static_cast<std::uint64_t>(bound))); }

// A random complete grid. Up to 16x16 it comes from a randomized solve of an empty grid: a
// different distribution from the generator's pattern-based grids, so these properties don't
// only see grids the library built itself. The solve restarts after a small node budget,
// because a randomized fill occasionally wanders into a region that takes millions of nodes to
// escape. From 25x25 up that happens too often, so the library's constructive grid is used.
Board randomFullGrid(int boxSize, Rng& rng) {
    if (boxSize >= 5) return randomCompletedGrid(boxSize, rng);
    const Board empty(boxSize);
    const auto budget = static_cast<std::uint64_t>(10 * empty.cellCount());
    for (int attempt = 0; attempt < 1000; ++attempt) {
        auto grid = MRVBacktrackingSolver().run(empty, {.nodeLimit = budget, .rng = &rng}).firstSolution;
        if (grid) return *grid;
    }
    throw std::logic_error("could not fill an empty grid in 1000 attempts");
}

// ===========================================================================
// Sudoku-preserving transformations
// ===========================================================================

// Relabels symbols, permutes rows within each band (and the bands themselves), permutes columns
// within each stack (and the stacks), and optionally transposes. Rows, columns and boxes map
// onto rows, columns and boxes, so validity and solution counts are unchanged.
struct Transform {
    std::vector<int> symbol;  // symbol[v] = new value for v; symbol[0] = 0
    std::vector<int> row;     // row[r] = source row of output row r
    std::vector<int> col;     // col[c] = source column of output column c
    bool transpose = false;
};

// A permutation of 0..n*n-1 that keeps each block of n indices together (bands or stacks).
std::vector<int> blockPreservingPermutation(int n, Rng& rng) {
    std::vector<int> blocks(static_cast<std::size_t>(n));
    std::iota(blocks.begin(), blocks.end(), 0);
    shuffle(std::span(blocks), rng);
    std::vector<int> permutation;
    for (const int block : blocks) {
        std::vector<int> within(static_cast<std::size_t>(n));
        std::iota(within.begin(), within.end(), 0);
        shuffle(std::span(within), rng);
        for (const int offset : within) permutation.push_back(block * n + offset);
    }
    return permutation;
}

Transform randomTransform(int boxSize, Rng& rng) {
    const int size = boxSize * boxSize;
    Transform t;
    std::vector<int> values(static_cast<std::size_t>(size));
    std::iota(values.begin(), values.end(), 1);
    shuffle(std::span(values), rng);
    t.symbol.push_back(0);
    t.symbol.insert(t.symbol.end(), values.begin(), values.end());
    t.row = blockPreservingPermutation(boxSize, rng);
    t.col = blockPreservingPermutation(boxSize, rng);
    t.transpose = randomBelow(rng, 2) == 1;
    return t;
}

Board apply(const Transform& t, const Board& board) {
    const int size = board.size();
    Board out(board.boxSize());
    for (int r = 0; r < size; ++r) {
        for (int c = 0; c < size; ++c) {
            int sourceRow = t.row[static_cast<std::size_t>(r)];
            int sourceCol = t.col[static_cast<std::size_t>(c)];
            if (t.transpose) std::swap(sourceRow, sourceCol);
            out.setAt(r * size + c, t.symbol[static_cast<std::size_t>(board.at(sourceRow * size + sourceCol))]);
        }
    }
    return out;
}

std::string describe(const Transform& t) {
    const auto list = [](const std::vector<int>& items, std::size_t from) {
        std::string out;
        for (std::size_t i = from; i < items.size(); ++i) out += (i > from ? "," : "") + std::to_string(items[i]);
        return out;
    };
    return mini_test::concat("symbols 1..N -> [", list(t.symbol, 1), "], rows <- [", list(t.row, 0), "], cols <- [",
                             list(t.col, 0), "]", t.transpose ? ", transposed" : "");
}

// ===========================================================================
// Independent oracle: every valid 4x4 grid, enumerated without the solver
// ===========================================================================

const std::vector<Board>& all4x4Grids() {
    static const std::vector<Board> grids = [] {
        std::vector<Board> result;
        Board grid(2);
        // Plain cell-by-cell enumeration with a direct row/column/box scan, deliberately sharing
        // no code with Constraints or the solver.
        const auto fits = [&](int cell, int value) {
            const int r = cell / 4;
            const int c = cell % 4;
            for (int i = 0; i < 4; ++i) {
                if (grid.get(r, i) == value || grid.get(i, c) == value) return false;
            }
            const int br = r / 2 * 2;
            const int bc = c / 2 * 2;
            for (int i = 0; i < 2; ++i) {
                for (int j = 0; j < 2; ++j) {
                    if (grid.get(br + i, bc + j) == value) return false;
                }
            }
            return true;
        };
        const auto fill = [&](const auto& self, int cell) -> void {
            if (cell == 16) {
                result.push_back(grid);
                return;
            }
            for (int value = 1; value <= 4; ++value) {
                if (!fits(cell, value)) continue;
                grid.setAt(cell, value);
                self(self, cell + 1);
                grid.setAt(cell, 0);
            }
        };
        fill(fill, 0);
        return result;
    }();
    return grids;
}

std::size_t oracleCount4x4(const Board& puzzle) {
    return static_cast<std::size_t>(std::ranges::count_if(all4x4Grids(), [&](const Board& grid) {
        for (int i = 0; i < 16; ++i) {
            if (puzzle.at(i) != 0 && puzzle.at(i) != grid.at(i)) return false;
        }
        return true;
    }));
}

}  // namespace

// ===========================================================================
// Generation
// ===========================================================================

TEST("Generation", "generated puzzles are valid, uniquely solvable and keep their clues") {
    const MRVBacktrackingSolver solver;
    for (const auto& [boxSize, cases] : {std::pair{2, 1000}, {3, 200}, {4, 10}, {5, 3}, {6, 1}}) {
        forAll("generated puzzle is valid and unique", boxSize, cases, [&](std::uint64_t seed) {
            const Puzzle p = Generator().generate({.boxSize = boxSize, .seed = seed});
            CONTEXT("puzzle: ", p.puzzle.toString(), " | reproduce: sudoku generate --size ", boxSize, " --seed ",
                    seed);
            CHECK(p.puzzle.isValid());
            CHECK(p.puzzle.clueCount() < p.puzzle.cellCount());
            checkSolves(p.solution, p.puzzle);  // valid, complete, and every clue preserved
            const SolveResult result = solver.run(p.puzzle, {.solutionLimit = 2});
            CHECK(!result.aborted);
            CHECK_EQ(result.solutionCount, 1u);
            CHECK_EQ(result.firstSolution, p.solution);
        });
    }
}

TEST("Generation", "the same seed always regenerates the same puzzle") {
    for (const auto& [boxSize, cases] : {std::pair{2, 300}, {3, 50}}) {
        forAll("generation is deterministic", boxSize, cases, [&](std::uint64_t seed) {
            const Puzzle a = Generator().generate({.boxSize = boxSize, .seed = seed});
            const Puzzle b = Generator().generate({.boxSize = boxSize, .seed = seed});
            CONTEXT("puzzle: ", a.puzzle.toString());
            CHECK_EQ(b.puzzle, a.puzzle);
            CHECK_EQ(b.solution, a.solution);
        });
    }
}

// ===========================================================================
// Transforms
// ===========================================================================

TEST("Transforms", "transformed complete grids are complete and valid; conflicts survive transforms") {
    for (const auto& [boxSize, cases] : {std::pair{2, 1000}, {3, 500}, {4, 50}, {6, 20}, {8, 10}, {10, 5}}) {
        forAll("transform preserves validity", boxSize, cases, [&](std::uint64_t seed) {
            Rng rng = testRng(seed);
            const Board grid = randomFullGrid(boxSize, rng);
            const Transform t = randomTransform(boxSize, rng);
            CONTEXT("grid: ", grid.toString(), " | transform: ", describe(t));
            const Board moved = apply(t, grid);
            CHECK(moved.isFilled());
            CHECK(moved.isValid());

            // Copy a row neighbour's value into the first cell: the conflict must survive the transform.
            Board broken = grid;
            broken.setAt(0, grid.at(1));
            CHECK(!broken.isValid());
            CHECK(!apply(t, broken).isValid());
        });
    }
}

TEST("Transforms", "transformed unique puzzles stay unique, with the transformed solution") {
    const MRVBacktrackingSolver solver;
    for (const auto& [boxSize, cases] : {std::pair{2, 300}, {3, 100}, {4, 5}}) {
        forAll("transform preserves uniqueness", boxSize, cases, [&](std::uint64_t seed) {
            const Puzzle p = Generator().generate({.boxSize = boxSize, .seed = seed});
            Rng rng = testRng(seed);
            const Transform t = randomTransform(boxSize, rng);
            const Board puzzle = apply(t, p.puzzle);
            const Board solution = apply(t, p.solution);
            CONTEXT("original puzzle: ", p.puzzle.toString(), " | transform: ", describe(t));
            CONTEXT("transformed puzzle: ", puzzle.toString());
            CHECK(puzzle.isValid());
            CHECK_EQ(puzzle.clueCount(), p.puzzle.clueCount());
            checkSolves(solution, puzzle);
            const SolveResult result = solver.run(puzzle, {.solutionLimit = 2});
            CHECK(!result.aborted);
            CHECK_EQ(result.solutionCount, 1u);
            CHECK_EQ(result.firstSolution, solution);
        });
    }
}

TEST("Transforms", "transforms preserve the solution count of ambiguous puzzles") {
    const MRVBacktrackingSolver solver;
    for (const auto& [boxSize, cases, limit] : {std::tuple{2, 500, 1000}, {3, 100, 500}}) {
        forAll("transform preserves solution count", boxSize, cases, [&](std::uint64_t seed) {
            Rng rng = testRng(seed);
            // A generated puzzle with 1-3 clues removed usually has several solutions.
            Board puzzle = Generator().generate({.boxSize = boxSize, .seed = seed}).puzzle;
            for (int removals = 1 + randomBelow(rng, 3); removals > 0; --removals) {
                std::vector<int> clues;
                for (int i = 0; i < puzzle.cellCount(); ++i) {
                    if (puzzle.at(i) != 0) clues.push_back(i);
                }
                puzzle.setAt(clues[static_cast<std::size_t>(randomBelow(rng, static_cast<int>(clues.size())))], 0);
            }
            const Transform t = randomTransform(boxSize, rng);
            CONTEXT("puzzle: ", puzzle.toString(), " | transform: ", describe(t), " | count limit: ", limit);
            const std::size_t original = solver.countSolutions(puzzle, static_cast<std::size_t>(limit));
            const std::size_t transformed = solver.countSolutions(apply(t, puzzle), static_cast<std::size_t>(limit));
            CHECK(original >= 1);
            CHECK_EQ(transformed, original);
        });
    }
}

// ===========================================================================
// Solver on modified, invalid and contradictory grids
// ===========================================================================

TEST("Solver", "a duplicated value makes a grid invalid and unsolvable") {
    const MRVBacktrackingSolver solver;
    for (const auto& [boxSize, cases] : {std::pair{2, 500}, {3, 300}, {4, 30}, {6, 20}, {10, 5}}) {
        forAll("duplicate value is rejected", boxSize, cases, [&](std::uint64_t seed) {
            Rng rng = testRng(seed);
            Board grid = randomFullGrid(boxSize, rng);
            const int size = grid.size();
            const int cell = randomBelow(rng, grid.cellCount());
            const int row = cell / size;
            const int col = cell % size;

            // Pick a different cell in the same row, column or box and copy its value.
            int peer = cell;
            switch (randomBelow(rng, 3)) {
                case 0: peer = row * size + (col + 1 + randomBelow(rng, size - 1)) % size; break;
                case 1: peer = ((row + 1 + randomBelow(rng, size - 1)) % size) * size + col; break;
                default:
                    while (peer == cell) {
                        const int boxRow = row / boxSize * boxSize + randomBelow(rng, boxSize);
                        const int boxCol = col / boxSize * boxSize + randomBelow(rng, boxSize);
                        peer = boxRow * size + boxCol;
                    }
            }
            grid.setAt(cell, grid.at(peer));
            // Blank about half of the other cells so it reads as a puzzle rather than a full grid.
            for (int i = 0; i < grid.cellCount(); ++i) {
                if (i != cell && i != peer && randomBelow(rng, 2) == 0) grid.setAt(i, 0);
            }
            CONTEXT("puzzle: ", grid.toString(), " | duplicate ", grid.at(cell), " at cells ", cell, " and ", peer);
            CHECK(!grid.isValid());
            const SolveResult result = solver.run(grid, {.solutionLimit = 1});
            CHECK_EQ(result.solutionCount, 0u);
            CHECK(!result.firstSolution.has_value());
            CHECK(!result.aborted);
        });
    }
}

TEST("Solver", "a wrong but non-conflicting value in a unique puzzle leaves no solution") {
    // If a puzzle has exactly one solution S, every solution has S's value in each cell. Placing a
    // different (locally allowed) value therefore leaves no solution at all.
    const MRVBacktrackingSolver solver;
    for (const auto& [boxSize, cases, nodeLimit] :
         {std::tuple{2, 300, std::uint64_t{0}}, {3, 100, std::uint64_t{0}}, {4, 5, std::uint64_t{2'000'000}}}) {
        int skipped = 0;
        int inconclusive = 0;
        forAll("wrong value is a contradiction", boxSize, cases, [&](std::uint64_t seed) {
            const Puzzle p = Generator().generate({.boxSize = boxSize, .seed = seed});
            Rng rng = testRng(seed);
            const Constraints constraints = *Constraints::fromBoard(p.puzzle);

            std::vector<std::pair<int, int>> options;  // (cell, wrong value)
            for (int i = 0; i < p.puzzle.cellCount(); ++i) {
                if (p.puzzle.at(i) != 0) continue;
                for (const int v : constraints.candidates(i / p.puzzle.size(), i % p.puzzle.size())) {
                    if (v != p.solution.at(i)) options.emplace_back(i, v);
                }
            }
            if (options.empty()) {  // every empty cell is forced; nothing to test for this seed
                ++skipped;
                return;
            }
            const auto [cell, value] = options[static_cast<std::size_t>(randomBelow(rng, static_cast<int>(options.size())))];
            Board contradiction = p.puzzle;
            contradiction.setAt(cell, value);
            CONTEXT("puzzle: ", contradiction.toString(), " | placed ", value, " at (", cell / p.puzzle.size(), ',',
                    cell % p.puzzle.size(), "), solution has ", p.solution.at(cell));
            CHECK(contradiction.isValid());
            const SolveResult result = solver.run(contradiction, {.solutionLimit = 1, .nodeLimit = nodeLimit});
            if (result.aborted) {
                ++inconclusive;  // proving 16x16 contradictions can be expensive; not a failure
                return;
            }
            CHECK_EQ(result.solutionCount, 0u);
        });
        TRACE(gridName(boxSize), skipped, " seeds skipped (all cells forced), ", inconclusive,
              " inconclusive (node limit ", nodeLimit, ")");
    }
}

TEST("Solver", "solution counts on random 4x4 grids match a brute-force oracle") {
    // Random givens anywhere, including conflicting ones. The oracle is the list of all 288 valid
    // 4x4 grids, so the expected count is just how many of them agree with the givens.
    REQUIRE_EQ(all4x4Grids().size(), 288u);
    const MRVBacktrackingSolver solver;
    forAll("solver count matches oracle", 2, 2000, [&](std::uint64_t seed) {
        Rng rng = testRng(seed);
        Board puzzle(2);
        for (int k = randomBelow(rng, 9); k > 0; --k) puzzle.setAt(randomBelow(rng, 16), 1 + randomBelow(rng, 4));
        CONTEXT("puzzle: ", puzzle.toString(), puzzle.isValid() ? "" : " (conflicting givens)");
        const SolveResult result = solver.run(puzzle, {.solutionLimit = 0});
        CHECK_EQ(result.solutionCount, oracleCount4x4(puzzle));
        if (result.firstSolution) checkSolves(*result.firstSolution, puzzle);
    });
}

TEST("Solver", "solver answers on random 9x9 grids are self-consistent") {
    // No cheap oracle for 9x9. Instead: conflicting givens give 0 solutions, and any solution
    // returned really solves the puzzle.
    const MRVBacktrackingSolver solver;
    int inconclusive = 0;
    forAll("solver answer is consistent", 3, 500, [&](std::uint64_t seed) {
        Rng rng = testRng(seed);
        Board puzzle(3);
        for (int k = randomBelow(rng, 31); k > 0; --k) puzzle.setAt(randomBelow(rng, 81), 1 + randomBelow(rng, 9));
        CONTEXT("puzzle: ", puzzle.toString(), puzzle.isValid() ? "" : " (conflicting givens)");
        const SolveResult result = solver.run(puzzle, {.solutionLimit = 2, .nodeLimit = 200'000});
        if (!puzzle.isValid()) {
            CHECK_EQ(result.solutionCount, 0u);
            CHECK(!result.aborted);
            return;
        }
        if (result.aborted) {
            ++inconclusive;
            return;
        }
        CHECK(result.solutionCount <= 2);
        CHECK_EQ(result.firstSolution.has_value(), result.solutionCount > 0);
        if (result.firstSolution) checkSolves(*result.firstSolution, puzzle);
    });
    TRACE("inconclusive", inconclusive, " cases hit the node limit");
}

TEST("Solver", "large grids with 20% of cells blank solve to completions of their givens") {
    // Plain MRV backtracking has no extra propagation, so heavily blanked large grids can take
    // arbitrarily long (see the README). At 20% blanks these sizes are practical; a case that
    // still hits the node limit is counted as inconclusive, never as a pass or a failure.
    const MRVBacktrackingSolver solver;
    for (const auto& [boxSize, cases] : {std::pair{5, 50}, {6, 20}, {8, 10}, {9, 5}, {10, 5}}) {
        int inconclusive = 0;
        forAll("blanked large grid solves", boxSize, cases, [&](std::uint64_t seed) {
            Rng rng = testRng(seed);
            Board puzzle = randomCompletedGrid(boxSize, rng);
            for (int i = 0; i < puzzle.cellCount(); ++i) {
                if (randomBelow(rng, 5) == 0) puzzle.setAt(i, 0);
            }
            CONTEXT("puzzle: ", mini_test::abbreviate(puzzle.toString()), " (", puzzle.clueCount(), " clues)");
            const SolveResult result = solver.run(puzzle, {.nodeLimit = 1'000'000});
            if (result.aborted) {
                ++inconclusive;
                return;
            }
            REQUIRE(result.firstSolution.has_value());
            checkSolves(*result.firstSolution, puzzle);
        });
        TRACE(gridName(boxSize), inconclusive, " inconclusive (node limit 1000000)");
    }
}

// ===========================================================================
// Cross-solver properties: naive, MRV and DLX against an oracle and each other
// ===========================================================================

// Records every solution a search reports, as text, through the observer interface.
class SolutionCollector final : public SolverObserver {
public:
    void onSolverEvent(const SolverEvent& event) override {
        if (event.kind == SolverEvent::Kind::Solution) solutions.insert(event.board->toString());
    }
    std::set<std::string> solutions;
};

TEST("Solvers", "every solver enumerates exactly the oracle's set of solutions on random 4x4 puzzles") {
    // Stronger than comparing counts: each solver must report precisely the valid 4x4 grids that
    // agree with the givens, each once, and nothing else. Includes conflicting givens.
    REQUIRE_EQ(all4x4Grids().size(), 288u);
    forAll("solution set matches oracle", 2, 2000, [&](std::uint64_t seed) {
        Rng rng = testRng(seed);
        Board puzzle(2);
        for (int k = randomBelow(rng, 9); k > 0; --k) puzzle.setAt(randomBelow(rng, 16), 1 + randomBelow(rng, 4));
        CONTEXT("puzzle: ", puzzle.toString(), puzzle.isValid() ? "" : " (conflicting givens)");
        std::set<std::string> expected;
        for (const Board& grid : all4x4Grids()) {
            bool agrees = true;
            for (int i = 0; i < 16; ++i) agrees = agrees && (puzzle.at(i) == 0 || puzzle.at(i) == grid.at(i));
            if (agrees) expected.insert(grid.toString());
        }
        for (const Solver* solver : allSolvers()) {
            CONTEXT("solver: ", solver->name());
            SolutionCollector collector;
            const SolveResult result = solver->run(puzzle, {.solutionLimit = 0, .observer = &collector});
            CHECK_EQ(result.solutionCount, expected.size());
            CHECK_EQ(collector.solutions.size(), result.solutionCount);  // no solution reported twice
            CHECK(collector.solutions == expected);
            CHECK(!result.aborted);
            if (result.firstSolution) checkSolutionIndependently(*result.firstSolution, puzzle);
        }
    });
}

TEST("Solvers", "every solver gives the same capped solution count on random 9x9 puzzles of every sparsity") {
    // Counts are capped at 1000 solutions, so "agree" means min(true count, 1000) is the same. A
    // solver that hits its node budget is inconclusive for that case (counted, never a pass).
    constexpr std::size_t kCap = 1000;
    int inconclusive = 0;
    int compared = 0;
    forAll("capped counts agree across solvers", 3, 600, [&](std::uint64_t seed) {
        Rng rng = testRng(seed);
        Board puzzle = randomFullGrid(3, rng);
        const int percent = 10 + 10 * randomBelow(rng, 7);  // 10% .. 70% empty
        std::vector<int> cells(81);
        std::iota(cells.begin(), cells.end(), 0);
        shuffle(std::span(cells), rng);
        const int empty = (percent * 81 + 50) / 100;
        for (int k = 0; k < empty; ++k) puzzle.setAt(cells[static_cast<std::size_t>(k)], 0);
        CONTEXT("puzzle: ", puzzle.toString(), " (", percent, "% empty)");
        std::optional<std::size_t> agreed;
        for (const Solver* solver : allSolvers()) {
            CONTEXT("solver: ", solver->name());
            const std::uint64_t budget = solver->name() == "backtracking-naive" ? 20'000'000 : 2'000'000;
            const SolveResult result = solver->run(puzzle, {.solutionLimit = kCap, .nodeLimit = budget});
            if (result.aborted) {
                ++inconclusive;
                continue;
            }
            CHECK(result.solutionCount >= 1);  // blanked from a complete grid
            REQUIRE(result.firstSolution.has_value());
            checkSolutionIndependently(*result.firstSolution, puzzle);
            if (!agreed) agreed = result.solutionCount;
            CHECK_EQ(result.solutionCount, *agreed);
            ++compared;
        }
    });
    TRACE("coverage", compared, " solver results compared, ", inconclusive, " inconclusive (node budget)");
}

TEST("Solvers", "every solver gives the same uniqueness verdict on 16x16 and 25x25 puzzles") {
    // Generated puzzles must come out unique; blanked complete grids are mostly ambiguous. Verdicts
    // are 0, 1 or 2 (= "2 or more"). Node-limited runs are inconclusive, counted and reported.
    for (const auto& [boxSize, cases] : {std::pair{4, 40}, {5, 10}}) {
        std::map<std::string, int> inconclusive;  // per solver
        forAll("uniqueness verdicts agree", boxSize, cases, [&](std::uint64_t seed) {
            Rng rng = testRng(seed);
            const bool generated = seed % 2 == 0;
            Board puzzle = generated ? Generator().generate({.boxSize = boxSize, .seed = seed}).puzzle
                                     : randomCompletedGrid(boxSize, rng);
            if (!generated) {
                for (int i = 0; i < puzzle.cellCount(); ++i) {
                    if (randomBelow(rng, 100) < 35) puzzle.setAt(i, 0);
                }
            }
            CONTEXT("puzzle: ", mini_test::abbreviate(puzzle.toString()), generated ? " (generated)" : " (blanked)");
            std::optional<std::size_t> agreed;
            for (const Solver* solver : allSolvers()) {
                CONTEXT("solver: ", solver->name());
                const SolveResult result = solver->run(puzzle, {.solutionLimit = 2, .nodeLimit = 2'000'000});
                if (result.aborted) {
                    ++inconclusive[std::string(solver->name())];
                    continue;
                }
                if (generated) CHECK_EQ(result.solutionCount, 1u);
                if (result.firstSolution) checkSolutionIndependently(*result.firstSolution, puzzle);
                if (!agreed) agreed = result.solutionCount;
                CHECK_EQ(result.solutionCount, *agreed);
            }
        });
        for (const auto& [name, count] : inconclusive) {
            TRACE(gridName(boxSize), name, ": ", count, " runs inconclusive (node limit 2000000)");
        }
    }
}

// ===========================================================================
// Logical solver
// ===========================================================================

TEST("Logic", "every logical deduction on generated puzzles agrees with the unique solution") {
    for (const auto& [boxSize, cases] : {std::pair{2, 500}, {3, 150}, {4, 5}}) {
        forAll("logical deductions are sound", boxSize, cases, [&](std::uint64_t seed) {
            const Puzzle p = Generator().generate({.boxSize = boxSize, .seed = seed});
            CONTEXT("puzzle: ", p.puzzle.toString());
            const LogicalResult result = LogicalSolver({.recordSteps = true}).solve(p.puzzle);
            CHECK(result.status != LogicalStatus::Contradiction);
            for (const Deduction& d : result.deductions) {
                if (d.placement && d.placement->value != p.solution.at(d.placement->cell)) {
                    FAIL(name(d.technique), " placed ", d.placement->value, " at cell ", d.placement->cell);
                    return;
                }
                for (const auto& [cell, value] : d.eliminations) {
                    if (value == p.solution.at(cell)) {
                        FAIL(name(d.technique), " removed the solution value ", value, " from cell ", cell);
                        return;
                    }
                }
            }
            if (result.status == LogicalStatus::Solved) CHECK_EQ(result.board, p.solution);
        });
    }
}

TEST("Difficulty", "grading finds the unique solution and agrees with capped logic") {
    for (const auto& [boxSize, cases] : {std::pair{2, 500}, {3, 150}}) {
        forAll("rate() solves and grades consistently", boxSize, cases, [&](std::uint64_t seed) {
            const Puzzle p = Generator().generate({.boxSize = boxSize, .seed = seed});
            CONTEXT("puzzle: ", p.puzzle.toString());
            const Rating rating = rate(p.puzzle);
            REQUIRE(rating.status == RatingStatus::Solved);
            CHECK_EQ(rating.solution, p.solution);
            REQUIRE(rating.difficulty.has_value());
            const Difficulty grade = *rating.difficulty;
            CHECK_EQ(rating.guesses > 0, grade == Difficulty::Expert);
            // Capping at the grade solves; capping one tier lower does not.
            if (grade != Difficulty::Expert) {
                CHECK(rate(p.puzzle, {.cap = grade}).status == RatingStatus::Solved);
            }
            // The generator grades unique puzzles without guessing: capped at Hard, a solve gives
            // the tier and a stall means Expert. That must equal the full grade.
            const Rating capped = rate(p.puzzle, {.cap = Difficulty::Hard});
            CHECK(capped.guesses == 0u);
            CHECK((capped.solved() ? *capped.difficulty : Difficulty::Expert) == grade);
            if (grade != Difficulty::Easy) {
                const auto lower = static_cast<Difficulty>(static_cast<int>(grade) - 1);
                CHECK(rate(p.puzzle, {.cap = lower}).status == RatingStatus::AboveCap);
            }
        });
    }
}

TEST("Difficulty", "puzzles generated for a target are graded exactly that and unique by MRV and DLX") {
    // Hard needs about 300 attempts per puzzle on 9x9, so it gets fewer cases.
    const std::vector<std::tuple<int, Difficulty, int>> plan{{2, Difficulty::Easy, 200},
                                                            {3, Difficulty::Easy, 30},
                                                            {3, Difficulty::Medium, 30},
                                                            {3, Difficulty::Hard, 3},
                                                            {3, Difficulty::Expert, 30}};
    for (const auto& [boxSize, target, cases] : plan) {
        const std::string property = std::string("generated at ") + std::string(name(target));
        forAll(property, boxSize, cases, [&, target = target, boxSize = boxSize](std::uint64_t seed) {
            const Puzzle p = Generator().generate({.boxSize = boxSize, .seed = seed, .difficulty = target});
            CONTEXT("puzzle: ", p.puzzle.toString(), " | reproduce: sudoku generate --size ", boxSize, " --seed ",
                    p.seed, " --difficulty ", name(target));
            REQUIRE(p.targetReached);
            CHECK(rate(p.puzzle).difficulty == target);
            checkSolves(p.solution, p.puzzle);
            // Uniqueness by MRV and DLX, independently of the grader. (Naive backtracking is left
            // out: it can need millions of nodes even on puzzles that are easy for people.)
            const MRVBacktrackingSolver mrv;
            const DLXSolver dlx;
            for (const Solver* solver : {static_cast<const Solver*>(&mrv), static_cast<const Solver*>(&dlx)}) {
                CONTEXT("solver: ", solver->name());
                const SolveResult result = solver->run(p.puzzle, {.solutionLimit = 2, .nodeLimit = 5'000'000});
                CHECK(!result.aborted);
                CHECK_EQ(result.solutionCount, 1u);
                CHECK_EQ(result.firstSolution, p.solution);
            }
        });
    }
}

TEST("Generation", "constructive completed grids are valid for every box size from 1 to 10") {
    for (const auto& [boxSize, cases] :
         {std::pair{1, 3}, {2, 500}, {3, 500}, {4, 200}, {5, 100}, {6, 50}, {7, 20}, {8, 20}, {9, 10}, {10, 10}}) {
        forAll("constructive grid is valid", boxSize, cases, [&](std::uint64_t seed) {
            Rng rng(seed);
            const Board grid = randomCompletedGrid(boxSize, rng);
            CONTEXT("grid: ", mini_test::abbreviate(grid.toString()));
            CHECK(grid.isFilled());
            CHECK(grid.isValid());
        });
    }
}

// ===========================================================================
// Parsing
// ===========================================================================

TEST("Parsing", "random boards of every size round-trip through both text formats") {
    for (const auto& [boxSize, cases] : {std::pair{2, 500}, {3, 500}, {4, 200}, {5, 50}, {6, 20}, {8, 5}, {10, 3}}) {
        forAll("parse(print(board)) == board", boxSize, cases, [&](std::uint64_t seed) {
            Rng rng = testRng(seed);
            // Not necessarily a valid Sudoku: parsing only cares about the characters.
            Board board(boxSize);
            for (int i = 0; i < board.cellCount(); ++i) {
                if (randomBelow(rng, 2) == 0) board.setAt(i, 1 + randomBelow(rng, board.size()));
            }
            const std::string compact = board.toString();
            CONTEXT("board: ", mini_test::abbreviate(compact));
            CHECK_EQ(Board::parse(compact), board);
            CHECK_EQ(Board::parse(board.toPrettyString()), board);

            std::string lower = compact;
            std::ranges::transform(lower, lower.begin(),
                                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
            CHECK_EQ(Board::parse(lower), board);

            // Random whitespace and separator noise between cells must be ignored. In the number
            // format a cell is a whole number, so noise goes only where cells already meet.
            constexpr std::string_view kNoise = " \n\t|-+";
            const auto noise = [&] { return kNoise[static_cast<std::size_t>(randomBelow(rng, 6))]; };
            std::string noisy;
            for (const char ch : compact) {
                if (board.usesCharacterFormat()) {
                    noisy += ch;
                    if (randomBelow(rng, 3) == 0) noisy += noise();
                } else {
                    noisy += ch == ' ' ? std::string{noise()} : std::string{ch};
                    if (ch == ' ' && randomBelow(rng, 3) == 0) noisy += noise();
                }
            }
            CHECK_EQ(Board::parse(noisy), board);
        });
    }
}

int main(int argc, char* argv[]) {
    if (!loadConfig()) return 2;
    std::cout << "Property tests: base seed " << config().baseSeed << ", iteration scale " << config().scale
              << " (SUDOKU_PROPERTY_SEED / SUDOKU_PROPERTY_SCALE)\n";
    return mini_test::runAll(argc, argv);
}
