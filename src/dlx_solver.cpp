#include "sudoku/dlx_solver.hpp"

#include "sudoku/constraints.hpp"
#include "sudoku/trace.hpp"

#include <array>
#include <chrono>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace sudoku {

namespace {

using Kind = SolverEvent::Kind;
using Clock = std::chrono::steady_clock;

// ---- Sizing, with saturating arithmetic so that nothing can overflow ---------------

constexpr std::uint64_t kSaturated = std::numeric_limits<std::uint64_t>::max();

std::uint64_t mulSat(std::uint64_t a, std::uint64_t b) { return a != 0 && b > kSaturated / a ? kSaturated : a * b; }
std::uint64_t addSat(std::uint64_t a, std::uint64_t b) { return b > kSaturated - a ? kSaturated : a + b; }

// One level of the search: the chosen column and the placements (row nodes) still to try.
struct Frame {
    std::size_t optionBase = 0;  // this level's placements are options_[optionBase, optionBase + optionCount)
    int column = 0;              // header node of the covered column
    int optionCount = 0;
    int next = 0;     // index of the next placement to try
    int placed = -1;  // row node currently selected, -1 if none
};

using MatrixSize = DlxEstimate;

MatrixSize measure(const Board& board, const Constraints& constraints) {
    MatrixSize m;
    for (int i = 0; i < board.cellCount(); ++i) {
        if (board.at(i) != 0) continue;
        ++m.emptyCells;
        m.placements += static_cast<std::uint64_t>(constraints.candidateCount(i / board.size(), i % board.size()));
    }
    // Each valid given satisfies exactly one constraint of each kind, so 4 per empty cell are open.
    m.columns = 4 * static_cast<std::uint64_t>(m.emptyCells);
    m.nodes = addSat(addSat(1, m.columns), mulSat(4, m.placements));

    const auto cells = static_cast<std::uint64_t>(board.cellCount());
    const std::uint64_t headers = m.columns + 1;
    std::uint64_t bytes = mulSat(mulSat(5, m.nodes), sizeof(int));          // left, right, up, down, column
    bytes = addSat(bytes, mulSat(headers, sizeof(int)));                      // column sizes
    bytes = addSat(bytes, mulSat(headers, sizeof(std::int64_t)));             // constraint of each column
    bytes = addSat(bytes, mulSat(mulSat(4, cells), sizeof(int)));             // constraint -> column (build only)
    bytes = addSat(bytes, mulSat(m.placements, 2 * sizeof(int)));             // cell and value of each row
    bytes = addSat(bytes, mulSat(m.placements, sizeof(int)));                 // option stack
    bytes = addSat(bytes, mulSat(static_cast<std::uint64_t>(m.emptyCells), sizeof(Frame)));  // frame stack
    bytes = addSat(bytes, mulSat(mulSat(2, cells), sizeof(int)));             // working board + first solution
    m.bytes = bytes;
    return m;
}

std::string mebibytes(std::uint64_t bytes) {
    if (bytes == kSaturated) return "more than 16 EiB";
    if (bytes < (std::uint64_t{1} << 20)) return std::to_string(bytes) + " bytes";
    return std::to_string(addSat(bytes, (std::uint64_t{1} << 20) - 1) >> 20) + " MiB";
}

// ---- Search ---------------------------------------------------------------------------

// Holds one run's matrix and search state, so DLXSolver itself stays stateless.
//
// Node layout in the parallel arrays left_/right_/up_/down_/column_:
//   0                  root of the header list (the list of still-open columns)
//   1 .. columns       column headers; size_[h] counts the rows still linked in column h
//   columns + 1 ..     row nodes, 4 consecutive nodes per placement, linked left/right in a ring
//
// The search is iterative, like the backtracking solvers: frames_[d] records the column
// covered at depth d and which of its placements is selected, so backtracking only has to
// undo the links of the current frame. kTraced selects whether events are reported.
template <bool kTraced>
class DlxSearch {
public:
    // `start` is when run() was entered: the time limit and the timings count from there, so
    // matrix construction counts against the limit.
    DlxSearch(const Board& board, const Constraints& constraints, const MatrixSize& m, const SolveOptions& options,
              Clock::time_point start)
        : board_(board), options_(options), observer_(options.observer), emptyCells_(m.emptyCells), start_(start) {
        build(constraints, m);
        if constexpr (DLXSolver::kValidates) {
            checkStructure(true);
            snapshot_ = {left_, right_, up_, down_, size_};
        }
        frames_.resize(static_cast<std::size_t>(m.emptyCells));
        options_stack_.reserve(static_cast<std::size_t>(m.placements));
        if (options.timeLimit.count() > 0) deadline_ = start + options.timeLimit;
    }

