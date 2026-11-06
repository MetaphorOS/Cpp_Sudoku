#pragma once

// A human-style logical solver: it keeps an explicit candidate set per cell and applies
// deduction techniques, easiest first, the way a person solves on paper. It never guesses;
// when no technique applies it reports that it stalled. Every step is returned as a
// Deduction that says what pattern was found and what it proved, so a solve can be explained.
//
// Techniques, in the order they are tried:
//   NakedSingle       a cell has one candidate left
//   HiddenSingle      a value has one possible cell left in a row, column or box
//   LockedCandidates  pointing: a value's candidates in a box lie in one row/column, so the
//                     value can be removed from the rest of that line; claiming: a value's
//                     candidates in a row/column lie in one box, so it can be removed from the
//                     rest of that box
//   NakedPair         two cells of a unit have the same two candidates, so those values can be
//                     removed from the other cells of the unit
//   XWing             a value's candidates in two rows lie in the same two columns (or the
//                     transposed form), so it can be removed from the rest of those columns
//
// All techniques work for any box size. Every deduction is sound: it only removes candidates
// that no solution can use, so a puzzle solved by logic alone has exactly one solution.

#include "sudoku/board.hpp"
#include "sudoku/constraints.hpp"
#include "sudoku/deadline.hpp"

#include <array>
#include <cassert>
#include <cstddef>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace sudoku {

enum class Technique { NakedSingle, HiddenSingle, LockedCandidates, NakedPair, XWing };
inline constexpr int kTechniqueCount = 5;
inline constexpr std::array<Technique, kTechniqueCount> kAllTechniques{
    Technique::NakedSingle, Technique::HiddenSingle, Technique::LockedCandidates, Technique::NakedPair,
    Technique::XWing};

// "naked single", "hidden single", "locked candidates", "naked pair", "X-Wing".
[[nodiscard]] std::string_view name(Technique technique) noexcept;

// Units are numbered 0..3*size-1: rows first, then columns, then boxes (row-major).
enum class UnitKind { Row, Column, Box };
[[nodiscard]] inline UnitKind unitKind(int unit, int size) noexcept {
    return unit < size ? UnitKind::Row : unit < 2 * size ? UnitKind::Column : UnitKind::Box;
}
// "row 3", "column 7", "box 5": 1-based, as in Sudoku notation.
[[nodiscard]] std::string unitName(int unit, int size);

struct CellValue {
    int cell = 0;
    int value = 0;
    bool operator==(const CellValue&) const = default;
};

// One step of a logical solve: the pattern that was found and what it proves.
struct Deduction {
    Technique technique = Technique::NakedSingle;
    // The cells that form the pattern: the single's cell, the pair, the box-line intersection
    // cells holding the value, the four X-Wing corners. Ascending.
    std::vector<int> cells{};
    // The value(s) the pattern is about: one value, or the pair's two values. Ascending.
    std::vector<int> values{};
    // The units involved, in a fixed order:
    //   HiddenSingle      {the unit where the value has one place}
    //   LockedCandidates  {source, target}: pointing is {box, line}, claiming is {line, box}
    //   NakedPair         {the shared unit}
    //   XWing             {base 1, base 2, cover 1, cover 2}: the two lines holding the
    //                     pattern, then the two lines the value is removed from
    //   NakedSingle       {}
    std::vector<int> units{};
    std::optional<CellValue> placement{}; // singles
    std::vector<CellValue> eliminations{};// the other techniques; sorted by cell, then value
};

// The board plus an explicit candidate set for every empty cell. Filled cells have no
// candidates. Copying is cheap apart from the candidate words; the unit tables are shared.
class CandidateGrid {
public:
    // Candidates are the values not used in the cell's row, column or box. Empty if two givens
    // conflict.
    [[nodiscard]] static std::optional<CandidateGrid> fromBoard(const Board& board);

    [[nodiscard]] const Board& board() const noexcept { return board_; }
    [[nodiscard]] int boxSize() const noexcept { return board_.boxSize(); }
    [[nodiscard]] int size() const noexcept { return board_.size(); }
    [[nodiscard]] int cellCount() const noexcept { return board_.cellCount(); }
    [[nodiscard]] int value(int cell) const noexcept { return board_.at(cell); }

    // Unchecked, for the technique scans. Preconditions (asserted in debug builds):
    // 0 <= cell < cellCount() and 1 <= value <= size().
    [[nodiscard]] bool has(int cell, int value) const noexcept;
    [[nodiscard]] int count(int cell) const noexcept;
    [[nodiscard]] std::vector<int> candidates(int cell) const;
    // True if the two cells have exactly the same candidate set.
    [[nodiscard]] bool sameCandidates(int a, int b) const noexcept;

    [[nodiscard]] int unitCount() const noexcept { return 3 * size(); }
    // The size() cells of a unit, ascending within rows and columns, row-major within boxes.
    [[nodiscard]] std::span<const int> unitCells(int unit) const noexcept;
    [[nodiscard]] int rowUnit(int cell) const noexcept { return cell / size(); }
    [[nodiscard]] int colUnit(int cell) const noexcept { return size() + cell % size(); }
    [[nodiscard]] int boxUnit(int cell) const noexcept {
        return 2 * size() + board_.boxIndex(cell / size(), cell % size());
    }

