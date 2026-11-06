#pragma once

#include "sudoku/solver.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>

namespace sudoku {

// Size of the exact-cover matrix DLXSolver would build for a board, computed without
// allocating it. All quantities use saturating 64-bit arithmetic.
struct DlxEstimate {
    int emptyCells = 0;
    std::uint64_t placements = 0;  // matrix rows: legal (row, col, value) placements in empty cells
    std::uint64_t columns = 0;     // constraints the givens leave open (4 per empty cell)
    std::uint64_t nodes = 0;       // root + column headers + 4 nodes per placement
    std::uint64_t bytes = 0;       // all significant allocations of one run (matrix + search state)
};

// Knuth's Algorithm X on Dancing Links (DLX).
//
// Sudoku as exact cover: every placement (row, col, value) satisfies exactly four
// constraints - the cell is filled, the row has the value, the column has the value, the box
// has the value - and a solution is a set of placements that satisfies every constraint
// exactly once. The matrix holds only what the givens leave open: one column per
// unsatisfied constraint and one row per legal placement in an empty cell. The search
// repeatedly branches on the open constraint with the fewest remaining placements.
//
// Nodes (SolveResult::nodes) count constraint choices, the DLX counterpart of the
// backtracking solvers' cell choices, so the numbers are comparable but not identical in
// meaning.
class DLXSolver final : public Solver {
public:
    // Upper bound on the memory one run() may allocate for the exact-cover matrix and its
    // search state. The cost is about 92 bytes per legal placement, so a blank 121x121 grid
    // (1.8 million placements) fits and a blank 144x144 grid does not.
    static constexpr std::size_t kDefaultMemoryBudget = std::size_t{256} * 1024 * 1024;

    explicit DLXSolver(std::size_t memoryBudgetBytes = kDefaultMemoryBudget) noexcept
        : memoryBudget_(memoryBudgetBytes) {}

    // True in builds configured with SUDOKU_DLX_VALIDATE: every run checks the linked
    // structure's invariants and throws std::logic_error on a violation (slow; for testing).
#ifdef SUDOKU_DLX_VALIDATE
    static constexpr bool kValidates = true;
#else
    static constexpr bool kValidates = false;
#endif

    [[nodiscard]] std::string_view name() const noexcept override { return "dlx"; }
    [[nodiscard]] std::size_t memoryBudget() const noexcept { return memoryBudget_; }

    // The matrix size and memory run() would need for this board; empty if the givens
    // conflict. run() succeeds exactly when bytes <= memoryBudget() and nodes fit in an int.
    [[nodiscard]] static std::optional<DlxEstimate> estimate(const Board& board);

    // Throws std::length_error, before allocating the matrix, if it would need more than
    // memoryBudget() bytes or more nodes than an int can index. That is a resource limit,
    // not a statement about the puzzle; an unsolvable puzzle returns 0 solutions as usual.
    [[nodiscard]] SolveResult run(const Board& board, const SolveOptions& options) const override;

private:
    std::size_t memoryBudget_;
};

}  // namespace sudoku
