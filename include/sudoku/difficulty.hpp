#pragma once

// Human-aware difficulty: a puzzle is graded by the hardest technique a person needs, not by
// its clue count.
//
//   Easy    naked singles and hidden singles
//   Medium  Easy + locked candidates and naked pairs
//   Hard    Medium + X-Wing
//   Expert  those techniques stall, so the solve needs controlled backtracking (guessing)
//
// The grade is the lowest sufficient tier. LogicalSolver always applies the easiest technique
// available, and the techniques only ever remove candidates, so the set of deductions a tier
// can reach on its own is fixed. A harder technique is used only once every easier one has
// stalled, so the tier of the hardest technique used is exactly the lowest tier that solves
// the puzzle.

#include "sudoku/board.hpp"
#include "sudoku/deadline.hpp"
#include "sudoku/logical_solver.hpp"

#include <array>
#include <cstdint>
#include <optional>
#include <string_view>

namespace sudoku {

enum class Difficulty { Easy, Medium, Hard, Expert };
inline constexpr int kDifficultyCount = 4;
inline constexpr std::array<Difficulty, kDifficultyCount> kAllDifficulties{
    Difficulty::Easy, Difficulty::Medium, Difficulty::Hard, Difficulty::Expert};

// The tier a technique belongs to.
[[nodiscard]] Difficulty tierOf(Technique technique) noexcept;
// The hardest technique a tier allows (Expert allows every technique, plus guessing).
[[nodiscard]] Technique hardestTechnique(Difficulty difficulty) noexcept;
// "easy", "medium", "hard", "expert".
[[nodiscard]] std::string_view name(Difficulty difficulty) noexcept;
// Case-insensitive inverse of name(); empty for anything else.
[[nodiscard]] std::optional<Difficulty> parseDifficulty(std::string_view text);
// The hardest grade any uniquely solvable puzzle of this box size can have. For box size
// <= 2 this is Easy: an exhaustive check graded all 13 579 680 uniquely solvable 4x4
// puzzles, and singles solve every one. Larger grids can reach every grade.
[[nodiscard]] constexpr Difficulty hardestPossible(int boxSize) noexcept {
    return boxSize <= 2 ? Difficulty::Easy : Difficulty::Expert;
}

enum class RatingStatus {
    Solved,      // a solution was found (by logic, or by logic plus guessing for Expert)
    NoSolution,  // the givens conflict, or every branch ended in a contradiction
    GuessLimit,  // logic stalled and guessing hit RateOptions::guessLimit: Expert, unsolved
    AboveCap,    // RateOptions::cap was set and its techniques stalled: harder than the cap
    TimeLimit,   // RateOptions::deadline expired before a grade was known
};
[[nodiscard]] std::string_view describe(RatingStatus status) noexcept;

struct RateOptions {
    // Most candidate values tried in total while guessing (0 = no limit).
    std::uint64_t guessLimit = 100'000;
    // If set, only this tier's techniques are tried and nothing is guessed: a puzzle they
    // cannot finish is reported as AboveCap. Cheaper when only "is it at most T?" matters.
    std::optional<Difficulty> cap{};
    // Sees the deductions of the first logic pass, before any guess (not those inside
    // guessed branches) and its summary.
    LogicalObserver* observer = nullptr;
    // Checked before each logic step and before each guess. Once expired, rate() returns
    // TimeLimit with no grade. Never expires by default.
    Deadline deadline{};
};

struct Rating {
    RatingStatus status = RatingStatus::NoSolution;
    // The lowest sufficient tier, when it is known:
    //  - Solved: exact;
    //  - GuessLimit: Expert (every technique stalled; no solution found within the limit);
    //  - AboveCap: the tier above the cap, a lower bound;
    //  - NoSolution, TimeLimit: empty.
    std::optional<Difficulty> difficulty{};
    std::optional<Board> solution;            // Solved only
    std::array<int, kTechniqueCount> uses{};  // logic steps per technique, guessed branches included
    int steps = 0;                            // all logic steps, guessed branches included
    int logicSteps = 0;                       // steps of the first pass, before any guess
    std::uint64_t guesses = 0;                // candidate values tried while guessing
    // Deepest level of nested guesses (0 if none). Guessing keeps one grid copy per level, so
    // its memory is about maxGuessDepth x (cells x words x 8 bytes + the board).
    int maxGuessDepth = 0;

    [[nodiscard]] bool solved() const noexcept { return status == RatingStatus::Solved; }
};

// Grades `board`. Pure logic first; if it stalls, controlled backtracking: pick the empty cell
// with the fewest candidates, try each candidate on a copy of the grid and run full logic
// again, depth-first (an explicit stack, not recursion), until one branch solves. Stops at the first solution found; it does not
// check uniqueness (a puzzle solved by logic alone is unique, one that needed guessing may not be).
[[nodiscard]] Rating rate(const Board& board, const RateOptions& options = {});

}  // namespace sudoku