    // Fills `cell` with `value` and removes `value` from the candidates of its peers. Checked:
    // throws std::out_of_range for a cell or value out of range and std::logic_error if the
    // cell is already filled. Placing a non-candidate is allowed (it shows up as a
    // contradiction if it breaks a unit).
    void place(int cell, int value);
    // Removes one candidate; returns true if it was present. Unchecked, same preconditions as has().
    bool eliminate(int cell, int value) noexcept;
    // Applies a deduction's placement and eliminations; returns true if anything changed.
    bool apply(const Deduction& deduction);

    [[nodiscard]] bool solved() const noexcept { return empty_ == 0; }
    [[nodiscard]] int emptyCells() const noexcept { return empty_; }
    // True if the grid cannot be completed: an empty cell has no candidates, a unit holds a
    // value twice, or a value missing from a unit has no candidate cell left in it.
    [[nodiscard]] bool contradiction() const;

private:
    struct Geometry;
    [[nodiscard]] static std::shared_ptr<const Geometry> makeGeometry(int boxSize);
    CandidateGrid(Board board, std::shared_ptr<const Geometry> geometry);

    [[nodiscard]] const Word* mask(int cell) const noexcept {
        assert(cell >= 0 && cell < cellCount());
        return candidates_.data() + static_cast<std::size_t>(cell) * static_cast<std::size_t>(words_);
    }
    [[nodiscard]] Word* mask(int cell) noexcept {
        assert(cell >= 0 && cell < cellCount());
        return candidates_.data() + static_cast<std::size_t>(cell) * static_cast<std::size_t>(words_);
    }

    Board board_;
    std::shared_ptr<const Geometry> geometry_;
    int words_;
    int empty_ = 0;
    std::vector<Word> candidates_;  // words_ per cell; bit (v - 1) set means v is a candidate
};

// Technique finders. Each looks for one instance of its pattern that changes the grid (a
// placement, or at least one elimination), without applying it, scanning in a fixed order so
// results are deterministic. They are independent: each can be called on any grid.
[[nodiscard]] std::optional<Deduction> findNakedSingle(const CandidateGrid& grid);
[[nodiscard]] std::optional<Deduction> findHiddenSingle(const CandidateGrid& grid);
[[nodiscard]] std::optional<Deduction> findLockedCandidates(const CandidateGrid& grid);
[[nodiscard]] std::optional<Deduction> findNakedPair(const CandidateGrid& grid);
[[nodiscard]] std::optional<Deduction> findXWing(const CandidateGrid& grid);
[[nodiscard]] std::optional<Deduction> find(Technique technique, const CandidateGrid& grid);

enum class LogicalStatus {
    Solved,         // every cell filled by logic
    Stalled,        // no allowed technique applies; the board holds what logic proved so far
    Contradiction,  // the givens conflict or the puzzle has no solution
    TimeLimit,      // LogicalOptions::deadline expired first; the board holds what was proved so far
};
[[nodiscard]] std::string_view describe(LogicalStatus status) noexcept;

struct LogicalResult {
    LogicalStatus status = LogicalStatus::Stalled;
    Board board;                              // the givens plus every placement made
    std::array<int, kTechniqueCount> uses{};  // steps per technique, indexed by Technique
    int steps = 0;
    std::vector<Deduction> deductions{};      // every step, if LogicalOptions::recordSteps

    // The hardest technique used, if any step was taken.
    [[nodiscard]] std::optional<Technique> hardestUsed() const noexcept;
};

class LogicalObserver {
public:
    virtual ~LogicalObserver() = default;
    // Called after each deduction is applied; `grid` is the state after it.
    virtual void onDeduction(const Deduction& deduction, const CandidateGrid& grid) = 0;
    virtual void onFinished(const LogicalResult& result) = 0;
};

struct LogicalOptions {
    // Techniques harder than this are not used (they come later in Technique's order).
    Technique hardest = Technique::XWing;
    LogicalObserver* observer = nullptr;
    bool recordSteps = false;  // keep every Deduction in LogicalResult::deductions
    // Checked before each step (not inside a technique's scan): once expired, solve() stops
    // with LogicalStatus::TimeLimit. Never expires by default.
    Deadline deadline{};
};

// Repeatedly finds the easiest applicable deduction and applies it, starting again from the
// easiest technique after every step, until the grid is solved, contradictory or stuck.
class LogicalSolver {
public:
    explicit LogicalSolver(LogicalOptions options = {}) : options_(options) {}

    // The easiest deduction available within the allowed techniques, applied to `grid`.
    std::optional<Deduction> step(CandidateGrid& grid) const;

    [[nodiscard]] LogicalResult solve(const Board& board) const;
    // Solves in place, so callers can continue from the resulting candidates.
    LogicalResult solve(CandidateGrid& grid) const;

private:
    LogicalOptions options_;
};

}  // namespace sudoku
