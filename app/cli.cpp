// Command-line front end: argument parsing and I/O only; all logic lives in sudoku_core.

#include "cli.hpp"

#include "sudoku/auto_solver.hpp"
#include "sudoku/backtracking_solver.hpp"
#include "sudoku/board.hpp"
#include "sudoku/difficulty.hpp"
#include "sudoku/dlx_solver.hpp"
#include "sudoku/generator.hpp"
#include "sudoku/text_trace.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <new>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <optional>
#include <random>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace sudoku::cli {

namespace {

constexpr std::string_view kUsage = R"(usage: sudoku <command> [options]

commands:
  generate [--size n] [--seed s] [--pretty] [--show-solution] [--time-limit sec] [--solver name]
           [--difficulty level] [--attempts k] [--trace-generation]
      Generate a puzzle with a unique solution. n is the box size, any n >= 1
      (2 = 4x4, 3 = 9x9 default, 4 = 16x16, 10 = 100x100). The seed is echoed to stderr.
      With --time-limit, clue removal stops when time runs out: the puzzle is still
      unique but may not be minimal (a warning says so).
      With --difficulty (easy, medium, hard, expert), the puzzle is graded exactly that
      level; failed attempts retry with seed + 1, seed + 2, ... up to k attempts (default
      5000), all within --time-limit. The echoed seed is the successful attempt's. If no
      attempt hits the level, the hardest graded puzzle is printed with a warning and exit
      code 3; if time ran out before any attempt was graded, the level is "unknown". Hard
      is rare on 9x9 (about 1 attempt in 160).
  rate [puzzle|file|-] [--guess-limit k] [--time-limit sec] [--trace-logic]
      Grade a puzzle by the techniques a person needs:
        easy    naked and hidden singles
        medium  + locked candidates (pointing, claiming) and naked pairs
        hard    + X-Wing
        expert  those techniques stall; solved by controlled guessing (at most k tried
                values, default 100000; exit code 3 if that runs out)
      --time-limit stops grading (exit code 3) when time runs out.
      Prints the level, the steps per technique and the guesses. --trace-logic explains
      every deduction of the logical pass on stderr.
  solve [puzzle|file|-] [--pretty] [--stats] [--node-limit k] [--time-limit sec] [--solver name]
        [--trace-solver]
      Solve a puzzle. Reads stdin if the puzzle is omitted or '-'.
  count [puzzle|file|-] [--limit k] [--node-limit k] [--time-limit sec] [--solver name] [--trace-solver]
      Count solutions, stopping at k (default 1000; 0 = no limit).
  show [puzzle|file|-]
      Pretty-print a puzzle with its clue count and validity.

Limits: --node-limit and --time-limit stop a search early. Large or hard puzzles can take
arbitrarily long, so use them when that matters. A stopped search is reported, never
silent: exit code 3 means a limit cut the work short (or the grid is too large to
represent or allocate).

Solvers (--solver; generate uses it for its uniqueness checks):
  auto    mrv or dlx per puzzle, by box size and share of empty cells (default for solve, count)
  mrv     backtracking that branches on the cell with the fewest candidates (default for generate)
  naive   the same backtracking, branching on the next empty cell in row-major order
  dlx     Knuth's Algorithm X on Dancing Links (exact cover)

Tracing (written to stderr; stdout is unchanged):
  --trace-solver       every branch choice, placement, dead end, backtrack and solution
  --trace-generation   the filled grid, every clue-removal attempt and its verdict
  --trace-logic        every deduction of a logical solve (rate)
  --trace-limit n      print at most n trace events (default 1000; 0 = no limit)

Puzzles, in either of two formats ('.' or 0 is empty; '|', '-', '+' are ignored):
  one character per cell: 1-9 then A-Z, for grids up to 25x25 (whitespace ignored)
  numbers separated by whitespace: any grid size, e.g. "10 . 3 ..." for 36x36 and up
Pretty output can be piped back in. A puzzle argument that names a file is read from it.
)";

constexpr std::size_t kDefaultTraceLimit = 1000;

struct UsageError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

struct Streams {
    std::istream& in;
    std::ostream& out;
    std::ostream& err;
};

