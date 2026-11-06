// Unit tests for the human-style logical solver, difficulty grading and difficulty-targeted
// generation (Steps 11 and 12). Built into the same sudoku_tests executable as test_main.cpp;
// the test registry is shared, so categories and names are as before the split.

#include "test_framework.hpp"

#include "sudoku_test_helpers.hpp"
#include "cli_test_helpers.hpp"

#include "sudoku/backtracking_solver.hpp"
#include "sudoku/board.hpp"
#include "sudoku/deadline.hpp"
#include "sudoku/difficulty.hpp"
#include "sudoku/dlx_solver.hpp"
#include "sudoku/generator.hpp"
#include "sudoku/logical_solver.hpp"
#include "sudoku/text_trace.hpp"

#include <chrono>
#include <cstdint>
#include <limits>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

using sudoku::Board;
using sudoku::Generator;
using sudoku::GeneratorOptions;
using sudoku::MRVBacktrackingSolver;
using sudoku::SolveResult;
using namespace sudoku_test;

// ===========================================================================
// Logical solver (Step 11A): each technique on a hand-built candidate grid
// ===========================================================================

namespace {

using sudoku::CandidateGrid;
using sudoku::CellValue;
using sudoku::Deduction;
using sudoku::LogicalSolver;
using sudoku::LogicalStatus;
using sudoku::Technique;

// A published puzzle (Arto Inkala, 2012) that singles, locked candidates, naked pairs and
// X-Wing cannot finish.
constexpr const char* kLogicResistantPuzzle =
    "800000000003600000070090200050007000000045700000100030001000068008500010090000400";

// An empty 9x9 grid: every cell has all nine candidates. Tests carve patterns into it.
CandidateGrid emptyGrid() { return *CandidateGrid::fromBoard(Board(3)); }

constexpr int cellAt(int row, int col) { return row * 9 + col; }
constexpr int rowUnit(int row) { return row; }
constexpr int colUnit(int col) { return 9 + col; }
constexpr int boxUnit(int box) { return 18 + box; }

// Removes `value` from every cell of `cells`.
void eliminateAll(CandidateGrid& grid, int value, std::initializer_list<int> cells) {
    for (const int cell : cells) grid.eliminate(cell, value);
}

// Removes `value` from all cells of row `row` except the given columns.
void keepInRow(CandidateGrid& grid, int value, int row, std::initializer_list<int> cols) {
    for (int c = 0; c < 9; ++c) {
        if (std::find(cols.begin(), cols.end(), c) == cols.end()) grid.eliminate(cellAt(row, c), value);
    }
}

// Removes `value` from all cells of column `col` except the given rows.
void keepInCol(CandidateGrid& grid, int value, int col, std::initializer_list<int> rows) {
    for (int r = 0; r < 9; ++r) {
        if (std::find(rows.begin(), rows.end(), r) == rows.end()) grid.eliminate(cellAt(r, col), value);
    }
}

std::vector<CellValue> eliminationsOf(int value, std::initializer_list<int> cells) {
    std::vector<CellValue> out;
    for (const int cell : cells) out.push_back({cell, value});
    return out;
}

std::string describeDeduction(const std::optional<Deduction>& d) {
    return d ? sudoku::explain(*d, 9) : std::string("(none)");
}

}  // namespace

TEST("Logic", "candidate grid: givens' values are removed from peers; conflicting givens are rejected") {
    const Board board = Board::parse(kClassicPuzzle);
    const auto grid = CandidateGrid::fromBoard(board);
    REQUIRE(grid.has_value());
    CHECK_EQ(grid->emptyCells(), board.cellCount() - board.clueCount());
    // r1c3 sees 5, 3, 7 in its row, 8 in its column and 6, 9, 8 in its box.
    CHECK_EQ(grid->candidates(cellAt(0, 2)), (std::vector<int>{1, 2, 4}));
    CHECK_EQ(grid->count(cellAt(0, 0)), 0);  // a given has no candidates
    CHECK(!grid->contradiction());
    CHECK(!CandidateGrid::fromBoard(Board::parse("11" + std::string(79, '.'))).has_value());

    CandidateGrid copy = *grid;
    copy.place(cellAt(0, 2), 4);
    CHECK_EQ(copy.value(cellAt(0, 2)), 4);
    CHECK(!copy.has(cellAt(0, 3), 4));       // same row
    CHECK(!copy.has(cellAt(2, 0), 4));       // same box
    CHECK_EQ(grid->value(cellAt(0, 2)), 0);  // the original is untouched
}

TEST("Logic", "naked single: a cell with one candidate is placed; two candidates are not enough") {
    CandidateGrid grid = emptyGrid();
    for (int v = 1; v <= 9; ++v) {
        if (v != 8 && v != 3) grid.eliminate(40, v);
    }
    CHECK_EQ(describeDeduction(sudoku::findNakedSingle(grid)), "(none)");
    grid.eliminate(40, 3);
    const auto d = sudoku::findNakedSingle(grid);
    REQUIRE(d.has_value());
    CHECK(d->technique == Technique::NakedSingle);
    CHECK_EQ(d->placement, (CellValue{40, 8}));
    CHECK_EQ(d->cells, (std::vector<int>{40}));
    CHECK_EQ(d->values, (std::vector<int>{8}));
    CHECK(d->eliminations.empty());
    CHECK_EQ(sudoku::explain(*d, 9), "naked single: r5c5 = 8 (its only candidate)");
}