    SolveResult run() && {
        if constexpr (kTraced) notify({.kind = Kind::Start, .open = emptyCells_, .board = &board_});
        const auto searchStart = Clock::now();
        result_.setupTime = searchStart - start_;
        result_.stopReason = search();
        result_.searchTime = Clock::now() - searchStart;
        result_.aborted = result_.stopReason == StopReason::NodeLimit || result_.stopReason == StopReason::TimeLimit;
        if constexpr (DLXSolver::kValidates) {
            // An exhausted search has undone every cover, so the links must be exactly as built.
            if (result_.stopReason == StopReason::Exhausted &&
                Snapshot{left_, right_, up_, down_, size_} != snapshot_) {
                throw std::logic_error("DLX invariant violated: links not restored after an exhausted search");
            }
        }
        if constexpr (kTraced) {
            notify({.kind = Kind::Finish, .solutionLimit = options_.solutionLimit, .reason = result_.stopReason});
        }
        // Stopping early leaves columns covered; the matrix is discarded with this object, so
        // nothing needs to be restored. firstSolution was copied when it was found.
        return std::move(result_);
    }

private:
    // ---- Invariant checks (SUDOKU_DLX_VALIDATE builds only) ----

    struct Snapshot {
        std::vector<int> left, right, up, down, size;
        bool operator==(const Snapshot&) const = default;
    };

    // Matrices up to this many nodes get a full structural check at every search node.
    static constexpr std::size_t kFullCheckNodes = 20'000;

    [[noreturn]] static void fail(const std::string& what) {
        throw std::logic_error("DLX invariant violated: " + what);
    }

    // Checks the active part of the structure: the header list is a consistent doubly linked
    // ring; every active column's vertical list is a consistent doubly linked ring whose length
    // equals size_; every node in it belongs to that column; and every row it reaches is a ring
    // of 4 nodes covering one constraint of each kind (cell, row, column, box), in that order.
    // `initial` also requires every column to be active, as right after construction.
    void checkStructure(bool initial) const {
        const auto headers = static_cast<int>(size_.size());  // header nodes are 1 .. headers - 1
        const auto nodes = static_cast<int>(left_.size());
        std::vector<char> active(size_.size(), 0);
        int count = 0;
        int previous = 0;
        for (int c = right_[0]; c != 0; c = right_[at(c)]) {
            if (c < 1 || c >= headers) fail("header list points outside the column headers");
            if (left_[at(c)] != previous) fail("header list left link of column " + std::to_string(c));
            if (active[at(c)] != 0 || ++count >= headers) fail("header list revisits a column");
            active[at(c)] = 1;
            previous = c;
        }
        if (left_[0] != previous) fail("root left link");
        if (initial && count != headers - 1) fail("not every column is active after construction");

        for (int c = 1; c < headers; ++c) {
            if (active[at(c)] == 0) continue;
            int length = 0;
            int above = c;
            for (int i = down_[at(c)]; i != c; i = down_[at(i)]) {
                if (i < firstRowNode_ || i >= nodes) fail("vertical list leaves the row nodes");
                if (up_[at(i)] != above) fail("vertical up link in column " + std::to_string(c));
                if (column_[at(i)] != c) fail("node in the wrong column");
                if (++length > size_[at(c)]) fail("column " + std::to_string(c) + " longer than its size");
                checkRow(i);
                above = i;
            }
            if (up_[at(c)] != above) fail("column tail link");
            if (length != size_[at(c)]) fail("column " + std::to_string(c) + " shorter than its size");
        }
    }

    void checkRow(int node) const {
        const int base = firstRowNode_ + static_cast<int>(rowIndex(node)) * 4;
        const std::int64_t cells = board_.cellCount();
        for (int k = 0; k < 4; ++k) {
            const int j = base + k;
            if (right_[at(j)] != base + (k + 1) % 4 || left_[at(j)] != base + (k + 3) % 4) fail("row ring links");
            if (constraintOf_[at(column_[at(j)])] / cells != k) fail("row does not cover one constraint of each kind");
        }
    }

    // Cheap enough per search node only on small matrices.
    void checkDuringSearch() const {
        if (left_.size() <= kFullCheckNodes) checkStructure(false);
    }

    // ---- Construction ----

