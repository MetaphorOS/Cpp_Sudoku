#include "sudoku/text_trace.hpp"

#include <iomanip>
#include <ostream>
#include <sstream>
#include <string>
#include <vector>

namespace sudoku {

namespace {

std::string cellName(int row, int col) { return "r" + std::to_string(row + 1) + "c" + std::to_string(col + 1); }

std::string candidateList(const std::vector<int>& candidates) {
    std::string out = "{";
    for (const int value : candidates) {
        if (out.size() > 1) out += ',';
        out += std::to_string(value);
    }
    return out + '}';
}

std::string plural(std::uint64_t count, const char* singular, const char* pluralForm) {
    return std::to_string(count) + ' ' + (count == 1 ? singular : pluralForm);
}

void printIndented(std::ostream& out, const std::string& text) {
    std::istringstream lines(text);
    for (std::string line; std::getline(lines, line);) out << "    " << line << '\n';
}

void printSuppressed(std::ostream& out, const EventBudget& budget, const char* prefix) {
    if (budget.suppressed() > 0) {
        out << prefix << " ... " << budget.suppressed() << " more events not shown (raise --trace-limit, 0 = no limit)\n";
    }
}

}  // namespace

void TextSolverTracer::onSolverEvent(const SolverEvent& e) {
    using Kind = SolverEvent::Kind;

    if (e.kind == Kind::Finish) {  // the summary is never suppressed
        printSuppressed(out_, budget_, "[solve]");
        out_ << "[solve] finished: ";
        switch (e.reason) {
            case StopReason::SolutionLimit: out_ << "stopped at the solution limit (" << e.solutionLimit << ")"; break;
            case StopReason::NodeLimit: out_ << "stopped at the node limit; the count is a lower bound"; break;
            case StopReason::TimeLimit: out_ << "stopped at the time limit; the count is a lower bound"; break;
            case StopReason::Exhausted: out_ << "search space exhausted, so the count is exact"; break;
            case StopReason::InvalidGivens: out_ << "the givens conflict, so there is nothing to search"; break;
        }
        out_ << "; " << plural(e.solutions, "solution", "solutions") << ", "
             << plural(e.nodes, "search node", "search nodes") << ", " << plural(e.backtracks, "backtrack", "backtracks")
             << '\n';
        return;
    }
    if (!budget_.take()) return;

    if (e.kind == Kind::Start) {
        out_ << "[solve] start: " << e.board->size() << 'x' << e.board->size() << ", "
             << plural(static_cast<std::uint64_t>(e.board->clueCount()), "given", "givens") << ", "
             << plural(static_cast<std::uint64_t>(e.open), "empty cell", "empty cells") << '\n';
        return;
    }

    out_ << "[d" << std::setw(3) << std::left << e.depth << std::right << "] ";
    switch (e.kind) {
        case Kind::Select: {
            if (e.selection == SolverEvent::Selection::Constraint) {
                out_ << "choose " << e.constraint << ": " << plural(static_cast<std::uint64_t>(e.options), "option", "options")
                     << (e.options == 1 ? " (forced)" : "");
                break;
            }
            const bool forced = e.candidates.size() == 1;
            out_ << "select " << cellName(e.row, e.col) << ": candidates " << candidateList(e.candidates) << " ("
                 << (forced ? "forced; " : "");
            if (e.selection == SolverEvent::Selection::FixedOrder) {
                out_ << "next empty cell in fixed row-major order)";
            } else {
                out_ << "MRV scanned " << e.scanned << " of " << e.open << " open cells"
                     << (e.scanned < e.open ? ", stopped early" : "") << ')';
            }
            break;
        }
        case Kind::Place: out_ << "try    " << cellName(e.row, e.col) << " = " << e.value; break;
        case Kind::Backtrack:
            out_ << "undo   " << cellName(e.row, e.col) << " = " << e.value << " (backtrack #" << e.backtracks << ')';
            break;
        case Kind::DeadEnd:
            if (e.constraint.empty()) {
                out_ << "dead   " << cellName(e.row, e.col) << ": no candidates left";
            } else {
                out_ << "dead   " << e.constraint << ": no placement left can satisfy it";
            }
            break;
        case Kind::Solution:
            out_ << "solution #" << e.solutions << " found after " << plural(e.nodes, "node", "nodes") << ", "
                 << plural(e.backtracks, "backtrack", "backtracks");
            break;
        case Kind::Start:
        case Kind::Finish: break;  // handled above
    }
    out_ << '\n';
}

void TextGeneratorTracer::onGeneratorEvent(const GeneratorEvent& e) {
    using Kind = GeneratorEvent::Kind;
    using Verdict = GeneratorEvent::Verdict;

    if (e.kind == Kind::Finished) {  // the summary is never suppressed
        printSuppressed(out_, budget_, "[generate]");
        const GenerationStats& s = *e.stats;
        out_ << "[generate] finished: " << plural(static_cast<std::uint64_t>(e.clues), "clue", "clues") << " left; ";
        if (e.target) {
            out_ << (s.removedForced + s.removedByLogic + s.removedBySearch) << " removed (" << s.removedForced
                 << " forced, " << s.removedByLogic << " solved by logic, " << s.removedBySearch
                 << " checked by search), "
                 << (s.keptTooHard + s.keptAmbiguous + s.keptUnverified) << " kept (" << s.keptTooHard
                 << " too hard, " << s.keptAmbiguous << " would allow a second solution, " << s.keptUnverified
                 << " uniqueness checks hit the node limit)\n";
            out_ << "[generate] difficulty: " << (e.graded ? name(e.difficulty) : "unknown") << " (target "
                 << name(*e.target) << ", " << plural(static_cast<std::uint64_t>(e.attempts), "attempt", "attempts")
                 << ", seed " << e.seed;
            if (!e.graded) {
                out_ << "; the time limit ran out before any attempt was graded";
            } else if (!e.targetReached) {
                out_ << (e.stoppedByTimeLimit ? "; time limit reached, returning the hardest graded puzzle"
                                              : "; target not reached, returning the hardest puzzle found");
            }
            out_ << ")\n";
        } else {
            out_ << (s.removedBySearch + s.removedForced) << " removed (" << s.removedForced << " forced, "
                 << s.removedBySearch << " checked by search), " << (s.keptAmbiguous + s.keptUnverified)
                 << " kept (" << s.keptAmbiguous << " would allow a second solution, " << s.keptUnverified
                 << " uniqueness checks hit the node limit)\n";
        }
        if (e.timeLimitReached) {
            out_ << "[generate] note: the time limit ran out before every cell was tried; the puzzle is unique but "
                    "may not be minimal\n";
        }
        out_ << "  puzzle:\n";
        printIndented(out_, e.board->toPrettyString());
        out_ << "  solution:\n";
        printIndented(out_, e.solution->toPrettyString());
        return;
    }
    if (!budget_.take()) return;

    if (e.kind == Kind::FilledGrid) {
        out_ << "[generate] seed " << e.seed << ", " << e.board->size() << 'x' << e.board->size()
             << ": random complete grid (base pattern + random symbol, row and column permutations):\n";
        printIndented(out_, e.board->toPrettyString());
        return;
    }

    if (e.kind == Kind::Rated) {
        out_ << "[generate] attempt " << e.index << '/' << e.total << " (seed " << e.seed << "): "
             << plural(static_cast<std::uint64_t>(e.clues), "clue", "clues");
        if (!e.graded) {
            out_ << ", grade unknown (time limit)\n";
        } else {
            out_ << ", graded " << name(e.difficulty)
                 << (e.difficulty == *e.target ? ", target reached" : ", target " + std::string(name(*e.target)) + " missed")
                 << '\n';
        }
        return;
    }

    out_ << "[generate] removal " << e.index << '/' << e.total << ": " << cellName(e.row, e.col) << " = " << e.value
         << " -> ";
    switch (e.verdict) {
        case Verdict::Accepted: out_ << "removed, puzzle still unique"; break;
        case Verdict::Forced: out_ << "removed, the other clues still force it (no search needed)"; break;
        case Verdict::MultipleSolutions: out_ << "kept, removing it allows a second solution"; break;
        case Verdict::CheckAborted: out_ << "kept, uniqueness check hit its " << e.nodeLimit << "-node limit"; break;
        case Verdict::TimeLimit: out_ << "kept, the generation time limit ran out during the check"; break;
        case Verdict::SolvedByLogic:
            out_ << "removed, " << name(*e.target) << " techniques still solve it (so it is unique)";
            break;
        case Verdict::TooHard: out_ << "kept, removing it needs techniques beyond " << name(*e.target); break;
    }
    const bool searched = e.verdict != Verdict::Forced && e.verdict != Verdict::SolvedByLogic &&
                          e.verdict != Verdict::TooHard;
    if (searched) out_ << " (check: " << plural(e.nodes, "node", "nodes") << ')';
    out_ << "; " << e.clues << " clues\n";
}

namespace {

std::string cellOf(int cell, int size) { return cellName(cell / size, cell % size); }

// "rows 2,6" / "columns 3,8": the line numbers of two units of the same kind.
std::string linePair(int a, int b, int size) {
    const bool rows = unitKind(a, size) == UnitKind::Row;
    const int base = rows ? 0 : size;
    return std::string(rows ? "rows " : "columns ") + std::to_string(a - base + 1) + ',' + std::to_string(b - base + 1);
}

std::string eliminationList(const std::vector<CellValue>& eliminations, int size, bool sameValue) {
    std::string out;
    for (const auto& [cell, value] : eliminations) {
        if (!out.empty()) out += ", ";
        if (!sameValue) out += std::to_string(value) + " from ";
        out += cellOf(cell, size);
    }
    return out;
}

}  // namespace

std::string explain(const Deduction& d, int size) {
    std::string out(name(d.technique));
    switch (d.technique) {
        case Technique::NakedSingle:
            out += ": " + cellOf(d.placement->cell, size) + " = " + std::to_string(d.placement->value) +
                   " (its only candidate)";
            return out;
        case Technique::HiddenSingle:
            out += ": " + cellOf(d.placement->cell, size) + " = " + std::to_string(d.placement->value) +
                   " (the only place for " + std::to_string(d.placement->value) + " in " + unitName(d.units[0], size) +
                   ')';
            return out;
        case Technique::LockedCandidates: {
            const bool pointing = unitKind(d.units[0], size) == UnitKind::Box;
            out += pointing ? " (pointing): " : " (claiming): ";
            out += std::to_string(d.values[0]) + " in " + unitName(d.units[0], size) + " is confined to " +
                   unitName(d.units[1], size) + "; remove " + std::to_string(d.values[0]) + " from " +
                   eliminationList(d.eliminations, size, true);
            return out;
        }
        case Technique::NakedPair: {
            out += " " + candidateList(d.values) + " at " + cellOf(d.cells[0], size) + ", " + cellOf(d.cells[1], size) +
                   " in " + unitName(d.units[0], size) + "; remove " + eliminationList(d.eliminations, size, false);
            return out;
        }
        case Technique::XWing:
            out += " on " + std::to_string(d.values[0]) + ": " + linePair(d.units[0], d.units[1], size) + " / " +
                   linePair(d.units[2], d.units[3], size) + "; remove " + std::to_string(d.values[0]) + " from " +
                   eliminationList(d.eliminations, size, true);
            return out;
    }
    return out;
}

void TextLogicalTracer::onDeduction(const Deduction& deduction, const CandidateGrid& grid) {
    if (!budget_.take()) return;
    out_ << "[logic] " << explain(deduction, grid.size()) << '\n';
}

void TextLogicalTracer::onFinished(const LogicalResult& result) {
    printSuppressed(out_, budget_, "[logic]");
    out_ << "[logic] finished: " << describe(result.status) << " after "
         << plural(static_cast<std::uint64_t>(result.steps), "step", "steps");
    std::string uses;
    for (const Technique t : kAllTechniques) {
        const int n = result.uses[static_cast<std::size_t>(t)];
        if (n == 0) continue;
        if (!uses.empty()) uses += ", ";
        uses += std::string(name(t)) + ' ' + std::to_string(n);
    }
    if (!uses.empty()) out_ << " (" << uses << ')';
    out_ << '\n';
}

}  // namespace sudoku