TEST("Logic", "hidden single: the only place for a value in a row or column, not mistaken for a naked single") {
    CandidateGrid grid = emptyGrid();
    keepInRow(grid, 3, 0, {5});
    CHECK_EQ(describeDeduction(sudoku::findNakedSingle(grid)), "(none)");  // r1c6 still has nine candidates
    auto d = sudoku::findHiddenSingle(grid);
    REQUIRE(d.has_value());
    CHECK(d->technique == Technique::HiddenSingle);
    CHECK_EQ(d->placement, (CellValue{cellAt(0, 5), 3}));
    CHECK_EQ(d->units, (std::vector<int>{rowUnit(0)}));
    CHECK_EQ(sudoku::explain(*d, 9), "hidden single: r1c6 = 3 (the only place for 3 in row 1)");

    CandidateGrid column = emptyGrid();
    keepInCol(column, 6, 2, {7});
    d = sudoku::findHiddenSingle(column);
    REQUIRE(d.has_value());
    CHECK_EQ(d->placement, (CellValue{cellAt(7, 2), 6}));
    CHECK_EQ(d->units, (std::vector<int>{colUnit(2)}));

    CandidateGrid twoPlaces = emptyGrid();
    keepInRow(twoPlaces, 3, 0, {5, 6});
    CHECK_EQ(describeDeduction(sudoku::findHiddenSingle(twoPlaces)), "(none)");
}

TEST("Logic", "locked candidates: pointing along a row and a column, and claiming into a box") {
    {  // pointing: 4 in box 1 only in row 1, so 4 leaves the rest of row 1
        CandidateGrid grid = emptyGrid();
        eliminateAll(grid, 4, {cellAt(1, 0), cellAt(1, 1), cellAt(1, 2), cellAt(2, 0), cellAt(2, 1), cellAt(2, 2)});
        const auto d = sudoku::findLockedCandidates(grid);
        REQUIRE(d.has_value());
        CHECK(d->technique == Technique::LockedCandidates);
        CHECK_EQ(d->cells, (std::vector<int>{0, 1, 2}));
        CHECK_EQ(d->values, (std::vector<int>{4}));
        CHECK_EQ(d->units, (std::vector<int>{boxUnit(0), rowUnit(0)}));
        CHECK_EQ(d->eliminations, eliminationsOf(4, {3, 4, 5, 6, 7, 8}));
        CHECK_EQ(sudoku::explain(*d, 9),
                 "locked candidates (pointing): 4 in box 1 is confined to row 1; remove 4 from r1c4, r1c5, r1c6, "
                 "r1c7, r1c8, r1c9");
        CHECK(grid.apply(*d));
        CHECK(!grid.has(cellAt(0, 6), 4));
        CHECK(grid.has(cellAt(0, 1), 4));
    }
    {  // pointing along a column
        CandidateGrid grid = emptyGrid();
        eliminateAll(grid, 4, {cellAt(0, 1), cellAt(1, 1), cellAt(2, 1), cellAt(0, 2), cellAt(1, 2), cellAt(2, 2)});
        const auto d = sudoku::findLockedCandidates(grid);
        REQUIRE(d.has_value());
        CHECK_EQ(d->units, (std::vector<int>{boxUnit(0), colUnit(0)}));
        CHECK_EQ(d->eliminations,
                 eliminationsOf(4, {cellAt(3, 0), cellAt(4, 0), cellAt(5, 0), cellAt(6, 0), cellAt(7, 0), cellAt(8, 0)}));
    }
    {  // claiming: 4 in row 1 only inside box 1, so 4 leaves the rest of box 1
        CandidateGrid grid = emptyGrid();
        keepInRow(grid, 4, 0, {0, 1, 2});
        const auto d = sudoku::findLockedCandidates(grid);
        REQUIRE(d.has_value());
        CHECK_EQ(d->units, (std::vector<int>{rowUnit(0), boxUnit(0)}));
        CHECK_EQ(d->eliminations,
                 eliminationsOf(4, {cellAt(1, 0), cellAt(1, 1), cellAt(1, 2), cellAt(2, 0), cellAt(2, 1), cellAt(2, 2)}));
        CHECK(sudoku::explain(*d, 9).starts_with("locked candidates (claiming): 4 in row 1 is confined to box 1"));
    }
    {  // near miss: 4 in box 1 spread over two rows and three columns
        CandidateGrid grid = emptyGrid();
        eliminateAll(grid, 4, {cellAt(1, 0), cellAt(1, 1), cellAt(1, 2)});
        CHECK_EQ(describeDeduction(sudoku::findLockedCandidates(grid)), "(none)");
    }
}

TEST("Logic", "naked pair: two cells with the same two candidates clear them from their unit") {
    CandidateGrid grid = emptyGrid();
    for (const int cell : {cellAt(0, 0), cellAt(0, 3)}) {
        for (int v = 1; v <= 9; ++v) {
            if (v != 2 && v != 5) grid.eliminate(cell, v);
        }
    }
    const auto d = sudoku::findNakedPair(grid);
    REQUIRE(d.has_value());
    CHECK(d->technique == Technique::NakedPair);
    CHECK_EQ(d->cells, (std::vector<int>{0, 3}));
    CHECK_EQ(d->values, (std::vector<int>{2, 5}));
    CHECK_EQ(d->units, (std::vector<int>{rowUnit(0)}));
    CHECK_EQ(d->eliminations.size(), 14u);  // 2 and 5 from the seven other cells of row 1
    CHECK(sudoku::explain(*d, 9).starts_with("naked pair {2,5} at r1c1, r1c4 in row 1; remove 2 from r1c2, 5 from r1c2"));

    grid.apply(*d);  // once applied, the pair has nothing left to remove
    CHECK_EQ(describeDeduction(sudoku::findNakedPair(grid)), "(none)");

    CandidateGrid mismatch = emptyGrid();
    for (int v = 1; v <= 9; ++v) {
        if (v != 2 && v != 5) mismatch.eliminate(cellAt(0, 0), v);
        if (v != 2 && v != 6) mismatch.eliminate(cellAt(0, 3), v);
    }
    CHECK_EQ(describeDeduction(sudoku::findNakedPair(mismatch)), "(none)");
}

