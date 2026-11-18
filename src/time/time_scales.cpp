#include "satsim/time/time_scales.hpp"

#include "satsim/time/utc_time.hpp"

namespace satsim::time {

UtcTime ut1_from_utc(const UtcTime& utc, double ut1_minus_utc_s) noexcept {
  return utc.plus_seconds(ut1_minus_utc_s);
}

UtcTime tt_from_utc(const UtcTime& utc) noexcept {
  // 69.184 is not exact in binary; plus_seconds rounds it to exactly 69.184000000 s.
  return utc.plus_seconds(kTaiMinusUtcS + kTtMinusTaiS);
}

}  // namespace satsim::time
