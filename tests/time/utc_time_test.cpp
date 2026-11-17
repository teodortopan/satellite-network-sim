#include "satsim/time/utc_time.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <optional>

#include "satsim/time/civil_time.hpp"

namespace {

using satsim::time::CivilTime;
using satsim::time::UtcTime;

UtcTime at(int year, int month, int day, int hour = 0, int minute = 0, double second = 0.0) {
  const std::optional<UtcTime> t = UtcTime::from_civil({year, month, day, hour, minute, second});
  if (!t.has_value()) {
    // A test fixture names a date that does not exist: the test itself is wrong.
    ADD_FAILURE() << "invalid fixture date " << year << "-" << month << "-" << day;
    std::abort();
  }
  return *t;
}

void expect_civil(const CivilTime& actual, const CivilTime& expected, double second_tolerance) {
  EXPECT_EQ(actual.year, expected.year);
  EXPECT_EQ(actual.month, expected.month);
  EXPECT_EQ(actual.day, expected.day);
  EXPECT_EQ(actual.hour, expected.hour);
  EXPECT_EQ(actual.minute, expected.minute);
  EXPECT_NEAR(actual.second, expected.second, second_tolerance);
}

// ---------------------------------------------------------------------------------------
// Calendar <-> Julian date.
//
// jd_whole() is an integer MJD plus 2400000.5, exact in double, so it is compared exactly.
// jd_fraction() is one correctly rounded division of exact integers, so it is within half
// an ULP of the true fraction: EXPECT_DOUBLE_EQ (4 ULPs) covers it.
// ---------------------------------------------------------------------------------------

struct JdCase {
  CivilTime civil;
  double jd_whole{};
  double jd_fraction{};
  const char* source{};
};

// Sources:
//  - J2000.0 = 2000 January 1, 12h = JD 2451545.0 (IAU definition; Meeus, Astronomical
//    Algorithms 2nd ed., ch. 7).
//  - Meeus ch. 7, Example 7.a: 1957 October 4.81 (Sputnik 1 launch) = JD 2436116.31.
//    4.81 d = day 4 at 19:26:24.
//  - Meeus ch. 7, table of example dates: 1999 Jan 1.0 = 2451179.5, 1987 Jan 27.0 =
//    2446822.5, 1988 Jun 19.5 = 2447332.0, 1900 Jan 1.0 = 2415020.5, 1600 Jan 1.0 =
//    2305447.5, 1600 Dec 31.0 = 2305812.5.
//  - The remaining dates: pyerfa 2.0.1.5, erfa.cal2jd(y, m, d) (returns 2400000.5 + MJD).
//    Every Meeus value above was also cross-checked with erfa.cal2jd and agrees.
const std::array kJdCases{
    JdCase{{2000, 1, 1, 12, 0, 0.0}, 2451544.5, 0.5, "J2000.0"},
    JdCase{{1957, 10, 4, 19, 26, 24.0}, 2436115.5, 0.81, "Meeus Ex. 7.a"},
    JdCase{{1999, 1, 1, 0, 0, 0.0}, 2451179.5, 0.0, "Meeus ch. 7"},
    JdCase{{1987, 1, 27, 0, 0, 0.0}, 2446822.5, 0.0, "Meeus ch. 7"},
    JdCase{{1988, 6, 19, 12, 0, 0.0}, 2447331.5, 0.5, "Meeus ch. 7"},
    JdCase{{1900, 1, 1, 0, 0, 0.0}, 2415020.5, 0.0, "Meeus ch. 7"},
    JdCase{{1600, 1, 1, 0, 0, 0.0}, 2305447.5, 0.0, "Meeus ch. 7"},
    JdCase{{1600, 12, 31, 0, 0, 0.0}, 2305812.5, 0.0, "Meeus ch. 7"},
    JdCase{{2026, 11, 17, 18, 0, 0.0}, 2461361.5, 0.75, "erfa.cal2jd(2026, 11, 17)"},
    JdCase{{2024, 2, 29, 6, 0, 0.0}, 2460369.5, 0.25, "erfa.cal2jd(2024, 2, 29)"},
    JdCase{{2000, 2, 29, 0, 0, 0.0}, 2451603.5, 0.0, "erfa.cal2jd(2000, 2, 29)"},
    JdCase{{1900, 3, 1, 0, 0, 0.0}, 2415079.5, 0.0, "erfa.cal2jd(1900, 3, 1)"},
    JdCase{{2100, 3, 1, 0, 0, 0.0}, 2488128.5, 0.0, "erfa.cal2jd(2100, 3, 1)"},
    JdCase{{2004, 4, 6, 0, 0, 0.0}, 2453101.5, 0.0, "erfa.cal2jd(2004, 4, 6)"},
};

TEST(UtcTimeTest, KnownJulianDates) {
  for (const JdCase& c : kJdCases) {
    const UtcTime t =
        at(c.civil.year, c.civil.month, c.civil.day, c.civil.hour, c.civil.minute, c.civil.second);
    EXPECT_EQ(t.jd_whole(), c.jd_whole) << c.source;
    EXPECT_DOUBLE_EQ(t.jd_fraction(), c.jd_fraction) << c.source;
  }
}

// Meeus Example 7.a states the total, JD 2436116.31. Summing the two parts into one double
// rounds once, by at most half an ULP at 2.4e6 (2.3e-10 d); 1e-9 d covers that.
TEST(UtcTimeTest, SputnikLaunchTotalJulianDate) {
  const UtcTime t = at(1957, 10, 4, 19, 26, 24.0);
  EXPECT_NEAR(t.jd_whole() + t.jd_fraction(), 2436116.31, 1e-9);
}

TEST(UtcTimeTest, JdWholeEndsInHalfAndFractionInRange) {
  const UtcTime t = at(2026, 11, 17, 23, 59, 59.999999999);
  EXPECT_EQ(t.jd_whole() - std::floor(t.jd_whole()), 0.5);
  EXPECT_GE(t.jd_fraction(), 0.0);
  EXPECT_LT(t.jd_fraction(), 1.0);
}

// ---------------------------------------------------------------------------------------
// Leap years (Gregorian rule: divisible by 4, except centuries not divisible by 400).
// ---------------------------------------------------------------------------------------

TEST(UtcTimeTest, LeapYearRule) {
  EXPECT_TRUE(UtcTime::from_civil({2000, 2, 29, 0, 0, 0.0}).has_value());   // 400 divides
  EXPECT_TRUE(UtcTime::from_civil({2024, 2, 29, 0, 0, 0.0}).has_value());   // 4 divides
  EXPECT_FALSE(UtcTime::from_civil({1900, 2, 29, 0, 0, 0.0}).has_value());  // century
  EXPECT_FALSE(UtcTime::from_civil({2100, 2, 29, 0, 0, 0.0}).has_value());  // century
  EXPECT_FALSE(UtcTime::from_civil({2023, 2, 29, 0, 0, 0.0}).has_value());
  EXPECT_FALSE(UtcTime::from_civil({2026, 2, 29, 0, 0, 0.0}).has_value());
}

// Day counts across February follow from the rule: 1 March minus 1 January is 31 + 28 or
// 31 + 29 days.
TEST(UtcTimeTest, FebruaryLengthInDayCounts) {
  EXPECT_EQ(at(1900, 3, 1).jd_whole() - at(1900, 1, 1).jd_whole(), 59.0);
  EXPECT_EQ(at(2000, 3, 1).jd_whole() - at(2000, 1, 1).jd_whole(), 60.0);
  EXPECT_EQ(at(2024, 3, 1).jd_whole() - at(2024, 1, 1).jd_whole(), 60.0);
  EXPECT_EQ(at(2100, 3, 1).jd_whole() - at(2100, 1, 1).jd_whole(), 59.0);
  // 2000 to 2100: 25 leap days (2000, 2004, ..., 2096; 2100 is not leap).
  EXPECT_EQ(at(2100, 1, 1).jd_whole() - at(2000, 1, 1).jd_whole(), 36525.0);
}

// ---------------------------------------------------------------------------------------
// Invalid input.
// ---------------------------------------------------------------------------------------

TEST(UtcTimeTest, RejectsNonexistentDatesAndTimes) {
  constexpr double kNan = std::numeric_limits<double>::quiet_NaN();
  constexpr double kInf = std::numeric_limits<double>::infinity();
  const std::array<CivilTime, 22> kInvalid{{
      {2026, 0, 1, 0, 0, 0.0},      {2026, 13, 1, 0, 0, 0.0},  {2026, 1, 0, 0, 0, 0.0},
      {2026, 1, 32, 0, 0, 0.0},     {2026, 2, 30, 0, 0, 0.0},  {2026, 4, 31, 0, 0, 0.0},
      {2026, 6, 31, 0, 0, 0.0},     {2026, 9, 31, 0, 0, 0.0},  {2026, 11, 31, 0, 0, 0.0},
      {2026, 1, 1, -1, 0, 0.0},     {2026, 1, 1, 24, 0, 0.0},  {2026, 1, 1, 0, -1, 0.0},
      {2026, 1, 1, 0, 60, 0.0},     {2026, 1, 1, 0, 0, -1e-9}, {2026, 1, 1, 0, 0, 60.0},
      {2016, 12, 31, 23, 59, 60.5},  // a real leap second, deliberately rejected
      {2026, 1, 1, 0, 0, kNan},     {2026, 1, 1, 0, 0, kInf},  {2026, 1, 1, 0, 0, -kInf},
      {0, 1, 1, 0, 0, 0.0},         {10000, 1, 1, 0, 0, 0.0},  {2026, -1, 1, 0, 0, 0.0},
  }};
  for (const CivilTime& c : kInvalid) {
    EXPECT_FALSE(UtcTime::from_civil(c).has_value())
        << c.year << "-" << c.month << "-" << c.day << " " << c.hour << ":" << c.minute << ":"
        << c.second;
  }
}

TEST(UtcTimeTest, AcceptsBoundaryValues) {
  EXPECT_TRUE(UtcTime::from_civil({1, 1, 1, 0, 0, 0.0}).has_value());
  EXPECT_TRUE(UtcTime::from_civil({9999, 12, 31, 23, 59, 59.999999999}).has_value());
  EXPECT_TRUE(UtcTime::from_civil({2026, 1, 31, 0, 0, 0.0}).has_value());
  EXPECT_TRUE(UtcTime::from_civil({2026, 4, 30, 0, 0, 0.0}).has_value());
  EXPECT_TRUE(UtcTime::from_civil({2026, 12, 31, 23, 59, 59.5}).has_value());
}

// ---------------------------------------------------------------------------------------
// Round trips. from_civil rounds the second to the nearest nanosecond (error <= 0.5 ns) and
// to_civil divides an exact integer count of nanoseconds once (relative error 1.1e-16), so
// the second comes back within 1 ns. The contract asks for 1 us; 1 ns is what the design
// guarantees.
// ---------------------------------------------------------------------------------------

constexpr double kSecondRoundTripTolerance = 1e-9;

TEST(UtcTimeTest, CivilRoundTripAcrossBoundaries) {
  const std::array<CivilTime, 12> kCases{{
      {1957, 10, 4, 19, 28, 34.0},
      {1999, 12, 31, 23, 59, 59.999999},
      {2000, 1, 1, 0, 0, 0.0},
      {2000, 2, 29, 12, 34, 56.789012345},
      {2024, 2, 28, 23, 59, 59.5},
      {2024, 2, 29, 0, 0, 0.000001},
      {2026, 11, 17, 7, 51, 28.386009},
      {2026, 12, 31, 23, 59, 59.999999999},
      {2027, 1, 1, 0, 0, 0.0},
      {2100, 2, 28, 23, 59, 59.0},
      {2100, 3, 1, 0, 0, 0.0},
      {2100, 12, 31, 23, 59, 59.123456789},
  }};
  for (const CivilTime& c : kCases) {
    const UtcTime t = at(c.year, c.month, c.day, c.hour, c.minute, c.second);
    expect_civil(t.to_civil(), c, kSecondRoundTripTolerance);
  }
}

// Every day from 1957 to 2100 at a time with a fractional second: walking by whole days
// through plus_seconds must agree with constructing each date directly.
TEST(UtcTimeTest, DailyWalkMatchesDirectConstruction) {
  UtcTime walker = at(1957, 1, 1, 13, 7, 41.25);
  const UtcTime end = at(2101, 1, 1);
  int days = 0;
  while (walker < end) {
    const CivilTime c = walker.to_civil();
    ASSERT_EQ(c.hour, 13);
    ASSERT_EQ(c.minute, 7);
    ASSERT_EQ(c.second, 41.25);  // 41.25 s is exact in binary and in nanoseconds
    const std::optional<UtcTime> direct = UtcTime::from_civil(c);
    ASSERT_TRUE(direct.has_value());
    ASSERT_EQ(direct, walker);
    walker = walker.plus_seconds(86400.0);
    ++days;
  }
  // 1957-01-01 to 2101-01-01: 144 years, 35 leap days (1960, ..., 2096; not 2100).
  EXPECT_EQ(days, 144 * 365 + 35);
}

// Seconds just below 60 must never read back as 60.0; a value that rounds to a whole minute
// at nanosecond resolution carries into the next minute instead.
TEST(UtcTimeTest, SecondNeverReadsAsSixty) {
  const CivilTime almost = at(2026, 12, 31, 23, 59, 59.999999999).to_civil();
  EXPECT_LT(almost.second, 60.0);
  EXPECT_EQ(almost.minute, 59);

  const CivilTime carried = at(2026, 12, 31, 23, 59, 59.9999999996).to_civil();
  expect_civil(carried, {2027, 1, 1, 0, 0, 0.0}, 0.0);
}

// ---------------------------------------------------------------------------------------
// plus_seconds / seconds_since. Whole-second offsets are exact (integer nanoseconds), so
// results are compared exactly; fractional offsets round to the nearest nanosecond.
// ---------------------------------------------------------------------------------------

TEST(UtcTimeTest, PlusSecondsCrossesBoundaries) {
  EXPECT_EQ(at(2026, 12, 31, 23, 59, 59.0).plus_seconds(1.0), at(2027, 1, 1));
  EXPECT_EQ(at(2024, 2, 28, 23, 0, 0.0).plus_seconds(3600.0), at(2024, 2, 29));
  EXPECT_EQ(at(2023, 2, 28, 23, 0, 0.0).plus_seconds(3600.0), at(2023, 3, 1));
  EXPECT_EQ(at(2026, 1, 31, 12, 0, 0.0).plus_seconds(1.5 * 86400.0), at(2026, 2, 2));
  EXPECT_EQ(at(2000, 1, 1, 12, 0, 0.0).plus_seconds(36525.0 * 86400.0), at(2100, 1, 1, 12));
}

TEST(UtcTimeTest, PlusSecondsNegative) {
  EXPECT_EQ(at(2000, 3, 1).plus_seconds(-1.0), at(2000, 2, 29, 23, 59, 59.0));
  EXPECT_EQ(at(2000, 1, 1).plus_seconds(-0.25), at(1999, 12, 31, 23, 59, 59.75));
  EXPECT_EQ(at(2025, 1, 1).plus_seconds(-366.0 * 86400.0), at(2024, 1, 1));  // 2024 is leap
  EXPECT_EQ(at(2026, 3, 1, 0, 0, 0.5).plus_seconds(-2.0 * 86400.0 - 1.0),
            at(2026, 2, 26, 23, 59, 59.5));
  EXPECT_EQ(at(2026, 5, 5).plus_seconds(0.0), at(2026, 5, 5));
}

TEST(UtcTimeTest, PlusThenMinusIsIdentity) {
  const UtcTime t = at(2026, 11, 17, 7, 51, 28.386009);
  constexpr std::array kOffsets{-1e9, -86400.5, -0.001, 0.123456789, 59.9, 86399.999, 3.2e8};
  for (const double offset : kOffsets) {
    EXPECT_EQ(t.plus_seconds(offset).plus_seconds(-offset), t) << "offset = " << offset;
  }
}

// seconds_since is a day difference times 86400 plus a nanosecond difference, both exact
// integers, combined with two roundings: relative error ~2.2e-16. For offsets up to 1e9 s
// that is < 1e-6 s; the offsets themselves are first rounded to the nanosecond.
TEST(UtcTimeTest, SecondsSinceInvertsPlusSeconds) {
  const UtcTime t = at(1957, 10, 4, 19, 28, 34.0);
  constexpr std::array kOffsets{-3.0e8, -1.0, -1e-9, 0.0, 1e-7, 0.5, 86400.0, 4.5e9};
  for (const double offset : kOffsets) {
    const double tolerance =
        0.5e-9 + 4.0 * std::numeric_limits<double>::epsilon() * std::abs(offset);
    EXPECT_NEAR(t.plus_seconds(offset).seconds_since(t), offset, tolerance)
        << "offset = " << offset;
  }
  EXPECT_EQ(at(2027, 1, 1).seconds_since(at(2026, 12, 31, 23, 59, 59.0)), 1.0);
  EXPECT_EQ(at(2026, 12, 31, 23, 59, 59.0).seconds_since(at(2027, 1, 1)), -1.0);
  EXPECT_EQ(at(2000, 3, 1).seconds_since(at(2000, 2, 28)), 2.0 * 86400.0);
}

// ---------------------------------------------------------------------------------------
// Ordering and resolution.
// ---------------------------------------------------------------------------------------

TEST(UtcTimeTest, Ordering) {
  const UtcTime a = at(2026, 11, 17, 23, 59, 59.0);
  const UtcTime b = at(2026, 11, 18);
  EXPECT_LT(a, b);
  EXPECT_GT(b, a);
  EXPECT_LE(a, a);
  EXPECT_GE(b, b);
  EXPECT_NE(a, b);
  EXPECT_EQ(a, at(2026, 11, 17, 23, 59, 59.0));
  // Later time of day on an earlier date is still earlier.
  EXPECT_LT(at(1999, 12, 31, 23, 59, 59.9), at(2000, 1, 1));
}

// A single-double Julian date near 2100 resolves only ~40 us; this type must separate
// instants 1 us, 100 ns and 1 ns apart at both ends of the 1957-2100 range.
TEST(UtcTimeTest, SubMicrosecondResolution) {
  for (const UtcTime t0 : {at(1957, 10, 4, 19, 28, 34.0), at(2100, 12, 31, 23, 59, 59.0)}) {
    for (const double step : {1e-6, 1e-7, 1e-9}) {
      const UtcTime t1 = t0.plus_seconds(step);
      EXPECT_LT(t0, t1) << "step = " << step;
      // Both instants are whole nanoseconds; the difference is the step to within one
      // rounding of the nanosecond count's conversion.
      EXPECT_NEAR(t1.seconds_since(t0), step, 1e-15) << "step = " << step;
    }
  }
  EXPECT_NEAR(at(2100, 12, 31, 23, 59, 59.0).plus_seconds(1e-7).to_civil().second, 59.0000001,
              kSecondRoundTripTolerance);
}

}  // namespace