TEST("Logic", "X-Wing: rows and columns as the base; near misses find nothing") {
    {  // 7 in rows 2 and 6 only in columns 3 and 8
        CandidateGrid grid = emptyGrid();
        keepInRow(grid, 7, 1, {2, 7});
        keepInRow(grid, 7, 5, {2, 7});
        const auto d = sudoku::findXWing(grid);
        REQUIRE(d.has_value());
        CHECK(d->technique == Technique::XWing);
        CHECK_EQ(d->cells, (std::vector<int>{cellAt(1, 2), cellAt(1, 7), cellAt(5, 2), cellAt(5, 7)}));
        CHECK_EQ(d->units, (std::vector<int>{rowUnit(1), rowUnit(5), colUnit(2), colUnit(7)}));
        CHECK_EQ(d->eliminations.size(), 14u);
        for (const auto& [cell, value] : d->eliminations) {
            CHECK_EQ(value, 7);
            CHECK(cell % 9 == 2 || cell % 9 == 7);
            CHECK(cell / 9 != 1 && cell / 9 != 5);
        }
        CHECK(sudoku::explain(*d, 9).starts_with("X-Wing on 7: rows 2,6 / columns 3,8; remove 7 from r1c3, r1c8, r3c3"));
    }
    {  // transposed: 7 in columns 2 and 6 only in rows 3 and 8
        CandidateGrid grid = emptyGrid();
        keepInCol(grid, 7, 1, {2, 7});
        keepInCol(grid, 7, 5, {2, 7});
        const auto d = sudoku::findXWing(grid);
        REQUIRE(d.has_value());
        CHECK_EQ(d->units, (std::vector<int>{colUnit(1), colUnit(5), rowUnit(2), rowUnit(7)}));
        CHECK_EQ(d->eliminations.size(), 14u);
        CHECK(sudoku::explain(*d, 9).starts_with("X-Wing on 7: columns 2,6 / rows 3,8"));
    }
    {  // different column pairs
        CandidateGrid grid = emptyGrid();
        keepInRow(grid, 7, 1, {2, 7});
        keepInRow(grid, 7, 5, {2, 8});
        CHECK_EQ(describeDeduction(sudoku::findXWing(grid)), "(none)");
    }
    {  // a third position in one row
        CandidateGrid grid = emptyGrid();
        keepInRow(grid, 7, 1, {2, 7});
        keepInRow(grid, 7, 5, {2, 4, 7});
        CHECK_EQ(describeDeduction(sudoku::findXWing(grid)), "(none)");
    }
}

TEST("Logic", "the logical solver solves the classic puzzle with singles and counts every step") {
    const Board puzzle = Board::parse(kClassicPuzzle);
    const auto result = LogicalSolver({.recordSteps = true}).solve(puzzle);
    CHECK(result.status == LogicalStatus::Solved);
    CHECK_EQ(result.board.toString(), kClassicSolution);
    CHECK_EQ(result.steps, puzzle.cellCount() - puzzle.clueCount());  // singles only: one placement per step
    CHECK_EQ(static_cast<int>(result.deductions.size()), result.steps);
    CHECK_EQ(std::accumulate(result.uses.begin(), result.uses.end(), 0), result.steps);
    CHECK(result.hardestUsed() <= std::optional(Technique::HiddenSingle));
}

TEST("Logic", "conflicting givens and unsolvable grids end in a contradiction") {
    const auto conflict = LogicalSolver().solve(Board::parse("11" + std::string(79, '.')));
    CHECK(conflict.status == LogicalStatus::Contradiction);
    // No two givens conflict, but r1c9 has no candidate: row 1 holds 1-8 and column 9 holds 9.
    const auto dead = LogicalSolver().solve(Board::parse("12345678." "........9" + std::string(63, '.')));
    CHECK(dead.status == LogicalStatus::Contradiction);
    CHECK_EQ(dead.steps, 0);
}

TEST("Logic", "a logic-resistant puzzle stalls, and every deduction made agrees with its unique solution") {
    const Board puzzle = Board::parse(kLogicResistantPuzzle);
    const SolveResult solution = MRVBacktrackingSolver().run(puzzle, {.solutionLimit = 2});
    REQUIRE_EQ(solution.solutionCount, 1u);
    const Board& expected = *solution.firstSolution;

    const auto result = LogicalSolver({.recordSteps = true}).solve(puzzle);
    CHECK(result.status == LogicalStatus::Stalled);
    CHECK(result.board.clueCount() < result.board.cellCount());
    for (int cell = 0; cell < puzzle.cellCount(); ++cell) {
        if (result.board.at(cell) != 0) CHECK_EQ(result.board.at(cell), expected.at(cell));
    }
    for (const auto& d : result.deductions) {
        if (d.placement) CHECK_EQ(d.placement->value, expected.at(d.placement->cell));
        for (const auto& [cell, value] : d.eliminations) CHECK(value != expected.at(cell));
    }

    // Restricting the techniques never uses a harder one.
    const auto singlesOnly = LogicalSolver({.hardest = Technique::HiddenSingle}).solve(puzzle);
    for (const Technique t : {Technique::LockedCandidates, Technique::NakedPair, Technique::XWing}) {
        CHECK_EQ(singlesOnly.uses[static_cast<std::size_t>(t)], 0);
    }
}