// Minimal flag parser: "--name value", "--flag" and at most one positional argument.
class Args {
public:
    Args(std::vector<std::string_view> args, std::vector<std::string_view> valueFlags)
        : args_(std::move(args)), valueFlags_(std::move(valueFlags)) {
        for (std::size_t i = 0; i < args_.size(); ++i) {
            const std::string_view arg = args_[i];
            if (arg.starts_with("--")) {
                if (isValueFlag(arg) && ++i >= args_.size()) throw UsageError("missing value for " + std::string(arg));
            } else if (positional_) {
                throw UsageError("unexpected argument '" + std::string(arg) + "'");
            } else {
                positional_ = arg;
            }
        }
    }

    [[nodiscard]] bool has(std::string_view flag) const {
        for (const auto arg : args_) {
            if (arg == flag) return true;
        }
        return false;
    }

    [[nodiscard]] std::optional<std::string_view> value(std::string_view flag) const {
        for (std::size_t i = 0; i + 1 < args_.size(); ++i) {
            if (args_[i] == flag) return args_[i + 1];
        }
        return std::nullopt;
    }

    [[nodiscard]] std::optional<std::string_view> positional() const { return positional_; }

    void allowOnly(const std::vector<std::string_view>& flags) const {
        for (std::size_t i = 0; i < args_.size(); ++i) {
            const std::string_view arg = args_[i];
            if (!arg.starts_with("--")) continue;
            if (std::find(flags.begin(), flags.end(), arg) == flags.end()) {
                throw UsageError("unknown option " + std::string(arg));
            }
            if (isValueFlag(arg)) ++i;
        }
    }

private:
    [[nodiscard]] bool isValueFlag(std::string_view arg) const {
        return std::find(valueFlags_.begin(), valueFlags_.end(), arg) != valueFlags_.end();
    }

    std::vector<std::string_view> args_;
    std::vector<std::string_view> valueFlags_;
    std::optional<std::string_view> positional_;
};

std::uint64_t parseUnsigned(std::string_view text, std::string_view what) {
    if (text.empty()) throw UsageError("empty value for " + std::string(what));
    constexpr std::uint64_t kMax = std::numeric_limits<std::uint64_t>::max();
    std::uint64_t value = 0;
    for (const char ch : text) {
        if (ch < '0' || ch > '9') throw UsageError("invalid number for " + std::string(what) + ": " + std::string(text));
        const auto digit = static_cast<std::uint64_t>(ch - '0');
        if (value > (kMax - digit) / 10) throw UsageError("number too large for " + std::string(what) + ": " + std::string(text));
        value = value * 10 + digit;
    }
    return value;
}

// A box size for --size; values beyond int range are reported as too large, like Board does.
int parseBoxSize(std::string_view text) {
    const std::uint64_t value = parseUnsigned(text, "--size");
    if (value > static_cast<std::uint64_t>(std::numeric_limits<int>::max())) {
        throw std::length_error("box size " + std::string(text) + " is too large");
    }
    return static_cast<int>(value);
}

// Seconds for --time-limit, e.g. "2" or "0.5", as whole milliseconds (at least 1).
std::chrono::milliseconds parseSeconds(std::string_view text, std::string_view what) {
    const std::string copy(text);
    char* end = nullptr;
    const double seconds = std::strtod(copy.c_str(), &end);
    if (copy.empty() || *end != '\0' || !(seconds > 0) || seconds > 1e9) {
        throw UsageError("invalid number of seconds for " + std::string(what) + ": " + copy);
    }
    return std::max(std::chrono::milliseconds{1},
                    std::chrono::milliseconds{static_cast<std::int64_t>(std::ceil(seconds * 1000))});
}

// --node-limit and --time-limit for solve and count.
void applySearchLimits(const Args& args, SolveOptions& options) {
    if (const auto nodes = args.value("--node-limit")) options.nodeLimit = parseUnsigned(*nodes, "--node-limit");
    if (const auto time = args.value("--time-limit")) options.timeLimit = parseSeconds(*time, "--time-limit");
}

std::string readAll(std::istream& in) { return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()}; }

// The puzzle is the positional argument: read from that file if it names one, otherwise parsed
// as puzzle text. Without an argument, or with "-", it is read from stdin.
Board readPuzzle(const Args& args, std::istream& in) {
    const auto source = args.positional();
    if (!source || *source == "-") return Board::parse(readAll(in));
    const std::filesystem::path path{std::string(*source)};
    std::error_code error;  // puzzle text is usually not a valid path; treat errors as "not a file"
    if (std::filesystem::is_regular_file(path, error)) {
        std::ifstream file(path);
        if (!file) throw std::runtime_error("cannot read " + path.string());
        return Board::parse(readAll(file));
    }
    return Board::parse(*source);
}

