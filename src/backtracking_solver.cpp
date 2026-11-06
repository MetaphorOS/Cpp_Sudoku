#include "sudoku/backtracking_solver.hpp"

#include "sudoku/constraints.hpp"
#include "sudoku/trace.hpp"

#include <algorithm>
#include <chrono>
#include <limits>
#include <optional>
#include <utility>
#include <vector>

namespace sudoku {

namespace {

using Kind = SolverEvent::Kind;
using Clock = std::chrono::steady_clock;

// Holds the mutable state of one solve so the solver classes themselves stay stateless.
//
// kWords is the candidate bitset width when known at compile time (1 for grids up to 64x64,
// the common case), or 0 to use the run-time width. kTraced selects whether events are
// reported; the untraced instantiation contains no tracing code at all. kMrv selects the cell
// to branch on: the open cell with the fewest candidates (MRV), or simply the next open cell
// in row-major order (naive). Everything else is shared.
//
// The depth-first search uses an explicit stack instead of recursion: a 100x100 grid can have
// thousands of empty cells, and one call-stack frame per cell could overflow the stack.
template <int kWords, bool kTraced, bool kMrv>
class Search {
public:
    // `start` is when run() was entered: the time limit and the timings count from there.
    Search(const Board& board, Constraints constraints, const SolveOptions& options, Clock::time_point start)
        : board_(board),
          constraints_(std::move(constraints)),
          options_(options),
          observer_(options.observer),
          start_(start) {
        for (int i = 0; i < board_.cellCount(); ++i) {
            if (board_.at(i) == 0) empty_.push_back(i);
        }
        frames_.resize(empty_.size());
        if (options.timeLimit.count() > 0) deadline_ = start + options.timeLimit;
    }

    SolveResult run() && {
        if constexpr (kTraced) notify({.kind = Kind::Start, .open = openCells(0), .board = &board_});
        const auto searchStart = Clock::now();
        result_.setupTime = searchStart - start_;
        result_.stopReason = search();
        result_.searchTime = Clock::now() - searchStart;
        result_.aborted = result_.stopReason == StopReason::NodeLimit || result_.stopReason == StopReason::TimeLimit;
        if constexpr (kTraced) {
            notify({.kind = Kind::Finish, .solutionLimit = options_.solutionLimit, .reason = result_.stopReason});
        }
        return std::move(result_);
    }

private:
    // One level of the search: the cell empty_[depth] and the candidates still to try there.
    struct Frame {
        std::size_t valueBase = 0;  // this level's candidates are values_[valueBase, valueBase + valueCount)
        int valueCount = 0;
        int next = 0;    // index of the next candidate to try
        int placed = 0;  // value currently placed in the cell, 0 if none
    };

    // empty_[0, depth) are filled; empty_[depth, end) are still open.
    StopReason search() {
        std::size_t depth = 0;
        bool descending = true;  // just arrived at `depth`: choose a cell; otherwise resume its frame
        while (true) {
            if (descending) {
                if (depth == empty_.size()) {
                    const bool stop = recordSolution();
                    if constexpr (kTraced) notify({.kind = Kind::Solution, .depth = static_cast<int>(depth), .board = &board_});
                    if (stop) return StopReason::SolutionLimit;
                    if (depth == 0) return StopReason::Exhausted;  // the board was already complete
                    --depth;
                    descending = false;
                    continue;
                }
                if (const auto limit = limitReached()) return *limit;
                ++result_.nodes;
                if (!select(depth)) {  // dead end: go back up
                    if (depth == 0) return StopReason::Exhausted;
                    --depth;
                    descending = false;
                    continue;
                }
            }

            // Resume the frame at `depth`: undo the previous candidate, then try the next one.
            Frame& frame = frames_[depth];
            const int cell = empty_[depth];
            if (frame.placed != 0) {
                unplace(cell, frame.placed);
                ++result_.backtracks;
                if constexpr (kTraced) notify(cellEvent(Kind::Backtrack, depth, cell, frame.placed));
                frame.placed = 0;
            }
            if (frame.next == frame.valueCount) {  // all candidates tried
                values_.resize(frame.valueBase);
                if (depth == 0) return StopReason::Exhausted;
                --depth;
                continue;
            }
            const int value = values_[frame.valueBase + static_cast<std::size_t>(frame.next++)];
            if constexpr (kTraced) notify(cellEvent(Kind::Place, depth, cell, value));
            place(cell, value);
            frame.placed = value;
            ++depth;
            if (depth > result_.maxDepth) result_.maxDepth = depth;
            descending = true;
        }
    }