TEST("Logic", "the text tracer explains each deduction and always prints the summary") {
    std::ostringstream out;
    sudoku::TextLogicalTracer tracer(out, 2);
    const auto result = LogicalSolver({.observer = &tracer}).solve(Board::parse(kClassicPuzzle));
    CHECK(result.status == LogicalStatus::Solved);
    const std::string text = out.str();
    TRACE("trace", text);
    CHECK(text.starts_with("[logic] "));
    CHECK(text.find("single: r") != std::string::npos);
    CHECK(text.find("more events not shown") != std::string::npos);
    CHECK(text.find("[logic] finished: solved after " + std::to_string(result.steps) + " steps (") != std::string::npos);
}

// ===========================================================================
// Difficulty grading (Step 11B)
// ===========================================================================

namespace {

using sudoku::Difficulty;
using sudoku::Rating;
using sudoku::RatingStatus;

struct GradedPuzzle {
    const char* puzzle;
    Difficulty difficulty;
    Technique mustUse;  // a technique the grade depends on
    const char* note;
};

// Found by generating and grading 9x9 puzzles; each has a unique solution.
const std::vector<GradedPuzzle>& gradedPuzzles() {
    static const std::vector<GradedPuzzle> puzzles{
        {kClassicPuzzle, Difficulty::Easy, Technique::NakedSingle, "classic, naked singles"},
        {"..6.3...5.2..1...4.....692....2.....5.8.4.......8.1.4...3.....78...6.39..64....8.", Difficulty::Easy,
         Technique::HiddenSingle, "needs hidden singles"},
        {"4....1.6...2.8.......3...1........956..4.......47.3.........6722.65....3.....7.8.", Difficulty::Medium,
         Technique::LockedCandidates, "needs locked candidates"},
        {"8...2.7.5.9...1.4.....3.....4..62............7......9..1...46..4......572..7.5..8", Difficulty::Medium,
         Technique::NakedPair, "needs a naked pair"},
        {".94......7..3.......18...525..68..276...72.13............1..47.47..5..8...6......", Difficulty::Hard,
         Technique::XWing, "needs an X-Wing"},
        {".4.65.1..1....3.5.........95......83..37......6...4..7..2......8...3...29.52..81.", Difficulty::Hard,
         Technique::XWing, "needs an X-Wing"},
        {kLogicResistantPuzzle, Difficulty::Expert, Technique::NakedSingle, "Inkala's puzzle"},
        {"....4..1..8..2..5.2.1..9...128.......4.81...65..9......7.4.1..5....3.9...52..78..", Difficulty::Expert,
         Technique::NakedSingle, "generated; logic stalls"},
    };
    return puzzles;
}

}  // namespace

TEST("Difficulty", "tiers, technique ceilings and names") {
    CHECK(sudoku::tierOf(Technique::NakedSingle) == Difficulty::Easy);
    CHECK(sudoku::tierOf(Technique::HiddenSingle) == Difficulty::Easy);
    CHECK(sudoku::tierOf(Technique::LockedCandidates) == Difficulty::Medium);
    CHECK(sudoku::tierOf(Technique::NakedPair) == Difficulty::Medium);
    CHECK(sudoku::tierOf(Technique::XWing) == Difficulty::Hard);
    for (const Technique t : sudoku::kAllTechniques) {
        CHECK(t <= sudoku::hardestTechnique(sudoku::tierOf(t)));  // a tier allows its own techniques
    }
    CHECK(sudoku::hardestTechnique(Difficulty::Easy) == Technique::HiddenSingle);
    CHECK(sudoku::hardestTechnique(Difficulty::Medium) == Technique::NakedPair);
    for (const Difficulty d : sudoku::kAllDifficulties) CHECK(sudoku::parseDifficulty(sudoku::name(d)) == d);
    CHECK(sudoku::parseDifficulty("HARD") == Difficulty::Hard);
    CHECK(!sudoku::parseDifficulty("insane").has_value());
    CHECK(!sudoku::parseDifficulty("").has_value());
}

TEST("Difficulty", "known puzzles get their expected grade and the correct solution") {
    for (const auto& [text, difficulty, mustUse, note] : gradedPuzzles()) {
        CONTEXT(note, ": ", text);
        const Board puzzle = Board::parse(text);
        const SolveResult unique = MRVBacktrackingSolver().run(puzzle, {.solutionLimit = 2});
        REQUIRE_EQ(unique.solutionCount, 1u);
        const Rating rating = sudoku::rate(puzzle);
        CHECK(rating.status == RatingStatus::Solved);
        REQUIRE(rating.difficulty.has_value());
        CHECK_EQ(sudoku::name(*rating.difficulty), sudoku::name(difficulty));
        CHECK_EQ(rating.solution, unique.firstSolution);
        CHECK(rating.uses[static_cast<std::size_t>(mustUse)] > 0);
        if (difficulty == Difficulty::Expert) {
            CHECK(rating.guesses > 0u);
            CHECK(rating.steps >= rating.logicSteps);
        } else {
            CHECK_EQ(rating.guesses, 0u);
            CHECK_EQ(rating.steps, rating.logicSteps);
        }
    }
}

TEST("Difficulty", "the grade is the lowest sufficient tier: one tier lower stalls, the grade's tier solves") {
    for (const auto& [text, difficulty, mustUse, note] : gradedPuzzles()) {
        CONTEXT(note, ": ", text);
        const Board puzzle = Board::parse(text);
        const auto solveWith = [&](Difficulty tier) {
            return LogicalSolver({.hardest = sudoku::hardestTechnique(tier)}).solve(puzzle).status;
        };
        if (difficulty == Difficulty::Expert) {
            CHECK(solveWith(Difficulty::Hard) == LogicalStatus::Stalled);
            continue;
        }
        CHECK(solveWith(difficulty) == LogicalStatus::Solved);
        if (difficulty != Difficulty::Easy) {
            CHECK(solveWith(static_cast<Difficulty>(static_cast<int>(difficulty) - 1)) == LogicalStatus::Stalled);
        }
    }
}

