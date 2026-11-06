#pragma once

// Helpers shared by the sudoku_bench subcommands: hashing, seeds, memory sampling, CSV output
// and an independent solution checker.

#include "sudoku/board.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace bench {

// FNV-1a, 64 bit: content hashes of fixture files and seed derivation.
std::uint64_t fnv1a(std::string_view text);
// One splitmix64 step: a well-mixed 64-bit value from any input.
std::uint64_t splitmix64(std::uint64_t x);
std::string hex(std::uint64_t value);

// Process memory, in bytes (zeros on platforms without an implementation).
struct MemorySample {
    std::uint64_t workingSet = 0;
    std::uint64_t peakWorkingSet = 0;
    std::uint64_t commit = 0;
    std::uint64_t peakCommit = 0;
};
MemorySample sampleMemory();

// Checks a returned solution without using the solvers' Constraints code: complete, every row,
// column and box a permutation of 1..N, every given kept. Empty string if correct.
std::string solutionProblem(const sudoku::Board& solution, const sudoku::Board& puzzle);

std::string readFile(const std::string& path);
void writeFile(const std::string& path, const std::string& text);

// One CSV row of named fields, kept in order. Values are sanitized (no commas or newlines).
class Row {
public:
    void add(std::string name, std::string value);
    void add(std::string name, std::uint64_t value) { add(std::move(name), std::to_string(value)); }
    void add(std::string name, std::int64_t value) { add(std::move(name), std::to_string(value)); }
    void add(std::string name, int value) { add(std::move(name), std::to_string(value)); }
    void add(std::string name, double value);
    [[nodiscard]] std::string header() const;
    [[nodiscard]] std::string values() const;

private:
    std::vector<std::pair<std::string, std::string>> fields_;
};

}  // namespace bench