// --solver: one of the stateless solver instances. solve and count default to auto (MRV or DLX
// per puzzle); generate defaults to MRV, so seeded output stays the same as before auto existed.
const Solver& solverFor(const Args& args, std::string_view fallback) {
    static const AutoSolver automatic;
    static const MRVBacktrackingSolver mrv;
    static const NaiveBacktrackingSolver naive;
    static const DLXSolver dlx;
    const std::string_view name = args.value("--solver").value_or(fallback);
    if (name == "auto") return automatic;
    if (name == "mrv") return mrv;
    if (name == "naive") return naive;
    if (name == "dlx") return dlx;
    throw UsageError("unknown solver '" + std::string(name) + "' (expected auto, mrv, naive or dlx)");
}

// The solver that actually ran, e.g. "auto (dlx)" when auto delegated to DLX.
std::string solverLabel(const Solver& solver, const Board& board) {
    if (const auto* automatic = dynamic_cast<const AutoSolver*>(&solver)) {
        return "auto (" + std::string(automatic->choose(board).name()) + ")";
    }
    return std::string(solver.name());
}

// --trace-limit only makes sense together with the trace flag it limits.
std::size_t traceLimit(const Args& args, std::string_view traceFlag) {
    const auto text = args.value("--trace-limit");
    if (!text) return kDefaultTraceLimit;
    if (!args.has(traceFlag)) throw UsageError("--trace-limit requires " + std::string(traceFlag));
    return static_cast<std::size_t>(parseUnsigned(*text, "--trace-limit"));
}

void printBoard(std::ostream& out, const Board& board, bool pretty) {
    out << (pretty ? board.toPrettyString() : board.toString() + '\n');
}

Difficulty parseDifficultyFlag(std::string_view text) {
    if (const auto difficulty = parseDifficulty(text)) return *difficulty;
    throw UsageError("unknown difficulty '" + std::string(text) + "' (expected easy, medium, hard or expert)");
}

int runGenerate(const Args& args, const Streams& io) {
    args.allowOnly({"--size", "--seed", "--pretty", "--show-solution", "--time-limit", "--solver", "--difficulty",
                    "--attempts", "--trace-generation", "--trace-limit"});
    const Solver& solver = solverFor(args, "mrv");
    GeneratorOptions options;
    if (const auto size = args.value("--size")) options.boxSize = parseBoxSize(*size);
    if (const auto time = args.value("--time-limit")) options.timeLimit = parseSeconds(*time, "--time-limit");
    if (const auto level = args.value("--difficulty")) options.difficulty = parseDifficultyFlag(*level);
    if (const auto attempts = args.value("--attempts")) {
        if (!options.difficulty) throw UsageError("--attempts requires --difficulty");
        const std::uint64_t value = parseUnsigned(*attempts, "--attempts");
        if (value < 1 || value > static_cast<std::uint64_t>(std::numeric_limits<int>::max())) {
            throw UsageError("--attempts must be between 1 and " + std::to_string(std::numeric_limits<int>::max()));
        }
        options.maxAttempts = static_cast<int>(value);
    }
    if (const auto seed = args.value("--seed")) {
        options.seed = parseUnsigned(*seed, "--seed");
    } else {
        std::random_device device;
        options.seed = (std::uint64_t{device()} << 32) | device();
    }
    std::optional<TextGeneratorTracer> tracer;
    const std::size_t limit = traceLimit(args, "--trace-generation");
    if (args.has("--trace-generation")) options.observer = &tracer.emplace(io.err, limit);

    const Puzzle puzzle = Generator(solver).generate(options);
    io.err << "seed: " << puzzle.seed << ", clues: " << puzzle.puzzle.clueCount();
    if (options.difficulty) {
        io.err << ", difficulty: " << (puzzle.difficulty ? name(*puzzle.difficulty) : "unknown")
               << ", attempts: " << puzzle.attempts;
    }
    io.err << '\n';
    if (!puzzle.targetReached) {
        const std::string attempts = std::to_string(puzzle.attempts) + (puzzle.attempts == 1 ? " attempt" : " attempts");
        if (!puzzle.difficulty) {
            io.err << "warning: the time limit ran out before any attempt was fully graded (" << attempts
                   << "); the puzzle is unique but its level is unknown. Raise --time-limit\n";
        } else if (puzzle.timeLimitReached) {
            io.err << "warning: time limit reached after " << attempts << " without a puzzle graded "
                   << name(*options.difficulty) << "; printing the hardest graded one (" << name(*puzzle.difficulty)
                   << "). Raise --time-limit\n";
        } else {
            io.err << "warning: no puzzle graded " << name(*options.difficulty) << " within " << attempts
                   << "; printing the hardest one found (" << name(*puzzle.difficulty)
                   << "). Raise --attempts or --time-limit\n";
        }
    }
    const GenerationStats& stats = puzzle.stats;
    if (stats.timeLimitReached) {
        io.err << "warning: time limit reached after trying " << stats.cellsTried << " of "
               << puzzle.puzzle.cellCount() << " cells; the puzzle is unique but may not be minimal\n";
    }
    if (stats.keptUnverified > 0) {
        io.err << "note: " << stats.keptUnverified << " clues were kept because their uniqueness check hit the "
               << options.uniquenessNodeLimit << "-node limit; the puzzle is unique but may not be minimal\n";
    }
    const bool pretty = args.has("--pretty");
    printBoard(io.out, puzzle.puzzle, pretty);
    if (args.has("--show-solution")) {
        io.out << '\n';
        printBoard(io.out, puzzle.solution, pretty);
    }
    return puzzle.targetReached ? kExitOk : kExitLimit;
}

