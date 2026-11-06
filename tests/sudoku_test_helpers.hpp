#pragma once

// Sudoku-specific helpers shared by the unit tests and the property tests.

#include "test_framework.hpp"

#include "sudoku/backtracking_solver.hpp"
#include "sudoku/board.hpp"
#include "sudoku/dlx_solver.hpp"
#include "sudoku/solver.hpp"

#include <algorithm>
#include <array>
#include <string>
#include <string_view>
#include <vector>

namespace sudoku_test {

// The well-known example puzzle from Wikipedia's "Sudoku" article, with its unique solution.
inline constexpr const char* kClassicPuzzle =
    "530070000600195000098000060800060003400803001700020006060000280000419005000080079";
inline constexpr const char* kClassicSolution =
    "534678912672195348198342567859761423426853791713924856961537284287419635345286179";

// The three solvers under test, for checks that must hold for every algorithm. Failures name
// the solver through CONTEXT("solver: ", solver.name()).
inline const std::array<const sudoku::Solver*, 3>& allSolvers() {
    static const sudoku::NaiveBacktrackingSolver naive;
    static const sudoku::MRVBacktrackingSolver mrv;
    static const sudoku::DLXSolver dlx;
    static const std::array<const sudoku::Solver*, 3> solvers{&naive, &mrv, &dlx};
    return solvers;
}

// Independent check of a returned solution: it does not use sudoku::Constraints or
// Board::isValid, the code the solvers themselves rely on. Returns an empty string if
// `solution` is a complete grid in which every row, column and box holds each value 1..N
// exactly once and every given of `puzzle` is kept; otherwise a description of the first
// problem found.
inline std::string solutionProblem(const sudoku::Board& solution, const sudoku::Board& puzzle) {
    const int n = puzzle.boxSize();
    const int size = puzzle.size();
    if (solution.boxSize() != n) return mini_test::concat("box size ", solution.boxSize(), " instead of ", n);
    for (int i = 0; i < puzzle.cellCount(); ++i) {
        const int value = solution.at(i);
        if (value < 1 || value > size) return mini_test::concat("cell ", i, " holds ", value);
        if (puzzle.at(i) != 0 && puzzle.at(i) != value) {
            return mini_test::concat("cell ", i, " is given as ", puzzle.at(i), " but the solution has ", value);
        }
    }
    // unit u of kind k: rows (k = 0), columns (k = 1), boxes (k = 2); cell j of that unit.
    const auto cellOf = [&](int kind, int unit, int j) {
        if (kind == 0) return unit * size + j;
        if (kind == 1) return j * size + unit;
        const int row = (unit / n) * n + j / n;
        const int col = (unit % n) * n + j % n;
        return row * size + col;
    };
    static constexpr const char* kKinds[] = {"row", "column", "box"};
    std::vector<char> seen(static_cast<std::size_t>(size) + 1);
    for (int kind = 0; kind < 3; ++kind) {
        for (int unit = 0; unit < size; ++unit) {
            std::fill(seen.begin(), seen.end(), 0);
            for (int j = 0; j < size; ++j) {
                const int value = solution.at(cellOf(kind, unit, j));
                if (seen[static_cast<std::size_t>(value)] != 0) {
                    return mini_test::concat(kKinds[kind], ' ', unit, " repeats ", value);
                }
                seen[static_cast<std::size_t>(value)] = 1;
            }
        }
    }
    return {};
}

// CHECKs solutionProblem() and reports the problem on failure.
inline void checkSolutionIndependently(const sudoku::Board& solution, const sudoku::Board& puzzle) {
    const std::string problem = solutionProblem(solution, puzzle);
    if (!problem.empty()) FAIL("returned solution is wrong: ", problem);
}

// "16x16 (box 4)": how failure context names a grid size.
inline std::string gridName(int boxSize) {
    return mini_test::concat(boxSize * boxSize, 'x', boxSize * boxSize, " (box ", boxSize, ')');
}

// For TRACE output: size and clue count, then the pretty grid up to 9x9. Larger grids get one
// abbreviated line unless --full.
inline std::string boardText(const sudoku::Board& board) {
    const std::string summary = mini_test::concat(board.size(), 'x', board.size(), ", ", board.clueCount(), " clues");
    if (board.size() <= 9 || mini_test::settings().full) return summary + '\n' + board.toPrettyString();
    return summary + ": " + mini_test::abbreviate(board.toString());
}

inline std::string statsText(const sudoku::SolveResult& result) {
    return mini_test::concat(result.solutionCount, result.solutionCount == 1 ? " solution" : " solutions", ", ",
                             result.nodes, " search nodes", result.aborted ? ", aborted at node limit" : "");
}

// Quotes text with newlines and tabs escaped, so odd inputs stay on one line.
inline std::string escaped(std::string_view text) {
    std::string out = "\"";
    for (const char ch : text) {
        if (ch == '\n') {
            out += "\\n";
        } else if (ch == '\t') {
            out += "\\t";
        } else {
            out += ch;
        }
    }
    return out + '"';
}

// Checks that `solution` is a complete, valid grid that keeps every given of `puzzle`.
inline void checkSolves(const sudoku::Board& solution, const sudoku::Board& puzzle) {
    CHECK_EQ(solution.boxSize(), puzzle.boxSize());
    CHECK(solution.isFilled());
    CHECK(solution.isValid());
    for (int i = 0; i < puzzle.cellCount(); ++i) {
        if (puzzle.at(i) != 0 && puzzle.at(i) != solution.at(i)) {
            FAIL("cell (", i / puzzle.size(), ',', i % puzzle.size(), ") is given as ", puzzle.at(i),
                 " but the solution has ", solution.at(i));
            return;
        }
    }
}

}  // namespace sudoku_test
