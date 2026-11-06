#pragma once

#include <cassert>
#include <string>
#include <string_view>
#include <vector>

namespace sudoku {

// An n^2 x n^2 Sudoku grid made of n x n boxes, for any box size n >= 1. Cells hold
// 0 (empty) or 1..size(). Board is a plain value type: it stores values and knows the
// geometry, but holds no solver state.
//
// Rows, columns, values and cell indices are ints, so the only size bound is that the
// n^4 cells fit in an int (n <= 215, about 2.1 billion cells, far more than fits in
// memory). Dimensions are computed with overflow checks.
class Board {
public:
    static constexpr int kMinBoxSize = 1;
    // Largest value the one-character format can write: 1-9 then A-Z.
    static constexpr int kMaxCharacterValue = 35;

    // Throws std::invalid_argument for boxSize < 1, std::length_error if the grid's cell
    // count does not fit in an int, and std::bad_alloc if there is not enough memory.
    explicit Board(int boxSize = 3);

    // Parses a grid in either text format; the grid size is inferred from the cell count.
    //  - Number format: n^4 numbers, '.' or 0 for empty, without leading zeros, separated by
    //    whitespace or the box separators '|', '-', '+' (e.g. "10 . 3 ..."). Needed for grids
    //    larger than 25x25.
    //  - One-character format: one character per cell, '.' or '0' for empty, then 1-9 and
    //    A-Z (case-insensitive); whitespace and '|', '-', '+' are ignored.
    // toString() and toPrettyString() output parse back. Text that is valid as numbers is read
    // as numbers; for the one-character inputs of earlier versions both readings agree.
    // Throws std::invalid_argument.
    [[nodiscard]] static Board parse(std::string_view text);

    [[nodiscard]] int boxSize() const noexcept { return boxSize_; }
    [[nodiscard]] int size() const noexcept { return size_; }
    [[nodiscard]] int cellCount() const noexcept { return size_ * size_; }

    // True if toString() writes one character per cell (values up to kMaxCharacterValue).
    [[nodiscard]] bool usesCharacterFormat() const noexcept { return size_ <= kMaxCharacterValue; }

    // Checked access by (row, col). Throws std::out_of_range.
    [[nodiscard]] int get(int row, int col) const;
    void set(int row, int col, int value);

    // Unchecked access by flat index (row * size() + col), for hot loops.
    // Precondition: 0 <= index < cellCount() (asserted in debug builds).
    // setAt does not check the value: a board may end up holding a value outside 0..size().
    // Every consumer rejects such a board cleanly: isValid() is false, the solvers report
    // InvalidGivens, and CandidateGrid::fromBoard / rate() treat it as having no solution.
    [[nodiscard]] int at(int index) const noexcept {
        assert(index >= 0 && index < cellCount());
        return cells_[static_cast<std::size_t>(index)];
    }
    void setAt(int index, int value) noexcept {
        assert(index >= 0 && index < cellCount());
        cells_[static_cast<std::size_t>(index)] = value;
    }

    [[nodiscard]] int boxIndex(int row, int col) const noexcept {
        return (row / boxSize_) * boxSize_ + col / boxSize_;
    }

    // True if no two givens conflict in a row, column or box. Says nothing about solvability.
    [[nodiscard]] bool isValid() const;
    [[nodiscard]] bool isFilled() const noexcept;
    [[nodiscard]] int clueCount() const noexcept;

    // One line: one character per cell up to 25x25 ('.' for empty), otherwise the number
    // format with single spaces.
    [[nodiscard]] std::string toString() const;
    // Multi-line with box separators; numbers are right-aligned in the number format.
    [[nodiscard]] std::string toPrettyString() const;

    bool operator==(const Board&) const = default;

private:
    int boxSize_;
    int size_;
    std::vector<int> cells_;
};

}  // namespace sudoku
