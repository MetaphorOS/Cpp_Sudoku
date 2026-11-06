#include "common.hpp"

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <sstream>
#include <stdexcept>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
// psapi.h needs windows.h first.
#include <psapi.h>
#endif

namespace bench {

std::uint64_t fnv1a(std::string_view text) {
    std::uint64_t hash = 0xcbf29ce484222325ULL;
    for (const char ch : text) {
        hash ^= static_cast<unsigned char>(ch);
        hash *= 0x100000001b3ULL;
    }
    return hash;
}

std::uint64_t splitmix64(std::uint64_t x) {
    x += 0x9e3779b97f4a7c15ULL;
    x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ULL;
    x = (x ^ (x >> 27)) * 0x94d049bb133111ebULL;
    return x ^ (x >> 31);
}

std::string hex(std::uint64_t value) {
    char buffer[17];
    std::snprintf(buffer, sizeof buffer, "%016llx", static_cast<unsigned long long>(value));
    return buffer;
}

MemorySample sampleMemory() {
    MemorySample sample;
#ifdef _WIN32
    PROCESS_MEMORY_COUNTERS counters{};
    if (GetProcessMemoryInfo(GetCurrentProcess(), &counters, sizeof counters)) {
        sample.workingSet = counters.WorkingSetSize;
        sample.peakWorkingSet = counters.PeakWorkingSetSize;
        sample.commit = counters.PagefileUsage;
        sample.peakCommit = counters.PeakPagefileUsage;
    }
#endif
    return sample;
}

std::string solutionProblem(const sudoku::Board& solution, const sudoku::Board& puzzle) {
    const int n = puzzle.boxSize();
    const int size = puzzle.size();
    if (solution.boxSize() != n) return "wrong box size";
    for (int i = 0; i < puzzle.cellCount(); ++i) {
        const int value = solution.at(i);
        if (value < 1 || value > size) return "cell " + std::to_string(i) + " holds " + std::to_string(value);
        if (puzzle.at(i) != 0 && puzzle.at(i) != value) return "given at cell " + std::to_string(i) + " changed";
    }
    const auto cellOf = [&](int kind, int unit, int j) {
        if (kind == 0) return unit * size + j;
        if (kind == 1) return j * size + unit;
        return ((unit / n) * n + j / n) * size + (unit % n) * n + j % n;
    };
    std::vector<char> seen(static_cast<std::size_t>(size) + 1);
    for (int kind = 0; kind < 3; ++kind) {
        for (int unit = 0; unit < size; ++unit) {
            std::fill(seen.begin(), seen.end(), 0);
            for (int j = 0; j < size; ++j) {
                const auto value = static_cast<std::size_t>(solution.at(cellOf(kind, unit, j)));
                if (seen[value] != 0) {
                    static constexpr const char* kKinds[] = {"row", "column", "box"};
                    return std::string(kKinds[kind]) + " " + std::to_string(unit) + " repeats " +
                           std::to_string(value);
                }
                seen[value] = 1;
            }
        }
    }
    return {};
}

std::string readFile(const std::string& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) throw std::runtime_error("cannot read " + path);
    return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}

void writeFile(const std::string& path, const std::string& text) {
    std::ofstream file(path, std::ios::binary);
    if (!file) throw std::runtime_error("cannot write " + path);
    file << text;
    if (!file) throw std::runtime_error("failed writing " + path);
}

void Row::add(std::string name, std::string value) {
    for (char& ch : value) {
        if (ch == ',' || ch == '\n' || ch == '\r' || ch == '"') ch = ';';
    }
    fields_.emplace_back(std::move(name), std::move(value));
}

void Row::add(std::string name, double value) {
    char buffer[64];
    std::snprintf(buffer, sizeof buffer, "%.6g", value);
    add(std::move(name), std::string(buffer));
}

std::string Row::header() const {
    std::string out;
    for (const auto& [name, value] : fields_) out += (out.empty() ? "" : ",") + name;
    return out;
}

std::string Row::values() const {
    std::string out;
    bool first = true;
    for (const auto& [name, value] : fields_) {
        if (!first) out += ',';
        out += value;
        first = false;
    }
    return out;
}

}  // namespace bench