    // Constraint ids, n2 = number of cells, N = board size, v = value - 1:
    //   [0, n2)      cell (r, c) is filled         r * N + c
    //   [n2, 2n2)    row r contains v              n2 + r * N + v
    //   [2n2, 3n2)   column c contains v           2 * n2 + c * N + v
    //   [3n2, 4n2)   box b contains v              3 * n2 + b * N + v
    [[nodiscard]] std::array<std::int64_t, 4> constraintsOf(int cell, int value) const {
        const std::int64_t n = board_.size();
        const std::int64_t n2 = board_.cellCount();
        const int row = cell / board_.size();
        const int col = cell % board_.size();
        const std::int64_t v = value - 1;
        return {cell, n2 + row * n + v, 2 * n2 + col * n + v, 3 * n2 + board_.boxIndex(row, col) * n + v};
    }

    void build(const Constraints& constraints, const MatrixSize& m) {
        const auto nodes = static_cast<std::size_t>(m.nodes);
        const auto columns = static_cast<int>(m.columns);
        left_.resize(nodes);
        right_.resize(nodes);
        up_.resize(nodes);
        down_.resize(nodes);
        column_.resize(nodes);
        size_.assign(static_cast<std::size_t>(columns) + 1, 0);
        constraintOf_.assign(static_cast<std::size_t>(columns) + 1, -1);
        rowCell_.reserve(static_cast<std::size_t>(m.placements));
        rowValue_.reserve(static_cast<std::size_t>(m.placements));

        // Constraint id -> column header; -1 for constraints the givens already satisfy.
        std::vector<int> columnOf(4 * static_cast<std::size_t>(board_.cellCount()), 0);
        for (int cell = 0; cell < board_.cellCount(); ++cell) {
            if (board_.at(cell) == 0) continue;
            for (const std::int64_t id : constraintsOf(cell, board_.at(cell))) columnOf[static_cast<std::size_t>(id)] = -1;
        }

        // Header list: root 0, then one header per open constraint, in constraint-id order.
        left_[0] = right_[0] = 0;
        int header = 0;
        for (std::size_t id = 0; id < columnOf.size(); ++id) {
            if (columnOf[id] < 0) continue;
            ++header;
            columnOf[id] = header;
            constraintOf_[static_cast<std::size_t>(header)] = static_cast<std::int64_t>(id);
            up_[at(header)] = down_[at(header)] = column_[at(header)] = header;
            left_[at(header)] = left_[0];
            right_[at(header)] = 0;
            right_[at(left_[0])] = header;
            left_[0] = header;
        }

        // One matrix row per legal placement; each of its 4 nodes goes to the bottom of its column.
        firstRowNode_ = columns + 1;
        int node = firstRowNode_;
        std::vector<int> values;
        for (int cell = 0; cell < board_.cellCount(); ++cell) {
            if (board_.at(cell) != 0) continue;
            values.clear();
            constraints.appendCandidates(cell / board_.size(), cell % board_.size(), values);
            for (const int value : values) {
                rowCell_.push_back(cell);
                rowValue_.push_back(value);
                const auto ids = constraintsOf(cell, value);
                for (int k = 0; k < 4; ++k) {
                    const int n = node + k;
                    const int col = columnOf[static_cast<std::size_t>(ids[static_cast<std::size_t>(k)])];
                    column_[at(n)] = col;
                    up_[at(n)] = up_[at(col)];
                    down_[at(n)] = col;
                    down_[at(up_[at(col)])] = n;
                    up_[at(col)] = n;
                    ++size_[at(col)];
                    left_[at(n)] = node + (k + 3) % 4;
                    right_[at(n)] = node + (k + 1) % 4;
                }
                node += 4;
            }
        }
    }

    // ---- Knuth's cover and uncover ----

    // Removes column c from the header list, and every row that has a node in c from the
    // other columns it touches. c's own nodes stay linked vertically, so uncover can walk them.
    void cover(int c) {
        if constexpr (kSolverCounters) ++result_.counters.covers;
        right_[at(left_[at(c)])] = right_[at(c)];
        left_[at(right_[at(c)])] = left_[at(c)];
        for (int i = down_[at(c)]; i != c; i = down_[at(i)]) {
            if constexpr (kSolverCounters) ++result_.counters.rowsExamined;
            for (int j = right_[at(i)]; j != i; j = right_[at(j)]) {
                if constexpr (kSolverCounters) ++result_.counters.linkUpdates;
                up_[at(down_[at(j)])] = up_[at(j)];
                down_[at(up_[at(j)])] = down_[at(j)];
                --size_[at(column_[at(j)])];
            }
        }
    }