TEST("Difficulty", "a cap stops at the cap's techniques and never guesses") {
    const auto& puzzles = gradedPuzzles();
    const Board medium = Board::parse(puzzles[2].puzzle);
    const Board hard = Board::parse(puzzles[4].puzzle);
    const Board expert = Board::parse(kLogicResistantPuzzle);

    const Rating within = sudoku::rate(medium, {.cap = Difficulty::Medium});
    CHECK(within.status == RatingStatus::Solved);
    CHECK(within.difficulty == Difficulty::Medium);

    const Rating above = sudoku::rate(hard, {.cap = Difficulty::Medium});
    CHECK(above.status == RatingStatus::AboveCap);
    CHECK(above.difficulty == Difficulty::Hard);
    CHECK(!above.solution.has_value());
    CHECK_EQ(above.uses[static_cast<std::size_t>(Technique::XWing)], 0);

    const Rating expertCapped = sudoku::rate(expert, {.cap = Difficulty::Hard});
    CHECK(expertCapped.status == RatingStatus::AboveCap);
    CHECK(expertCapped.difficulty == Difficulty::Expert);
    CHECK_EQ(expertCapped.guesses, 0u);
}

TEST("Difficulty", "controlled guessing: limits, contradictions, and puzzles with several solutions") {
    const Rating limited = sudoku::rate(Board::parse(kLogicResistantPuzzle), {.guessLimit = 1});
    CHECK(limited.status == RatingStatus::GuessLimit);
    CHECK(limited.difficulty == Difficulty::Expert);
    CHECK_EQ(limited.guesses, 1u);
    CHECK(!limited.solution.has_value());

    CHECK(sudoku::rate(Board::parse("11" + std::string(79, '.'))).status == RatingStatus::NoSolution);
    CHECK(sudoku::rate(Board::parse("12345678." "........9" + std::string(63, '.'))).status == RatingStatus::NoSolution);

    // An empty grid has no deductions at all, so it is graded Expert and solved by guessing;
    // grading stops at the first solution and does not check uniqueness.
    const Board empty(2);
    const Rating open = sudoku::rate(empty);
    CHECK(open.status == RatingStatus::Solved);
    CHECK(open.difficulty == Difficulty::Expert);
    REQUIRE(open.solution.has_value());
    checkSolves(*open.solution, empty);
}

// ===========================================================================
// Difficulty-targeted generation (Step 11C)
// ===========================================================================

namespace {

// Seed 549 is the first seed whose first attempt is graded hard, so tests can ask for hard
// without spending hundreds of attempts.
constexpr std::uint64_t kHardSeed = 549;

// Uniqueness checked independently of the grader: MRV and DLX must each find exactly one
// solution. (Naive backtracking can need millions of nodes even on puzzles easy for people.)
void checkUniqueByMrvAndDlx(const sudoku::Puzzle& p) {
    const MRVBacktrackingSolver mrv;
    const sudoku::DLXSolver dlx;
    for (const sudoku::Solver* solver : {static_cast<const sudoku::Solver*>(&mrv), static_cast<const sudoku::Solver*>(&dlx)}) {
        CONTEXT("solver: ", solver->name());
        const SolveResult result = solver->run(p.puzzle, {.solutionLimit = 2});
        CHECK(!result.aborted);
        CHECK_EQ(result.solutionCount, 1u);
        CHECK_EQ(result.firstSolution, p.solution);
    }
}

}  // namespace

TEST("Generation", "a difficulty target yields a puzzle graded exactly that level and unique by MRV and DLX") {
    for (const Difficulty target : sudoku::kAllDifficulties) {
        const std::uint64_t seed = target == Difficulty::Hard ? kHardSeed : 1;
        CONTEXT("target: ", sudoku::name(target), ", seed: ", seed);
        const sudoku::Puzzle p = Generator().generate({.seed = seed, .difficulty = target});
        TRACE("puzzle", boardText(p.puzzle), "\nattempts: ", p.attempts, ", seed: ", p.seed);
        CHECK(p.targetReached);
        CHECK(p.difficulty == target);
        CHECK(sudoku::rate(p.puzzle).difficulty == target);
        CHECK(p.seed >= seed);
        CHECK_EQ(p.seed - seed + 1, static_cast<std::uint64_t>(p.attempts));
        checkUniqueByMrvAndDlx(p);
        if (target == Difficulty::Expert) {
            CHECK_EQ(p.stats.removedByLogic + p.stats.keptTooHard, 0);  // removals use the search check
        } else {
            CHECK_EQ(p.stats.removedBySearch + p.stats.keptAmbiguous + p.stats.keptUnverified, 0);  // logic only
            CHECK(p.stats.removedByLogic > 0);
            CHECK(p.stats.minimal());
        }

        // Deterministic, and the reported seed reproduces the puzzle in one attempt.
        const sudoku::Puzzle again = Generator().generate({.seed = seed, .difficulty = target});
        CHECK_EQ(again.puzzle, p.puzzle);
        const sudoku::Puzzle direct = Generator().generate({.seed = p.seed, .difficulty = target});
        CHECK_EQ(direct.puzzle, p.puzzle);
        CHECK_EQ(direct.attempts, 1);
    }
}

TEST("Generation", "without a target nothing is graded; a missed target returns the hardest puzzle found") {
    const sudoku::Puzzle plain = Generator().generate({.seed = 1});
    CHECK(!plain.difficulty.has_value());
    CHECK_EQ(plain.attempts, 1);
    CHECK(plain.targetReached);
    CHECK_EQ(plain.stats.removedByLogic + plain.stats.keptTooHard, 0);

    const sudoku::Puzzle missed = Generator().generate({.seed = 1, .difficulty = Difficulty::Hard, .maxAttempts = 3});
    CHECK(!missed.targetReached);
    CHECK_EQ(missed.attempts, 3);
    REQUIRE(missed.difficulty.has_value());
    CHECK(*missed.difficulty < Difficulty::Hard);
    CHECK(sudoku::rate(missed.puzzle).difficulty == *missed.difficulty);
    checkUniqueByMrvAndDlx(missed);

    CHECK_THROWS(Generator().generate({.difficulty = Difficulty::Easy, .maxAttempts = 0}), std::invalid_argument);
}

