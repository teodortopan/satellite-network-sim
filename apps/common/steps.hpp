#pragma once

#include <cstdint>

#include "satsim/time/utc_time.hpp"

// The time steps of a run, shared by the programs that step through time.

namespace satsim::app {

inline constexpr std::int64_t kNanosecondsPerSecond = 1'000'000'000;

/// Longest accepted run: about 31.7 years, far beyond any Earth-orientation file. It keeps
/// every offset in nanoseconds (step index times step) well inside int64_t and
/// UtcTime::plus_seconds's precondition.
inline constexpr std::int64_t kMaxDurationNs = kNanosecondsPerSecond * kNanosecondsPerSecond;

/// Number of times in a run of `duration_ns` every `step_ns`: start + i * step for i = 0, 1,
/// ... up to the last one not after start + duration, so the end is included exactly when it
/// lands on a step. Precondition: step_ns > 0, duration_ns >= 0.
[[nodiscard]] constexpr std::int64_t step_count(std::int64_t duration_ns,
                                                std::int64_t step_ns) noexcept {
  return duration_ns / step_ns + 1;
}

/// Time number `index` (from 0): start + index * step_ns. The offset is computed in integer
/// nanoseconds and handed to UtcTime::plus_seconds as whole seconds then the remaining
/// nanoseconds, two doubles that are each exact, so every time is exact to the nanosecond
/// however long the run (no drift, unlike repeated addition of a double step).
/// Precondition: 0 <= index * step_ns <= kMaxDurationNs.
[[nodiscard]] time::UtcTime time_of_step(const time::UtcTime& start, std::int64_t step_ns,
                                         std::int64_t index) noexcept;

}  // namespace satsim::app