int runRate(const Args& args, const Streams& io) {
    args.allowOnly({"--guess-limit", "--time-limit", "--trace-logic", "--trace-limit"});
    const Board board = readPuzzle(args, io.in);
    RateOptions options;
    if (const auto limit = args.value("--guess-limit")) options.guessLimit = parseUnsigned(*limit, "--guess-limit");
    if (const auto time = args.value("--time-limit")) {
        options.deadline = Deadline::after(parseSeconds(*time, "--time-limit"));
    }
    std::optional<TextLogicalTracer> tracer;
    const std::size_t maxEvents = traceLimit(args, "--trace-logic");
    if (args.has("--trace-logic")) options.observer = &tracer.emplace(io.err, maxEvents);

    const Rating rating = rate(board, options);
    if (rating.status == RatingStatus::NoSolution) {
        io.err << (board.isValid() ? "no solution\n" : "invalid puzzle: conflicting givens\n");
        return kExitNoSolution;
    }
    if (rating.status == RatingStatus::TimeLimit) {
        io.err << "error: stopped at the time limit before the grade was known (" << rating.steps << " logic steps, "
               << rating.guesses << " guesses). Raise --time-limit\n";
        return kExitLimit;
    }
    io.out << "difficulty: " << name(*rating.difficulty) << '\n';
    for (const Technique t : kAllTechniques) {
        io.out << name(t) << ": " << rating.uses[static_cast<std::size_t>(t)] << '\n';
    }
    io.out << "guesses: " << rating.guesses << '\n';
    if (rating.status == RatingStatus::GuessLimit) {
        io.err << "error: guessing stopped at the guess limit (" << options.guessLimit
               << ") before a solution was found; the puzzle is expert. Raise --guess-limit\n";
        return kExitLimit;
    }
    if (rating.difficulty == Difficulty::Expert) {
        io.err << "note: steps include those made inside guessed branches; logic alone made " << rating.logicSteps
               << " steps before stalling\n";
        // Logic alone proves uniqueness; after guessing, check it separately.
        const SolveResult check = MRVBacktrackingSolver().run(
            board, {.solutionLimit = 2, .nodeLimit = 10'000'000, .timeLimit = options.deadline.remaining()});
        if (check.solutionCount > 1) {
            io.err << "warning: puzzle has more than one solution; the grade is for the first one found\n";
        } else if (check.aborted) {
            io.err << "warning: could not confirm that the solution is unique (" << describe(check.stopReason) << ")\n";
        }
    }
    return kExitOk;
}

