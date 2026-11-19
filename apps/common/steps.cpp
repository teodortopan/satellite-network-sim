#include "common/steps.hpp"

#include <cstdint>

#include "satsim/time/utc_time.hpp"

namespace satsim::app {

time::UtcTime time_of_step(const time::UtcTime& start, std::int64_t step_ns,
                           std::int64_t index) noexcept {
  const std::int64_t offset_ns = index * step_ns;
  // Whole seconds (at most 1e9, exact in a double) then the rest (below 1e9 ns, so the
  // quotient by 1e9 is within 1e-16 s of it and plus_seconds rounds it back exactly).
  const std::int64_t whole_s = offset_ns / kNanosecondsPerSecond;
  const std::int64_t rest_ns = offset_ns % kNanosecondsPerSecond;
  return start.plus_seconds(static_cast<double>(whole_s))
      .plus_seconds(static_cast<double>(rest_ns) / static_cast<double>(kNanosecondsPerSecond));
}

}  // namespace satsim::app