TEST("Generation", "4x4 and 1x1 grids reject levels above easy at once; larger grids accept every level") {
    CHECK(sudoku::hardestPossible(1) == Difficulty::Easy);
    CHECK(sudoku::hardestPossible(2) == Difficulty::Easy);
    CHECK(sudoku::hardestPossible(3) == Difficulty::Expert);
    for (const int boxSize : {1, 2}) {
        for (const Difficulty level : {Difficulty::Medium, Difficulty::Hard, Difficulty::Expert}) {
            CHECK_THROWS(Generator().generate({.boxSize = boxSize, .difficulty = level}), std::invalid_argument);
        }
        const sudoku::Puzzle easy = Generator().generate({.boxSize = boxSize, .seed = 7, .difficulty = Difficulty::Easy});
        CHECK(easy.targetReached);
        CHECK_EQ(easy.attempts, 1);
    }
}

TEST("Tracing", "the generator trace reports logic-based removals, each attempt's grade and the final level") {
    std::ostringstream out;
    sudoku::TextGeneratorTracer tracer(out, 0);
    const auto p = Generator().generate({.seed = 1, .observer = &tracer, .difficulty = Difficulty::Medium});
    const std::string text = out.str();
    TRACE("trace output", "\n", text);
    CHECK(text.find("medium techniques still solve it (so it is unique)") != std::string::npos);
    CHECK(text.find("[generate] attempt 1/5000 (seed 1): ") != std::string::npos);
    CHECK(text.find("graded medium, target reached") != std::string::npos);
    CHECK(text.find("[generate] difficulty: medium (target medium, " + std::to_string(p.attempts) +
                    " attempts, seed " + std::to_string(p.seed) + ")") != std::string::npos);
    CHECK(text.find(std::to_string(p.stats.removedByLogic) + " solved by logic") != std::string::npos);
}

TEST("CLI", "generate --difficulty and rate: output, warnings and exit codes") {
    const CliResult hard = runCli({"generate", "--seed", std::to_string(kHardSeed), "--difficulty", "hard"});
    CHECK_EQ(hard.exitCode, sudoku::cli::kExitOk);
    CHECK(hard.err.starts_with("seed: " + std::to_string(kHardSeed) + ", clues: "));
    CHECK(hard.err.find(", difficulty: hard, attempts: 1\n") != std::string::npos);

    const CliResult rated = runCli({"rate", "-"}, hard.out);
    CHECK_EQ(rated.exitCode, sudoku::cli::kExitOk);
    CHECK(rated.out.starts_with("difficulty: hard\nnaked single: "));
    CHECK(rated.out.find("\nX-Wing: 1\nguesses: 0\n") != std::string::npos);
    CHECK(rated.err.empty());

    const CliResult missed = runCli({"generate", "--seed", "1", "--difficulty", "HARD", "--attempts", "2"});
    CHECK_EQ(missed.exitCode, sudoku::cli::kExitLimit);
    CHECK(missed.err.find("warning: no puzzle graded hard within 2 attempts") != std::string::npos);
    CHECK_EQ(Board::parse(missed.out).clueCount() > 0, true);  // the hardest puzzle found is still printed

    const CliResult classic = runCli({"rate", kClassicPuzzle, "--trace-logic"});
    CHECK_EQ(classic.exitCode, sudoku::cli::kExitOk);
    CHECK(classic.out.starts_with("difficulty: easy\n"));
    CHECK(classic.err.find("[logic] finished: solved after") != std::string::npos);

    const CliResult expert = runCli({"rate", kLogicResistantPuzzle});
    CHECK_EQ(expert.exitCode, sudoku::cli::kExitOk);
    CHECK(expert.out.starts_with("difficulty: expert\n"));
    CHECK(expert.err.find("logic alone made") != std::string::npos);
    CHECK(expert.err.find("more than one solution") == std::string::npos);

    const CliResult ambiguous = runCli({"rate", "................"});
    CHECK_EQ(ambiguous.exitCode, sudoku::cli::kExitOk);
    CHECK(ambiguous.err.find("warning: puzzle has more than one solution") != std::string::npos);

    CHECK_EQ(runCli({"rate", kLogicResistantPuzzle, "--guess-limit", "1"}).exitCode, sudoku::cli::kExitLimit);
    CHECK_EQ(runCli({"rate", "11" + std::string(79, '.')}).exitCode, sudoku::cli::kExitNoSolution);
    CHECK_EQ(runCli({"generate", "--difficulty", "insane"}).exitCode, sudoku::cli::kExitUsage);
    const CliResult small = runCli({"generate", "--size", "2", "--difficulty", "medium"});
    CHECK_EQ(small.exitCode, sudoku::cli::kExitUsage);
    CHECK(small.out.empty());
    CHECK_EQ(small.err, std::string("error: no 4x4 puzzle can be graded medium: singles solve every one, so the "
                                    "hardest level is easy\n"));
    CHECK_EQ(runCli({"generate", "--attempts", "5"}).exitCode, sudoku::cli::kExitUsage);
    CHECK_EQ(runCli({"generate", "--difficulty", "easy", "--attempts", "0"}).exitCode, sudoku::cli::kExitUsage);
    CHECK_EQ(runCli({"rate", kClassicPuzzle, "--trace-limit", "3"}).exitCode, sudoku::cli::kExitUsage);
}