int runSolve(const Args& args, const Streams& io) {
    args.allowOnly(
        {"--pretty", "--stats", "--node-limit", "--time-limit", "--solver", "--trace-solver", "--trace-limit"});
    const Solver& solver = solverFor(args, "auto");
    const Board board = readPuzzle(args, io.in);
    std::optional<TextSolverTracer> tracer;
    const std::size_t limit = traceLimit(args, "--trace-solver");

    // Ask for two solutions so we can warn when the puzzle is not unique.
    SolveOptions options{.solutionLimit = 2};
    applySearchLimits(args, options);
    if (args.has("--trace-solver")) options.observer = &tracer.emplace(io.err, limit);
    const auto start = std::chrono::steady_clock::now();
    const SolveResult result = solver.run(board, options);
    const auto elapsed = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start);

    if (args.has("--stats")) {
        io.err << "solver: " << solverLabel(solver, board) << ", nodes: " << result.nodes << ", backtracks: " << result.backtracks
               << ", time: " << elapsed.count() << " ms\n";
    }
    if (!result.solved()) {
        if (result.aborted) {
            io.err << "error: search stopped (" << describe(result.stopReason)
                   << ") before a solution was found; raise --node-limit or --time-limit\n";
            return kExitLimit;
        }
        io.err << (board.isValid() ? "no solution\n" : "invalid puzzle: conflicting givens\n");
        return kExitNoSolution;
    }
    if (result.solutionCount > 1) io.err << "warning: puzzle has more than one solution\n";
    if (result.aborted) {
        io.err << "warning: could not confirm that the solution is unique: the search for a second one stopped ("
               << describe(result.stopReason) << ")\n";
    }
    printBoard(io.out, *result.firstSolution, args.has("--pretty"));
    return kExitOk;
}

int runCount(const Args& args, const Streams& io) {
    args.allowOnly({"--limit", "--node-limit", "--time-limit", "--solver", "--trace-solver", "--trace-limit"});
    const Solver& solver = solverFor(args, "auto");
    const std::size_t limit = args.value("--limit") ? parseUnsigned(*args.value("--limit"), "--limit") : 1000;
    const Board board = readPuzzle(args, io.in);
    std::optional<TextSolverTracer> tracer;
    const std::size_t maxEvents = traceLimit(args, "--trace-solver");
    SolveOptions options{.solutionLimit = limit};
    applySearchLimits(args, options);
    if (args.has("--trace-solver")) options.observer = &tracer.emplace(io.err, maxEvents);
    const SolveResult result = solver.run(board, options);
    const std::size_t count = result.solutionCount;
    if (result.aborted) {  // the count is only a lower bound
        io.out << count << " (lower bound: " << describe(result.stopReason) << ")\n";
        return kExitLimit;
    }
    io.out << count << (limit != 0 && count >= limit ? " (limit reached)" : "") << '\n';
    return count > 0 ? kExitOk : kExitNoSolution;
}

int runShow(const Args& args, const Streams& io) {
    args.allowOnly({});
    const Board board = readPuzzle(args, io.in);
    io.out << board.toPrettyString() << board.size() << 'x' << board.size() << ", clues: " << board.clueCount()
           << ", " << (board.isValid() ? "valid" : "invalid (conflicting givens)") << '\n';
    return kExitOk;
}

}  // namespace

int run(const std::vector<std::string_view>& args, std::istream& in, std::ostream& out, std::ostream& err) {
    const Streams io{in, out, err};
    try {
        if (args.empty()) throw UsageError("missing command");
        const std::string_view command = args.front();
        if (command == "--help" || command == "-h" || command == "help") {
            out << kUsage;
            return kExitOk;
        }

        const Args parsed({args.begin() + 1, args.end()},
                          {"--size", "--seed", "--limit", "--trace-limit", "--node-limit", "--time-limit", "--solver",
                           "--difficulty", "--attempts", "--guess-limit"});
        if (command == "generate") return runGenerate(parsed, io);
        if (command == "rate") return runRate(parsed, io);
        if (command == "solve") return runSolve(parsed, io);
        if (command == "count") return runCount(parsed, io);
        if (command == "show") return runShow(parsed, io);
        throw UsageError("unknown command '" + std::string(command) + "'");
    } catch (const UsageError& e) {
        err << "error: " << e.what() << "\n\n" << kUsage;
        return kExitUsage;
    } catch (const std::length_error& e) {  // grid too large to index, or over the DLX memory budget
        err << "error: " << e.what() << '\n';
        return kExitLimit;
    } catch (const std::bad_alloc&) {
        err << "error: not enough memory for a grid of this size\n";
        return kExitLimit;
    } catch (const std::exception& e) {
        err << "error: " << e.what() << '\n';
        return kExitUsage;
    }
}

}  // namespace sudoku::cli
