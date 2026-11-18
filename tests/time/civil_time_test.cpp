#include "satsim/time/civil_time.hpp"

#include <gtest/gtest.h>

#include <array>
#include <optional>
#include <string_view>

#include "satsim/time/utc_time.hpp"

namespace {

using satsim::time::CivilTime;
using satsim::time::parse_iso_civil_time;
using satsim::time::UtcTime;

TEST(ParseIsoCivilTimeTest, ReadsEveryField) {
  EXPECT_EQ(parse_iso_civil_time("2026-11-18T21:05:07"), (CivilTime{2026, 11, 18, 21, 5, 7.0}));
  EXPECT_EQ(parse_iso_civil_time("0001-01-01T00:00:00"), (CivilTime{1, 1, 1, 0, 0, 0.0}));
  EXPECT_EQ(parse_iso_civil_time("2026-11-18T21:05:07.5"), (CivilTime{2026, 11, 18, 21, 5, 7.5}));
}

// Up to nine fraction digits, and the instant they name is exact to the nanosecond.
TEST(ParseIsoCivilTimeTest, FractionGivesTheExactNanosecond) {
  const std::optional<CivilTime> civil = parse_iso_civil_time("2026-11-18T21:00:59.999999999");
  if (!civil) {
    FAIL() << "not parsed";
  }
  const std::optional<UtcTime> t = UtcTime::from_civil(*civil);
  const std::optional<UtcTime> whole = UtcTime::from_civil({2026, 11, 18, 21, 1, 0.0});
  if (!t || !whole) {
    FAIL() << "not a valid time";
  }
  EXPECT_EQ(t->plus_seconds(1e-9), *whole);
}

// Only the form is checked; whether the date exists is UtcTime::from_civil's job.
TEST(ParseIsoCivilTimeTest, DoesNotCheckRanges) {
  const std::optional<CivilTime> civil = parse_iso_civil_time("2026-13-45T99:99:99");
  EXPECT_EQ(civil, (CivilTime{2026, 13, 45, 99, 99, 99.0}));
  if (!civil) {
    FAIL() << "not parsed";
  }
  EXPECT_FALSE(UtcTime::from_civil(*civil).has_value());
}

TEST(ParseIsoCivilTimeTest, RejectsOtherForms) {
  constexpr std::array<std::string_view, 15> kBad = {
      "",
      "2026-11-18",
      "2026-11-18 21:00:00",
      "2026-11-18T21:00",
      "2026-11-18T21:00:00Z",
      "2026-11-18T21:00:00.",
      "2026-11-18T21:00:00.1234567890",
      "2026-11-18T21:00:00,5",
      "2026-11-18T21:00:0a",
      " 2026-11-18T21:00:00",
      "2026-11-18T21:00:00 ",
      "26-11-18T21:00:00",
      "2026/11/18T21:00:00",
      "+026-11-18T21:00:00",
      "2026-11-18T21:00:00.+5",
  };
  for (const std::string_view text : kBad) {
    EXPECT_FALSE(parse_iso_civil_time(text).has_value()) << '"' << text << '"';
  }
}

}  // namespace
