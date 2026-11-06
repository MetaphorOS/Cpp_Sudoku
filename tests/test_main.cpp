#include "test_framework.hpp"

#include "sudoku_test_helpers.hpp"
#include "cli_test_helpers.hpp"

#include "cli.hpp"
#include "sudoku/auto_solver.hpp"
#include "sudoku/backtracking_solver.hpp"
#include "sudoku/board.hpp"
#include "sudoku/generator.hpp"
#include "sudoku/logical_solver.hpp"
#include "sudoku/difficulty.hpp"
#include "sudoku/constraints.hpp"
#include "sudoku/random.hpp"
#include "sudoku/text_trace.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <fstream>
#include <numeric>
#include <optional>
#include <span>
#include <set>
#include <utility>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

using sudoku::MRVBacktrackingSolver;
using sudoku::Board;
using sudoku::Generator;
using sudoku::GeneratorOptions;
using sudoku::Rng;
using sudoku::SolveOptions;
using sudoku::SolveResult;
using namespace sudoku_test;

namespace {

struct Given {
    int row;
    int col;
    int value;
};

Board boardWith(int boxSize, std::initializer_list<Given> givens) {
    Board board(boxSize);
    for (const auto& g : givens) board.set(g.row, g.col, g.value);
    return board;
}

// solve() and countSolutions() hide the search statistics, so this does an extra, trace-only
// run of the same solver to show what the search did on `puzzle`. Only called inside TRACE(...),
// whose arguments are evaluated under --trace alone.
std::string solverStats(const Board& puzzle, std::size_t limit = 2) {
    const auto start = std::chrono::steady_clock::now();
    const SolveResult result = MRVBacktrackingSolver().run(puzzle, {.solutionLimit = limit});
    const std::chrono::duration<double, std::milli> elapsed = std::chrono::steady_clock::now() - start;
    return mini_test::concat(statsText(result), " (solution limit ", limit, ", ",
                             mini_test::formatDuration(elapsed.count()), ')');
}

// Every property a generated puzzle must have, for one (boxSize, seed) pair.
void checkGeneratedPuzzle(int boxSize, std::uint64_t seed) {
    CONTEXT("grid: ", gridName(boxSize), ", seed: ", seed);
    const auto generated = Generator().generate({.boxSize = boxSize, .seed = seed});
    CONTEXT("puzzle: ", mini_test::abbreviate(generated.puzzle.toString()), " (", generated.puzzle.clueCount(),
            " clues)");
    TRACE("generated puzzle", boardText(generated.puzzle));
    TRACE("completed solution", boardText(generated.solution));

    CHECK_EQ(generated.seed, seed);
    CHECK_EQ(generated.puzzle.boxSize(), boxSize);
    CHECK(generated.puzzle.isValid());
    CHECK(generated.puzzle.clueCount() < generated.puzzle.cellCount());  // something was removed
    checkSolves(generated.solution, generated.puzzle);

    const SolveResult result = MRVBacktrackingSolver().run(generated.puzzle, {.solutionLimit = 2});
    TRACE("uniqueness check", statsText(result), " (searching for up to 2 solutions)");
    CHECK(!result.aborted);
    CHECK_EQ(result.solutionCount, 1u);
    CHECK_EQ(result.firstSolution, generated.solution);
}

// Wraps the real solver and counts calls, to observe that the generator uses the solver it is given.
// (Test-only: the mutable counter breaks the "stateless solver" contract, which is fine single-threaded.)
class CountingSolver final : public sudoku::Solver {
public:
    [[nodiscard]] std::string_view name() const noexcept override { return "counting"; }
    [[nodiscard]] SolveResult run(const Board& board, const SolveOptions& options) const override {
        ++calls;
        return inner_.run(board, options);
    }
    mutable int calls = 0;

private:
    MRVBacktrackingSolver inner_;
};

// Expects Board::parse to reject `text`. The (shortened) input and the reason show up in the
// trace and in any failure.
void checkParseRejects(const std::string& text, std::string_view reason) {
    constexpr std::size_t kShown = 30;
    const bool shorten = text.size() > kShown && !mini_test::settings().full;
    CONTEXT("input: ", escaped(shorten ? text.substr(0, kShown) : text), shorten ? "..." : "", " (", text.size(),
            " chars) - ", reason);
    CHECK_THROWS(Board::parse(text), std::invalid_argument);
}

}  // namespace

// ===========================================================================
// Board: construction, geometry and validity
// ===========================================================================

TEST("Board", "default board is an empty, valid 9x9") {
    const Board board;
    CHECK_EQ(board.boxSize(), 3);
    CHECK_EQ(board.size(), 9);
    CHECK_EQ(board.cellCount(), 81);
    CHECK_EQ(board.clueCount(), 0);
    CHECK(board.isValid());
    CHECK(!board.isFilled());
}

TEST("Board", "any positive box size gives an n^2 x n^2 grid, from 1x1 to 100x100") {
    for (const int n : {1, 2, 3, 4, 5, 6, 8, 9, 10}) {
        CONTEXT("grid: ", gridName(n));
        const Board board(n);
        CHECK_EQ(board.size(), n * n);
        CHECK_EQ(board.cellCount(), n * n * n * n);
        CHECK(board.isValid());
    }
}

TEST("Board", "box sizes below 1 are rejected") {
    CHECK_THROWS(Board(0), std::invalid_argument);
    CHECK_THROWS(Board(-1), std::invalid_argument);
    CHECK_THROWS(Board(std::numeric_limits<int>::min()), std::invalid_argument);
}

