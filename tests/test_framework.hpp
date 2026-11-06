#pragma once

// A deliberately tiny test harness so the project has no external dependencies.
//
//   TEST("Category", "what it checks")  registers a test; output is grouped by category
//   CHECK(cond)                  records a failure and keeps going
//   CHECK_EQ(actual, expected)   same, printing both values on failure
//   REQUIRE(cond) / REQUIRE_EQ   same, but stops the current test (use before
//                                dereferencing something the check guards)
//   CHECK_THROWS(expr, Type)     expects `expr` to throw `Type`
//   FAIL(parts...)               records a failure with a custom message
//   CONTEXT(parts...)            RAII: attaches e.g. "grid: 16x16, seed: 7" to every
//                                failure reported while it is in scope
//   TRACE(label, parts...)       extra detail shown only under --trace; the arguments
//                                are not even evaluated otherwise
//
// Run with --help for the command-line options (verbose, trace, filtering).
// The process exits nonzero if any test fails, which is all CTest needs.

#include <algorithm>
#include <cctype>
#include <chrono>
#include <concepts>
#include <cstddef>
#include <exception>
#include <iomanip>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace mini_test {

struct TestCase {
    std::string_view category;
    std::string_view description;
    void (*fn)();
};

// One line item of a test run. Passes and infos are only recorded under --trace.
struct Event {
    enum class Kind { Pass, Fail, Info };
    Kind kind;
    std::string location;  // "test_main.cpp:72"; empty for passes and infos
    std::string message;   // may span several lines
    std::vector<std::string> context;
};

inline std::vector<TestCase>& registry() {
    static std::vector<TestCase> tests;
    return tests;
}

struct Settings {
    bool trace = false;  // record passing checks and TRACE details
    bool full = false;   // under --trace: no abbreviation and no per-test entry limit
};

inline Settings& settings() {
    static Settings s;
    return s;
}

inline bool tracing() { return settings().trace; }

// Events and context of the test that is currently running.
struct RunState {
    std::vector<Event> events;
    std::vector<std::string> context;
};

inline RunState& runState() {
    static RunState state;
    return state;
}

// Thrown by REQUIRE to end the current test; the failure is already recorded.
struct AbortTest {};

struct Registrar {
    Registrar(std::string_view category, std::string_view description, void (*fn)()) {
        registry().push_back({category, description, fn});
    }
};

template <typename... Parts>
std::string concat(const Parts&... parts) {
    std::ostringstream out;
    (out << ... << parts);
    return out.str();
}

// Shortens long single-line values in trace output unless --full was given.
inline std::string abbreviate(std::string text, std::size_t limit = 100) {
    if (settings().full || text.size() <= limit) return text;
    const std::size_t total = text.size();
    text.resize(limit);
    return concat(text, "... (", total, " chars; --full shows all)");
}

class ScopedContext {
public:
    explicit ScopedContext(std::string text) { runState().context.push_back(std::move(text)); }
    ~ScopedContext() { runState().context.pop_back(); }
    ScopedContext(const ScopedContext&) = delete;
    ScopedContext& operator=(const ScopedContext&) = delete;
};

inline std::string_view baseName(std::string_view path) {
    const auto slash = path.find_last_of("/\\");
    return slash == std::string_view::npos ? path : path.substr(slash + 1);
}

inline void reportFailure(const char* file, int line, std::string message) {
    runState().events.push_back(
        {Event::Kind::Fail, concat(baseName(file), ':', line), std::move(message), runState().context});
}

// Takes a view so a passing CHECK costs no allocation unless --trace is on.
inline void reportPass(std::string_view message) {
    if (tracing()) runState().events.push_back({Event::Kind::Pass, {}, std::string(message), runState().context});
}

// Number of failures recorded so far in the running test (lets loops stop at the first bad case).
inline std::size_t failuresSoFar() {
    return static_cast<std::size_t>(std::ranges::count_if(runState().events, [](const Event& e) {
        return e.kind == Event::Kind::Fail;
    }));
}

inline void trace(std::string_view label, std::string text) {
    runState().events.push_back({Event::Kind::Info, {}, concat(label, ": ", text), runState().context});
}

// ---- Value printing for CHECK_EQ ------------------------------------------

template <typename T>
concept HasToString = requires(const T& value) {
    { value.toString() } -> std::convertible_to<std::string>;
};

template <typename T>
concept Streamable = requires(std::ostream& out, const T& value) { out << value; };

template <typename T>
std::string display(const T& value) {
    if constexpr (std::is_same_v<T, bool>) {
        return value ? "true" : "false";
    } else if constexpr (std::is_convertible_v<const T&, std::string_view>) {
        return concat('"', std::string_view(value), '"');
    } else if constexpr (HasToString<T>) {
        return value.toString();
    } else if constexpr (requires { value.has_value(); *value; }) {
        return value ? display(*value) : std::string("(empty optional)");
    } else if constexpr (Streamable<T>) {
        return concat(value);
    } else {
        return "(unprintable value)";
    }
}