// ---------------------------------------------------------------------------
// Step 12A: deadlines in the logic solver, the grader and targeted generation. A
// Deadline::afterChecks deadline expires after a fixed number of checks, so these paths are
// deterministic; only one test below looks at elapsed time, loosely.
// ---------------------------------------------------------------------------

TEST("Difficulty", "an expired deadline stops the logic solver and the grader with no grade") {
    using sudoku::Deadline;
    using sudoku::LogicalSolver;
    using sudoku::LogicalStatus;
    const Board classic = Board::parse(kClassicPuzzle);
    for (const Deadline& expired : {Deadline::afterChecks(0), Deadline::at(Deadline::Clock::now() - std::chrono::seconds(1))}) {
        const auto logic = LogicalSolver({.recordSteps = true, .deadline = expired}).solve(classic);
        CHECK(logic.status == LogicalStatus::TimeLimit);
        CHECK_EQ(logic.steps, 0);
        CHECK(logic.deductions.empty());
        CHECK_EQ(logic.board, classic);

        const Rating rating = sudoku::rate(classic, {.deadline = expired});
        CHECK(rating.status == RatingStatus::TimeLimit);
        CHECK(!rating.difficulty.has_value());
        CHECK(!rating.solution.has_value());
    }
    // Five checks allow exactly five steps.
    const auto partial = LogicalSolver({.deadline = Deadline::afterChecks(5)}).solve(classic);
    CHECK(partial.status == LogicalStatus::TimeLimit);
    CHECK_EQ(partial.steps, 5);
    CHECK_EQ(partial.board.clueCount(), classic.clueCount() + 5);
    CHECK_EQ(sudoku::describe(LogicalStatus::TimeLimit), std::string_view("stopped at the time limit"));
}

TEST("Difficulty", "a deadline that expires during guessing reports TimeLimit, not a grade") {
    using sudoku::Deadline;
    const Board inkala = Board::parse(kLogicResistantPuzzle);
    const Rating full = sudoku::rate(inkala);
    REQUIRE(full.status == RatingStatus::Solved);
    REQUIRE(full.guesses > 0u);
    // The first pass checks before each of its steps and once more before stalling; the next
    // check is the one before the first guess.
    const auto checks = static_cast<long long>(full.logicSteps) + 1;
    const Rating stopped = sudoku::rate(inkala, {.deadline = Deadline::afterChecks(checks)});
    CHECK(stopped.status == RatingStatus::TimeLimit);
    CHECK_EQ(stopped.logicSteps, full.logicSteps);
    CHECK_EQ(stopped.guesses, 0u);
    CHECK(!stopped.difficulty.has_value());
    // With more checks it gets into the guesses, and still stops cleanly.
    const Rating later = sudoku::rate(inkala, {.deadline = Deadline::afterChecks(checks + 40)});
    CHECK(later.status == RatingStatus::TimeLimit);
    CHECK(later.guesses > 0u);
    CHECK(!later.difficulty.has_value());
}

TEST("Generation", "time limit before any attempt is graded: the interrupted puzzle, unique, with no grade") {
    using sudoku::Deadline;
    // No removal at all: the first check fails.
    const sudoku::Puzzle none =
        Generator().generate({.seed = 1, .deadline = Deadline::afterChecks(0), .difficulty = Difficulty::Medium});
    CHECK(!none.difficulty.has_value());
    CHECK(!none.targetReached);
    CHECK(none.timeLimitReached);
    CHECK(none.stats.timeLimitReached);
    CHECK_EQ(none.attempts, 1);
    CHECK_EQ(none.puzzle, none.solution);

    // Interrupted part-way through carving (inside a logic rating or between removals).
    for (const long long checks : {200LL, 1500LL}) {
        CONTEXT("checks: ", checks);
        const sudoku::Puzzle partial =
            Generator().generate({.seed = 1, .deadline = Deadline::afterChecks(checks), .difficulty = Difficulty::Hard});
        CHECK(!partial.difficulty.has_value());
        CHECK(!partial.targetReached);
        CHECK(partial.timeLimitReached);
        CHECK(partial.stats.timeLimitReached);
        CHECK(partial.stats.cellsTried < partial.puzzle.cellCount());
        CHECK(partial.puzzle.clueCount() < partial.puzzle.cellCount());
        checkUniqueByMrvAndDlx(partial);
    }
}

TEST("Generation", "time limit after a graded attempt: the best graded puzzle keeps its exact grade") {
    using sudoku::Deadline;
    const GeneratorOptions base{.seed = 1, .difficulty = Difficulty::Hard};
    GeneratorOptions firstOnly = base;
    firstOnly.maxAttempts = 1;
    const sudoku::Puzzle first = Generator().generate(firstOnly);  // attempt 1 alone (seed 1 misses Hard)
    REQUIRE(first.difficulty.has_value());
    REQUIRE(*first.difficulty != Difficulty::Hard);

    // The fewest checks that let a second attempt start; that attempt is then interrupted at once.
    const auto attemptsWith = [&](long long checks) {
        GeneratorOptions options = base;
        options.deadline = Deadline::afterChecks(checks);
        return Generator().generate(options);
    };
    long long lo = 0;
    long long hi = 1;
    while (attemptsWith(hi).attempts < 2) hi *= 2;
    while (lo + 1 < hi) {
        const long long mid = lo + (hi - lo) / 2;
        (attemptsWith(mid).attempts < 2 ? lo : hi) = mid;
    }
    std::ostringstream trace;
    sudoku::TextGeneratorTracer tracer(trace, 0);
    GeneratorOptions options = base;
    options.deadline = Deadline::afterChecks(hi);
    options.observer = &tracer;
    const sudoku::Puzzle p = Generator().generate(options);
    CHECK_EQ(p.attempts, 2);
    REQUIRE(p.difficulty.has_value());
    CHECK(*p.difficulty == *first.difficulty);
    CHECK_EQ(p.puzzle, first.puzzle);
    CHECK_EQ(p.seed, 1u);
    CHECK(!p.targetReached);
    CHECK(p.timeLimitReached);
    CHECK(!p.stats.timeLimitReached);  // the returned attempt itself was carved completely
    const std::string text = trace.str();
    TRACE("trace output", "\n", text);
    CHECK(text.find("[generate] attempt 2/5000 (seed 2): ") != std::string::npos);
    CHECK(text.find("grade unknown (time limit)") != std::string::npos);
    CHECK(text.find("; time limit reached, returning the hardest graded puzzle)") != std::string::npos);
}

