#include "sudoku/board.hpp"

#include "sudoku/constraints.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>
#include <stdexcept>

namespace sudoku {

namespace {

constexpr char kEmptyChar = '.';

char valueToChar(int value) {
    if (value == 0) return kEmptyChar;
    if (value <= 9) return static_cast<char>('0' + value);
    return static_cast<char>('A' + (value - 10));
}

// Returns 0 for empty, the value for a digit/letter, or -1 for an invalid character.
int charToValue(char ch) {
    if (ch == '.' || ch == '0') return 0;
    if (ch >= '1' && ch <= '9') return ch - '0';
    const char upper = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
    if (upper >= 'A' && upper <= 'Z') return 10 + (upper - 'A');
    return -1;
}

bool isSpace(char ch) { return std::isspace(static_cast<unsigned char>(ch)) != 0; }
bool isSeparator(char ch) { return ch == '|' || ch == '-' || ch == '+'; }

std::string gridName(std::int64_t size) { return std::to_string(size) + "x" + std::to_string(size); }

// Checks that an n^2 x n^2 grid can be represented: n >= 1 and n^4 cells fit in an int.
void checkBoxSize(int boxSize) {
    if (boxSize < Board::kMinBoxSize) {
        throw std::invalid_argument("box size must be a positive integer, got " + std::to_string(boxSize));
    }
    const std::int64_t size = std::int64_t{boxSize} * boxSize;  // fits: boxSize <= INT_MAX
    if (size > std::numeric_limits<int>::max() / size) {
        throw std::length_error("box size " + std::to_string(boxSize) + " is too large: a " + gridName(size) +
                                " grid has more cells than fit in an int");
    }
}

// The box size n with n^4 == count, if there is one. The floating-point root is only a
// guess; the candidates around it are checked exactly, with overflow-checked products.
std::optional<int> boxSizeForCellCount(std::size_t count) {
    if (count == 0) return std::nullopt;
    const auto target = static_cast<std::uint64_t>(count);
    const auto guess = static_cast<std::uint64_t>(std::llround(std::sqrt(std::sqrt(static_cast<double>(count)))));
    constexpr std::uint64_t kMax = std::numeric_limits<std::uint64_t>::max();
    for (std::uint64_t n = guess > 1 ? guess - 1 : 1; n <= guess + 1; ++n) {
        if (n > static_cast<std::uint64_t>(std::numeric_limits<int>::max())) break;
        const std::uint64_t size = n * n;  // n < 2^31, so no overflow
        if (size > kMax / size) break;     // size^2 would overflow; larger n only gets bigger
        if (size * size == target) return static_cast<int>(n);
    }
    return std::nullopt;
}

// ---- Number format --------------------------------------------------------

struct NumberScan {
    std::vector<int> values;  // -1 marks a token that is not a valid number for this grid
    bool numeric = true;      // every token is '.' or digits
    bool wellFormed = true;   // ... and none has a leading zero
    std::string firstProblem;
};

// Cells of the number format: text between whitespace and the box separators '|', '-', '+'.
std::vector<std::string_view> tokens(std::string_view text) {
    const auto isDelimiter = [](char ch) { return isSpace(ch) || isSeparator(ch); };
    std::vector<std::string_view> result;
    std::size_t i = 0;
    while (i < text.size()) {
        while (i < text.size() && isDelimiter(text[i])) ++i;
        const std::size_t start = i;
        while (i < text.size() && !isDelimiter(text[i])) ++i;
        if (i > start) result.push_back(text.substr(start, i - start));
    }
    return result;
}

// Parses each token as a cell; values above `maxValue` are reported but kept as -1.
NumberScan scanNumbers(const std::vector<std::string_view>& cells, int maxValue) {
    NumberScan scan;
    scan.values.reserve(cells.size());
    for (const std::string_view token : cells) {
        if (token == "." || token == "0") {
            scan.values.push_back(0);
            continue;
        }
        if (!std::ranges::all_of(token, [](char ch) { return ch >= '0' && ch <= '9'; })) {
            scan.numeric = scan.wellFormed = false;
            return scan;
        }
        if (token.front() == '0') {
            scan.wellFormed = false;
            if (scan.firstProblem.empty()) scan.firstProblem = "number \"" + std::string(token) + "\" has a leading zero";
            scan.values.push_back(-1);
            continue;
        }
        std::int64_t value = 0;
        for (const char ch : token) {
            value = value * 10 + (ch - '0');
            if (value > maxValue) break;  // also stops long tokens before they overflow
        }
        if (value > maxValue) {
            if (scan.firstProblem.empty()) {
                scan.firstProblem = "value " + std::string(token) + " is too large for a " + gridName(maxValue) + " grid";
            }
            scan.values.push_back(-1);
        } else {
            scan.values.push_back(static_cast<int>(value));
        }
    }
    return scan;
}

// ---- One-character format -------------------------------------------------

struct CharacterScan {
    std::vector<int> values;
    std::optional<char> invalid;  // first character that is not a cell, space or separator
};

CharacterScan scanCharacters(std::string_view text) {
    CharacterScan scan;
    scan.values.reserve(text.size());
    for (const char ch : text) {
        if (isSpace(ch) || isSeparator(ch)) continue;
        const int value = charToValue(ch);
        if (value < 0) {
            scan.invalid = ch;
            return scan;
        }
        scan.values.push_back(value);
    }
    return scan;
}

Board boardFrom(int boxSize, const std::vector<int>& values) {
    Board board(boxSize);
    for (int i = 0; i < board.cellCount(); ++i) board.setAt(i, values[static_cast<std::size_t>(i)]);
    return board;
}

std::string tooLarge(int value, int size) {
    return "value " + std::to_string(value) + " is too large for a " + gridName(size) + " grid";
}

}  // namespace

Board::Board(int boxSize) : boxSize_(boxSize), size_(0) {
    checkBoxSize(boxSize);
    size_ = boxSize * boxSize;
    cells_.assign(static_cast<std::size_t>(size_) * static_cast<std::size_t>(size_), 0);
}

Board Board::parse(std::string_view text) {
    // Number format first: it is the only one that can express values above 35.
    const std::vector<std::string_view> cells = tokens(text);
    const std::optional<int> numberBoxSize = boxSizeForCellCount(cells.size());
    NumberScan numbers;
    if (numberBoxSize) {
        numbers = scanNumbers(cells, *numberBoxSize * *numberBoxSize);
        if (numbers.numeric && numbers.wellFormed && numbers.firstProblem.empty()) {
            return boardFrom(*numberBoxSize, numbers.values);
        }
    }

    const CharacterScan characters = scanCharacters(text);
    const std::optional<int> characterBoxSize = boxSizeForCellCount(characters.values.size());
    if (!characters.invalid && characterBoxSize) {
        const int size = *characterBoxSize * *characterBoxSize;
        const auto largest = std::ranges::max(characters.values);
        if (largest <= size) return boardFrom(*characterBoxSize, characters.values);
        // Fall through: report the more likely reading below.
    }

    // Neither reading works. Report the problem of the reading the text most resembles.
    if (numberBoxSize && numbers.numeric && cells.size() > 1) throw std::invalid_argument(numbers.firstProblem);
    if (characters.invalid) throw std::invalid_argument(std::string("invalid character '") + *characters.invalid + "'");
    if (characterBoxSize) {
        const int size = *characterBoxSize * *characterBoxSize;
        throw std::invalid_argument(tooLarge(std::ranges::max(characters.values), size));
    }
    throw std::invalid_argument("found " + std::to_string(characters.values.size()) + " cells (" +
                                std::to_string(cells.size()) +
                                " if read as numbers); a grid needs n^4 cells for a box size n, e.g. 16, 81, 256, "
                                "625 or 1296");
}

int Board::get(int row, int col) const {
    if (row < 0 || row >= size_ || col < 0 || col >= size_) throw std::out_of_range("cell out of range");
    return at(row * size_ + col);
}

void Board::set(int row, int col, int value) {
    if (row < 0 || row >= size_ || col < 0 || col >= size_) throw std::out_of_range("cell out of range");
    if (value < 0 || value > size_) throw std::out_of_range("value out of range");
    setAt(row * size_ + col, value);
}

bool Board::isValid() const { return Constraints::fromBoard(*this).has_value(); }

bool Board::isFilled() const noexcept {
    return std::ranges::none_of(cells_, [](int v) { return v == 0; });
}

int Board::clueCount() const noexcept {
    return static_cast<int>(std::ranges::count_if(cells_, [](int v) { return v != 0; }));
}

std::string Board::toString() const {
    std::string out;
    if (usesCharacterFormat()) {
        out.reserve(cells_.size());
        for (const int value : cells_) out += valueToChar(value);
        return out;
    }
    for (std::size_t i = 0; i < cells_.size(); ++i) {
        if (i > 0) out += ' ';
        out += cells_[i] == 0 ? std::string(1, kEmptyChar) : std::to_string(cells_[i]);
    }
    return out;
}

std::string Board::toPrettyString() const {
    // In the number format every cell is right-aligned to the width of the largest value.
    const std::size_t width = usesCharacterFormat() ? 1 : std::to_string(size_).size();
    const auto cellText = [&](int value) {
        std::string text = usesCharacterFormat() ? std::string(1, valueToChar(value))
                                                 : (value == 0 ? std::string(1, kEmptyChar) : std::to_string(value));
        return std::string(width - text.size(), ' ') + text;
    };

    std::string out;
    std::string separator;
    for (int row = 0; row < size_; ++row) {
        std::string line;
        for (int col = 0; col < size_; ++col) {
            if (col > 0 && col % boxSize_ == 0) line += "| ";
            line += cellText(at(row * size_ + col));
            line += ' ';
        }
        line.pop_back();

        if (separator.empty()) {
            // Same width as a row: '|' becomes '+', everything else '-'.
            separator.reserve(line.size());
            for (const char ch : line) separator += (ch == '|') ? '+' : '-';
        }
        if (row > 0 && row % boxSize_ == 0) out += separator + '\n';
        out += line + '\n';
    }
    return out;
}

}  // namespace sudoku
