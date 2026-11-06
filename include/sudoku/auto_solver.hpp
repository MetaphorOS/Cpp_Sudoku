#pragma once

#include "sudoku/backtracking_solver.hpp"
#include "sudoku/dlx_solver.hpp"
#include "sudoku/solver.hpp"

#include <optional>

namespace sudoku {

// Picks MRV backtracking or Dancing Links per puzzle, from its box size and how many cells are
// empty, then runs that solver unchanged (same result, nodes and trace as calling it directly).
//
// The rule comes from the Step 10 solver study (benchmarks/reports/performance.md):
// - MRV wins easy and medium puzzles. Its cost per node stays flat as boards grow, while DLX
//   scans every open constraint at every node (median 28x slower at 10% empty).
// - DLX wins hard ones: it branches on the most constrained of all four constraint kinds, so
//   its search tree is up to 67,000x smaller.
// The crossover moves to lower sparsity as the box size grows; the thresholds sit midway
// between the measured sparsity steps. On the 645 core-benchmark puzzles that either solver
// finished, the rule solved all of them, at a median 1.0x and worst 4.2x the faster solver's
// time (always-MRV missed 24). The thresholds were fitted to that same data.
//
// DLX is used only if its matrix fits its memory budget; otherwise the puzzle goes to MRV.
class AutoSolver final : public Solver {
public:
    explicit AutoSolver(std::size_t dlxMemoryBudgetBytes = DLXSolver::kDefaultMemoryBudget) noexcept
        : dlx_(dlxMemoryBudgetBytes) {}

    // Minimum percentage of empty cells at which DLX is chosen for this box size; empty for
    // box sizes where MRV is always chosen (n <= 3).
    [[nodiscard]] static std::optional<int> dlxThresholdPercent(int boxSize) noexcept;

    // The solver run() would use for this board.
    [[nodiscard]] const Solver& choose(const Board& board) const;

    [[nodiscard]] std::string_view name() const noexcept override { return "auto"; }
    [[nodiscard]] SolveResult run(const Board& board, const SolveOptions& options) const override {
        return choose(board).run(board, options);
    }

private:
    MRVBacktrackingSolver mrv_;
    DLXSolver dlx_;
};

}  // namespace sudoku
