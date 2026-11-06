#include "sudoku/logical_solver.hpp"

#include <cassert>
#include <stdexcept>
#include <algorithm>
#include <bit>
#include <utility>

namespace sudoku {

// The cells of every unit, built once per grid size and shared by copies of a grid.
struct CandidateGrid::Geometry {
    int size = 0;
    std::vector<int> unitCells;  // 3 * size units of size cells each
};

std::string_view name(Technique technique) noexcept {
    switch (technique) {
        case Technique::NakedSingle: return "naked single";
        case Technique::HiddenSingle: return "hidden single";
        case Technique::LockedCandidates: return "locked candidates";
        case Technique::NakedPair: return "naked pair";
        case Technique::XWing: return "X-Wing";
    }
    return "unknown";
}

std::string unitName(int unit, int size) {
    switch (unitKind(unit, size)) {
        case UnitKind::Row: return "row " + std::to_string(unit + 1);
        case UnitKind::Column: return "column " + std::to_string(unit - size + 1);
        case UnitKind::Box: return "box " + std::to_string(unit - 2 * size + 1);
    }
    return "unit " + std::to_string(unit);
}

std::string_view describe(LogicalStatus status) noexcept {
    switch (status) {
        case LogicalStatus::Solved: return "solved";
        case LogicalStatus::Stalled: return "stalled";
        case LogicalStatus::Contradiction: return "contradiction";
        case LogicalStatus::TimeLimit: return "stopped at the time limit";
    }
    return "unknown";
}

std::optional<Technique> LogicalResult::hardestUsed() const noexcept {
    for (int t = kTechniqueCount - 1; t >= 0; --t) {
        if (uses[static_cast<std::size_t>(t)] > 0) return static_cast<Technique>(t);
    }
    return std::nullopt;
}

// ---------------------------------------------------------------------------
// CandidateGrid
// ---------------------------------------------------------------------------


std::shared_ptr<const CandidateGrid::Geometry> CandidateGrid::makeGeometry(int boxSize) {
    auto geometry = std::make_shared<Geometry>();
    const int n = boxSize;
    const int size = n * n;
    geometry->size = size;
    geometry->unitCells.reserve(static_cast<std::size_t>(3) * static_cast<std::size_t>(size) *
                                static_cast<std::size_t>(size));
    for (int r = 0; r < size; ++r) {
        for (int j = 0; j < size; ++j) geometry->unitCells.push_back(r * size + j);
    }
    for (int c = 0; c < size; ++c) {
        for (int j = 0; j < size; ++j) geometry->unitCells.push_back(j * size + c);
    }
    for (int b = 0; b < size; ++b) {
        for (int j = 0; j < size; ++j) {
            const int row = (b / n) * n + j / n;
            const int col = (b % n) * n + j % n;
            geometry->unitCells.push_back(row * size + col);
        }
    }
    return geometry;
}

CandidateGrid::CandidateGrid(Board board, std::shared_ptr<const Geometry> geometry)
    : board_(std::move(board)),
      geometry_(std::move(geometry)),
      words_((board_.size() + kWordBits - 1) / kWordBits),
      candidates_(static_cast<std::size_t>(board_.cellCount()) * static_cast<std::size_t>(words_), 0) {}

std::optional<CandidateGrid> CandidateGrid::fromBoard(const Board& board) {
    const auto constraints = Constraints::fromBoard(board);
    if (!constraints) return std::nullopt;
    CandidateGrid grid(board, makeGeometry(board.boxSize()));
    std::vector<int> values;
    for (int cell = 0; cell < board.cellCount(); ++cell) {
        if (board.at(cell) != 0) continue;
        ++grid.empty_;
        values.clear();
        constraints->appendCandidates(cell / board.size(), cell % board.size(), values);
        Word* m = grid.mask(cell);
        for (const int v : values) m[(v - 1) / kWordBits] |= Word{1} << ((v - 1) % kWordBits);
    }
    return grid;
}

bool CandidateGrid::has(int cell, int value) const noexcept {
    assert(value >= 1 && value <= size());
    return (mask(cell)[(value - 1) / kWordBits] >> ((value - 1) % kWordBits) & 1U) != 0;
}

int CandidateGrid::count(int cell) const noexcept {
    const Word* m = mask(cell);
    int total = 0;
    for (int w = 0; w < words_; ++w) total += std::popcount(m[w]);
    return total;
}

std::vector<int> CandidateGrid::candidates(int cell) const {
    std::vector<int> values;
    const Word* m = mask(cell);
    for (int w = 0; w < words_; ++w) {
        for (Word bits = m[w]; bits != 0; bits &= bits - 1) values.push_back(w * kWordBits + std::countr_zero(bits) + 1);
    }
    return values;
}

bool CandidateGrid::sameCandidates(int a, int b) const noexcept {
    return std::equal(mask(a), mask(a) + words_, mask(b));
}

std::span<const int> CandidateGrid::unitCells(int unit) const noexcept {
    const auto offset = static_cast<std::size_t>(unit) * static_cast<std::size_t>(size());
    return {geometry_->unitCells.data() + offset, static_cast<std::size_t>(size())};
}

void CandidateGrid::place(int cell, int value) {
    if (cell < 0 || cell >= cellCount()) throw std::out_of_range("cell out of range");
    if (value < 1 || value > size()) throw std::out_of_range("value out of range");
    if (board_.at(cell) != 0) throw std::logic_error("cell is already filled");
    board_.setAt(cell, value);
    std::fill(mask(cell), mask(cell) + words_, Word{0});
    --empty_;
    for (const int unit : {rowUnit(cell), colUnit(cell), boxUnit(cell)}) {
        for (const int peer : unitCells(unit)) eliminate(peer, value);
    }
}

bool CandidateGrid::eliminate(int cell, int value) noexcept {
    assert(value >= 1 && value <= size());
    Word& word = mask(cell)[(value - 1) / kWordBits];
    const Word bit = Word{1} << ((value - 1) % kWordBits);
    const bool present = (word & bit) != 0;
    word &= ~bit;
    return present;
}

bool CandidateGrid::apply(const Deduction& deduction) {
    bool changed = false;
    if (deduction.placement && board_.at(deduction.placement->cell) == 0) {
        place(deduction.placement->cell, deduction.placement->value);
        changed = true;
    }
    for (const auto& [cell, value] : deduction.eliminations) changed = eliminate(cell, value) || changed;
    return changed;
}

bool CandidateGrid::contradiction() const {
    for (int cell = 0; cell < cellCount(); ++cell) {
        if (board_.at(cell) == 0 && count(cell) == 0) return true;
    }
    // Per unit: every value must be placed once, or still be a candidate somewhere.
    std::vector<Word> placed(static_cast<std::size_t>(words_));
    std::vector<Word> possible(static_cast<std::size_t>(words_));
    const Word lastWord = size() % kWordBits == 0 ? ~Word{0} : (Word{1} << (size() % kWordBits)) - 1;
    for (int unit = 0; unit < unitCount(); ++unit) {
        std::fill(placed.begin(), placed.end(), Word{0});
        std::fill(possible.begin(), possible.end(), Word{0});
        for (const int cell : unitCells(unit)) {
            const int v = board_.at(cell);
            if (v != 0) {
                Word& word = placed[static_cast<std::size_t>((v - 1) / kWordBits)];
                const Word bit = Word{1} << ((v - 1) % kWordBits);
                if ((word & bit) != 0) return true;  // the same value twice
                word |= bit;
            } else {
                const Word* m = mask(cell);
                for (int w = 0; w < words_; ++w) possible[static_cast<std::size_t>(w)] |= m[w];
            }
        }
        for (int w = 0; w < words_; ++w) {
            const Word full = w == words_ - 1 ? lastWord : ~Word{0};
            const auto i = static_cast<std::size_t>(w);
            if ((placed[i] | possible[i]) != full) return true;
        }
    }
    return false;
}

// ---------------------------------------------------------------------------
// Technique finders
// ---------------------------------------------------------------------------

namespace {

// positions[v] = the cells of `unit` that have v as a candidate, in unit order.
void positionsByValue(const CandidateGrid& grid, int unit, std::vector<std::vector<int>>& positions) {
    positions.resize(static_cast<std::size_t>(grid.size()) + 1);
    for (auto& cells : positions) cells.clear();
    for (const int cell : grid.unitCells(unit)) {
        if (grid.value(cell) != 0) continue;
        for (const int v : grid.candidates(cell)) positions[static_cast<std::size_t>(v)].push_back(cell);
    }
}

bool contains(const std::vector<int>& cells, int cell) { return std::find(cells.begin(), cells.end(), cell) != cells.end(); }

Deduction finish(Deduction d) {
    std::sort(d.cells.begin(), d.cells.end());
    std::sort(d.eliminations.begin(), d.eliminations.end(),
              [](const CellValue& a, const CellValue& b) { return std::pair(a.cell, a.value) < std::pair(b.cell, b.value); });
    return d;
}

// Locked candidates from `source` into `target`: the value's candidates in `source` all lie in
// `target` too, so it is removed from the rest of `target`.
std::optional<Deduction> lockedInto(const CandidateGrid& grid, int source, int target, int value,
                                    const std::vector<int>& cells) {
    Deduction d{.technique = Technique::LockedCandidates, .cells = cells, .values = {value}, .units = {source, target}};
    for (const int cell : grid.unitCells(target)) {
        if (!contains(cells, cell) && grid.has(cell, value)) d.eliminations.push_back({cell, value});
    }
    if (d.eliminations.empty()) return std::nullopt;
    return finish(std::move(d));
}

}  // namespace

std::optional<Deduction> findNakedSingle(const CandidateGrid& grid) {
    for (int cell = 0; cell < grid.cellCount(); ++cell) {
        if (grid.value(cell) != 0 || grid.count(cell) != 1) continue;
        const int value = grid.candidates(cell).front();
        return Deduction{.technique = Technique::NakedSingle,
                         .cells = {cell},
                         .values = {value},
                         .placement = CellValue{cell, value}};
    }
    return std::nullopt;
}

std::optional<Deduction> findHiddenSingle(const CandidateGrid& grid) {
    std::vector<std::vector<int>> positions;
    for (int unit = 0; unit < grid.unitCount(); ++unit) {
        positionsByValue(grid, unit, positions);
        for (int v = 1; v <= grid.size(); ++v) {
            const auto& cells = positions[static_cast<std::size_t>(v)];
            if (cells.size() != 1) continue;
            const int cell = cells.front();
            return Deduction{.technique = Technique::HiddenSingle,
                             .cells = {cell},
                             .values = {v},
                             .units = {unit},
                             .placement = CellValue{cell, v}};
        }
    }
    return std::nullopt;
}

std::optional<Deduction> findLockedCandidates(const CandidateGrid& grid) {
    const int size = grid.size();
    std::vector<std::vector<int>> positions;
    // Pointing: box -> row or column.
    for (int box = 2 * size; box < 3 * size; ++box) {
        positionsByValue(grid, box, positions);
        for (int v = 1; v <= size; ++v) {
            const auto& cells = positions[static_cast<std::size_t>(v)];
            if (cells.size() < 2) continue;
            const int row = grid.rowUnit(cells.front());
            const int col = grid.colUnit(cells.front());
            const bool oneRow = std::all_of(cells.begin(), cells.end(), [&](int c) { return grid.rowUnit(c) == row; });
            const bool oneCol = std::all_of(cells.begin(), cells.end(), [&](int c) { return grid.colUnit(c) == col; });
            if (oneRow) {
                if (auto d = lockedInto(grid, box, row, v, cells)) return d;
            } else if (oneCol) {
                if (auto d = lockedInto(grid, box, col, v, cells)) return d;
            }
        }
    }
    // Claiming: row or column -> box.
    for (int line = 0; line < 2 * size; ++line) {
        positionsByValue(grid, line, positions);
        for (int v = 1; v <= size; ++v) {
            const auto& cells = positions[static_cast<std::size_t>(v)];
            if (cells.size() < 2) continue;
            const int box = grid.boxUnit(cells.front());
            if (std::all_of(cells.begin(), cells.end(), [&](int c) { return grid.boxUnit(c) == box; })) {
                if (auto d = lockedInto(grid, line, box, v, cells)) return d;
            }
        }
    }
    return std::nullopt;
}

std::optional<Deduction> findNakedPair(const CandidateGrid& grid) {
    std::vector<int> pairs;
    for (int unit = 0; unit < grid.unitCount(); ++unit) {
        pairs.clear();
        for (const int cell : grid.unitCells(unit)) {
            if (grid.value(cell) == 0 && grid.count(cell) == 2) pairs.push_back(cell);
        }
        for (std::size_t i = 0; i < pairs.size(); ++i) {
            for (std::size_t j = i + 1; j < pairs.size(); ++j) {
                const int a = pairs[i];
                const int b = pairs[j];
                if (!grid.sameCandidates(a, b)) continue;
                Deduction d{.technique = Technique::NakedPair, .cells = {a, b}, .values = grid.candidates(a), .units = {unit}};
                for (const int cell : grid.unitCells(unit)) {
                    if (cell == a || cell == b) continue;
                    for (const int v : d.values) {
                        if (grid.has(cell, v)) d.eliminations.push_back({cell, v});
                    }
                }
                if (!d.eliminations.empty()) return finish(std::move(d));
            }
        }
    }
    return std::nullopt;
}

std::optional<Deduction> findXWing(const CandidateGrid& grid) {
    const int size = grid.size();
    struct Line {
        int unit;
        int cover1;  // the two cover units (columns for row bases, rows for column bases), ascending
        int cover2;
        int cell1;
        int cell2;
    };
    std::vector<Line> lines;
    for (int v = 1; v <= size; ++v) {
        for (const bool rowBase : {true, false}) {
            lines.clear();
            const int first = rowBase ? 0 : size;
            for (int unit = first; unit < first + size; ++unit) {
                int found = 0;
                int cells[2] = {-1, -1};
                for (const int cell : grid.unitCells(unit)) {
                    if (!grid.has(cell, v)) continue;
                    if (found < 2) cells[found] = cell;
                    if (++found > 2) break;
                }
                if (found != 2) continue;
                const auto cover = [&](int cell) { return rowBase ? grid.colUnit(cell) : grid.rowUnit(cell); };
                lines.push_back({unit, cover(cells[0]), cover(cells[1]), cells[0], cells[1]});
            }
            for (std::size_t i = 0; i < lines.size(); ++i) {
                for (std::size_t j = i + 1; j < lines.size(); ++j) {
                    const Line& a = lines[i];
                    const Line& b = lines[j];
                    if (a.cover1 != b.cover1 || a.cover2 != b.cover2) continue;
                    Deduction d{.technique = Technique::XWing,
                                .cells = {a.cell1, a.cell2, b.cell1, b.cell2},
                                .values = {v},
                                .units = {a.unit, b.unit, a.cover1, a.cover2}};
                    for (const int coverUnit : {a.cover1, a.cover2}) {
                        for (const int cell : grid.unitCells(coverUnit)) {
                            const int base = rowBase ? grid.rowUnit(cell) : grid.colUnit(cell);
                            if (base != a.unit && base != b.unit && grid.has(cell, v)) d.eliminations.push_back({cell, v});
                        }
                    }
                    if (!d.eliminations.empty()) return finish(std::move(d));
                }
            }
        }
    }
    return std::nullopt;
}

std::optional<Deduction> find(Technique technique, const CandidateGrid& grid) {
    switch (technique) {
        case Technique::NakedSingle: return findNakedSingle(grid);
        case Technique::HiddenSingle: return findHiddenSingle(grid);
        case Technique::LockedCandidates: return findLockedCandidates(grid);
        case Technique::NakedPair: return findNakedPair(grid);
        case Technique::XWing: return findXWing(grid);
    }
    return std::nullopt;
}

// ---------------------------------------------------------------------------
// LogicalSolver
// ---------------------------------------------------------------------------

std::optional<Deduction> LogicalSolver::step(CandidateGrid& grid) const {
    for (const Technique technique : kAllTechniques) {
        if (technique > options_.hardest) break;
        if (auto deduction = find(technique, grid)) {
            grid.apply(*deduction);
            return deduction;
        }
    }
    return std::nullopt;
}

LogicalResult LogicalSolver::solve(const Board& board) const {
    auto grid = CandidateGrid::fromBoard(board);
    if (!grid) {
        LogicalResult result{.status = LogicalStatus::Contradiction, .board = board};
        if (options_.observer) options_.observer->onFinished(result);
        return result;
    }
    return solve(*grid);
}

LogicalResult LogicalSolver::solve(CandidateGrid& grid) const {
    LogicalResult result{.board = grid.board()};
    LogicalStatus status = LogicalStatus::Stalled;
    if (grid.contradiction()) {
        status = LogicalStatus::Contradiction;
    } else {
        while (true) {
            if (grid.solved()) {
                status = LogicalStatus::Solved;
                break;
            }
            if (options_.deadline.expired()) {
                status = LogicalStatus::TimeLimit;
                break;
            }
            auto deduction = step(grid);
            if (!deduction) break;  // stalled
            ++result.steps;
            ++result.uses[static_cast<std::size_t>(deduction->technique)];
            if (options_.observer) options_.observer->onDeduction(*deduction, grid);
            if (options_.recordSteps) result.deductions.push_back(std::move(*deduction));
            if (grid.contradiction()) {
                status = LogicalStatus::Contradiction;
                break;
            }
        }
    }
    result.status = status;
    result.board = grid.board();
    if (options_.observer) options_.observer->onFinished(result);
    return result;
}

}  // namespace sudoku