template <typename A, typename B>
bool equal(const A& a, const B& b) {
    // Compare mixed signed/unsigned integers by value, without -Wsign-compare noise.
    if constexpr (std::is_integral_v<A> && std::is_integral_v<B> && !std::is_same_v<A, bool> &&
                  !std::is_same_v<B, bool>) {
        return std::cmp_equal(a, b);
    } else {
        return a == b;
    }
}

template <typename A, typename B>
bool checkEqual(const A& actual, const B& expected, const char* actualExpr, const char* expectedExpr,
                const char* file, int line) {
    const bool ok = equal(actual, expected);
    if (ok && !tracing()) return true;  // fast path: no strings built for passing checks

    const std::string expectedText = display(expected);
    const std::string actualText = display(actual);
    const std::string head = concat("CHECK_EQ(", actualExpr, ", ", expectedExpr, ")");
    if (ok) {
        reportPass(concat(head, "\n  expected: ", abbreviate(expectedText), "\n  actual:   ", abbreviate(actualText)));
        return true;
    }
    std::string message = concat(head, "\n  expected: ", expectedText, "\n  actual:   ", actualText);
    // Long values such as grid strings are hard to compare by eye, so point at the first difference.
    if (expectedText.size() > 20 || actualText.size() > 20) {
        const auto [e, a] = std::ranges::mismatch(expectedText, actualText);
        message += concat("\n  first difference at character ", e - expectedText.begin());
    }
    reportFailure(file, line, std::move(message));
    return false;
}

// ---- Runner ---------------------------------------------------------------

struct Options {
    bool verbose = false;
    bool list = false;
    std::string filter;  // case-insensitive substring of "Category: description"
};

// Under --trace (without --full), at most this many passes/infos are shown per test.
// Failures are always shown.
inline constexpr std::size_t kTraceEntryLimit = 40;

inline std::string lower(std::string_view text) {
    std::string out(text);
    std::ranges::transform(out, out.begin(), [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return out;
}

inline std::string fullName(const TestCase& test) { return concat(test.category, ": ", test.description); }

inline std::string formatDuration(double ms) {
    std::ostringstream out;
    out << std::fixed << std::setprecision(ms < 1000 ? 1 : 2);
    if (ms < 1000) {
        out << ms << " ms";
    } else {
        out << ms / 1000 << " s";
    }
    return out.str();
}

inline void printLines(std::string_view text, std::string_view firstPrefix, std::string_view restPrefix) {
    std::istringstream lines{std::string(text)};
    std::string_view prefix = firstPrefix;
    for (std::string line; std::getline(lines, line);) {
        std::cout << prefix << line << '\n';
        prefix = restPrefix;
    }
}

inline void printFailures(const std::vector<Event>& events, std::string_view indent) {
    for (const auto& event : events) {
        if (event.kind != Event::Kind::Fail) continue;
        std::cout << indent << event.location << '\n';
        printLines(event.message, concat(indent, "  "), concat(indent, "  "));
        for (const auto& context : event.context) std::cout << indent << "  with " << context << '\n';
    }
}

// Trace view: every event in order, with the context printed whenever it changes.
inline void printTrace(const std::vector<Event>& events) {
    const std::string indent = "        ";
    const std::vector<std::string>* lastContext = nullptr;
    std::size_t shown = 0;
    std::size_t hidden = 0;
    for (const auto& event : events) {
        const bool isFail = event.kind == Event::Kind::Fail;
        if (!isFail && !settings().full && shown >= kTraceEntryLimit) {
            ++hidden;
            continue;
        }
        if (!isFail) ++shown;
        if (lastContext == nullptr || *lastContext != event.context) {
            for (const auto& context : event.context) std::cout << indent << "[" << context << "]\n";
            lastContext = &event.context;
        }
        switch (event.kind) {
            case Event::Kind::Pass: printLines(event.message, indent + "ok    ", indent + "      "); break;
            case Event::Kind::Info: printLines(event.message, indent + "info  ", indent + "      "); break;
            case Event::Kind::Fail:
                printLines(event.message, indent + "FAIL  ", indent + "      ");
                std::cout << indent << "      at " << event.location << '\n';
                break;
        }
    }
    if (hidden > 0) {
        std::cout << indent << "... " << hidden << " more entries hidden (--full shows all)\n";
    }
}

inline constexpr std::string_view kHelp = R"(usage: sudoku_tests [options] [filter]

  filter          run only tests whose "Category: description" contains this text
                  (case-insensitive), e.g. "solver:" for a category or "16x16"
  -v, --verbose   list every test with its duration, not just one line per category
  -t, --trace     like --verbose, plus every check (passing ones too), inputs,
                  boards and solver statistics; best combined with a filter
      --full      with --trace: show whole grids and every entry, unabbreviated
  -l, --list      list the tests without running them
  -h, --help      show this help

Exit code: 0 if all selected tests pass, 1 if any fail or none match, 2 on bad options.
)";

// Returns nullopt (after printing why) for --help or a bad option.
inline std::optional<Options> parseOptions(int argc, char* argv[], int& exitCode) {
    Options options;
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        if (arg == "-v" || arg == "--verbose") {
            options.verbose = true;
        } else if (arg == "-t" || arg == "--trace") {
            settings().trace = true;
        } else if (arg == "--full") {
            settings().full = true;
        } else if (arg == "-l" || arg == "--list") {
            options.list = true;
        } else if (arg == "-h" || arg == "--help") {
            std::cout << kHelp;
            exitCode = 0;
            return std::nullopt;
        } else if (arg.starts_with('-') || !options.filter.empty()) {
            std::cerr << "unexpected argument: " << arg << "\n\n" << kHelp;
            exitCode = 2;
            return std::nullopt;
        } else {
            options.filter = arg;
        }
    }
    if (settings().full && !settings().trace) {
        std::cerr << "--full only applies together with --trace\n\n" << kHelp;
        exitCode = 2;
        return std::nullopt;
    }
    if (settings().trace) options.verbose = true;  // trace output is laid out per test, like verbose
    return options;
}

