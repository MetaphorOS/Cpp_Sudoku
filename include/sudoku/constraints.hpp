#pragma once

#include "sudoku/board.hpp"

#include <bit>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace sudoku {

// Bitsets are stored as 64-bit words; bit (v - 1) of a unit's set means value v is used.
using Word = std::uint64_t;
inline constexpr int kWordBits = 64;

// Tracks which values are already used in each row, column and box, as one bitset per unit
// of words() 64-bit words. The candidates of a cell are full & ~(row | col | box), word by word.
//
// Grids up to 64x64 need a single word. The hot operations take an optional compile-time word
// count (kWords = 1), so the solver's fast path is the same single-mask code as a fixed 64-bit
// design; kWords = 0 means "use words() at run time" and works for any grid size.
class Constraints {
public:
    explicit Constraints(int boxSize)
        : boxSize_(boxSize),
          size_(boxSize * boxSize),
          words_((size_ + kWordBits - 1) / kWordBits),
          lastWord_(size_ % kWordBits == 0 ? ~Word{0} : (Word{1} << (size_ % kWordBits)) - 1),
          rows_(units()),
          cols_(units()),
          boxes_(units()) {}

    // Builds the bitsets from a board's givens; empty if two givens conflict or a cell holds a
    // value outside 0..size (possible through Board::setAt, which does not check values).
    [[nodiscard]] static std::optional<Constraints> fromBoard(const Board& board) {
        Constraints constraints(board.boxSize());
        for (int row = 0; row < board.size(); ++row) {
            for (int col = 0; col < board.size(); ++col) {
                const int value = board.at(row * board.size() + col);
                if (value == 0) continue;
                if (value < 0 || value > board.size()) return std::nullopt;
                if (!constraints.canPlace(row, col, value)) return std::nullopt;
                constraints.place(row, col, value);
            }
        }
        return constraints;
    }

    [[nodiscard]] int size() const noexcept { return size_; }
    [[nodiscard]] int words() const noexcept { return words_; }

    [[nodiscard]] bool canPlace(int row, int col, int value) const noexcept {
        const auto [word, bit] = locate(value);
        return ((rowWords(row)[word] | colWords(col)[word] | boxWords(row, col)[word]) & bit) == 0;
    }

    void place(int row, int col, int value) noexcept {
        const auto [word, bit] = locate(value);
        rowWords(row)[word] |= bit;
        colWords(col)[word] |= bit;
        boxWords(row, col)[word] |= bit;
    }

    void remove(int row, int col, int value) noexcept {
        const auto [word, bit] = locate(value);
        rowWords(row)[word] &= ~bit;
        colWords(col)[word] &= ~bit;
        boxWords(row, col)[word] &= ~bit;
    }

    // Number of values that could go in (row, col).
    template <int kWords = 0>
    [[nodiscard]] int candidateCount(int row, int col) const noexcept {
        const Word* r = rowWords(row);
        const Word* c = colWords(col);
        const Word* b = boxWords(row, col);
        int count = 0;
        for (int w = 0; w < wordCount<kWords>(); ++w) count += std::popcount(free<kWords>(w) & ~(r[w] | c[w] | b[w]));
        return count;
    }

    // Appends the candidate values of (row, col) to `out`, in ascending order.
    template <int kWords = 0>
    void appendCandidates(int row, int col, std::vector<int>& out) const {
        const Word* r = rowWords(row);
        const Word* c = colWords(col);
        const Word* b = boxWords(row, col);
        for (int w = 0; w < wordCount<kWords>(); ++w) {
            for (Word m = free<kWords>(w) & ~(r[w] | c[w] | b[w]); m != 0; m &= m - 1) {
                out.push_back(w * kWordBits + std::countr_zero(m) + 1);
            }
        }
    }

    [[nodiscard]] std::vector<int> candidates(int row, int col) const {
        std::vector<int> values;
        appendCandidates(row, col, values);
        return values;
    }

private:
    struct Location {
        int word;
        Word bit;
    };

    [[nodiscard]] std::size_t units() const {
        return static_cast<std::size_t>(size_) * static_cast<std::size_t>(words_);
    }
    [[nodiscard]] static Location locate(int value) noexcept {
        return {(value - 1) / kWordBits, Word{1} << ((value - 1) % kWordBits)};
    }
    template <int kWords>
    [[nodiscard]] int wordCount() const noexcept {
        return kWords != 0 ? kWords : words_;
    }
    // Bits that are real values in word w (the last word may be partly unused).
    template <int kWords>
    [[nodiscard]] Word free(int w) const noexcept {
        if constexpr (kWords == 1) return lastWord_;
        return w == words_ - 1 ? lastWord_ : ~Word{0};
    }

    [[nodiscard]] std::size_t offset(int unit) const noexcept {
        return static_cast<std::size_t>(unit) * static_cast<std::size_t>(words_);
    }
    [[nodiscard]] int box(int row, int col) const noexcept { return (row / boxSize_) * boxSize_ + col / boxSize_; }
    [[nodiscard]] Word* rowWords(int row) noexcept { return rows_.data() + offset(row); }
    [[nodiscard]] Word* colWords(int col) noexcept { return cols_.data() + offset(col); }
    [[nodiscard]] Word* boxWords(int row, int col) noexcept { return boxes_.data() + offset(box(row, col)); }
    [[nodiscard]] const Word* rowWords(int row) const noexcept { return rows_.data() + offset(row); }
    [[nodiscard]] const Word* colWords(int col) const noexcept { return cols_.data() + offset(col); }
    [[nodiscard]] const Word* boxWords(int row, int col) const noexcept {
        return boxes_.data() + offset(box(row, col));
    }

    int boxSize_;
    int size_;
    int words_;
    Word lastWord_;
    std::vector<Word> rows_;
    std::vector<Word> cols_;
    std::vector<Word> boxes_;
};

}  // namespace sudoku
