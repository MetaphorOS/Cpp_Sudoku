// sudoku_bench: the measurement tool of the solver study (benchmarks/README.md).
//
//   sudoku_bench make-fixture --population A|AS|B --n N --empty E --target-pct P --seed-index S --out DIR
//                             [--budget-s 600] [--verify-budget-s 600] [--check-nodes 1000000]
//       Builds one fixture, writes its files under DIR and prints "header\nrow" (its manifest row).
//
//   sudoku_bench run --puzzle FILE [--solution FILE] --population A|AS|B --solver naive|mrv|dlx
//                    --op first|unique|enumerate --time-limit-s T [--dlx-budget BYTES]
//                    [--expect-hash HEX] [--timing auto|single] [--no-internal-limit] [--header]
//       Runs one benchmark job in this process and prints one CSV row (with --header, the header
//       line first). The orchestrator (scripts/run_campaign.py) adds tier, job and concurrency.
//
//   sudoku_bench env
//       Prints build information as key=value lines.
//
// Exit codes: 0 = a row was printed (whatever the run's status), 2 = usage or I/O error.

#include "common.hpp"
#include "fixtures.hpp"

#include "sudoku/backtracking_solver.hpp"
#include "sudoku/dlx_solver.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <map>
#include <new>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#ifdef SUDOKU_SOLVER_COUNTERS
// Allocation counting, in the counters build only (it is not free).
namespace {
std::atomic<std::uint64_t> gAllocations{0};
std::atomic<std::uint64_t> gAllocatedBytes{0};
}  // namespace
void* operator new(std::size_t size) {
    gAllocations.fetch_add(1, std::memory_order_relaxed);
    gAllocatedBytes.fetch_add(size, std::memory_order_relaxed);
    if (void* p = std::malloc(size == 0 ? 1 : size)) return p;
    throw std::bad_alloc();
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
#endif

namespace {

using namespace bench;
using Clock = std::chrono::steady_clock;
using sudoku::Board;
using sudoku::SolveResult;

struct UsageError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

class Args {
public:
    Args(int argc, char* argv[], int first) {
        for (int i = first; i < argc; ++i) {
            const std::string arg = argv[i];
            if (!arg.starts_with("--")) throw UsageError("unexpected argument " + arg);
            const bool isFlag = i + 1 >= argc || std::string_view(argv[i + 1]).starts_with("--");
            values_[arg] = isFlag ? "" : argv[++i];
        }
    }
    [[nodiscard]] bool has(const std::string& name) const { return values_.contains(name); }
    [[nodiscard]] std::string get(const std::string& name) const {
        const auto it = values_.find(name);
        if (it == values_.end() || it->second.empty()) throw UsageError("missing " + name);
        return it->second;
    }
    [[nodiscard]] std::string get(const std::string& name, const std::string& fallback) const {
        return has(name) ? get(name) : fallback;
    }

private:
    std::map<std::string, std::string> values_;
};

std::uint64_t toUnsigned(const std::string& text) {
    std::size_t used = 0;
    const unsigned long long value = std::stoull(text, &used);
    if (used != text.size()) throw UsageError("not a number: " + text);
    return value;
}

// ---- run ----

const sudoku::Solver& solverNamed(const std::string& name, std::optional<sudoku::DLXSolver>& dlx, std::size_t budget) {
    static const sudoku::NaiveBacktrackingSolver naive;
    static const sudoku::MRVBacktrackingSolver mrv;
    if (name == "naive") return naive;
    if (name == "mrv") return mrv;
    if (name == "dlx") return dlx.emplace(budget);
    throw UsageError("unknown solver " + name);
}

double nanos(Clock::duration d) { return static_cast<double>(std::chrono::duration_cast<std::chrono::nanoseconds>(d).count()); }

double quantile(std::vector<double> values, double q) {
    std::sort(values.begin(), values.end());
    if (values.empty()) return 0;
    const double pos = q * static_cast<double>(values.size() - 1);
    const auto lo = static_cast<std::size_t>(pos);
    const std::size_t hi = std::min(lo + 1, values.size() - 1);
    return values[lo] + (pos - static_cast<double>(lo)) * (values[hi] - values[lo]);
}

int runCommand(const Args& args) {
    const std::string puzzlePath = args.get("--puzzle");
    const std::string population = args.get("--population");
    const std::string solverName = args.get("--solver");
    const std::string op = args.get("--op");
    const double budgetSeconds = std::stod(args.get("--time-limit-s"));
    const std::size_t dlxBudget = args.has("--dlx-budget") ? static_cast<std::size_t>(toUnsigned(args.get("--dlx-budget")))
                                                           : sudoku::DLXSolver::kDefaultMemoryBudget;
    const std::string timingMode = args.get("--timing", "auto");

    std::size_t limit = 1;
    if (op == "unique") {
        limit = 2;
    } else if (op == "enumerate") {
        limit = 0;
    } else if (op != "first") {
        throw UsageError("unknown op " + op);
    }

    Row row;
    const std::string text = readFile(puzzlePath);
    const std::string hash = hex(fnv1a(text));
    const Board puzzle = Board::parse(text);
    std::optional<Board> storedSolution;
    if (args.has("--solution")) storedSolution = Board::parse(readFile(args.get("--solution")));
    std::optional<sudoku::DLXSolver> dlx;
    const sudoku::Solver& solver = solverNamed(solverName, dlx, dlxBudget);

    const auto budget = std::chrono::nanoseconds(static_cast<std::int64_t>(budgetSeconds * 1e9));
    sudoku::SolveOptions options{.solutionLimit = limit};
    if (!args.has("--no-internal-limit")) {
        options.timeLimit = std::chrono::ceil<std::chrono::milliseconds>(budget);
    }

    std::string status;
    std::string message;
    if (args.has("--expect-hash") && args.get("--expect-hash") != hash) {
        status = "error";
        message = "fixture hash mismatch: expected " + args.get("--expect-hash") + " got " + hash;
    }

    const MemorySample before = sampleMemory();
#ifdef SUDOKU_SOLVER_COUNTERS
    gAllocations = 0;
    gAllocatedBytes = 0;
#endif
    SolveResult result;
    double coldNs = 0;
    if (status.empty()) {
        const auto start = Clock::now();
        try {
            result = solver.run(puzzle, options);
            coldNs = nanos(Clock::now() - start);
        } catch (const std::length_error& e) {
            coldNs = nanos(Clock::now() - start);
            status = "memory_rejected";
            message = e.what();
        } catch (const std::bad_alloc&) {
            coldNs = nanos(Clock::now() - start);
            status = "error";
            message = "std::bad_alloc";
        }
    }
    const MemorySample after = sampleMemory();
#ifdef SUDOKU_SOLVER_COUNTERS
    const std::uint64_t allocations = gAllocations;
    const std::uint64_t allocatedBytes = gAllocatedBytes;
#else
    const std::uint64_t allocations = 0;
    const std::uint64_t allocatedBytes = 0;
#endif

    if (status.empty()) {
        if (result.stopReason == sudoku::StopReason::InvalidGivens) {
            status = "error";
            message = "fixture has conflicting givens";
        } else if (result.aborted) {
            status = "timeout";
        } else if (coldNs > static_cast<double>(budget.count())) {
            status = "timeout";
            message = "finished after the budget (setup counted)";
        } else {
            status = "completed";
        }
    }

    // Validate every completed result independently.
    std::string solutionValid = "n/a";
    if (status == "completed") {
        std::string problem;
        if (result.solutionCount == 0) {
            problem = "no solution reported for a puzzle that has one";
        } else if (!result.firstSolution) {
            problem = "solution count without a solution";
        } else {
            problem = solutionProblem(*result.firstSolution, puzzle);
        }
        if (problem.empty() && population == "B") {
            if (op != "first" && result.solutionCount != 1) problem = "unique puzzle reported with several solutions";
            if (storedSolution && result.firstSolution != storedSolution) problem = "solution differs from the stored one";
        }
        solutionValid = problem.empty() ? "yes" : "no";
        if (!problem.empty()) {
            status = "error";
            message = "invalid result: " + problem;
        }
    }

    // Repetitions for precise timing (completed runs only).
    std::string timingKind = "single";
    std::vector<double> samples{coldNs};
    std::uint64_t batchSize = 1;
    double batchMinMs = 0;
    std::size_t invocations = 1;
    if (status == "completed" && timingMode == "auto") {
        const auto rerun = [&] {
            const SolveResult again = solver.run(puzzle, options);
            if (again.solutionCount != result.solutionCount || again.nodes != result.nodes) {
                throw std::logic_error("repeated run differs from the first run");
            }
        };
        try {
            if (coldNs < 10e6) {  // under 10 ms: batches of at least 50 ms each
                timingKind = "batch";
                const auto timeBatch = [&](std::uint64_t b) {
                    const auto start = Clock::now();
                    for (std::uint64_t i = 0; i < b; ++i) rerun();
                    return nanos(Clock::now() - start);
                };
                while (timeBatch(batchSize) < 50e6) batchSize *= 2;  // calibration batches are not kept
                samples.clear();
                batchMinMs = 1e300;
                for (int b = 0; b < 15; ++b) {
                    const double total = timeBatch(batchSize);
                    batchMinMs = std::min(batchMinMs, total / 1e6);
                    samples.push_back(total / static_cast<double>(batchSize));
                }
                invocations = 15 * batchSize;
            } else if (coldNs <= 10e9) {  // 10 ms - 10 s: up to 5 runs or 30 s
                timingKind = "repeat";
                samples.clear();
                double spent = 0;
                while (samples.size() < 5 && spent < 30e9) {
                    const auto start = Clock::now();
                    rerun();
                    samples.push_back(nanos(Clock::now() - start));
                    spent += samples.back();
                }
                invocations = samples.size();
            }
        } catch (const std::logic_error& e) {
            status = "error";
            message = e.what();
        }
    }

    const auto& c = result.counters;
    row.add("status", status);
    row.add("stop_reason", status == "memory_rejected" ? std::string("-") : std::string(sudoku::describe(result.stopReason)));
    row.add("solutions", static_cast<std::uint64_t>(result.solutionCount));
    row.add("nodes", result.nodes);
    row.add("backtracks", result.backtracks);
    row.add("max_depth", result.maxDepth);
    row.add("nodes_at_first", result.nodesAtFirstSolution);
    row.add("time_to_first_ns", static_cast<std::int64_t>(result.timeToFirstSolution.count()));
    row.add("setup_ns", static_cast<std::int64_t>(result.setupTime.count()));
    row.add("search_ns", static_cast<std::int64_t>(result.searchTime.count()));
    row.add("cold_total_ns", coldNs);
    row.add("total_ns", quantile(samples, 0.5));
    row.add("total_ns_q1", quantile(samples, 0.25));
    row.add("total_ns_q3", quantile(samples, 0.75));
    row.add("total_ns_min", *std::min_element(samples.begin(), samples.end()));
    row.add("total_ns_max", *std::max_element(samples.begin(), samples.end()));
    row.add("timing_kind", timingKind);
    row.add("timing_samples", static_cast<std::uint64_t>(samples.size()));
    row.add("batch_size", batchSize);
    row.add("timed_invocations", static_cast<std::uint64_t>(invocations));
    row.add("batch_min_ms", batchMinMs);
    row.add("mem_baseline_ws", before.workingSet);
    row.add("mem_peak_ws_delta", after.peakWorkingSet > before.workingSet ? after.peakWorkingSet - before.workingSet : 0);
    row.add("mem_peak_commit_delta", after.peakCommit > before.commit ? after.peakCommit - before.commit : 0);
    const auto estimate = sudoku::DLXSolver::estimate(puzzle);
    row.add("dlx_est_bytes", estimate ? estimate->bytes : 0);
    row.add("fixture_hash", hash);
    row.add("solution_valid", solutionValid);
    row.add("cnt_candidates", c.candidateComputations);
    row.add("cnt_cells_scanned", c.cellsScanned);
    row.add("cnt_placements", c.placements);
    row.add("cnt_columns_inspected", c.columnsInspected);
    row.add("cnt_covers", c.covers);
    row.add("cnt_uncovers", c.uncovers);
    row.add("cnt_rows_examined", c.rowsExamined);
    row.add("cnt_link_updates", c.linkUpdates);
    row.add("alloc_count", allocations);
    row.add("alloc_bytes", allocatedBytes);
    row.add("counters_build", sudoku::kSolverCounters ? "1" : "0");
    row.add("message", message);

    if (args.has("--header")) std::cout << row.header() << '\n';
    std::cout << row.values() << '\n';
    return 0;
}

int makeFixtureCommand(const Args& args) {
    FixtureSpec spec;
    spec.population = args.get("--population");
    spec.n = static_cast<int>(toUnsigned(args.get("--n")));
    spec.empty = static_cast<int>(toUnsigned(args.get("--empty")));
    spec.targetPct = static_cast<int>(toUnsigned(args.get("--target-pct", "0")));
    spec.seedIndex = static_cast<int>(toUnsigned(args.get("--seed-index", "1")));
    CarveOptions options;
    options.budget = std::chrono::milliseconds(static_cast<std::int64_t>(std::stod(args.get("--budget-s", "600")) * 1000));
    options.verifyBudget =
        std::chrono::milliseconds(static_cast<std::int64_t>(std::stod(args.get("--verify-budget-s", "600")) * 1000));
    options.checkNodes = toUnsigned(args.get("--check-nodes", "1000000"));
    const Row row = makeFixture(spec, args.get("--out"), options);
    std::cout << row.header() << '\n' << row.values() << '\n';
    return 0;
}

int envCommand() {
    std::cout << "compiler=" << __VERSION__ << '\n';
#ifdef SUDOKU_BENCH_BUILD_TYPE
    std::cout << "build_type=" << SUDOKU_BENCH_BUILD_TYPE << '\n';
#endif
#ifdef SUDOKU_BENCH_CXX_FLAGS
    std::cout << "cxx_flags=" << SUDOKU_BENCH_CXX_FLAGS << '\n';
#endif
    std::cout << "solver_counters=" << (sudoku::kSolverCounters ? 1 : 0) << '\n';
    std::cout << "dlx_validate=" << (sudoku::DLXSolver::kValidates ? 1 : 0) << '\n';
    std::cout << "dlx_default_budget=" << sudoku::DLXSolver::kDefaultMemoryBudget << '\n';
    return 0;
}

}  // namespace

int main(int argc, char* argv[]) {
    try {
        if (argc < 2) throw UsageError("missing command (make-fixture | run | env)");
        const std::string command = argv[1];
        const Args args(argc, argv, 2);
        if (command == "run") return runCommand(args);
        if (command == "make-fixture") return makeFixtureCommand(args);
        if (command == "env") return envCommand();
        throw UsageError("unknown command " + command);
    } catch (const std::exception& e) {
        std::cerr << "sudoku_bench: " << e.what() << '\n';
        return 2;
    }
}
