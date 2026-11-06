#pragma once

// A cooperative deadline for the logic solver, the grader and the generator. Work checks
// expired() at fixed points (see each caller) and stops cleanly when it returns true; nothing
// is interrupted in the middle of a step.

#include <algorithm>
#include <chrono>
#include <memory>
#include <optional>

namespace sudoku {

class Deadline {
public:
    using Clock = std::chrono::steady_clock;

    // Never expires.
    Deadline() = default;

    // Expires at `when`.
    [[nodiscard]] static Deadline at(Clock::time_point when) {
        Deadline d;
        d.at_ = when;
        return d;
    }
    // Expires `limit` from now; a zero or negative limit means never (as GeneratorOptions::timeLimit).
    [[nodiscard]] static Deadline after(std::chrono::milliseconds limit) {
        return limit.count() > 0 ? at(Clock::now() + limit) : Deadline{};
    }
    // Testing only: reports not-expired for the first `checks` calls to expired() and expired
    // from then on, independent of the clock. Copies share the count, so a deadline handed down
    // to the grader and the logic solver counts their checks together. This makes "stopped at
    // the time limit" paths deterministic in tests.
    [[nodiscard]] static Deadline afterChecks(long long checks) {
        Deadline d;
        d.checksLeft_ = std::make_shared<long long>(checks);
        return d;
    }

    [[nodiscard]] bool bounded() const noexcept { return at_.has_value() || checksLeft_ != nullptr; }

    [[nodiscard]] bool expired() const {
        if (checksLeft_) {
            if (*checksLeft_ <= 0) return true;
            --*checksLeft_;
            return false;
        }
        return at_ && Clock::now() >= *at_;
    }

    // Time left, at least 1 ms once bounded so that a nearly expired budget still stops a
    // search; 0 (= unbounded, as SolveOptions::timeLimit) for a deadline with no clock time.
    [[nodiscard]] std::chrono::milliseconds remaining() const {
        if (checksLeft_) return std::chrono::milliseconds{*checksLeft_ <= 0 ? 1 : 0};
        if (!at_) return std::chrono::milliseconds{0};
        const auto left = std::chrono::ceil<std::chrono::milliseconds>(*at_ - Clock::now());
        return std::max(left, std::chrono::milliseconds{1});
    }

private:
    std::optional<Clock::time_point> at_;
    std::shared_ptr<long long> checksLeft_;
};

}  // namespace sudoku
