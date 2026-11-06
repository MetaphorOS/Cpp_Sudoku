#pragma once

// Runs the command-line interface in-process, for the unit tests (not the property tests,
// which do not link the CLI library).

#include "cli.hpp"

#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace sudoku_test {

struct CliResult {
    int exitCode;
    std::string out;
    std::string err;
};

// Runs the CLI with the given arguments (without the program name) and stdin.
inline CliResult runCli(const std::vector<std::string_view>& args, const std::string& input = {}) {
    std::istringstream in(input);
    std::ostringstream out;
    std::ostringstream err;
    const int exitCode = sudoku::cli::run(args, in, out, err);
    return {exitCode, out.str(), err.str()};
}

}  // namespace sudoku_test
