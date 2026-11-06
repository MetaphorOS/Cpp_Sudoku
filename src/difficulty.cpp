#include "sudoku/difficulty.hpp"

#include <algorithm>
#include <vector>
#include <cctype>
#include <string>
#include <utility>

namespace sudoku {

Difficulty tierOf(Technique technique) noexcept {
    switch (technique) {
        case Technique::NakedSingle:
        case Technique::HiddenSingle: return Difficulty::Easy;
        case Technique::LockedCandidates:
        case Technique::NakedPair: return Difficulty::Medium;
        case Technique::XWing: return Difficulty::Hard;
    }
    return Difficulty::Expert;
}

Technique hardestTechnique(Difficulty difficulty) noexcept {
    switch (difficulty) {
        case Difficulty::Easy: return Technique::HiddenSingle;
        case Difficulty::Medium: return Technique::NakedPair;
        case Difficulty::Hard:
        case Difficulty::Expert: return Technique::XWing;
    }
    return Technique::XWing;
}

std::string_view name(Difficulty difficulty) noexcept {
    switch (difficulty) {
        case Difficulty::Easy: return "easy";
        case Difficulty::Medium: return "medium";
        case Difficulty::Hard: return "hard";
        case Difficulty::Expert: return "expert";
    }
    return "unknown";
}

std::optional<Difficulty> parseDifficulty(std::string_view text) {
    std::string lower(text);
    for (char& ch : lower) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    for (const Difficulty d : kAllDifficulties) {
        if (lower == name(d)) return d;
    }
    return std::nullopt;
}

std::string_view describe(RatingStatus status) noexcept {
    switch (status) {
        case RatingStatus::Solved: return "solved";
        case RatingStatus::NoSolution: return "no solution";
        case RatingStatus::GuessLimit: return "guess limit reached";
        case RatingStatus::AboveCap: return "harder than the cap";
        case RatingStatus::TimeLimit: return "stopped at the time limit";
    }
    return "unknown";
}

namespace {

enum class Outcome { Solved, Contradiction, Aborted, TimeLimit };

void addUses(Rating& rating, const LogicalResult& result) {
    for (std::size_t t = 0; t < rating.uses.size(); ++t) rating.uses[t] += result.uses[t];
    rating.steps += result.steps;
}

// The empty cell with the fewest candidates (the first one on ties).
int mostConstrainedCell(const CandidateGrid& grid) {
    int best = -1;
    int bestCount = 0;
    for (int cell = 0; cell < grid.cellCount(); ++cell) {
        if (grid.value(cell) != 0) continue;
        const int count = grid.count(cell);
        if (best < 0 || count < bestCount) {
            best = cell;
            bestCount = count;
            if (count <= 2) break;  // no cell has fewer than 2 once logic has stalled
        }
    }
    return best;
}

// Guesses on a stalled, consistent grid: each candidate of the most constrained cell is tried
// on a copy, followed by full logic and, if that stalls again, deeper guesses. Depth-first, in
// the same order a recursive search would use, but with an explicit stack: each frame holds the
// stalled grid of one branching level, so the call stack does not grow with the guess depth
// (memory still grows by one grid copy per level). On success `grid` holds the solution;
// otherwise it is left moved-from and must not be used.
Outcome guess(CandidateGrid& grid, const LogicalSolver& logic, const RateOptions& options, Rating& rating) {
    struct Frame {
        CandidateGrid grid;       // the stalled state at this level
        int cell;                 // the cell guessed at this level
        std::vector<int> values;  // its candidates, tried in ascending order
        std::size_t next = 0;     // the next one to try
    };
    std::vector<Frame> stack;
    const int firstCell = mostConstrainedCell(grid);
    std::vector<int> firstValues = grid.candidates(firstCell);
    stack.push_back(Frame{.grid = std::move(grid), .cell = firstCell, .values = std::move(firstValues)});
    rating.maxGuessDepth = 1;

    while (!stack.empty()) {
        Frame& top = stack.back();
        if (top.next == top.values.size()) {  // every candidate here led to a contradiction
            stack.pop_back();
            continue;
        }
        if (options.guessLimit != 0 && rating.guesses >= options.guessLimit) return Outcome::Aborted;
        if (options.deadline.expired()) return Outcome::TimeLimit;
        ++rating.guesses;
        CandidateGrid branch = top.grid;
        branch.place(top.cell, top.values[top.next++]);
        const LogicalResult result = logic.solve(branch);
        addUses(rating, result);
        switch (result.status) {
            case LogicalStatus::Solved: grid = std::move(branch); return Outcome::Solved;
            case LogicalStatus::TimeLimit: return Outcome::TimeLimit;
            case LogicalStatus::Contradiction: break;  // try the next candidate at this level
            case LogicalStatus::Stalled: {
                const int cell = mostConstrainedCell(branch);
                std::vector<int> values = branch.candidates(cell);
                stack.push_back(Frame{.grid = std::move(branch), .cell = cell, .values = std::move(values)});
                rating.maxGuessDepth = std::max(rating.maxGuessDepth, static_cast<int>(stack.size()));
                break;
            }
        }
    }
    return Outcome::Contradiction;
}

}  // namespace

Rating rate(const Board& board, const RateOptions& options) {
    Rating rating;
    auto grid = CandidateGrid::fromBoard(board);
    const Difficulty ceiling = options.cap.value_or(Difficulty::Expert);
    const LogicalSolver firstPass(
        {.hardest = hardestTechnique(ceiling), .observer = options.observer, .deadline = options.deadline});
    if (!grid) {
        static_cast<void>(firstPass.solve(board));  // reports the contradiction to the observer
        return rating;
    }

    const LogicalResult result = firstPass.solve(*grid);
    addUses(rating, result);
    rating.logicSteps = result.steps;
    const auto hardest = result.hardestUsed();
    rating.difficulty = hardest ? tierOf(*hardest) : Difficulty::Easy;  // reset below if no grade is known

    switch (result.status) {
        case LogicalStatus::Solved:
            rating.status = RatingStatus::Solved;
            rating.solution = grid->board();
            return rating;
        case LogicalStatus::Contradiction:
            rating.status = RatingStatus::NoSolution;
            rating.difficulty.reset();
            return rating;
        case LogicalStatus::TimeLimit:
            rating.status = RatingStatus::TimeLimit;
            rating.difficulty.reset();
            return rating;
        case LogicalStatus::Stalled: break;
    }

    if (ceiling != Difficulty::Expert) {
        rating.status = RatingStatus::AboveCap;
        rating.difficulty = static_cast<Difficulty>(static_cast<int>(ceiling) + 1);
        return rating;
    }

    rating.difficulty = Difficulty::Expert;
    const LogicalSolver logic({.hardest = hardestTechnique(Difficulty::Expert), .deadline = options.deadline});
    switch (guess(*grid, logic, options, rating)) {
        case Outcome::Solved:
            rating.status = RatingStatus::Solved;
            rating.solution = grid->board();
            break;
        case Outcome::Contradiction:
            rating.status = RatingStatus::NoSolution;
            rating.difficulty.reset();
            break;
        case Outcome::Aborted: rating.status = RatingStatus::GuessLimit; break;
        case Outcome::TimeLimit:
            rating.status = RatingStatus::TimeLimit;
            rating.difficulty.reset();
            break;
    }
    return rating;
}

}  // namespace sudoku
