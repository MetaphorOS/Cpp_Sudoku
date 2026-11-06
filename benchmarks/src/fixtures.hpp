#pragma once

// Fixture construction for the solver study (benchmarks/README.md, section 4).
//
//   A   randomly blanked complete grid (library constructive grid)
//   AS  randomly blanked complete grid from a randomized solver fill (ablation, n <= 4)
//   B   uniquely solvable puzzle carved to an exact number of empty cells

#include "common.hpp"

#include <chrono>
#include <cstdint>
#include <string>

namespace bench {

struct FixtureSpec {
    std::string population;  // "A", "AS" or "B"
    int n = 3;
    int empty = 0;
    int targetPct = 0;
    int seedIndex = 1;
};

struct CarveOptions {
    std::chrono::milliseconds budget{600'000};        // whole carving, all attempts
    std::chrono::milliseconds verifyBudget{600'000};  // independent re-verification
    std::uint64_t checkNodes = 1'000'000;             // node budget of each uniqueness check
    int attempts = 3;
};

// "A-n05-e000313-s001"
std::string fixtureId(const FixtureSpec& spec);

// Builds the fixture, writes <outRoot>/<population>/nNN/<id>.txt and .solution.txt (and, for a
// B target that was not reached, .best.txt / .best.solution.txt), and returns its manifest row.
Row makeFixture(const FixtureSpec& spec, const std::string& outRoot, const CarveOptions& options);

}  // namespace bench
