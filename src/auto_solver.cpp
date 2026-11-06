#include "sudoku/auto_solver.hpp"

#include <cstdint>
#include <limits>

namespace sudoku {

std::optional<int> AutoSolver::dlxThresholdPercent(int boxSize) noexcept {
    // Measured crossovers (population A and B, first and unique): n = 4 between 50% and 60%,
    // n = 5 between 40% and 50%, n = 6-8 between 30% and 40%, n = 9-11 between 20% and 30%.
    // n >= 12 reuses the n = 9-11 value: at 30% no solver finished in the study, so that part
    // is an extrapolation.
    if (boxSize <= 3) return std::nullopt;
    if (boxSize == 4) return 55;
    if (boxSize == 5) return 45;
    if (boxSize <= 8) return 35;
    return 25;
}

const Solver& AutoSolver::choose(const Board& board) const {
    const auto threshold = dlxThresholdPercent(board.boxSize());
    if (!threshold) return mrv_;
    const auto cells = static_cast<std::int64_t>(board.cellCount());
    const auto empty = cells - board.clueCount();
    if (empty * 100 < *threshold * cells) return mrv_;
    const auto estimate = DLXSolver::estimate(board);
    if (!estimate) return mrv_;  // conflicting givens: either solver reports them at once
    const bool fits = estimate->bytes <= dlx_.memoryBudget() &&
                      estimate->nodes <= static_cast<std::uint64_t>(std::numeric_limits<int>::max());
    return fits ? static_cast<const Solver&>(dlx_) : mrv_;
}

}  // namespace sudoku