TEST("Board", "box sizes whose cell count overflows an int are rejected, not wrapped") {
    // 215^4 = 2 136 750 625 cells still fits in an int; 216^4 = 2 176 782 336 does not. The check
    // happens before any allocation, so these are cheap to test.
    CHECK_THROWS(Board(216), std::length_error);
    CHECK_THROWS(Board(100'000), std::length_error);
    CHECK_THROWS(Board(std::numeric_limits<int>::max()), std::length_error);
}

TEST("Board", "boxes are numbered left to right, top to bottom") {
    const Board board(3);
    CHECK_EQ(board.boxIndex(0, 0), 0);
    CHECK_EQ(board.boxIndex(2, 2), 0);
    CHECK_EQ(board.boxIndex(0, 3), 1);
    CHECK_EQ(board.boxIndex(4, 4), 4);
    CHECK_EQ(board.boxIndex(8, 8), 8);
}

TEST("Board", "set and get work for any in-range cell, and 0 clears a cell") {
    Board board(2);
    board.set(3, 3, 4);
    CHECK_EQ(board.get(3, 3), 4);
    CHECK_EQ(board.clueCount(), 1);
    board.set(3, 3, 0);
    CHECK_EQ(board.get(3, 3), 0);
    CHECK_EQ(board.clueCount(), 0);
}

TEST("Board", "set and get reject out-of-range rows, columns and values") {
    Board board(2);
    CHECK_THROWS(board.set(4, 0, 1), std::out_of_range);   // row past the end
    CHECK_THROWS(board.set(0, -1, 1), std::out_of_range);  // negative column
    CHECK_THROWS(board.set(0, 0, 5), std::out_of_range);   // value > size
    CHECK_THROWS(board.set(0, 0, -1), std::out_of_range);  // negative value
    CHECK_THROWS(board.get(-1, 0), std::out_of_range);
    CHECK_THROWS(board.get(0, 4), std::out_of_range);
}

TEST("Board", "a board counts as filled only when no cell is empty") {
    Board board = Board::parse(kClassicSolution);
    CHECK(board.isFilled());
    board.set(8, 8, 0);
    CHECK(!board.isFilled());
}

TEST("Board", "a repeated value in one row makes the board invalid") {
    const Board board = boardWith(3, {{0, 0, 7}, {0, 8, 7}});
    TRACE("board", boardText(board));
    CHECK(!board.isValid());
}

TEST("Board", "a repeated value in one column makes the board invalid") {
    const Board board = boardWith(3, {{0, 4, 2}, {8, 4, 2}});
    TRACE("board", boardText(board));
    CHECK(!board.isValid());
}

TEST("Board", "a repeated value in one box makes the board invalid") {
    // (3,3) and (5,5) share the centre box but no row or column.
    const Board board = boardWith(3, {{3, 3, 9}, {5, 5, 9}});
    TRACE("board", boardText(board));
    CHECK(!board.isValid());
}

TEST("Board", "the same value in unrelated cells is valid") {
    // Different row, column and box: no conflict.
    const Board board = boardWith(3, {{0, 0, 1}, {4, 4, 1}, {8, 8, 1}});
    TRACE("board", boardText(board));
    CHECK(board.isValid());
    CHECK(Board(3).isValid());
    CHECK(Board::parse(kClassicSolution).isValid());
}

// ===========================================================================
// Parsing and printing
// ===========================================================================

TEST("Parsing", "digits are read as values, '.' and '0' as empty cells") {
    const Board board = Board::parse(kClassicPuzzle);
    CHECK_EQ(board.get(0, 0), 5);
    CHECK_EQ(board.get(0, 1), 3);
    CHECK_EQ(board.get(0, 2), 0);  // '0' in the input
    CHECK_EQ(board.clueCount(), 30);
    CHECK_EQ(Board::parse("1.3.............").get(0, 1), 0);  // '.' in the input
}

TEST("Parsing", "grid size is inferred from the number of cells") {
    CHECK_EQ(Board::parse(std::string(16, '.')).boxSize(), 2);
    CHECK_EQ(Board::parse(std::string(81, '.')).boxSize(), 3);
    CHECK_EQ(Board::parse(std::string(256, '.')).boxSize(), 4);
    CHECK_EQ(Board::parse(std::string(625, '.')).boxSize(), 5);
    CHECK_EQ(Board::parse(std::string(1296, '.')).boxSize(), 6);  // empty cells need no large symbols
    CHECK_EQ(Board::parse(".").boxSize(), 1);
}

TEST("Parsing", "whitespace and box separators are ignored") {
    const Board compact = Board::parse("1200003020000004");
    CHECK_EQ(Board::parse("12..\n..3.\n 2 . . . \n...4"), compact);
    CHECK_EQ(Board::parse("1 2 | . .\n. . | 3 .\n----+----\n2 . | . .\n. . | . 4\n"), compact);
}

TEST("Parsing", "letters encode values above 9, case-insensitively") {
    std::string text16(256, '.');
    text16[0] = 'G';  // 16, the largest value in a 16x16 grid
    text16[1] = 'a';  // 10; letters are case-insensitive
    const Board board16 = Board::parse(text16);
    CHECK_EQ(board16.get(0, 0), 16);
    CHECK_EQ(board16.get(0, 1), 10);

    std::string text25(625, '.');
    text25[0] = 'P';  // 25, the largest value in a 25x25 grid
    CHECK_EQ(Board::parse(text25).get(0, 0), 25);
}

TEST("Parsing", "every value survives printing and parsing, up to 100x100") {
    // Regression test, found by the property tests: value 36 once printed as '['. Grids above
    // 25x25 now print in the number format, which has no largest value.
    for (const int n : {5, 6, 8, 10}) {
        CONTEXT("grid: ", gridName(n));
        Board board(n);
        for (int v = 1; v <= board.size(); ++v) board.set(0, v - 1, v);
        CHECK_EQ(board.usesCharacterFormat(), n <= 5);
        CHECK_EQ(Board::parse(board.toString()), board);
        CHECK_EQ(Board::parse(board.toPrettyString()), board);
    }
}

TEST("Parsing", "the number format reads multi-digit values separated by whitespace") {
    // A 36x36 grid: values 10..36 need two digits.
    std::string text = "36 10 .";
    for (int i = 3; i < 1296; ++i) text += " 0";
    const Board board = Board::parse(text);
    CHECK_EQ(board.size(), 36);
    CHECK_EQ(board.get(0, 0), 36);
    CHECK_EQ(board.get(0, 1), 10);
    CHECK_EQ(board.get(0, 2), 0);
    CHECK_EQ(board.clueCount(), 2);
    // Small grids may use it too; it agrees with the one-character format.
    CHECK_EQ(Board::parse("1 2 . . . . 3 . 2 . . . . . . 4"), Board::parse("12....3.2......4"));
}

TEST("Parsing", "grids up to 25x25 print one character per cell, larger ones print numbers") {
    CHECK_EQ(Board::parse(kClassicPuzzle).toString().size(), 81u);  // unchanged one-character output
    Board big(6);
    big.set(0, 0, 36);
    big.set(0, 1, 7);
    const std::string text = big.toString();
    CHECK(text.starts_with("36 7 . . "));
    const std::string pretty = big.toPrettyString();
    TRACE("first pretty line", pretty.substr(0, pretty.find('\n')));
    CHECK(pretty.starts_with("36  7  .  .  .  . |  ."));  // right-aligned to two digits
}

TEST("Parsing", "the number format rejects too-large values, and leading zeros select the character format") {
    std::string tooLarge = "37";
    for (int i = 1; i < 1296; ++i) tooLarge += " .";
    checkParseRejects(tooLarge, "37 in a 36x36");
    // A 16x16 written as 16 lines of 16 digits: as numbers they would have leading zeros or be far
    // too large, so the text is read one character per cell, as in earlier versions.
    std::string rows;
    for (int r = 0; r < 16; ++r) rows += (r == 0 ? "1" : "0") + std::string(15, '0') + "\n";
    const Board board = Board::parse(rows);
    CHECK_EQ(board.size(), 16);
    CHECK_EQ(board.get(0, 0), 1);
    CHECK_EQ(board.clueCount(), 1);
}

TEST("Parsing", "a cell count that is not a supported grid size is rejected") {
    checkParseRejects("", "empty input");
    checkParseRejects("123", "3 cells");
    checkParseRejects(std::string(80, '.'), "one cell short of 9x9");
    checkParseRejects(std::string(82, '.'), "one cell over 9x9");
    checkParseRejects("   \n\t ", "only ignored characters");
}

TEST("Parsing", "characters that are not digits, letters or separators are rejected") {
    checkParseRejects(std::string(80, '.') + "*", "'*' is not a cell character");
    checkParseRejects(std::string(80, '.') + "#", "'#' is not a cell character");
    checkParseRejects(std::string(80, '.') + "_", "'_' is not a cell character");
}

TEST("Parsing", "values larger than the grid allows are rejected") {
    checkParseRejects("5...............", "5 in a 4x4");
    checkParseRejects(std::string(80, '.') + "A", "10 in a 9x9");
    checkParseRejects("H" + std::string(255, '.'), "17 in a 16x16");
    checkParseRejects(std::string(80, '.') + "x", "letters are values: 'x' = 33 in a 9x9");
}

TEST("Parsing", "the one-line format writes one character per cell, '.' for empty") {
    std::string expected(kClassicPuzzle);
    std::ranges::replace(expected, '0', '.');
    CHECK_EQ(Board::parse(kClassicPuzzle).toString(), expected);
}

TEST("Parsing", "the pretty format draws separators between boxes") {
    const std::string expected =
        "1 . | . .\n"
        ". . | . .\n"
        "----+----\n"
        ". . | . .\n"
        "4 . | . .\n";
    CHECK_EQ(boardWith(2, {{0, 0, 1}, {3, 0, 4}}).toPrettyString(), expected);
}

TEST("Parsing", "both output formats parse back to the same board, including 16x16") {
    const Board board9 = Board::parse(kClassicPuzzle);
    CHECK_EQ(Board::parse(board9.toString()), board9);
    CHECK_EQ(Board::parse(board9.toPrettyString()), board9);

    // A 16x16 grid exercises the letter encoding in both directions.
    const auto board16 = MRVBacktrackingSolver().solve(Board(4));
    REQUIRE(board16.has_value());
    CHECK_EQ(Board::parse(board16->toString()), *board16);
    CHECK_EQ(Board::parse(board16->toPrettyString()), *board16);
}

// ===========================================================================
// Solver: correctness, counting and limits
// ===========================================================================

TEST("Solver", "solves the classic 9x9 puzzle to its known solution") {
    const Board puzzle = Board::parse(kClassicPuzzle);
    TRACE("input puzzle", boardText(puzzle));
    const auto solution = MRVBacktrackingSolver().solve(puzzle);
    TRACE("solver", solverStats(puzzle));
    REQUIRE(solution.has_value());
    TRACE("solution", boardText(*solution));
    CHECK_EQ(solution->toString(), std::string(kClassicSolution));
}

TEST("Solver", "the solution of a 4x4 keeps every given") {
    const Board puzzle = Board::parse("1...  ..2.  .3..  ...4");
    TRACE("input puzzle", boardText(puzzle));
    const auto solution = MRVBacktrackingSolver().solve(puzzle);
    TRACE("solver", solverStats(puzzle));
    REQUIRE(solution.has_value());
    TRACE("solution", boardText(*solution));
    checkSolves(*solution, puzzle);
}

TEST("Solver", "fills empty 4x4, 9x9, 16x16 and 25x25 grids") {
    for (const int n : {2, 3, 4, 5}) {
        CONTEXT("grid: ", gridName(n));
        const auto solution = MRVBacktrackingSolver().solve(Board(n));
        TRACE("solver", solverStats(Board(n), 1));
        REQUIRE(solution.has_value());
        TRACE("solution", boardText(*solution));
        checkSolves(*solution, Board(n));
    }
}

TEST("Solver", "an already solved board comes back unchanged") {
    const Board solved = Board::parse(kClassicSolution);
    const SolveResult result = MRVBacktrackingSolver().run(solved, {.solutionLimit = 2});
    TRACE("solver", statsText(result));
    CHECK_EQ(result.solutionCount, 1u);
    CHECK_EQ(result.firstSolution, solved);
}

TEST("Solver", "finds no solution when a cell has no candidates left") {
    // The givens don't conflict, but (0,2) can't hold 1 or 2 (its row) or 3 or 4 (its column).
    const Board deadEnd = Board::parse("12.. .... ..3. ..4.");
    TRACE("input puzzle", boardText(deadEnd));
    REQUIRE(deadEnd.isValid());
    const SolveResult result = MRVBacktrackingSolver().run(deadEnd, {.solutionLimit = 0});
    TRACE("solver", statsText(result));
    CHECK_EQ(result.solutionCount, 0u);
    CHECK(!result.firstSolution.has_value());
    CHECK(!result.aborted);
}

TEST("Solver", "finds no solution when the givens conflict") {
    std::string conflicting(kClassicPuzzle);
    conflicting[2] = '5';  // a second 5 in the first row
    const Board puzzle = Board::parse(conflicting);
    TRACE("input puzzle", boardText(puzzle));
    REQUIRE(!puzzle.isValid());
    CHECK_EQ(MRVBacktrackingSolver().countSolutions(puzzle, 0), 0u);
    CHECK(!MRVBacktrackingSolver().solve(puzzle).has_value());
}

TEST("Solver", "counts all 288 grids of an empty 4x4") {
    CHECK_EQ(MRVBacktrackingSolver().countSolutions(Board(2), 0), 288u);  // a known result
}

TEST("Solver", "counts the 72 completions of a 4x4 with one given") {
    // Relabelling values maps grids to grids, so each value is equally likely in any cell:
    // fixing one cell leaves 288 / 4 = 72 completions.
    CHECK_EQ(MRVBacktrackingSolver().countSolutions(boardWith(2, {{0, 0, 1}}), 0), 72u);
}

TEST("Solver", "stops counting once the solution limit is reached") {
    const MRVBacktrackingSolver solver;
    CHECK_EQ(solver.countSolutions(Board(3), 1), 1u);
    CHECK_EQ(solver.countSolutions(Board(3), 5), 5u);
    CHECK_EQ(solver.countSolutions(Board(2), 300), 288u);  // a limit above the total changes nothing
    CHECK_EQ(solver.countSolutions(Board::parse(kClassicPuzzle), 10), 1u);
}

TEST("Solver", "the uniqueness check tells one solution apart from none or many") {
    const MRVBacktrackingSolver solver;
    CHECK(solver.hasUniqueSolution(Board::parse(kClassicPuzzle)));
    CHECK(!solver.hasUniqueSolution(Board(3)));                             // many solutions
    CHECK(!solver.hasUniqueSolution(Board::parse("12.. .... ..3. ..4.")));  // none
}

TEST("Solver", "aborts once the node limit is reached") {
    // Counting every 9x9 grid is infeasible, so the node limit must cut the search off.
    const SolveResult result = MRVBacktrackingSolver().run(Board(3), {.solutionLimit = 0, .nodeLimit = 100});
    TRACE("solver", statsText(result), " (node limit 100)");
    CHECK(result.aborted);
    CHECK(result.nodes <= 100);  // the budget is an upper bound; the exact count is an implementation detail
}

TEST("Solver", "without a node limit the result is never marked aborted") {
    const SolveResult result = MRVBacktrackingSolver().run(Board::parse(kClassicPuzzle), {.solutionLimit = 2});
    TRACE("solver", statsText(result));
    CHECK(!result.aborted);
    CHECK(result.nodes > 0);
}

TEST("Solver", "random value order gives the same grid for the same seed") {
    const MRVBacktrackingSolver solver;
    Rng first(1);
    Rng again(1);
    Rng other(2);
    const auto a = solver.run(Board(3), {.rng = &first}).firstSolution;
    const auto b = solver.run(Board(3), {.rng = &again}).firstSolution;
    const auto c = solver.run(Board(3), {.rng = &other}).firstSolution;
    REQUIRE(a && b && c);
    TRACE("grid from seed 1 (twice)", boardText(*a));
    TRACE("grid from seed 2", boardText(*c));
    checkSolves(*a, Board(3));
    CHECK_EQ(*a, *b);
    CHECK(*a != *c);  // fixed seeds, so this is deterministic, not flaky
}

// ===========================================================================
// Randomness: portable helpers
// ===========================================================================

TEST("Randomness", "uniformBelow returns every value in range and nothing outside it") {
    Rng rng(7);
    std::set<std::uint64_t> seen;
    for (int i = 0; i < 1000; ++i) {
        const std::uint64_t value = sudoku::uniformBelow(rng, 7);
        if (value >= 7) FAIL("uniformBelow(rng, 7) returned ", value);
        seen.insert(value);
    }
    CHECK_EQ(seen.size(), 7u);
    CHECK_EQ(sudoku::uniformBelow(rng, 1), 0u);
}

TEST("Randomness", "shuffle reorders the items without losing or duplicating any") {
    Rng rng(3);
    std::vector<int> items(50);
    std::iota(items.begin(), items.end(), 0);
    const std::vector<int> original = items;
    sudoku::shuffle(std::span(items), rng);
    CHECK(items != original);  // fixed seed: deterministic
    std::ranges::sort(items);
    CHECK(items == original);
}

// ===========================================================================
// Generation
// ===========================================================================

TEST("Generation", "4x4 puzzles have exactly one solution (20 seeds)") {
    for (std::uint64_t seed = 0; seed < 20; ++seed) checkGeneratedPuzzle(2, seed);
}

TEST("Generation", "9x9 puzzles have exactly one solution (5 seeds)") {
    for (std::uint64_t seed = 0; seed < 5; ++seed) checkGeneratedPuzzle(3, seed);
}

TEST("Generation", "16x16 puzzles have exactly one solution (2 seeds)") {
    for (const std::uint64_t seed : {1u, 7u}) checkGeneratedPuzzle(4, seed);
}

TEST("Generation", "no single clue can be removed without losing uniqueness") {
    // With an unbounded uniqueness check, every clue that survives was needed. Removing any one of
    // them must therefore give a second solution. (With a node budget, some clues are kept only
    // because their check ran out of budget, so this property is only promised when unbounded.)
    for (const int n : {2, 3}) {
        for (const std::uint64_t seed : {1u, 2u}) {
            CONTEXT("grid: ", gridName(n), ", seed: ", seed, ", unbounded uniqueness checks");
            const auto generated = Generator().generate({.boxSize = n, .seed = seed, .uniquenessNodeLimit = 0});
            CONTEXT("puzzle: ", generated.puzzle.toString());
            TRACE("generated puzzle", boardText(generated.puzzle));
            Board puzzle = generated.puzzle;
            for (int i = 0; i < puzzle.cellCount(); ++i) {
                const int value = puzzle.at(i);
                if (value == 0) continue;
                puzzle.setAt(i, 0);
                if (MRVBacktrackingSolver().countSolutions(puzzle, 2) != 2) {
                    FAIL("clue ", value, " at (", i / puzzle.size(), ',', i % puzzle.size(), ") is redundant");
                }
                puzzle.setAt(i, value);
            }
            TRACE("minimality", "removed each of the ", puzzle.clueCount(),
                  " clues on its own; any redundant clue is reported as a failure above");
        }
    }
}

TEST("Generation", "the same seed gives an identical puzzle and solution") {
    CONTEXT("grid: ", gridName(3), ", seed: 12345");
    const Generator generator;
    const auto a = generator.generate({.boxSize = 3, .seed = 12345});
    const auto b = generator.generate({.boxSize = 3, .seed = 12345});
    TRACE("puzzle from seed 12345", boardText(a.puzzle));
    CHECK_EQ(a.puzzle, b.puzzle);
    CHECK_EQ(a.solution, b.solution);
}

TEST("Generation", "different seeds give different puzzles") {
    CONTEXT("grid: ", gridName(3), ", seeds: 12345 and 54321");
    const Generator generator;
    const auto a = generator.generate({.boxSize = 3, .seed = 12345});
    const auto b = generator.generate({.boxSize = 3, .seed = 54321});
    CHECK(a.puzzle != b.puzzle);  // fixed seeds, so this is deterministic, not flaky
}

TEST("Generation", "fixed seeds reproduce the recorded puzzles") {
    // Pins the "same seed, same puzzle on every compiler" promise. These strings were
    // recorded with GCC 14 / libstdc++. A failure on another toolchain means generation
    // depends on implementation-defined behaviour. Update them only when the generation
    // algorithm changes on purpose. (Re-recorded when completed grids became constructive
    // instead of a randomized solve; see DEVELOPMENT_LOG.md, step 8.)
    const Generator generator;
    {
        CONTEXT("grid: ", gridName(2), ", seed: 5");
        CHECK_EQ(generator.generate({.boxSize = 2, .seed = 5}).puzzle.toString(), std::string("....12..4.3....."));
    }
    {
        CONTEXT("grid: ", gridName(3), ", seed: 42");
        CHECK_EQ(generator.generate({.boxSize = 3, .seed = 42}).puzzle.toString(),
                 std::string("..6.2...5.1....6.3..8......4....729........54...8....6..4.6.9...7.21....29.58...."));
    }
}

TEST("Generation", "the generator uses the solver it is given") {
    const CountingSolver solver;
    const auto generated = Generator(solver).generate({.boxSize = 2, .seed = 1});
    TRACE("injected solver calls", solver.calls, " (1 grid fill + 1 uniqueness check per cell)");
    TRACE("generated puzzle", boardText(generated.puzzle));
    CHECK(solver.calls > 0);
    CHECK(MRVBacktrackingSolver().hasUniqueSolution(generated.puzzle));
}

TEST("Generation", "16x16 seeds whose random fill used to stall now generate quickly") {
    // Regression test, found by the property tests: these seeds' first fill attempt needs more
    // than 5 million search nodes, which made `generate` appear to hang before fill restarts.
    for (const std::uint64_t seed : {20261116u, 20261189u}) {
        CONTEXT("grid: ", gridName(4), ", seed: ", seed);
        const auto start = std::chrono::steady_clock::now();
        const auto generated = Generator().generate({.boxSize = 4, .seed = seed});
        const std::chrono::duration<double> elapsed = std::chrono::steady_clock::now() - start;
        TRACE("generated in", mini_test::formatDuration(elapsed.count() * 1000));
        CHECK(MRVBacktrackingSolver().hasUniqueSolution(generated.puzzle));
        CHECK(elapsed.count() < 10.0);  // generous: a normal 16x16 takes about 0.2 s
    }
}

TEST("Generation", "box sizes below 1, or too large to represent, are rejected") {
    CHECK_THROWS(Generator().generate({.boxSize = 0}), std::invalid_argument);
    CHECK_THROWS(Generator().generate({.boxSize = -2}), std::invalid_argument);
    CHECK_THROWS(Generator().generate({.boxSize = 1000}), std::length_error);
}

TEST("Generation", "a 1x1 grid is generated as a single empty cell") {
    const auto generated = Generator().generate({.boxSize = 1, .seed = 3});
    CHECK_EQ(generated.solution.toString(), std::string("1"));
    CHECK_EQ(generated.puzzle.toString(), std::string("."));  // forced: 1 is the only value
    CHECK(MRVBacktrackingSolver().hasUniqueSolution(generated.puzzle));
}

// ===========================================================================
// Large grids: generalized sizes, scalable bitsets and explicit resource limits
// ===========================================================================

namespace {

// A complete grid with about `percent`% of its cells blanked, deterministically from `seed`.
Board blankedGrid(int boxSize, int percent, std::uint64_t seed) {
    Rng rng(seed);
    Board puzzle = sudoku::randomCompletedGrid(boxSize, rng);
    for (int i = 0; i < puzzle.cellCount(); ++i) {
        if (static_cast<int>(sudoku::uniformBelow(rng, 100)) < percent) puzzle.setAt(i, 0);
    }
    return puzzle;
}

}  // namespace

TEST("Large grids", "constructive completed grids are valid for every box size from 1 to 10") {
    for (int n = 1; n <= 10; ++n) {
        CONTEXT("grid: ", gridName(n));
        Rng rng(static_cast<std::uint64_t>(n));
        const Board grid = sudoku::randomCompletedGrid(n, rng);
        CHECK(grid.isFilled());
        CHECK(grid.isValid());
    }
}

TEST("Large grids", "candidate bitsets span word boundaries: 64x64 uses 1 word, 81x81 and 100x100 use 2") {
    // Value 64 is the top bit of the first word. In 81x81 and 100x100 the largest value lives in
    // the second word; in 64x64 the largest value is 64 itself, so 63 is used as the second value.
    for (const auto& [boxSize, words] : {std::pair{8, 1}, {9, 2}, {10, 2}}) {
        CONTEXT("grid: ", gridName(boxSize));
        sudoku::Constraints constraints(boxSize);
        CHECK_EQ(constraints.words(), words);
        const int top = boxSize * boxSize;
        const int second = top == 64 ? 63 : top;
        CHECK_EQ(constraints.candidateCount(0, 0), top);
        constraints.place(0, 0, 64);
        constraints.place(0, 1, second);
        CHECK(!constraints.canPlace(0, 5, 64));      // same row
        CHECK(!constraints.canPlace(7, 0, second));  // same box as (0, 1)
        CHECK(constraints.canPlace(top - 1, top - 1, 64));
        CHECK_EQ(constraints.candidateCount(0, 2), top - 2);
        const std::vector<int> candidates = constraints.candidates(0, 2);
        CHECK(std::ranges::find(candidates, 64) == candidates.end());
        CHECK(std::ranges::find(candidates, second) == candidates.end());
        CHECK_EQ(static_cast<int>(candidates.size()), top - 2);
        int expectedLast = top;  // ascending, and nothing beyond the grid's largest value
        while (expectedLast == 64 || expectedLast == second) --expectedLast;
        CHECK_EQ(candidates.back(), expectedLast);
        constraints.remove(0, 0, 64);
        CHECK(constraints.canPlace(0, 5, 64));
    }
}

TEST("Large grids", "solves 36x36, 64x64, 81x81 and 100x100 puzzles with 20% of cells blank") {
    for (const int n : {6, 8, 9, 10}) {
        CONTEXT("grid: ", gridName(n));
        const Board puzzle = blankedGrid(n, 20, 7);
        const SolveResult result = MRVBacktrackingSolver().run(puzzle, {.nodeLimit = 1'000'000});
        TRACE("solver", statsText(result), ", ", result.backtracks, " backtracks");
        REQUIRE(result.firstSolution.has_value());
        CHECK(result.stopReason == sudoku::StopReason::SolutionLimit);
        checkSolves(*result.firstSolution, puzzle);
    }
}

TEST("Large grids", "a search over its node limit stops and reports why, instead of failing silently") {
    // Deterministic: at 30% blanks this 100x100 puzzle needs far more than 50 000 nodes.
    const Board puzzle = blankedGrid(10, 30, 7);
    const SolveResult result = MRVBacktrackingSolver().run(puzzle, {.nodeLimit = 50'000});
    CHECK(result.aborted);
    CHECK(result.stopReason == sudoku::StopReason::NodeLimit);
    CHECK_EQ(result.nodes, 50'000u);
    CHECK_EQ(result.solutionCount, 0u);
}

TEST("Large grids", "a time limit stops a long search and reports it") {
    // Counting every 9x9 grid would take far longer than the 50 ms allowed.
    const auto start = std::chrono::steady_clock::now();
    const SolveResult result =
        MRVBacktrackingSolver().run(Board(3), {.solutionLimit = 0, .timeLimit = std::chrono::milliseconds{50}});
    const std::chrono::duration<double> elapsed = std::chrono::steady_clock::now() - start;
    TRACE("stopped after", mini_test::formatDuration(elapsed.count() * 1000), ", ", statsText(result));
    CHECK(result.aborted);
    CHECK(result.stopReason == sudoku::StopReason::TimeLimit);
    CHECK(result.solutionCount > 0);
    CHECK(elapsed.count() < 2.0);  // generous; the clock is checked every 256 nodes
}

TEST("Large grids", "time-limited 64x64 and 100x100 generation stops early, stays unique and says so") {
    for (const int n : {8, 10}) {
        CONTEXT("grid: ", gridName(n));
        const auto generated =
            Generator().generate({.boxSize = n, .seed = 1, .timeLimit = std::chrono::milliseconds{300}});
        const sudoku::GenerationStats& stats = generated.stats;
        TRACE("stats", stats.cellsTried, " of ", generated.puzzle.cellCount(), " cells tried, ",
              generated.puzzle.clueCount(), " clues");
        CHECK(stats.timeLimitReached);
        CHECK(!stats.minimal());
        CHECK(stats.cellsTried < generated.puzzle.cellCount());
        CHECK(generated.puzzle.clueCount() < generated.puzzle.cellCount());
        checkSolves(generated.solution, generated.puzzle);
        const SolveResult check = MRVBacktrackingSolver().run(generated.puzzle, {.solutionLimit = 2, .nodeLimit = 2'000'000});
        TRACE("uniqueness check", statsText(check));
        CHECK(!check.aborted);
        CHECK_EQ(check.solutionCount, 1u);
    }
}

TEST("Large grids", "a 1x1 grid solves to its single value") {
    const auto solution = MRVBacktrackingSolver().solve(Board(1));
    REQUIRE(solution.has_value());
    CHECK_EQ(solution->toString(), std::string("1"));
}

// ===========================================================================
// Tracing: solver and generator observers
// ===========================================================================

namespace {

using sudoku::GeneratorEvent;
using sudoku::SolverEvent;

// Keeps every event (with board pointers cleared, since they are only valid during the call).
class RecordingSolverObserver final : public sudoku::SolverObserver {
public:
    void onSolverEvent(const SolverEvent& event) override {
        events.push_back(event);
        events.back().board = nullptr;
    }
    [[nodiscard]] std::size_t count(SolverEvent::Kind kind) const {
        return static_cast<std::size_t>(std::ranges::count(events, kind, &SolverEvent::kind));
    }
    std::vector<SolverEvent> events;
};

class RecordingGeneratorObserver final : public sudoku::GeneratorObserver {
public:
    void onGeneratorEvent(const GeneratorEvent& event) override {
        if (event.kind == GeneratorEvent::Kind::FilledGrid) filled = *event.board;
        if (event.kind == GeneratorEvent::Kind::Finished) finished = {*event.board, *event.solution};
        events.push_back(event);
        events.back().board = nullptr;
        events.back().solution = nullptr;
    }
    std::vector<GeneratorEvent> events;
    std::optional<Board> filled;
    std::optional<std::pair<Board, Board>> finished;  // (puzzle, solution)
};

std::size_t lineCount(const std::string& text) { return static_cast<std::size_t>(std::ranges::count(text, '\n')); }

std::string lastLine(const std::string& text) {
    const auto end = text.find_last_not_of('\n');
    const auto start = text.rfind('\n', end);
    return text.substr(start == std::string::npos ? 0 : start + 1, end - (start == std::string::npos ? 0 : start + 1) + 1);
}

}  // namespace

TEST("Tracing", "solver results are identical with and without an observer") {
    struct Case {
        const char* name;
        Board puzzle;
        SolveOptions options;
    };
    const std::vector<Case> cases = {
        {"classic 9x9, limit 2", Board::parse(kClassicPuzzle), {.solutionLimit = 2}},
        {"empty 4x4, count all", Board(2), {.solutionLimit = 0}},
        {"dead-end 4x4", Board::parse("12.. .... ..3. ..4."), {.solutionLimit = 0}},
        {"empty 9x9, node limit 100", Board(3), {.solutionLimit = 0, .nodeLimit = 100}},
    };
    for (const auto& c : cases) {
        CONTEXT("case: ", c.name);
        RecordingSolverObserver observer;
        SolveOptions traced = c.options;
        traced.observer = &observer;
        const SolveResult plain = MRVBacktrackingSolver().run(c.puzzle, c.options);
        const SolveResult withObserver = MRVBacktrackingSolver().run(c.puzzle, traced);
        TRACE("result", statsText(plain), ", ", plain.backtracks, " backtracks; ", observer.events.size(), " events");
        CHECK_EQ(withObserver.solutionCount, plain.solutionCount);
        CHECK_EQ(withObserver.firstSolution, plain.firstSolution);
        CHECK_EQ(withObserver.nodes, plain.nodes);
        CHECK_EQ(withObserver.backtracks, plain.backtracks);
        CHECK_EQ(withObserver.aborted, plain.aborted);
    }
}

TEST("Tracing", "solver events account for every node, placement, backtrack and solution") {
    // Counting all 288 empty 4x4 grids exercises guessing, dead ends and backtracking.
    RecordingSolverObserver observer;
    const SolveResult result = MRVBacktrackingSolver().run(Board(2), {.solutionLimit = 0, .observer = &observer});
    const auto& events = observer.events;
    REQUIRE(events.size() >= 2);
    TRACE("events", events.size(), " total: ", observer.count(SolverEvent::Kind::Select), " select, ",
          observer.count(SolverEvent::Kind::Place), " place, ", observer.count(SolverEvent::Kind::Backtrack),
          " backtrack, ", observer.count(SolverEvent::Kind::DeadEnd), " dead end, ",
          observer.count(SolverEvent::Kind::Solution), " solution");

    CHECK(events.front().kind == SolverEvent::Kind::Start);
    CHECK(events.back().kind == SolverEvent::Kind::Finish);
    CHECK(events.back().reason == sudoku::StopReason::Exhausted);
    // Every node either selects a cell (MRV) or finds a dead end.
    CHECK_EQ(observer.count(SolverEvent::Kind::Select) + observer.count(SolverEvent::Kind::DeadEnd), result.nodes);
    CHECK_EQ(observer.count(SolverEvent::Kind::Backtrack), result.backtracks);
    CHECK_EQ(observer.count(SolverEvent::Kind::Solution), result.solutionCount);
    CHECK_EQ(events.back().solutions, 288u);

    // Each placement tries one of the candidates of the cell selected at the same depth.
    std::vector<SolverEvent> selected(17);
    int badPlacements = 0;
    for (const auto& e : events) {
        if (e.kind == SolverEvent::Kind::Select) selected[static_cast<std::size_t>(e.depth)] = e;
        if (e.kind != SolverEvent::Kind::Place) continue;
        const auto& s = selected[static_cast<std::size_t>(e.depth)];
        if (s.row != e.row || s.col != e.col || std::ranges::find(s.candidates, e.value) == s.candidates.end()) ++badPlacements;
    }
    CHECK_EQ(badPlacements, 0);
}

TEST("Tracing", "a dead end is reported at the cell that has no candidates") {
    // (0,2) can't hold 1 or 2 (its row) or 3 or 4 (its column), so MRV finds it immediately.
    RecordingSolverObserver observer;
    const SolveResult result =
        MRVBacktrackingSolver().run(Board::parse("12.. .... ..3. ..4."), {.solutionLimit = 0, .observer = &observer});
    REQUIRE_EQ(observer.events.size(), 3u);  // Start, DeadEnd, Finish
    const SolverEvent& deadEnd = observer.events[1];
    CHECK(deadEnd.kind == SolverEvent::Kind::DeadEnd);
    CHECK_EQ(deadEnd.row, 0);
    CHECK_EQ(deadEnd.col, 2);
    CHECK_EQ(deadEnd.depth, 0);
    CHECK(observer.events.back().reason == sudoku::StopReason::Exhausted);
    CHECK_EQ(result.solutionCount, 0u);
}

TEST("Tracing", "the finish event names why the search stopped") {
    const auto finishReason = [](const Board& board, SolveOptions options) {
        RecordingSolverObserver observer;
        options.observer = &observer;
        static_cast<void>(MRVBacktrackingSolver().run(board, options));
        return observer.events.back().reason;
    };
    std::string conflicting(kClassicPuzzle);
    conflicting[2] = '5';
    CHECK(finishReason(Board::parse(kClassicPuzzle), {.solutionLimit = 1}) == sudoku::StopReason::SolutionLimit);
    CHECK(finishReason(Board::parse(kClassicPuzzle), {.solutionLimit = 2}) == sudoku::StopReason::Exhausted);
    CHECK(finishReason(Board(3), {.solutionLimit = 0, .nodeLimit = 100}) == sudoku::StopReason::NodeLimit);
    CHECK(finishReason(Board::parse(conflicting), {}) == sudoku::StopReason::InvalidGivens);

    // Conflicting givens are rejected before any search, so Finish is the only event.
    RecordingSolverObserver observer;
    static_cast<void>(MRVBacktrackingSolver().run(Board::parse(conflicting), {.observer = &observer}));
    CHECK_EQ(observer.events.size(), 1u);
}

TEST("Tracing", "generator output is identical with and without an observer") {
    for (const auto& [boxSize, seed] : {std::pair{2, std::uint64_t{5}}, {3, 42}, {3, 7}, {4, 1}}) {
        CONTEXT("grid: ", gridName(boxSize), ", seed: ", seed);
        RecordingGeneratorObserver observer;
        const auto plain = Generator().generate({.boxSize = boxSize, .seed = seed});
        const auto traced = Generator().generate({.boxSize = boxSize, .seed = seed, .observer = &observer});
        CHECK_EQ(traced.puzzle, plain.puzzle);
        CHECK_EQ(traced.solution, plain.solution);
    }
}

TEST("Tracing", "generator events: the filled grid, one removal attempt per cell, then the result") {
    RecordingGeneratorObserver observer;
    const auto generated = Generator().generate({.boxSize = 3, .seed = 42, .observer = &observer});
    const auto& events = observer.events;
    REQUIRE_EQ(events.size(), 83u);  // FilledGrid + 81 removals + Finished
    CHECK(events.front().kind == GeneratorEvent::Kind::FilledGrid);
    CHECK_EQ(observer.filled, std::optional<Board>(generated.solution));
    REQUIRE(observer.finished.has_value());
    CHECK_EQ(observer.finished->first, generated.puzzle);
    CHECK_EQ(observer.finished->second, generated.solution);

    int clues = 81;
    int accepted = 0;
    int inconsistent = 0;
    for (std::size_t i = 1; i <= 81; ++i) {
        const GeneratorEvent& e = events[i];
        const bool removed =
            e.verdict == GeneratorEvent::Verdict::Accepted || e.verdict == GeneratorEvent::Verdict::Forced;
        if (removed) {
            --clues;
            ++accepted;
        }
        // The index runs 1..81, the clue count drops exactly when a removal is accepted, a removal
        // accepted by search found exactly one solution, and a forced removal ran no search.
        if (e.kind != GeneratorEvent::Kind::Removal || e.index != static_cast<int>(i) || e.clues != clues ||
            (e.verdict == GeneratorEvent::Verdict::Accepted && e.solutions != 1) ||
            (e.verdict == GeneratorEvent::Verdict::Forced && e.nodes != 0)) {
            ++inconsistent;
        }
    }
    const sudoku::GenerationStats& stats = generated.stats;
    CHECK_EQ(stats.cellsTried, 81);
    CHECK_EQ(stats.removedBySearch + stats.removedForced, accepted);
    CHECK_EQ(stats.keptAmbiguous + stats.keptUnverified, 81 - accepted);
    CHECK(stats.removedForced > 0);  // early removals from a full grid are always forced
    CHECK(stats.minimal());
    TRACE("removals", accepted, " accepted, ", 81 - accepted, " kept");
    CHECK_EQ(inconsistent, 0);
    CHECK_EQ(accepted, 81 - generated.puzzle.clueCount());
    CHECK_EQ(events.back().clues, generated.puzzle.clueCount());
}

TEST("Tracing", "the text tracer stops at the event limit but always prints the summary") {
    RecordingSolverObserver recorder;
    static_cast<void>(MRVBacktrackingSolver().run(Board(2), {.solutionLimit = 0, .observer = &recorder}));
    const std::size_t events = recorder.events.size();

    std::ostringstream out;
    sudoku::TextSolverTracer tracer(out, 10);
    static_cast<void>(MRVBacktrackingSolver().run(Board(2), {.solutionLimit = 0, .observer = &tracer}));
    const std::string text = out.str();
    TRACE("trace output", "\n", text);
    CHECK_EQ(tracer.budget().printed(), 10u);
    CHECK_EQ(tracer.budget().suppressed(), events - 1 - 10);  // the Finish summary is not counted
    CHECK_EQ(lineCount(text), 12u);                            // 10 events + "not shown" + summary
    CHECK(text.find("more events not shown") != std::string::npos);
    CHECK(lastLine(text).starts_with("[solve] finished: search space exhausted"));
}

TEST("Tracing", "a text trace limit of 0 prints every event") {
    RecordingSolverObserver recorder;
    static_cast<void>(MRVBacktrackingSolver().run(Board::parse(kClassicPuzzle), {.solutionLimit = 2, .observer = &recorder}));

    std::ostringstream out;
    sudoku::TextSolverTracer tracer(out, 0);
    static_cast<void>(MRVBacktrackingSolver().run(Board::parse(kClassicPuzzle), {.solutionLimit = 2, .observer = &tracer}));
    CHECK_EQ(tracer.budget().printed(), recorder.events.size() - 1);
    CHECK_EQ(tracer.budget().suppressed(), 0u);
    CHECK_EQ(lineCount(out.str()), recorder.events.size());
    CHECK(out.str().find("not shown") == std::string::npos);
}

TEST("Tracing", "the generator text trace ends with the final puzzle and solution") {
    std::ostringstream out;
    sudoku::TextGeneratorTracer tracer(out, 0);
    const auto generated = Generator().generate({.boxSize = 2, .seed = 5, .observer = &tracer});
    const std::string text = out.str();
    TRACE("trace output", "\n", text);
    CHECK(text.starts_with("[generate] seed 5, 4x4: random complete grid"));
    CHECK_EQ(lineCount(text), 1 + 5 + 16 + 1 + 1 + 5 + 1 + 5);  // header+grid, 16 removals, summary, puzzle, solution
    const sudoku::GenerationStats& stats = generated.stats;
    const std::string summary = "[generate] finished: " + std::to_string(generated.puzzle.clueCount()) +
                                " clues left; " + std::to_string(stats.removedBySearch + stats.removedForced) +
                                " removed (" + std::to_string(stats.removedForced) + " forced, ";
    CHECK(text.find(summary) != std::string::npos);
    std::string indentedPuzzle;  // the tracer indents each grid line by four spaces
    std::istringstream lines(generated.puzzle.toPrettyString());
    for (std::string line; std::getline(lines, line);) indentedPuzzle += "    " + line + '\n';
    CHECK(text.find("  puzzle:\n" + indentedPuzzle + "  solution:\n") != std::string::npos);
}

TEST("Tracing", "tracing is off unless an observer is given") {
    CHECK(SolveOptions{}.observer == nullptr);
    CHECK(GeneratorOptions{}.observer == nullptr);
}

// ===========================================================================
// CLI (in-process; CTest additionally runs the real executable)
// ===========================================================================

TEST("CLI", "generate --seed 1 prints a 9x9 puzzle with exactly one solution") {
    CONTEXT("command: sudoku generate --size 3 --seed 1");
    const CliResult result = runCli({"generate", "--size", "3", "--seed", "1"});
    CONTEXT("stderr: ", escaped(result.err));
    TRACE("stdout", escaped(result.out));
    REQUIRE_EQ(result.exitCode, sudoku::cli::kExitOk);
    const Board puzzle = Board::parse(result.out);
    CHECK_EQ(puzzle.boxSize(), 3);
    CHECK(MRVBacktrackingSolver().hasUniqueSolution(puzzle));
    CHECK(result.err.starts_with("seed: 1,"));
}

TEST("CLI", "solve prints the known solution of the classic puzzle") {
    CONTEXT("command: sudoku solve <classic puzzle>");
    const CliResult result = runCli({"solve", kClassicPuzzle});
    CONTEXT("stderr: ", escaped(result.err));
    CHECK_EQ(result.exitCode, sudoku::cli::kExitOk);
    CHECK_EQ(result.out, std::string(kClassicSolution) + "\n");
}

TEST("CLI", "solve rejects malformed input with exit code 2") {
    CONTEXT("command: sudoku solve 12x");
    const CliResult result = runCli({"solve", "12x"});
    TRACE("stderr", escaped(result.err));
    CHECK_EQ(result.exitCode, sudoku::cli::kExitUsage);
    CHECK_EQ(result.out, std::string());
    CHECK(result.err.starts_with("error:"));
}

TEST("CLI", "without trace flags, solve writes nothing to stderr") {
    const CliResult result = runCli({"solve", kClassicPuzzle});
    CHECK_EQ(result.err, std::string());
}

TEST("CLI", "solve --trace-solver traces to stderr and leaves stdout unchanged") {
    const CliResult plain = runCli({"solve", kClassicPuzzle});
    const CliResult traced = runCli({"solve", kClassicPuzzle, "--trace-solver"});
    TRACE("stderr (first line)", traced.err.substr(0, traced.err.find('\n')));
    CHECK_EQ(traced.exitCode, plain.exitCode);
    CHECK_EQ(traced.out, plain.out);
    CHECK(traced.err.starts_with("[solve] start: 9x9, 30 givens, 51 empty cells\n"));
    CHECK(traced.err.find("solution #1 found") != std::string::npos);
    CHECK(lastLine(traced.err).starts_with("[solve] finished:"));
}

TEST("CLI", "generate --trace-generation traces to stderr and leaves stdout unchanged") {
    const CliResult plain = runCli({"generate", "--seed", "42"});
    const CliResult traced = runCli({"generate", "--seed", "42", "--trace-generation"});
    CHECK_EQ(traced.exitCode, plain.exitCode);
    CHECK_EQ(traced.out, plain.out);
    CHECK(traced.err.starts_with("[generate] seed 42, 9x9: random complete grid"));
    CHECK(traced.err.find("[generate] removal 81/81:") != std::string::npos);
    const int clues = Board::parse(plain.out).clueCount();
    CHECK(traced.err.find("[generate] finished: " + std::to_string(clues) + " clues left") != std::string::npos);
}

TEST("CLI", "--trace-limit caps the trace; it needs a trace flag and a number") {
    const CliResult capped = runCli({"solve", kClassicPuzzle, "--trace-solver", "--trace-limit", "3"});
    TRACE("stderr", "\n", capped.err);
    CHECK_EQ(capped.exitCode, sudoku::cli::kExitOk);
    CHECK_EQ(lineCount(capped.err), 5u);  // 3 events + "not shown" + summary
    CHECK_EQ(runCli({"solve", kClassicPuzzle, "--trace-limit", "3"}).exitCode, sudoku::cli::kExitUsage);
    CHECK_EQ(runCli({"solve", kClassicPuzzle, "--trace-solver", "--trace-limit", "x"}).exitCode,
             sudoku::cli::kExitUsage);
    CHECK_EQ(runCli({"generate", "--trace-generation", "--trace-limit"}).exitCode, sudoku::cli::kExitUsage);
}

TEST("CLI", "a puzzle argument that names a file is read from the file") {
    const auto path = std::filesystem::temp_directory_path() / "sudoku_cli_test_puzzle.txt";
    {
        std::ofstream file(path);
        file << Board::parse(kClassicPuzzle).toPrettyString();
    }
    CONTEXT("file: ", path.string());
    const CliResult result = runCli({"solve", path.string()});
    std::filesystem::remove(path);
    CHECK_EQ(result.exitCode, sudoku::cli::kExitOk);
    CHECK_EQ(result.out, std::string(kClassicSolution) + "\n");
}

TEST("CLI", "generate works beyond 25x25 and prints the number format; --time-limit warns when it stops early") {
    const CliResult result = runCli({"generate", "--size", "6", "--seed", "2", "--time-limit", "0.2"});
    CONTEXT("stderr: ", escaped(result.err));
    REQUIRE_EQ(result.exitCode, sudoku::cli::kExitOk);
    const Board puzzle = Board::parse(result.out);
    CHECK_EQ(puzzle.size(), 36);
    CHECK(result.out.find(' ') != std::string::npos);  // numbers, not one character per cell
    CHECK(result.err.find("warning: time limit reached") != std::string::npos);
}

TEST("CLI", "a search stopped by --node-limit exits with code 3 and says what is incomplete") {
    const CliResult count = runCli({"count", std::string(81, '.'), "--limit", "0", "--node-limit", "1000"});
    TRACE("count stdout", escaped(count.out));
    CHECK_EQ(count.exitCode, sudoku::cli::kExitLimit);
    CHECK(count.out.find("(lower bound: node limit reached)") != std::string::npos);

    const auto path = std::filesystem::temp_directory_path() / "sudoku_cli_test_hard_100x100.txt";
    {
        std::ofstream file(path);
        file << blankedGrid(10, 30, 7).toString();
    }
    // Pinned to MRV: auto would pick DLX at 30% empty on 100x100, and DLX solves this within the limit.
    const CliResult solve = runCli({"solve", path.string(), "--node-limit", "20000", "--solver", "mrv"});
    std::filesystem::remove(path);
    TRACE("solve stderr", escaped(solve.err));
    CHECK_EQ(solve.exitCode, sudoku::cli::kExitLimit);
    CHECK_EQ(solve.out, std::string());
    CHECK(solve.err.starts_with("error: search stopped (node limit reached)"));
}

TEST("CLI", "grids too large to represent exit with code 3; bad limits are usage errors") {
    const CliResult tooLarge = runCli({"generate", "--size", "1000"});
    TRACE("stderr", escaped(tooLarge.err));
    CHECK_EQ(tooLarge.exitCode, sudoku::cli::kExitLimit);
    CHECK(tooLarge.err.find("too large") != std::string::npos);
    CHECK_EQ(runCli({"generate", "--size", "99999999999999"}).exitCode, sudoku::cli::kExitLimit);
    CHECK_EQ(runCli({"generate", "--size", "0"}).exitCode, sudoku::cli::kExitUsage);
    CHECK_EQ(runCli({"solve", kClassicPuzzle, "--time-limit", "abc"}).exitCode, sudoku::cli::kExitUsage);
    CHECK_EQ(runCli({"solve", kClassicPuzzle, "--time-limit", "-1"}).exitCode, sudoku::cli::kExitUsage);
    CHECK_EQ(runCli({"count", kClassicPuzzle, "--node-limit", "x"}).exitCode, sudoku::cli::kExitUsage);
}

// ===========================================================================
// Solvers: every check here runs against all three algorithms (naive, MRV, DLX)
// ===========================================================================

namespace {

using sudoku::DLXSolver;
using sudoku::StopReason;

// A well-known hard 9x9 puzzle with a unique solution.
constexpr const char* kHardPuzzle =
    "800000000003600000070090200050007000000045700000100030001000068008500010090000400";

// Valid givens but no solution: cell (0,2) can hold neither 1, 2 (its row) nor 3, 4 (its column).
constexpr const char* kUnsatisfiable4x4 = "12........3...4.";

// The classic puzzle with one empty cell filled by a candidate that is not its solution value:
// the givens stay conflict-free, but the unique solution is gone.
Board classicWithWrongCandidate() {
    Board puzzle = Board::parse(kClassicPuzzle);
    const Board solution = Board::parse(kClassicSolution);
    const auto constraints = sudoku::Constraints::fromBoard(puzzle);
    for (int i = 0; i < puzzle.cellCount(); ++i) {
        if (puzzle.at(i) != 0) continue;
        for (const int value : constraints->candidates(i / 9, i % 9)) {
            if (value != solution.at(i)) {
                puzzle.setAt(i, value);
                return puzzle;
            }
        }
    }
    throw std::logic_error("no wrong candidate found");
}

std::string resultText(const SolveResult& r) {
    return mini_test::concat(statsText(r), ", ", r.backtracks, " backtracks, stop: ", sudoku::describe(r.stopReason));
}

}  // namespace

TEST("Solvers", "every solver returns the known unique solution of the classic, hard, 4x4 and 16x16 puzzles") {
    const auto small = Generator().generate({.boxSize = 2, .seed = 5});
    const auto large = Generator().generate({.boxSize = 4, .seed = 39});  // seed 39: naive proves it unique in ~0.1 s (most 16x16 seeds take seconds)
    const std::vector<std::pair<Board, std::optional<Board>>> cases = {
        {Board::parse(kClassicPuzzle), Board::parse(kClassicSolution)},
        {Board::parse(kHardPuzzle), std::nullopt},  // solution not hard-coded: solvers must agree
        {small.puzzle, small.solution},
        {large.puzzle, large.solution},
    };
    for (const auto& [puzzle, known] : cases) {
        CONTEXT("puzzle: ", mini_test::abbreviate(puzzle.toString()));
        std::optional<Board> agreed = known;
        for (const sudoku::Solver* solver : allSolvers()) {
            CONTEXT("solver: ", solver->name());
            const SolveResult result = solver->run(puzzle, {.solutionLimit = 2});
            TRACE("result", resultText(result));
            CHECK_EQ(result.solutionCount, 1u);
            CHECK(result.stopReason == StopReason::Exhausted);
            CHECK(!result.aborted);
            REQUIRE(result.firstSolution.has_value());
            checkSolutionIndependently(*result.firstSolution, puzzle);
            if (!agreed) agreed = result.firstSolution;
            CHECK_EQ(*result.firstSolution, *agreed);
        }
    }
}

TEST("Solvers", "every solver counts 288 empty 4x4 grids, 72 with one given, and stops multi-solution 9x9 at 2") {
    Board multi = Board::parse(kClassicPuzzle);
    multi.setAt(0, 0);  // the classic puzzle without two of its clues has several solutions
    multi.setAt(1, 0);
    for (const sudoku::Solver* solver : allSolvers()) {
        CONTEXT("solver: ", solver->name());
        CHECK_EQ(solver->countSolutions(Board(2), 0), 288u);
        CHECK_EQ(solver->countSolutions(boardWith(2, {{0, 0, 1}}), 0), 72u);
        const SolveResult result = solver->run(multi, {.solutionLimit = 2});
        CHECK_EQ(result.solutionCount, 2u);
        CHECK(result.stopReason == StopReason::SolutionLimit);
        REQUIRE(result.firstSolution.has_value());
        checkSolutionIndependently(*result.firstSolution, multi);
    }
}

TEST("Solvers", "valid but unsatisfiable puzzles: every solver exhausts the search and reports 0 solutions") {
    const std::vector<Board> puzzles = {Board::parse(kUnsatisfiable4x4), classicWithWrongCandidate()};
    for (const Board& puzzle : puzzles) {
        CONTEXT("puzzle: ", puzzle.toString());
        REQUIRE(puzzle.isValid());
        for (const sudoku::Solver* solver : allSolvers()) {
            CONTEXT("solver: ", solver->name());
            const SolveResult result = solver->run(puzzle, {.solutionLimit = 0});
            TRACE("result", resultText(result));
            CHECK_EQ(result.solutionCount, 0u);
            CHECK(result.stopReason == StopReason::Exhausted);
            CHECK(!result.aborted);
            CHECK(!result.firstSolution.has_value());
        }
    }
}

TEST("Solvers", "conflicting givens: every solver reports InvalidGivens without searching") {
    for (const sudoku::Solver* solver : allSolvers()) {
        CONTEXT("solver: ", solver->name());
        const SolveResult result = solver->run(boardWith(3, {{0, 0, 5}, {0, 8, 5}}), {.solutionLimit = 0});
        CHECK_EQ(result.solutionCount, 0u);
        CHECK(result.stopReason == StopReason::InvalidGivens);
        CHECK_EQ(result.nodes, 0u);
        CHECK(!result.aborted);
    }
}

TEST("Solvers", "completed boards of box size 1 to 4: every solver reports exactly one solution with no search") {
    for (int n = 1; n <= 4; ++n) {
        Rng rng(static_cast<std::uint64_t>(n));
        const Board grid = sudoku::randomCompletedGrid(n, rng);
        for (const sudoku::Solver* solver : allSolvers()) {
            CONTEXT("grid: ", gridName(n), ", solver: ", solver->name());
            const SolveResult unique = solver->run(grid, {.solutionLimit = 2});
            CHECK_EQ(unique.solutionCount, 1u);
            CHECK(unique.stopReason == StopReason::Exhausted);
            CHECK_EQ(unique.nodes, 0u);
            CHECK_EQ(unique.firstSolution, grid);
            CHECK(solver->run(grid, {}).stopReason == StopReason::SolutionLimit);
        }
    }
}

TEST("Solvers", "empty boards of box size 1 to 4: every solver fills them with a valid grid") {
    for (int n = 1; n <= 4; ++n) {
        for (const sudoku::Solver* solver : allSolvers()) {
            CONTEXT("grid: ", gridName(n), ", solver: ", solver->name());
            const SolveResult result = solver->run(Board(n), {.nodeLimit = 1'000'000});
            TRACE("result", resultText(result));
            REQUIRE(result.firstSolution.has_value());
            checkSolutionIndependently(*result.firstSolution, Board(n));
            CHECK(result.stopReason == StopReason::SolutionLimit);
        }
    }
}

TEST("Solvers", "solution limits 0, 1 and 2 stop where documented, for every solver") {
    const Board unique = Board::parse(kClassicPuzzle);
    for (const sudoku::Solver* solver : allSolvers()) {
        CONTEXT("solver: ", solver->name());
        const SolveResult one = solver->run(Board(2), {.solutionLimit = 1});
        CHECK_EQ(one.solutionCount, 1u);
        CHECK(one.stopReason == StopReason::SolutionLimit);
        const SolveResult two = solver->run(Board(2), {.solutionLimit = 2});
        CHECK_EQ(two.solutionCount, 2u);
        CHECK(two.stopReason == StopReason::SolutionLimit);
        const SolveResult all = solver->run(Board(2), {.solutionLimit = 0});
        CHECK_EQ(all.solutionCount, 288u);
        CHECK(all.stopReason == StopReason::Exhausted);
        const SolveResult proved = solver->run(unique, {.solutionLimit = 2});
        CHECK_EQ(proved.solutionCount, 1u);
        CHECK(proved.stopReason == StopReason::Exhausted);
    }
}

TEST("Solvers", "node limits: every solver stops at exactly the limit, counts are lower bounds and never shrink") {
    for (const sudoku::Solver* solver : allSolvers()) {
        CONTEXT("solver: ", solver->name());
        std::size_t previous = 0;
        for (const std::uint64_t limit : {1u, 10u, 100u, 1'000u, 10'000u}) {
            CONTEXT("node limit: ", limit);
            const SolveResult result = solver->run(Board(3), {.solutionLimit = 0, .nodeLimit = limit});
            TRACE("result", resultText(result));
            CHECK(result.aborted);
            CHECK(result.stopReason == StopReason::NodeLimit);
            CHECK_EQ(result.nodes, limit);
            CHECK(result.solutionCount >= previous);
            previous = result.solutionCount;
            if (result.firstSolution) checkSolutionIndependently(*result.firstSolution, Board(3));
        }
    }
}

TEST("Solvers", "time limits: every solver stops an exhaustive count of an empty 9x9 with TimeLimit") {
    for (const sudoku::Solver* solver : allSolvers()) {
        CONTEXT("solver: ", solver->name());
        const auto start = std::chrono::steady_clock::now();
        const SolveResult result =
            solver->run(Board(3), {.solutionLimit = 0, .timeLimit = std::chrono::milliseconds{30}});
        const std::chrono::duration<double, std::milli> elapsed = std::chrono::steady_clock::now() - start;
        TRACE("result", resultText(result), " after ", mini_test::formatDuration(elapsed.count()));
        CHECK(result.aborted);
        CHECK(result.stopReason == StopReason::TimeLimit);
        CHECK(result.solutionCount > 0);
        CHECK(elapsed.count() < 2'000);  // checked every 256 nodes, so it overshoots by little
    }
}

TEST("Solvers", "repeated calls on one solver instance give identical results, also after an interrupted run") {
    const Board puzzle = Board::parse(kHardPuzzle);
    for (const sudoku::Solver* solver : allSolvers()) {
        CONTEXT("solver: ", solver->name());
        const SolveResult first = solver->run(puzzle, {.solutionLimit = 2});
        const SolveResult interrupted = solver->run(Board(3), {.solutionLimit = 0, .nodeLimit = 50});
        CHECK(interrupted.aborted);
        const SolveResult again = solver->run(puzzle, {.solutionLimit = 2});
        CHECK_EQ(again.solutionCount, first.solutionCount);
        CHECK_EQ(again.nodes, first.nodes);
        CHECK_EQ(again.backtracks, first.backtracks);
        CHECK_EQ(again.firstSolution, first.firstSolution);
    }
}

TEST("Solvers", "random value order: the same seed gives the same grid, different seeds differ, for every solver") {
    for (const int n : {2, 3}) {
        for (const sudoku::Solver* solver : allSolvers()) {
            CONTEXT("grid: ", gridName(n), ", solver: ", solver->name());
            Rng a(11), b(11), c(12);
            const SolveResult ra = solver->run(Board(n), {.nodeLimit = 5'000'000, .rng = &a});
            const SolveResult rb = solver->run(Board(n), {.nodeLimit = 5'000'000, .rng = &b});
            const SolveResult rc = solver->run(Board(n), {.nodeLimit = 5'000'000, .rng = &c});
            REQUIRE(ra.firstSolution.has_value());
            REQUIRE(rc.firstSolution.has_value());
            checkSolutionIndependently(*ra.firstSolution, Board(n));
            checkSolutionIndependently(*rc.firstSolution, Board(n));
            CHECK_EQ(rb.firstSolution, ra.firstSolution);
            CHECK_EQ(rb.nodes, ra.nodes);
            CHECK(rc.firstSolution != ra.firstSolution);
        }
    }
}

TEST("Solvers", "prefix property: a limit-2 search repeats the limit-1 search's work, then continues") {
    // The benchmark skips a uniqueness run when the first-solution run of the same fixture timed
    // out (pruned_by_prefix). That is only sound if the limit-2 search begins with exactly the
    // limit-1 search: same first solution, at least as many nodes, and under a node limit that
    // stops the limit-1 search before a solution, the same stop at the same node.
    Board multi = Board::parse(kClassicPuzzle);
    multi.setAt(0, 0);
    multi.setAt(1, 0);
    const std::vector<Board> puzzles = {Board::parse(kClassicPuzzle), Board::parse(kHardPuzzle), multi, Board(2),
                                        blankedGrid(3, 60, 4), Generator().generate({.boxSize = 4, .seed = 39}).puzzle};
    for (const Board& puzzle : puzzles) {
        CONTEXT("puzzle: ", mini_test::abbreviate(puzzle.toString()));
        for (const sudoku::Solver* solver : allSolvers()) {
            CONTEXT("solver: ", solver->name());
            const SolveResult first = solver->run(puzzle, {.solutionLimit = 1});
            const SolveResult unique = solver->run(puzzle, {.solutionLimit = 2});
            REQUIRE(first.firstSolution.has_value());
            CHECK_EQ(unique.firstSolution, first.firstSolution);
            CHECK(unique.nodes >= first.nodes);
            CHECK(unique.backtracks >= first.backtracks);
            if (first.nodes > 1) {
                const std::uint64_t cut = first.nodes / 2;  // stops the limit-1 search before its solution
                const SolveResult firstCut = solver->run(puzzle, {.solutionLimit = 1, .nodeLimit = cut});
                const SolveResult uniqueCut = solver->run(puzzle, {.solutionLimit = 2, .nodeLimit = cut});
                CHECK(firstCut.aborted);
                CHECK(uniqueCut.aborted);
                CHECK_EQ(uniqueCut.nodes, firstCut.nodes);
                CHECK_EQ(uniqueCut.solutionCount, firstCut.solutionCount);
            }
        }
    }
}

TEST("Solvers", "DLX memory budget: the estimate is exact at the boundary and a rejection leaves the solver usable") {
    const Board puzzle = Board::parse(kClassicPuzzle);
    const auto estimate = DLXSolver::estimate(puzzle);
    REQUIRE(estimate.has_value());
    TRACE("estimate", estimate->placements, " placements, ", estimate->columns, " columns, ", estimate->nodes,
          " nodes, ", estimate->bytes, " bytes");
    CHECK_EQ(estimate->emptyCells, 51);
    CHECK_EQ(estimate->columns, 4u * 51u);
    CHECK_EQ(estimate->nodes, 1u + estimate->columns + 4u * estimate->placements);

    const DLXSolver exact(static_cast<std::size_t>(estimate->bytes));
    CHECK_EQ(exact.run(puzzle, {}).firstSolution, Board::parse(kClassicSolution));
    const DLXSolver tooSmall(static_cast<std::size_t>(estimate->bytes - 1));
    CHECK_THROWS((void)tooSmall.run(puzzle, {}), std::length_error);
    // The same instance still solves a puzzle that fits its budget.
    const Board small = Generator().generate({.boxSize = 2, .seed = 5}).puzzle;
    REQUIRE(DLXSolver::estimate(small)->bytes < estimate->bytes - 1);
    CHECK_EQ(tooSmall.run(small, {.solutionLimit = 2}).solutionCount, 1u);

    // An empty 4x4 has 16 cells x 4 candidates; a complete grid has nothing to cover.
    const auto empty = DLXSolver::estimate(Board(2));
    CHECK_EQ(empty->placements, 64u);
    CHECK_EQ(empty->columns, 64u);
    CHECK_EQ(empty->nodes, 1u + 64u + 256u);
    Rng rng(1);
    const auto complete = DLXSolver::estimate(sudoku::randomCompletedGrid(3, rng));
    CHECK_EQ(complete->placements, 0u);
    CHECK_EQ(complete->columns, 0u);
    CHECK(!DLXSolver::estimate(boardWith(2, {{0, 0, 1}, {0, 1, 1}})).has_value());
}

TEST("Solvers", "DLX exhaustive searches leave the linked structure exactly as built (validated builds)") {
    // In SUDOKU_DLX_VALIDATE builds the solver checks its links at every search node of small
    // matrices and compares them with their state after construction when a search exhausts;
    // a violation throws std::logic_error and fails this test. In other builds it still checks
    // the counts.
    TRACE("validation", DLXSolver::kValidates ? "enabled (SUDOKU_DLX_VALIDATE)" : "not compiled in");
    const DLXSolver dlx;
    CHECK_EQ(dlx.run(Board(2), {.solutionLimit = 0}).solutionCount, 288u);
    CHECK_EQ(dlx.run(Board::parse(kUnsatisfiable4x4), {.solutionLimit = 0}).solutionCount, 0u);
    CHECK_EQ(dlx.run(Board::parse(kClassicPuzzle), {.solutionLimit = 0}).solutionCount, 1u);
    CHECK_EQ(dlx.run(classicWithWrongCandidate(), {.solutionLimit = 0}).solutionCount, 0u);
    for (std::uint64_t seed = 1; seed <= 20; ++seed) {
        const Board puzzle = blankedGrid(2, 70, seed);
        CONTEXT("puzzle: ", puzzle.toString());
        CHECK_EQ(dlx.run(puzzle, {.solutionLimit = 0}).solutionCount,
                 MRVBacktrackingSolver().run(puzzle, {.solutionLimit = 0}).solutionCount);
    }
}

TEST("Solvers", "every solver reports setup and search time, first-solution timing and maximum depth") {
    const Board puzzle = Board::parse(kHardPuzzle);
    const int empty = puzzle.cellCount() - puzzle.clueCount();
    for (const sudoku::Solver* solver : allSolvers()) {
        CONTEXT("solver: ", solver->name());
        const SolveResult result = solver->run(puzzle, {.solutionLimit = 2});
        TRACE("timing", result.setupTime.count(), " ns setup, ", result.searchTime.count(), " ns search, first at ",
              result.timeToFirstSolution.count(), " ns / node ", result.nodesAtFirstSolution, ", depth ",
              result.maxDepth);
        CHECK(result.setupTime.count() > 0);
        CHECK(result.searchTime.count() > 0);
        CHECK(result.timeToFirstSolution.count() > 0);
        CHECK(result.timeToFirstSolution <= result.setupTime + result.searchTime);
        CHECK(result.nodesAtFirstSolution > 0);
        CHECK(result.nodesAtFirstSolution <= result.nodes);
        // Every placement fills one empty cell, so the deepest level is the number of empty cells
        // when a solution was found.
        CHECK_EQ(result.maxDepth, static_cast<std::uint64_t>(empty));
        // Counters stay zero unless the counters build is used.
        if (!sudoku::kSolverCounters) CHECK_EQ(result.counters.placements, 0u);
        if (sudoku::kSolverCounters) CHECK(result.counters.placements > 0);
        // The prefix property also covers the first-solution figures.
        const SolveResult first = solver->run(puzzle, {});
        CHECK_EQ(first.nodesAtFirstSolution, result.nodesAtFirstSolution);
        CHECK_EQ(first.nodes, first.nodesAtFirstSolution);
    }
}

TEST("CLI", "--solver selects naive, mrv or dlx for solve, count and generate; DLX over its budget exits 3") {
    for (const std::string_view name : {"naive", "mrv", "dlx"}) {
        CONTEXT("--solver ", name);
        const CliResult solved = runCli({"solve", kClassicPuzzle, "--solver", name, "--stats"});
        CHECK_EQ(solved.exitCode, sudoku::cli::kExitOk);
        CHECK_EQ(solved.out, std::string(kClassicSolution) + '\n');
        CHECK(solved.err.find(name == "dlx" ? "solver: dlx" : "solver: backtracking-") != std::string::npos);
        CHECK_EQ(runCli({"count", "................", "--limit", "0", "--solver", name}).out, "288\n");
        const CliResult generated = runCli({"generate", "--size", "2", "--seed", "5", "--solver", name});
        CHECK_EQ(generated.exitCode, sudoku::cli::kExitOk);
        CHECK_EQ(Board::parse(generated.out).clueCount() > 0, true);
    }
    const CliResult naiveTrace = runCli({"solve", "....12..4.3.....", "--solver", "naive", "--trace-solver"});
    CHECK(naiveTrace.err.find("fixed row-major order") != std::string::npos);
    const CliResult dlxTrace = runCli({"solve", "....12..4.3.....", "--solver", "dlx", "--trace-solver"});
    CHECK(dlxTrace.err.find("choose cell r1c1") != std::string::npos);
    CHECK_EQ(runCli({"solve", kClassicPuzzle, "--solver", "bogus"}).exitCode, sudoku::cli::kExitUsage);

    std::string blank144;
    for (int i = 0; i < 144 * 144; ++i) blank144 += ". ";
    const CliResult rejected = runCli({"count", "-", "--solver", "dlx"}, blank144);
    CHECK_EQ(rejected.exitCode, sudoku::cli::kExitLimit);
    CHECK(rejected.err.find("memory budget") != std::string::npos);
}

namespace {

// A completed n x n-box grid with exactly `empty` cells blanked (the first cells in a seeded
// shuffle), so tests can sit on either side of AutoSolver's thresholds.
Board blanked(int boxSize, int empty, std::uint64_t seed) {
    Rng rng(seed);
    Board board = sudoku::randomCompletedGrid(boxSize, rng);
    std::vector<int> cells(static_cast<std::size_t>(board.cellCount()));
    std::iota(cells.begin(), cells.end(), 0);
    sudoku::shuffle(std::span(cells), rng);
    for (int i = 0; i < empty; ++i) board.setAt(cells[static_cast<std::size_t>(i)], 0);
    return board;
}

}  // namespace

TEST("AutoSolver", "thresholds: MRV always up to n = 3, then DLX from 55%, 45%, 35% and 25% empty") {
    using sudoku::AutoSolver;
    for (int n = 1; n <= 3; ++n) CHECK(!AutoSolver::dlxThresholdPercent(n).has_value());
    CHECK_EQ(AutoSolver::dlxThresholdPercent(4).value(), 55);
    CHECK_EQ(AutoSolver::dlxThresholdPercent(5).value(), 45);
    for (int n = 6; n <= 8; ++n) CHECK_EQ(AutoSolver::dlxThresholdPercent(n).value(), 35);
    for (int n : {9, 12, 15, 30}) CHECK_EQ(AutoSolver::dlxThresholdPercent(n).value(), 25);
}

TEST("AutoSolver", "chooses MRV below the threshold and DLX at or above it, by exact empty-cell count") {
    const sudoku::AutoSolver automatic;
    struct Case { int n; int threshold; };
    for (const Case c : {Case{4, 55}, Case{5, 45}, Case{6, 35}, Case{9, 25}}) {
        const int cells = c.n * c.n * c.n * c.n;
        const int atThreshold = (c.threshold * cells + 99) / 100;  // smallest count >= threshold%
        CONTEXT("n = ", c.n, ", empty = ", atThreshold);
        CHECK_EQ(automatic.choose(blanked(c.n, atThreshold - 1, 11)).name(), std::string_view("backtracking-mrv"));
        CHECK_EQ(automatic.choose(blanked(c.n, atThreshold, 11)).name(), std::string_view("dlx"));
    }
    CHECK_EQ(automatic.choose(blanked(3, 64, 5)).name(), std::string_view("backtracking-mrv"));  // 79% at n = 3
}

TEST("AutoSolver", "returns exactly what the chosen solver returns: solution, count, nodes and stop reason") {
    const sudoku::AutoSolver automatic;
    const MRVBacktrackingSolver mrv;
    const sudoku::DLXSolver dlx;
    for (const auto& [n, empty] : std::vector<std::pair<int, int>>{{3, 50}, {4, 60}, {4, 180}, {5, 200}, {5, 330}}) {
        const Board puzzle = blanked(n, empty, 21);
        CONTEXT("n = ", n, ", empty = ", empty);
        const sudoku::Solver& direct = automatic.choose(puzzle).name() == "dlx" ? static_cast<const sudoku::Solver&>(dlx)
                                                                                 : static_cast<const sudoku::Solver&>(mrv);
        const sudoku::SolveOptions options{.solutionLimit = 2};
        const SolveResult a = automatic.run(puzzle, options);
        const SolveResult b = direct.run(puzzle, options);
        CHECK_EQ(a.solutionCount, b.solutionCount);
        CHECK_EQ(a.nodes, b.nodes);
        CHECK_EQ(a.stopReason, b.stopReason);
        CHECK(a.firstSolution.has_value());
        CHECK_EQ(a.firstSolution->toString(), b.firstSolution->toString());
        CHECK_EQ(solutionProblem(*a.firstSolution, puzzle), "");
    }
}

TEST("AutoSolver", "falls back to MRV when DLX's matrix would exceed its budget, and on conflicting givens") {
    const Board puzzle = blanked(5, 400, 3);  // 64% empty: DLX territory
    CHECK_EQ(sudoku::AutoSolver().choose(puzzle).name(), std::string_view("dlx"));
    const auto bytes = sudoku::DLXSolver::estimate(puzzle)->bytes;
    const sudoku::AutoSolver tight(static_cast<std::size_t>(bytes - 1));
    CHECK_EQ(tight.choose(puzzle).name(), std::string_view("backtracking-mrv"));
    // MRV cannot finish this puzzle (the study's MRV timed out at 60% on 25x25), so only check that
    // the run goes to MRV instead of throwing DLX's memory error.
    const SolveResult capped = tight.run(puzzle, {.nodeLimit = 1000});
    CHECK(capped.solved() || capped.stopReason == sudoku::StopReason::NodeLimit);

    Board conflicting = blanked(5, 400, 3);
    int a = -1, b = -1;  // two empty cells in row 0: give them the same value
    for (int c = 0; c < conflicting.size() && b < 0; ++c) {
        if (conflicting.at(c) != 0) continue;
        (a < 0 ? a : b) = c;
    }
    REQUIRE(b >= 0);
    conflicting.set(0, a, 1);
    conflicting.set(0, b, 1);
    CHECK_EQ(sudoku::AutoSolver().choose(conflicting).name(), std::string_view("backtracking-mrv"));
    CHECK_EQ(sudoku::AutoSolver().run(conflicting, {}).stopReason, sudoku::StopReason::InvalidGivens);
}

TEST("CLI", "solve and count default to --solver auto; --stats names the solver auto picked; generate keeps mrv") {
    const CliResult classic = runCli({"solve", kClassicPuzzle, "--stats"});
    CHECK_EQ(classic.exitCode, sudoku::cli::kExitOk);
    CHECK_EQ(classic.out, std::string(kClassicSolution) + '\n');
    CHECK(classic.err.find("solver: auto (backtracking-mrv)") != std::string::npos);

    const Board hard = blanked(4, 180, 21);  // 70% empty 16x16
    const CliResult viaDlx = runCli({"solve", hard.toString(), "--stats"});
    CHECK(viaDlx.err.find("solver: auto (dlx)") != std::string::npos);
    CHECK_EQ(runCli({"count", "................", "--limit", "0"}).out, "288\n");
    CHECK_EQ(runCli({"count", "................", "--limit", "0", "--solver", "auto"}).out, "288\n");

    // generate's default is unchanged, so a seed prints the same puzzle as with --solver mrv.
    CHECK_EQ(runCli({"generate", "--size", "3", "--seed", "42"}).out,
             runCli({"generate", "--size", "3", "--seed", "42", "--solver", "mrv"}).out);
    CHECK_EQ(runCli({"generate", "--size", "2", "--seed", "5", "--solver", "auto"}).exitCode, sudoku::cli::kExitOk);

    std::string blank144;  // 100% empty 144x144: over DLX's budget, so auto must not pick it
    for (int i = 0; i < 144 * 144; ++i) blank144 += ". ";
    const CliResult big = runCli({"solve", "-", "--stats", "--node-limit", "1000"}, blank144);
    CHECK(big.err.find("solver: auto (backtracking-mrv)") != std::string::npos);
    CHECK(big.err.find("memory budget") == std::string::npos);
}

TEST("Board", "out-of-range values stored with setAt are rejected cleanly everywhere") {
    const MRVBacktrackingSolver mrv;
    const sudoku::NaiveBacktrackingSolver naive;
    const sudoku::DLXSolver dlx;
    for (const int boxSize : {2, 3, 9}) {  // one word, one word, two words per bitset
        const int size = boxSize * boxSize;
        for (const int bad : {size + 1, -1, std::numeric_limits<int>::max(), std::numeric_limits<int>::min()}) {
            CONTEXT("box size ", boxSize, ", value ", bad);
            Board board(boxSize);
            board.setAt(board.cellCount() / 2, bad);
            CHECK(!board.isValid());
            CHECK(!sudoku::Constraints::fromBoard(board).has_value());
            CHECK(!sudoku::CandidateGrid::fromBoard(board).has_value());
            const sudoku::Rating rating = sudoku::rate(board);
            CHECK(rating.status == sudoku::RatingStatus::NoSolution);
            CHECK(!rating.difficulty.has_value());
            CHECK(sudoku::LogicalSolver().solve(board).status == sudoku::LogicalStatus::Contradiction);
            for (const sudoku::Solver* solver : {static_cast<const sudoku::Solver*>(&mrv),
                                                 static_cast<const sudoku::Solver*>(&naive),
                                                 static_cast<const sudoku::Solver*>(&dlx)}) {
                CONTEXT("solver: ", solver->name());
                const SolveResult result = solver->run(board, {.solutionLimit = 2});
                CHECK(result.stopReason == sudoku::StopReason::InvalidGivens);
                CHECK_EQ(result.solutionCount, 0u);
            }
        }
    }
}


int main(int argc, char* argv[]) { return mini_test::runAll(argc, argv); }
