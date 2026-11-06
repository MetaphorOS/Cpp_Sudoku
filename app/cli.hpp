#pragma once

#include <iosfwd>
#include <string_view>
#include <vector>

namespace sudoku::cli {

constexpr int kExitOk = 0;
constexpr int kExitNoSolution = 1;
constexpr int kExitUsage = 2;
// A node or time limit stopped the search before it finished, or the grid is too large to
// represent or allocate. The output, if any, says what is incomplete.
constexpr int kExitLimit = 3;

// Runs one CLI command. `args` excludes the program name. Takes the streams as
// parameters so tests can drive the CLI in-process and inspect its output.
int run(const std::vector<std::string_view>& args, std::istream& in, std::ostream& out, std::ostream& err);

}  // namespace sudoku::cli