struct TestResult {
    const TestCase* test;
    std::vector<Event> events;
    double ms;

    [[nodiscard]] bool passed() const {
        return std::ranges::none_of(events, [](const Event& e) { return e.kind == Event::Kind::Fail; });
    }
};

inline TestResult runOne(const TestCase& test) {
    runState() = {};
    const auto start = std::chrono::steady_clock::now();
    try {
        test.fn();
    } catch (const AbortTest&) {
        // Failure already recorded by REQUIRE.
    } catch (const std::exception& e) {
        runState().events.push_back(
            {Event::Kind::Fail, "(test body)", concat("unexpected exception: ", e.what()), runState().context});
    } catch (...) {
        runState().events.push_back(
            {Event::Kind::Fail, "(test body)", "unexpected non-standard exception", runState().context});
    }
    const std::chrono::duration<double, std::milli> elapsed = std::chrono::steady_clock::now() - start;
    return {&test, std::move(runState().events), elapsed.count()};
}

inline int runAll(int argc, char* argv[]) {
    int exitCode = 0;
    const auto options = parseOptions(argc, argv, exitCode);
    if (!options) return exitCode;

    // Select tests, keeping categories in order of first appearance.
    std::vector<std::string_view> categories;
    std::vector<const TestCase*> selected;
    const std::string filter = lower(options->filter);
    for (const auto& test : registry()) {
        if (!filter.empty() && lower(fullName(test)).find(filter) == std::string::npos) continue;
        selected.push_back(&test);
        if (std::ranges::find(categories, test.category) == categories.end()) categories.push_back(test.category);
    }
    if (selected.empty()) {
        std::cout << "No tests match \"" << options->filter << "\".\n";
        return 1;
    }
    std::size_t width = 0;
    for (const auto category : categories) width = std::max(width, category.size());

    if (options->list) {
        for (const auto* test : selected) {
            std::cout << std::left << std::setw(static_cast<int>(width)) << test->category << "  " << test->description
                      << '\n';
        }
        return 0;
    }

    std::cout << "Running " << selected.size() << (selected.size() == 1 ? " test" : " tests") << " in "
              << categories.size() << (categories.size() == 1 ? " category" : " categories") << "\n\n";
    std::vector<TestResult> failed;
    double totalMs = 0;
    for (const auto category : categories) {
        if (options->verbose) std::cout << category << '\n';
        bool firstInCategory = true;
        int passed = 0;
        int ran = 0;
        double categoryMs = 0;
        for (const auto* test : selected) {
            if (test->category != category) continue;
            TestResult result = runOne(*test);
            ++ran;
            categoryMs += result.ms;
            const bool ok = result.passed();
            if (ok) ++passed;
            if (options->verbose) {
                if (tracing() && !firstInCategory) std::cout << '\n';  // separate trace blocks
                firstInCategory = false;
                std::cout << (ok ? "  PASS  " : "  FAIL  ") << test->description << "  (" << formatDuration(result.ms)
                          << ")\n";
                if (tracing()) {
                    printTrace(result.events);
                } else {
                    printFailures(result.events, "          ");
                }
            }
            if (!ok) failed.push_back(std::move(result));
        }
        totalMs += categoryMs;
        if (options->verbose) {
            std::cout << '\n';
        } else {
            std::cout << (passed == ran ? "  PASS  " : "  FAIL  ") << std::left << std::setw(static_cast<int>(width))
                      << category << "  " << std::right << std::setw(5) << concat(passed, '/', ran) << " passed  ("
                      << formatDuration(categoryMs) << ")\n";
        }
    }

    // In concise mode the details come after the overview, so they are never lost among passing tests.
    if (!options->verbose && !failed.empty()) {
        std::cout << "\nFailures:\n";
        for (const auto& result : failed) {
            std::cout << "\n  FAIL  " << fullName(*result.test) << '\n';
            printFailures(result.events, "        ");
        }
    }

    const std::size_t failedCount = failed.size();
    if (!options->verbose) std::cout << '\n';  // verbose mode already ends each category with a blank line
    std::cout << std::string(60, '-') << '\n'
              << (failedCount == 0 ? "PASSED" : "FAILED") << "  " << (selected.size() - failedCount) << " passed, "
              << failedCount << " failed, " << selected.size() << " total  (" << formatDuration(totalMs) << ")\n";
    if (tracing() && failedCount > 0) {
        for (const auto& result : failed) std::cout << "  failed: " << fullName(*result.test) << '\n';
    }
    if (!options->verbose && failedCount == 0) std::cout << "Run with --verbose to see every test.\n";
    return failedCount == 0 ? 0 : 1;
}

}  // namespace mini_test