    // Puts the cell to branch on at position `depth` and pushes its candidates as a new frame.
    // MRV moves the open cell with the fewest candidates there; naive keeps the cell already
    // there, which is the next open cell in row-major order. Returns false at a dead end
    // (the chosen cell has no candidates).
    bool select(std::size_t depth) {
        std::size_t best = depth;
        int bestCount = std::numeric_limits<int>::max();
        std::size_t i = depth;
        if constexpr (kMrv) {
            for (; i < empty_.size(); ++i) {
                const int cell = empty_[i];
                const int count = constraints_.template candidateCount<kWords>(rowOf(cell), colOf(cell));
                if constexpr (kSolverCounters) {
                    ++result_.counters.cellsScanned;
                    ++result_.counters.candidateComputations;
                }
                if (count < bestCount) {
                    best = i;
                    bestCount = count;
                    if (count <= 1) break;  // can't do better than forced (or dead)
                }
            }
        } else {
            const int cell = empty_[depth];
            bestCount = constraints_.template candidateCount<kWords>(rowOf(cell), colOf(cell));
            if constexpr (kSolverCounters) ++result_.counters.candidateComputations;
        }
        if (bestCount == 0) {
            if constexpr (kTraced) notify(cellEvent(Kind::DeadEnd, depth, empty_[best]));
            return false;
        }

        std::swap(empty_[depth], empty_[best]);  // a no-op for naive (best == depth)
        const int cell = empty_[depth];
        Frame& frame = frames_[depth];
        frame = {.valueBase = values_.size()};
        constraints_.template appendCandidates<kWords>(rowOf(cell), colOf(cell), values_);
        if constexpr (kSolverCounters) ++result_.counters.candidateComputations;
        frame.valueCount = static_cast<int>(values_.size() - frame.valueBase);
        const std::span<int> candidates(values_.data() + frame.valueBase, static_cast<std::size_t>(frame.valueCount));
        if constexpr (kTraced) {
            SolverEvent event = cellEvent(Kind::Select, depth, cell);
            event.candidates.assign(candidates.begin(), candidates.end());
            event.open = openCells(depth);
            event.scanned = static_cast<int>(std::min(i + 1, empty_.size()) - depth);
            event.selection = kMrv ? SolverEvent::Selection::Mrv : SolverEvent::Selection::FixedOrder;
            notify(std::move(event));
        }
        if (options_.rng != nullptr) shuffle(candidates, *options_.rng);
        return true;
    }

    std::optional<StopReason> limitReached() const {
        if (options_.nodeLimit != 0 && result_.nodes >= options_.nodeLimit) return StopReason::NodeLimit;
        // Reading the clock costs more than a node, so check it every 256 nodes.
        if (deadline_ && result_.nodes % 256 == 0 && Clock::now() >= *deadline_) return StopReason::TimeLimit;
        return std::nullopt;
    }

    bool recordSolution() {
        ++result_.solutionCount;
        if (!result_.firstSolution) {
            result_.firstSolution = board_;
            result_.nodesAtFirstSolution = result_.nodes;
            result_.timeToFirstSolution = Clock::now() - start_;
        }
        return options_.solutionLimit != 0 && result_.solutionCount >= options_.solutionLimit;
    }

    void place(int cell, int value) {
        if constexpr (kSolverCounters) ++result_.counters.placements;
        constraints_.place(rowOf(cell), colOf(cell), value);
        board_.setAt(cell, value);
    }
    void unplace(int cell, int value) {
        board_.setAt(cell, 0);
        constraints_.remove(rowOf(cell), colOf(cell), value);
    }

    [[nodiscard]] int rowOf(int cell) const noexcept { return cell / board_.size(); }
    [[nodiscard]] int colOf(int cell) const noexcept { return cell % board_.size(); }

    // ---- Tracing helpers; only reached when an observer is set. ----

    [[nodiscard]] int openCells(std::size_t depth) const { return static_cast<int>(empty_.size() - depth); }

    [[nodiscard]] SolverEvent cellEvent(Kind kind, std::size_t depth, int cell, int value = 0) const {
        return {.kind = kind, .depth = static_cast<int>(depth), .row = rowOf(cell), .col = colOf(cell), .value = value};
    }

    // Fills in the running totals and hands the event to the observer.
    void notify(SolverEvent event) {
        event.nodes = result_.nodes;
        event.backtracks = result_.backtracks;
        event.solutions = result_.solutionCount;
        observer_->onSolverEvent(event);
    }

    Board board_;
    Constraints constraints_;
    const SolveOptions& options_;
    SolverObserver* observer_;
    Clock::time_point start_;
    std::vector<int> empty_;
    std::vector<Frame> frames_;
    std::vector<int> values_;  // candidate lists of all active frames, stacked
    std::optional<Clock::time_point> deadline_;
    SolveResult result_;
};

template <int kWords, bool kMrv>
SolveResult runWith(const Board& board, Constraints constraints, const SolveOptions& options, Clock::time_point start) {
    if (options.observer) return Search<kWords, true, kMrv>(board, std::move(constraints), options, start).run();
    return Search<kWords, false, kMrv>(board, std::move(constraints), options, start).run();
}

template <bool kMrv>
SolveResult runBacktracking(const Board& board, const SolveOptions& options) {
    const auto start = Clock::now();
    auto constraints = Constraints::fromBoard(board);
    if (!constraints) {  // conflicting givens: no solutions
        if (options.observer) {
            options.observer->onSolverEvent({.kind = SolverEvent::Kind::Finish,
                                             .solutionLimit = options.solutionLimit,
                                             .reason = StopReason::InvalidGivens,
                                             .board = &board});
        }
        return {.stopReason = StopReason::InvalidGivens, .setupTime = Clock::now() - start};
    }
    // Grids up to 64x64 fit each bitset in one word: use the compile-time single-word path.
    if (constraints->words() == 1) return runWith<1, kMrv>(board, std::move(*constraints), options, start);
    return runWith<0, kMrv>(board, std::move(*constraints), options, start);
}

}  // namespace

SolveResult MRVBacktrackingSolver::run(const Board& board, const SolveOptions& options) const {
    return runBacktracking<true>(board, options);
}

SolveResult NaiveBacktrackingSolver::run(const Board& board, const SolveOptions& options) const {
    return runBacktracking<false>(board, options);
}

}  // namespace sudoku