    // Exact reverse of cover(c): rows bottom-up, nodes right-to-left, then the header.
    void uncover(int c) {
        if constexpr (kSolverCounters) ++result_.counters.uncovers;
        for (int i = up_[at(c)]; i != c; i = up_[at(i)]) {
            if constexpr (kSolverCounters) ++result_.counters.rowsExamined;
            for (int j = left_[at(i)]; j != i; j = left_[at(j)]) {
                if constexpr (kSolverCounters) ++result_.counters.linkUpdates;
                ++size_[at(column_[at(j)])];
                up_[at(down_[at(j)])] = j;
                down_[at(up_[at(j)])] = j;
            }
        }
        right_[at(left_[at(c)])] = c;
        left_[at(right_[at(c)])] = c;
    }

    // Selects the placement of row node r (its own column is already covered): covers the
    // other constraints the placement satisfies, left to right, and writes it on the board.
    void select(int r) {
        if constexpr (kSolverCounters) ++result_.counters.placements;
        for (int j = right_[at(r)]; j != r; j = right_[at(j)]) cover(column_[at(j)]);
        board_.setAt(cellOf(r), valueOf(r));
    }

    // Exact reverse of select(r).
    void deselect(int r) {
        board_.setAt(cellOf(r), 0);
        for (int j = left_[at(r)]; j != r; j = left_[at(j)]) uncover(column_[at(j)]);
    }

    // The open column with the fewest rows. Scans every open column, returning at once if one
    // has no rows left (the branch is unsatisfiable). Ties go to the lowest constraint id.
    [[nodiscard]] int chooseColumn() {
        int best = right_[0];
        for (int c = right_[0]; c != 0; c = right_[at(c)]) {
            if constexpr (kSolverCounters) ++result_.counters.columnsInspected;
            if (size_[at(c)] == 0) return c;
            if (size_[at(c)] < size_[at(best)]) best = c;
        }
        return best;
    }

    // ---- The search loop ----