TEST("Generation", "targeted 25x25 generation honours a short time limit") {
    const auto start = std::chrono::steady_clock::now();
    const sudoku::Puzzle p = Generator().generate(
        {.boxSize = 5, .seed = 1, .timeLimit = std::chrono::milliseconds(200), .difficulty = Difficulty::Expert});
    const auto elapsed = std::chrono::steady_clock::now() - start;
    CHECK(elapsed < std::chrono::seconds(5));  // generous: only catches an unbounded overrun
    CHECK(p.timeLimitReached);
    CHECK(!p.targetReached);
    checkUniqueByMrvAndDlx(p);
}

TEST("CLI", "--time-limit with --difficulty and with rate: unknown grade, warnings, exit 3") {
    // Building and carving a 36x36 grid takes far longer than 1 ms, so no attempt is graded.
    const CliResult generated = runCli({"generate", "--size", "6", "--seed", "1", "--difficulty", "hard", "--time-limit", "0.001"});
    CHECK_EQ(generated.exitCode, sudoku::cli::kExitLimit);
    CHECK(generated.err.find(", difficulty: unknown, attempts: 1\n") != std::string::npos);
    CHECK(generated.err.find("warning: the time limit ran out before any attempt was fully graded (1 attempt)") !=
          std::string::npos);
    CHECK(Board::parse(generated.out).clueCount() > 0);

    const CliResult rated = runCli({"rate", std::string(625, '.'), "--time-limit", "0.001"});
    CHECK_EQ(rated.exitCode, sudoku::cli::kExitLimit);
    CHECK(rated.out.empty());
    CHECK(rated.err.starts_with("error: stopped at the time limit before the grade was known"));

    CHECK_EQ(runCli({"rate", kClassicPuzzle, "--time-limit", "10"}).exitCode, sudoku::cli::kExitOk);
    CHECK_EQ(runCli({"rate", kClassicPuzzle, "--time-limit", "0"}).exitCode, sudoku::cli::kExitUsage);
}

// ---------------------------------------------------------------------------
// Step 12B: malformed boards and iterative guessing.
// ---------------------------------------------------------------------------

TEST("Logic", "CandidateGrid::place checks its cell and value") {
    auto grid = *sudoku::CandidateGrid::fromBoard(Board(3));
    CHECK_THROWS(grid.place(-1, 1), std::out_of_range);
    CHECK_THROWS(grid.place(81, 1), std::out_of_range);
    CHECK_THROWS(grid.place(0, 0), std::out_of_range);
    CHECK_THROWS(grid.place(0, 10), std::out_of_range);
    grid.place(0, 5);
    CHECK_EQ(grid.value(0), 5);
    CHECK_THROWS(grid.place(0, 6), std::logic_error);  // already filled
    CHECK_EQ(grid.value(0), 5);
    CHECK_EQ(grid.emptyCells(), 80);
}

TEST("Difficulty", "guessing results are pinned: guesses, steps and solution of the Expert fixtures") {
    // Recorded from the recursive implementation before Step 12B made guessing iterative; the
    // search order must not change.
    struct Pinned {
        const char* puzzle;
        std::uint64_t guesses;
        int steps;
        int logicSteps;
    };
    for (const Pinned& pin : {Pinned{kLogicResistantPuzzle, 33, 346, 0},
                              Pinned{"....4..1..8..2..5.2.1..9...128.......4.81...65..9......7.4.1..5....3.9...52..78..",
                                     4, 90, 22}}) {
        CONTEXT("puzzle: ", pin.puzzle);
        const Board board = Board::parse(pin.puzzle);
        const Rating rating = sudoku::rate(board);
        REQUIRE(rating.status == RatingStatus::Solved);
        CHECK_EQ(rating.guesses, pin.guesses);
        CHECK_EQ(rating.steps, pin.steps);
        CHECK_EQ(rating.logicSteps, pin.logicSteps);
        CHECK_EQ(rating.solution, MRVBacktrackingSolver().solve(board));
        CHECK(rating.maxGuessDepth >= 1);
    }
    CHECK_EQ(sudoku::rate(Board::parse(kClassicPuzzle)).maxGuessDepth, 0);
}

TEST("Difficulty", "deep guessing on an empty 25x25 board needs no deep call stack") {
    // An empty board stalls logic at once and guesses almost straight down: hundreds of levels.
    // Before Step 12B each level was a recursive call holding a grid copy.
    const Rating rating = sudoku::rate(Board(5), {.guessLimit = 0});
    REQUIRE(rating.status == RatingStatus::Solved);
    CHECK(rating.difficulty == Difficulty::Expert);
    CHECK(rating.maxGuessDepth > 400);
    CHECK(rating.guesses >= static_cast<std::uint64_t>(rating.maxGuessDepth));
    REQUIRE(rating.solution.has_value());
    CHECK(rating.solution->isFilled());
    CHECK(rating.solution->isValid());
}

