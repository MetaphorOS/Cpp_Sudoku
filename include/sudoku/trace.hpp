#pragma once

// Optional observation of the solving and generation algorithms.
//
// Pass an observer through SolveOptions::observer or GeneratorOptions::observer; the algorithm
// calls it at each decision point. Without an observer (the default) no tracing code runs: the
// solver is compiled in a traced and an untraced variant. Events are plain structs;
// text_trace.hpp turns them into readable output.

#include "sudoku/board.hpp"
#include "sudoku/difficulty.hpp"
#include "sudoku/generator.hpp"
#include "sudoku/solver.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace sudoku {

struct SolverEvent {
    enum class Kind {
        Start,      // search begins on `board` with `open` empty cells
        Select,     // the search branches next on (row, col) or on `constraint`; see `selection`
        Place,      // trying `value` at (row, col)
        Backtrack,  // `value` at (row, col) is undone and the search moves on
        DeadEnd,    // (row, col), or `constraint`, has no candidates left, so this branch fails
        Solution,   // `board` is complete; `solutions` counts it
        Finish,     // the search is over; see `reason`
    };
    // How a Select event's branch point was chosen.
    enum class Selection {
        Mrv,         // backtracking: (row, col) has the fewest candidates; scanned `scanned` of `open` cells
        FixedOrder,  // backtracking: (row, col) is the next empty cell in row-major order
        Constraint,  // DLX: `constraint` is the unsatisfied constraint with the fewest `options`
    };

    Kind kind = Kind::Start;
    Selection selection = Selection::Mrv;  // Select only
    int depth = 0;  // cells filled by the search so far
    int row = -1;   // 0-based; -1 when not about a cell
    int col = -1;
    int value = 0;
    std::vector<int> candidates{};  // Select: the cell's candidate values, ascending
    int open = 0;                 // empty cells left at this depth
    int scanned = 0;  // Select: cells MRV looked at; it stops early at a cell with 0 or 1 candidates
    std::string constraint{};      // DLX Select and DeadEnd: the constraint, e.g. "row 3 needs a 5"
    int options = 0;               // DLX Select: placements that would satisfy `constraint`
    std::uint64_t nodes = 0;       // running totals, as in SolveResult
    std::uint64_t backtracks = 0;
    std::size_t solutions = 0;
    std::size_t solutionLimit = 0;              // Finish: the limit that was in force (0 = none)
    StopReason reason = StopReason::Exhausted;  // Finish only
    const Board* board = nullptr;               // Start and Solution only; valid during the call
};

class SolverObserver {
public:
    virtual ~SolverObserver() = default;
    virtual void onSolverEvent(const SolverEvent& event) = 0;
};

struct GeneratorEvent {
    enum class Kind {
        FilledGrid,  // `board` is the random complete grid the puzzle is carved from
        Removal,     // tried to remove `value` at (row, col); the outcome is `verdict`
        Rated,       // difficulty target only: attempt `index` of `total` (seed `seed`) ended
                     // with `clues` clues, graded `difficulty` (if `graded`; otherwise the time
                     // limit ran out first and its grade is unknown)
        Finished,    // `board` is the final puzzle, `solution` its unique solution
    };
    enum class Verdict {
        Accepted,           // removed: the uniqueness check found exactly one solution
        Forced,             // removed without a search: the remaining clues still force the value
        MultipleSolutions,  // kept: removing it allows a second solution
        CheckAborted,       // kept: the uniqueness check hit its node limit
        TimeLimit,          // kept: the generation time limit ran out; generation stops here
        SolvedByLogic,      // removed: the target's techniques still solve it, so it is unique
        TooHard,            // kept: removing it needs techniques beyond the target
    };

    Kind kind = Kind::FilledGrid;
    std::uint64_t seed = 0;
    int index = 0;  // Removal: 1-based position in the random removal order
    int total = 0;  // Removal: number of cells to try
    int row = -1;
    int col = -1;
    int value = 0;
    Verdict verdict = Verdict::Accepted;
    std::size_t solutions = 0;     // Removal: solutions the uniqueness check found (it stops at 2)
    std::uint64_t nodes = 0;       // Removal: nodes the check used (0 for Forced)
    std::uint64_t nodeLimit = 0;   // Removal: the check's budget (0 = none)
    int clues = 0;                 // clues on the board after this event
    bool timeLimitReached = false; // Finished: generation stopped early, so the puzzle may not be minimal
    const Board* board = nullptr;  // FilledGrid and Finished; valid during the call
    const Board* solution = nullptr;  // Finished
    const GenerationStats* stats = nullptr;  // Finished: how the returned puzzle's removals went
    std::optional<Difficulty> target{};      // the difficulty target, if any (every event)
    Difficulty difficulty = Difficulty::Easy;  // Rated, and Finished with a target: the grade, if `graded`
    bool graded = true;                        // Rated, Finished with a target: false if the grade is unknown
    int attempts = 0;                          // Finished with a target: attempts made
    bool targetReached = false;                // Finished with a target
    bool stoppedByTimeLimit = false;           // Finished: the whole operation stopped on the time limit
};

class GeneratorObserver {
public:
    virtual ~GeneratorObserver() = default;
    virtual void onGeneratorEvent(const GeneratorEvent& event) = 0;
};

}  // namespace sudoku