    StopReason search() {
        std::size_t depth = 0;
        bool descending = true;  // just arrived at `depth`: choose a column; otherwise resume its frame
        while (true) {
            if (descending) {
                if (right_[0] == 0) {  // no open constraints: the board is complete
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
                if constexpr (DLXSolver::kValidates) checkDuringSearch();
                const int column = chooseColumn();
                if (size_[at(column)] == 0) {  // dead end: nothing can satisfy this constraint
                    if constexpr (kTraced) notify(constraintEvent(Kind::DeadEnd, depth, column));
                    if (depth == 0) return StopReason::Exhausted;
                    --depth;
                    descending = false;
                    continue;
                }
                if constexpr (kTraced) notify(constraintEvent(Kind::Select, depth, column));
                cover(column);
                Frame& frame = frames_[depth];
                frame = {.optionBase = options_stack_.size(), .column = column};
                for (int i = down_[at(column)]; i != column; i = down_[at(i)]) options_stack_.push_back(i);
                frame.optionCount = static_cast<int>(options_stack_.size() - frame.optionBase);
                if (options_.rng != nullptr) {
                    shuffle(std::span<int>(options_stack_.data() + frame.optionBase,
                                           static_cast<std::size_t>(frame.optionCount)),
                            *options_.rng);
                }
            }

            // Resume the frame at `depth`: undo the previous placement, then try the next one.
            Frame& frame = frames_[depth];
            if (frame.placed >= 0) {
                deselect(frame.placed);
                ++result_.backtracks;
                if constexpr (kTraced) notify(placementEvent(Kind::Backtrack, depth, frame.placed));
                frame.placed = -1;
            }
            if (frame.next == frame.optionCount) {  // every placement tried: reopen the column
                options_stack_.resize(frame.optionBase);
                uncover(frame.column);
                if (depth == 0) return StopReason::Exhausted;
                --depth;
                continue;
            }
            const int row = options_stack_[frame.optionBase + static_cast<std::size_t>(frame.next++)];
            if constexpr (kTraced) notify(placementEvent(Kind::Place, depth, row));
            select(row);
            frame.placed = row;
            ++depth;
            if (depth > result_.maxDepth) result_.maxDepth = depth;
            descending = true;
        }
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

    [[nodiscard]] static std::size_t at(int node) noexcept { return static_cast<std::size_t>(node); }
    [[nodiscard]] std::size_t rowIndex(int node) const noexcept { return at((node - firstRowNode_) / 4); }
    [[nodiscard]] int cellOf(int node) const noexcept { return rowCell_[rowIndex(node)]; }
    [[nodiscard]] int valueOf(int node) const noexcept { return rowValue_[rowIndex(node)]; }

    // ---- Tracing helpers; only reached when an observer is set. ----

    [[nodiscard]] std::string describe(std::int64_t id) const {
        const std::int64_t n = board_.size();
        const std::int64_t n2 = board_.cellCount();
        const std::int64_t rest = id % n2;
        const std::string a = std::to_string(rest / n + 1);
        const std::string b = std::to_string(rest % n + 1);
        switch (id / n2) {
            case 0: return "cell r" + a + "c" + b;
            case 1: return "row " + a + " needs a " + b;
            case 2: return "column " + a + " needs a " + b;
            default: return "box " + a + " needs a " + b;
        }
    }

    [[nodiscard]] SolverEvent constraintEvent(Kind kind, std::size_t depth, int column) const {
        SolverEvent event{.kind = kind, .selection = SolverEvent::Selection::Constraint, .depth = static_cast<int>(depth)};
        const std::int64_t id = constraintOf_[at(column)];
        if (id < board_.cellCount()) {  // a cell constraint names a cell
            event.row = static_cast<int>(id / board_.size());
            event.col = static_cast<int>(id % board_.size());
        }
        event.open = emptyCells_ - static_cast<int>(depth);
        event.constraint = describe(id);
        event.options = size_[at(column)];
        return event;
    }

    [[nodiscard]] SolverEvent placementEvent(Kind kind, std::size_t depth, int node) const {
        const int cell = cellOf(node);
        return {.kind = kind,
                .depth = static_cast<int>(depth),
                .row = cell / board_.size(),
                .col = cell % board_.size(),
                .value = valueOf(node)};
    }

    // Fills in the running totals and hands the event to the observer.
    void notify(SolverEvent event) {
        event.nodes = result_.nodes;
        event.backtracks = result_.backtracks;
        event.solutions = result_.solutionCount;
        observer_->onSolverEvent(event);
    }

    Board board_;  // givens plus the current placements
    const SolveOptions& options_;
    SolverObserver* observer_;
    int emptyCells_;
    Clock::time_point start_;
    int firstRowNode_ = 0;
    std::vector<int> left_, right_, up_, down_, column_;
    std::vector<int> size_;                  // per column header
    std::vector<std::int64_t> constraintOf_;  // per column header
    std::vector<int> rowCell_, rowValue_;    // per placement row
    std::vector<Frame> frames_;
    std::vector<int> options_stack_;  // placement lists of all active frames, stacked
    std::optional<Clock::time_point> deadline_;
    SolveResult result_;
    Snapshot snapshot_;  // links right after construction; filled only in validating builds
};

}  // namespace

std::optional<DlxEstimate> DLXSolver::estimate(const Board& board) {
    const auto constraints = Constraints::fromBoard(board);
    if (!constraints) return std::nullopt;
    return measure(board, *constraints);
}

SolveResult DLXSolver::run(const Board& board, const SolveOptions& options) const {
    const auto start = Clock::now();
    const auto constraints = Constraints::fromBoard(board);
    if (!constraints) {  // conflicting givens: no solutions
        if (options.observer) {
            options.observer->onSolverEvent({.kind = SolverEvent::Kind::Finish,
                                             .solutionLimit = options.solutionLimit,
                                             .reason = StopReason::InvalidGivens,
                                             .board = &board});
        }
        return {.stopReason = StopReason::InvalidGivens, .setupTime = Clock::now() - start};
    }

    const MatrixSize m = measure(board, *constraints);
    const std::string grid = std::to_string(board.size()) + "x" + std::to_string(board.size());
    if (m.nodes > static_cast<std::uint64_t>(std::numeric_limits<int>::max())) {
        throw std::length_error("DLX: the exact-cover matrix for this " + grid + " puzzle (" +
                                std::to_string(m.placements) + " placements) has more nodes than an int can index");
    }
    if (m.bytes > memoryBudget_) {
        throw std::length_error("DLX: the exact-cover matrix for this " + grid + " puzzle (" +
                                std::to_string(m.placements) + " placements) needs about " + mebibytes(m.bytes) +
                                ", more than the " + mebibytes(memoryBudget_) + " memory budget");
    }

    if (options.observer) return DlxSearch<true>(board, *constraints, m, options, start).run();
    return DlxSearch<false>(board, *constraints, m, options, start).run();
}

}  // namespace sudoku
