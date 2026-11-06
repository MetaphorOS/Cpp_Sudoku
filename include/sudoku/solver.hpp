#pragma once

#include "sudoku/board.hpp"
#include "sudoku/random.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>

namespace sudoku {

class SolverObserver;  // trace.hpp

// Why a search ended. NodeLimit and TimeLimit mean the search was cut short, so the
// solution count is only a lower bound.
enum class StopReason {
    Exhausted,      // every possibility was explored: the count is exact
    SolutionLimit,  // found as many solutions as requested
    NodeLimit,      // SolveOptions::nodeLimit reached
    TimeLimit,      // SolveOptions::timeLimit reached
    InvalidGivens,  // the givens conflict, so there was nothing to search
};

[[nodiscard]] constexpr std::string_view describe(StopReason reason) noexcept {
    switch (reason) {
        case StopReason::Exhausted: return "search space exhausted";
        case StopReason::SolutionLimit: return "solution limit reached";
        case StopReason::NodeLimit: return "node limit reached";
        case StopReason::TimeLimit: return "time limit reached";
        case StopReason::InvalidGivens: return "conflicting givens";
    }
    return "unknown";
}

struct SolveOptions {
    // Stop after this many solutions; 0 means count all of them.
    std::size_t solutionLimit = 1;
    // Abort after visiting this many search nodes; 0 means unbounded. Deterministic.
    std::uint64_t nodeLimit = 0;
    // If set, candidate values are tried in random order.
    Rng* rng = nullptr;
    // If set, receives an event at every search decision (see trace.hpp). Off by default.
    SolverObserver* observer = nullptr;
    // Abort after this much wall-clock time; 0 means unbounded. Checked every 256 nodes, so
    // it can overshoot slightly, and unlike nodeLimit the result depends on machine speed.
    std::chrono::milliseconds timeLimit{0};
};

// Fine-grained operation counts for profiling. They are only collected in builds configured
// with SUDOKU_SOLVER_COUNTERS (CMake option of the same name); otherwise they stay zero, and the
// counting code is not compiled in. Fields that do not apply to a solver stay zero.
struct SolverCounters {
    std::uint64_t candidateComputations = 0;  // backtracking: candidate counts / lists computed
    std::uint64_t cellsScanned = 0;           // MRV: open cells examined while choosing a cell
    std::uint64_t placements = 0;             // values or placement rows tried
    std::uint64_t columnsInspected = 0;       // DLX: open columns examined while choosing a column
    std::uint64_t covers = 0;                 // DLX: cover() calls
    std::uint64_t uncovers = 0;               // DLX: uncover() calls
    std::uint64_t rowsExamined = 0;           // DLX: rows walked inside cover() / uncover()
    std::uint64_t linkUpdates = 0;            // DLX: node unlink + relink operations
};

#ifdef SUDOKU_SOLVER_COUNTERS
inline constexpr bool kSolverCounters = true;
#else
inline constexpr bool kSolverCounters = false;
#endif

struct SolveResult {
    std::size_t solutionCount = 0;
    std::optional<Board> firstSolution{};
    std::uint64_t nodes = 0;       // search nodes visited; a machine-independent cost metric
    std::uint64_t backtracks = 0;  // placements undone so the search could try something else
    StopReason stopReason = StopReason::Exhausted;
    bool aborted = false;  // node or time limit hit, so solutionCount is a lower bound

    // Timing and shape of the search, for benchmarking. setupTime runs from run() entry to the
    // start of the search (validity check, constraint and matrix construction); searchTime from
    // there until run() returns. The time limit counts from run() entry, so it includes setup.
    std::chrono::nanoseconds setupTime{0};
    std::chrono::nanoseconds searchTime{0};
    std::chrono::nanoseconds timeToFirstSolution{0};  // from run() entry; 0 if none was found
    std::uint64_t nodesAtFirstSolution = 0;
    std::uint64_t maxDepth = 0;  // deepest search level reached (placements on the stack)
    SolverCounters counters{};

    [[nodiscard]] bool solved() const noexcept { return solutionCount > 0; }
};

// Interface shared by all solving algorithms, so the generator, CLI and future
// benchmarks can swap in naive backtracking or Dancing Links without changes.
// Implementations must be stateless (all search state lives inside run()), which
// makes a single solver instance safe to share between threads.
class Solver {
public:
    virtual ~Solver() = default;

    [[nodiscard]] virtual std::string_view name() const noexcept = 0;
    [[nodiscard]] virtual SolveResult run(const Board& board, const SolveOptions& options) const = 0;

    [[nodiscard]] std::optional<Board> solve(const Board& board) const {
        return run(board, SolveOptions{}).firstSolution;
    }

    [[nodiscard]] std::size_t countSolutions(const Board& board, std::size_t limit) const {
        return run(board, SolveOptions{.solutionLimit = limit}).solutionCount;
    }

    [[nodiscard]] bool hasUniqueSolution(const Board& board) const {
        return countSolutions(board, 2) == 1;
    }
};

}  // namespace sudoku
