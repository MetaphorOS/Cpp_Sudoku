#pragma once

#include <cstdint>
#include <random>
#include <span>
#include <utility>

namespace sudoku {

// mt19937_64's output sequence is fully specified by the standard, but std::shuffle
// and std::uniform_int_distribution are not. These helpers keep a given seed
// producing the same puzzle on every compiler and standard library.
using Rng = std::mt19937_64;

// Unbiased integer in [0, bound) by rejection sampling. bound must be > 0.
inline std::uint64_t uniformBelow(Rng& rng, std::uint64_t bound) {
    // Values below `threshold` would make some residues more likely; reject them.
    const std::uint64_t threshold = (0 - bound) % bound;  // == 2^64 mod bound
    for (;;) {
        const std::uint64_t r = rng();
        if (r >= threshold) return r % bound;
    }
}

// Fisher-Yates shuffle.
template <typename T>
void shuffle(std::span<T> items, Rng& rng) {
    for (std::size_t i = items.size(); i > 1; --i) {
        const auto j = static_cast<std::size_t>(uniformBelow(rng, i));
        std::swap(items[i - 1], items[j]);
    }
}

}  // namespace sudoku
