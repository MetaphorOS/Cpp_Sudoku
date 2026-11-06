#pragma once

#include "sudoku/solver.hpp"

namespace sudoku {

// The two backtracking solvers share one search engine (src/backtracking_solver.cpp): an
// iterative depth-first search over the empty cells with bitset candidate sets. They differ
// only in which empty cell they branch on next.

// Minimum Remaining Values: always branch on the empty cell with the fewest candidates.
class MRVBacktrackingSolver final : public Solver {
public:
    [[nodiscard]] std::string_view name() const noexcept override { return "backtracking-mrv"; }
    [[nodiscard]] SolveResult run(const Board& board, const SolveOptions& options) const override;
};

// Fixed order: branch on the next empty cell in row-major order, without looking at the
// other cells. Same candidates, placement, limits and tracing as the MRV solver.
class NaiveBacktrackingSolver final : public Solver {
public:
    [[nodiscard]] std::string_view name() const noexcept override { return "backtracking-naive"; }
    [[nodiscard]] SolveResult run(const Board& board, const SolveOptions& options) const override;
};

}  // namespace sudoku