#define MINI_TEST_CONCAT_INNER(a, b) a##b
#define MINI_TEST_CONCAT(a, b) MINI_TEST_CONCAT_INNER(a, b)

// The function name is generated from the line number; tests are identified by their description.
#define MINI_TEST_DEFINE(fn, category, description)                                                   \
    static void fn();                                                                                 \
    static const ::mini_test::Registrar MINI_TEST_CONCAT(fn, _registrar){category, description, &fn}; \
    static void fn()

#define TEST(category, description) MINI_TEST_DEFINE(MINI_TEST_CONCAT(mini_test_case_, __LINE__), category, description)

#define CONTEXT(...) \
    const ::mini_test::ScopedContext MINI_TEST_CONCAT(context_, __LINE__) { ::mini_test::concat(__VA_ARGS__) }

#define TRACE(label, ...)                                                                    \
    do {                                                                                     \
        if (::mini_test::tracing()) ::mini_test::trace(label, ::mini_test::concat(__VA_ARGS__)); \
    } while (false)

#define FAIL(...) ::mini_test::reportFailure(__FILE__, __LINE__, ::mini_test::concat(__VA_ARGS__))

#define CHECK(cond)                                                                    \
    do {                                                                               \
        if (cond) {                                                                    \
            ::mini_test::reportPass("CHECK(" #cond ")");                               \
        } else {                                                                       \
            ::mini_test::reportFailure(__FILE__, __LINE__, "CHECK(" #cond ")");        \
        }                                                                              \
    } while (false)

#define REQUIRE(cond)                                                                       \
    do {                                                                                    \
        if (cond) {                                                                         \
            ::mini_test::reportPass("REQUIRE(" #cond ")");                                  \
        } else {                                                                            \
            ::mini_test::reportFailure(__FILE__, __LINE__, "REQUIRE(" #cond ")");           \
            throw ::mini_test::AbortTest{};                                                 \
        }                                                                                   \
    } while (false)

#define CHECK_EQ(actual, expected) \
    static_cast<void>(::mini_test::checkEqual((actual), (expected), #actual, #expected, __FILE__, __LINE__))

#define REQUIRE_EQ(actual, expected)                                                                  \
    do {                                                                                              \
        if (!::mini_test::checkEqual((actual), (expected), #actual, #expected, __FILE__, __LINE__)) { \
            throw ::mini_test::AbortTest{};                                                           \
        }                                                                                             \
    } while (false)

#define CHECK_THROWS(expr, exception_type)                                                             \
    do {                                                                                               \
        try {                                                                                          \
            static_cast<void>(expr);                                                                   \
            ::mini_test::reportFailure(__FILE__, __LINE__, #expr " did not throw " #exception_type);   \
        } catch (const exception_type& e) {                                                            \
            ::mini_test::reportPass(                                                                   \
                ::mini_test::concat("CHECK_THROWS(" #expr ")\n  threw " #exception_type ": ", e.what())); \
        } catch (const std::exception& e) {                                                            \
            ::mini_test::reportFailure(__FILE__, __LINE__,                                             \
                                       ::mini_test::concat(#expr " threw the wrong exception type (",  \
                                                           e.what(), "), expected " #exception_type)); \
        }                                                                                              \
    } while (false)
