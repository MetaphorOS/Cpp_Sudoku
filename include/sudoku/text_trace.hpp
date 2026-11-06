#pragma once

// Observers that print solver and generator events as readable text, one line per event.
// Cells are written as r<row>c<col>, 1-based like Sudoku notation; values as numbers.

#include "sudoku/logical_solver.hpp"
#include "sudoku/trace.hpp"

#include <cstddef>
#include <iosfwd>
#include <string>

namespace sudoku {

// Prints at most `maxEvents` event lines (0 = no limit) and counts the rest. The closing
// summary line is always printed and reports how many events were left out.
class EventBudget {
public:
    explicit EventBudget(std::size_t maxEvents) : max_(maxEvents) {}

    // True if the next event may be printed; otherwise counts it as suppressed.
    bool take() {
        if (max_ == 0 || printed_ < max_) {
            ++printed_;
            return true;
        }
        ++suppressed_;
        return false;
    }

    [[nodiscard]] std::size_t printed() const noexcept { return printed_; }
    [[nodiscard]] std::size_t suppressed() const noexcept { return suppressed_; }

private:
    std::size_t max_;
    std::size_t printed_ = 0;
    std::size_t suppressed_ = 0;
};

class TextSolverTracer final : public SolverObserver {
public:
    explicit TextSolverTracer(std::ostream& out, std::size_t maxEvents = 1000) : out_(out), budget_(maxEvents) {}
    void onSolverEvent(const SolverEvent& event) override;
    [[nodiscard]] const EventBudget& budget() const noexcept { return budget_; }

private:
    std::ostream& out_;
    EventBudget budget_;
};

class TextGeneratorTracer final : public GeneratorObserver {
public:
    explicit TextGeneratorTracer(std::ostream& out, std::size_t maxEvents = 1000) : out_(out), budget_(maxEvents) {}
    void onGeneratorEvent(const GeneratorEvent& event) override;
    [[nodiscard]] const EventBudget& budget() const noexcept { return budget_; }

private:
    std::ostream& out_;
    EventBudget budget_;
};

// Explains a logical solve: one line per deduction, e.g.
//   [logic] X-Wing on 7: rows 2,6 / columns 3,8; remove 7 from r4c3, r9c8
// then a summary line with the outcome and the steps per technique.
class TextLogicalTracer final : public LogicalObserver {
public:
    explicit TextLogicalTracer(std::ostream& out, std::size_t maxEvents = 1000) : out_(out), budget_(maxEvents) {}
    void onDeduction(const Deduction& deduction, const CandidateGrid& grid) override;
    void onFinished(const LogicalResult& result) override;
    [[nodiscard]] const EventBudget& budget() const noexcept { return budget_; }

private:
    std::ostream& out_;
    EventBudget budget_;
};

// One line explaining a deduction (without the "[logic] " prefix or newline). `size` is the
// grid's side length, used to name cells and units.
[[nodiscard]] std::string explain(const Deduction& deduction, int size);

}  // namespace sudoku
