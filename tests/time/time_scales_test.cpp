#include "satsim/time/time_scales.hpp"

#include <gtest/gtest.h>

#include <cstdlib>
#include <optional>

#include "satsim/time/utc_time.hpp"

namespace {

using satsim::time::tt_from_utc;
using satsim::time::ut1_from_utc;
using satsim::time::UtcTime;

UtcTime at(int year, int month, int day, int hour = 0, int minute = 0, double second = 0.0) {
  const std::optional<UtcTime> t = UtcTime::from_civil({year, month, day, hour, minute, second});
  if (!t.has_value()) {
    ADD_FAILURE() << "invalid fixture date " << year << "-" << month << "-" << day;
    std::abort();
  }
  return *t;
}

// TT = UTC + 37 s (TAI - UTC since 2017-01-01, IERS Bulletin C) + 32.184 s (TT - TAI). The
// offset is a whole number of nanoseconds, so the result is exact.
TEST(TimeScalesTest, TtIsUtcPlus69_184Seconds) {
  EXPECT_EQ(tt_from_utc(at(2026, 11, 18)), at(2026, 11, 18, 0, 1, 9.184));
  EXPECT_EQ(tt_from_utc(at(2026, 12, 31, 23, 59, 0.0)), at(2027, 1, 1, 0, 0, 9.184));
  EXPECT_EQ(tt_from_utc(at(2017, 1, 1)).seconds_since(at(2017, 1, 1)), 69.184);
}

// UT1 = UTC + (UT1 - UTC); the published values have 0.1 us resolution, a whole number of
// nanoseconds, so the result is exact. Values: finals2000A rows of 2026-11-18 and
// 2017-01-01 (see earth_orientation_test.cpp).
TEST(TimeScalesTest, Ut1IsUtcPlusTheEarthOrientationOffset) {
  EXPECT_EQ(ut1_from_utc(at(2026, 11, 18), -0.0728860), at(2026, 11, 17, 23, 59, 59.927114));
  EXPECT_EQ(ut1_from_utc(at(2017, 1, 1, 12), 0.5912821), at(2017, 1, 1, 12, 0, 0.5912821));
  EXPECT_EQ(ut1_from_utc(at(2026, 11, 18, 6), 0.0), at(2026, 11, 18, 6));
}

}  // namespace
