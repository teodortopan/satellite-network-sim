#include "satsim/time/earth_orientation.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdlib>
#include <filesystem>
#include <ios>
#include <limits>
#include <numbers>
#include <optional>
#include <sstream>
#include <string>

#include "satsim/time/utc_time.hpp"

namespace {

using satsim::time::EarthOrientation;
using satsim::time::EarthOrientationLoadResult;
using satsim::time::EarthOrientationTable;
using satsim::time::load_earth_orientation;
using satsim::time::UtcTime;

constexpr double kArcsecondRad = std::numbers::pi / (180.0 * 3600.0);

// Excerpts of the IERS finals2000A.all file downloaded on 2026-11-18 (from
// https://maia.usno.navy.mil/ser7/finals2000A.all; identical to the IERS copy at
// datacenter.iers.org), rows copied byte for byte:
//  - 2026-10-01 to 2026-12-31 (MJD 61314 to 61405; measured rows up to 2026-11-12, then
//    Bulletin A predictions);
//  - 2016-12-28 to 2017-01-04, across the leap second at the end of 2016-12-31;
//  - the last three rows with values (2027-11-17 to 2027-11-19) and the first three rows
//    after them, which have dates only.
constexpr const char* kQ4Path = "tests/time/data/finals2000A_2026q4.txt";
constexpr const char* kLeapPath = "tests/time/data/finals2000A_leap_2016.txt";
constexpr const char* kEndPath = "tests/time/data/finals2000A_end.txt";

// The real row of 2026-11-17, used as the template for malformed input.
const std::string kRow20261117 =
    "261117 61361.00 P  0.118407 0.001502  0.326797 0.001302  P-0.0714893 0.0000464           "
    "      P     0.357    0.128     0.229    0.160                                           "
    "          ";

UtcTime at(int year, int month, int day, int hour = 0, int minute = 0, double second = 0.0) {
  const std::optional<UtcTime> t = UtcTime::from_civil({year, month, day, hour, minute, second});
  if (!t.has_value()) {
    ADD_FAILURE() << "invalid fixture date " << year << "-" << month << "-" << day;
    std::abort();
  }
  return *t;
}

EarthOrientationTable load_fixture(const char* path) {
  EarthOrientationLoadResult result = load_earth_orientation(std::filesystem::path(path));
  if (!result.table.has_value()) {
    ADD_FAILURE() << "fixture " << path << " did not load: " << result.error.value_or("?");
    std::abort();
  }
  return *result.table;
}

EarthOrientationLoadResult load_text(const std::string& text) {
  std::istringstream in(text);
  return load_earth_orientation(in);
}

// The value of a lookup that must succeed; a failure is reported and yields NaNs, which
// then fail every comparison.
EarthOrientation value_of(const std::optional<EarthOrientation>& sample) {
  if (!sample.has_value()) {
    ADD_FAILURE() << "no Earth orientation value";
  }
  constexpr double kNan = std::numeric_limits<double>::quiet_NaN();
  return sample.value_or(
      EarthOrientation{.ut1_minus_utc_s = kNan, .polar_x_rad = kNan, .polar_y_rad = kNan});
}

// Published values are converted from arcseconds by one multiplication, as the loader does,
// so they compare equal to within an ULP.
void expect_row(const std::optional<EarthOrientation>& sample, double ut1_minus_utc_s,
                double x_arcsec, double y_arcsec) {
  const EarthOrientation actual = value_of(sample);
  EXPECT_DOUBLE_EQ(actual.ut1_minus_utc_s, ut1_minus_utc_s);
  EXPECT_DOUBLE_EQ(actual.polar_x_rad, x_arcsec * kArcsecondRad);
  EXPECT_DOUBLE_EQ(actual.polar_y_rad, y_arcsec * kArcsecondRad);
}

// ---------------------------------------------------------------------------------------
// Real rows parse to the published values (Bulletin A columns of the rows quoted).
// ---------------------------------------------------------------------------------------

TEST(EarthOrientationTest, LoadsRealRows) {
  const EarthOrientationTable table = load_fixture(kQ4Path);
  EXPECT_EQ(table.row_count(), 92U);
  EXPECT_EQ(table.first_mjd(), 61314);  // 2026-10-01
  EXPECT_EQ(table.last_mjd(), 61405);   // 2026-12-31

  // 2610 1 61314.00 I  0.174596 ...  0.325269 ...  I-0.0225228
  expect_row(table.at(at(2026, 10, 1)), -0.0225228, 0.174596, 0.325269);
  // 261117 61361.00 P  0.118407 ...  0.326797 ...  P-0.0714893
  expect_row(table.at(at(2026, 11, 17)), -0.0714893, 0.118407, 0.326797);
  // 261118 61362.00 P  0.117290 ...  0.327387 ...  P-0.0728860
  expect_row(table.at(at(2026, 11, 18)), -0.0728860, 0.117290, 0.327387);
  // 261231 61405.00 P  0.083062 ...  0.362675 ...  P-0.1181743
  expect_row(table.at(at(2026, 12, 31)), -0.1181743, 0.083062, 0.362675);
}

// ---------------------------------------------------------------------------------------
// Interpolation between the rows of 2026-11-17 and 2026-11-18. The weight is the exact
// fraction of the day, and a + w (b - a) rounds a few times: within 1e-15 relative. The
// tolerances, 1e-12 s and 1e-15 rad (2e-10 arcsec), are a thousandth of the change a
// one-millisecond error in the weight would make, and far above rounding.
// ---------------------------------------------------------------------------------------

constexpr double kInterpolationToleranceS = 1e-12;
constexpr double kInterpolationToleranceRad = 1e-15;

void expect_between_rows(const EarthOrientationTable& table, const UtcTime& t, double weight) {
  const double ut1_a = -0.0714893;
  const double ut1_b = -0.0728860;
  const double x_a = 0.118407 * kArcsecondRad;
  const double x_b = 0.117290 * kArcsecondRad;
  const double y_a = 0.326797 * kArcsecondRad;
  const double y_b = 0.327387 * kArcsecondRad;
  const EarthOrientation value = value_of(table.at(t));
  EXPECT_NEAR(value.ut1_minus_utc_s, ut1_a + weight * (ut1_b - ut1_a), kInterpolationToleranceS);
  EXPECT_NEAR(value.polar_x_rad, x_a + weight * (x_b - x_a), kInterpolationToleranceRad);
  EXPECT_NEAR(value.polar_y_rad, y_a + weight * (y_b - y_a), kInterpolationToleranceRad);
}

TEST(EarthOrientationTest, InterpolatesWithinTheDay) {
  const EarthOrientationTable table = load_fixture(kQ4Path);
  expect_between_rows(table, at(2026, 11, 17, 6), 0.25);
  expect_between_rows(table, at(2026, 11, 17, 12), 0.5);
  expect_between_rows(table, at(2026, 11, 17, 19, 12), 0.8);
  // Mid-day value written out: (-0.0714893 + -0.0728860) / 2.
  EXPECT_NEAR(value_of(table.at(at(2026, 11, 17, 12))).ut1_minus_utc_s, -0.07218765,
              kInterpolationToleranceS);
}

TEST(EarthOrientationTest, JustBeforeMidnightIsAlmostTheNextRow) {
  const EarthOrientationTable table = load_fixture(kQ4Path);
  const UtcTime t = at(2026, 11, 17, 23, 59, 59.999999999);
  expect_between_rows(table, t, 1.0 - 1e-9 / 86400.0);
  // And it is the 2026-11-17 to -18 interval, not the next one: within 2e-14 s of the
  // 2026-11-18 row, far from the 2026-11-19 slope.
  EXPECT_NEAR(value_of(table.at(t)).ut1_minus_utc_s, -0.0728860, 1e-12);
}

// ---------------------------------------------------------------------------------------
// Never extrapolated.
// ---------------------------------------------------------------------------------------

TEST(EarthOrientationTest, OutsideTheTableIsAnError) {
  const EarthOrientationTable table = load_fixture(kQ4Path);
  EXPECT_FALSE(table.at(at(2026, 9, 30, 23, 59, 59.999999999)).has_value());
  EXPECT_FALSE(table.at(at(2026, 9, 30)).has_value());
  EXPECT_FALSE(table.at(at(2000, 1, 1)).has_value());
  // The last row itself is available; anything after it is not.
  EXPECT_TRUE(table.at(at(2026, 12, 31)).has_value());
  EXPECT_FALSE(table.at(at(2026, 12, 31).plus_seconds(1e-9)).has_value());
  EXPECT_FALSE(table.at(at(2026, 12, 31, 12)).has_value());
  EXPECT_FALSE(table.at(at(2027, 1, 1)).has_value());
  EXPECT_FALSE(table.at(at(2100, 1, 1)).has_value());
}

TEST(EarthOrientationTest, DefaultTableHasNoValues) {
  const EarthOrientationTable table;
  EXPECT_EQ(table.row_count(), 0U);
  EXPECT_FALSE(table.at(at(2026, 11, 17)).has_value());
}

// ---------------------------------------------------------------------------------------
// The leap second at the end of 2016-12-31: UT1 - UTC goes from -0.4077601 s (2016-12-31)
// to +0.5912821 s (2017-01-01) in the real rows.
// ---------------------------------------------------------------------------------------

TEST(EarthOrientationTest, LeapSecondIntervalIsAnError) {
  const EarthOrientationTable table = load_fixture(kLeapPath);
  EXPECT_EQ(table.row_count(), 8U);
  // 161231 57753.00 I  0.081400 ...  0.263094 ...  I-0.4077601
  expect_row(table.at(at(2016, 12, 31)), -0.4077601, 0.081400, 0.263094);
  // 17 1 1 57754.00 I  0.080504 ...  0.263145 ...  I 0.5912821
  expect_row(table.at(at(2017, 1, 1)), 0.5912821, 0.080504, 0.263145);

  EXPECT_FALSE(table.at(at(2016, 12, 31).plus_seconds(1e-9)).has_value());
  EXPECT_FALSE(table.at(at(2016, 12, 31, 12)).has_value());
  EXPECT_FALSE(table.at(at(2016, 12, 31, 23, 59, 59.999999999)).has_value());

  // The days on either side interpolate normally: 161230 has -0.4069180, 17 1 2 has
  // 0.5901752.
  EXPECT_NEAR(value_of(table.at(at(2016, 12, 30, 12))).ut1_minus_utc_s,
              (-0.4069180 + -0.4077601) / 2.0, kInterpolationToleranceS);
  EXPECT_NEAR(value_of(table.at(at(2017, 1, 1, 12))).ut1_minus_utc_s, (0.5912821 + 0.5901752) / 2.0,
              kInterpolationToleranceS);
}

// ---------------------------------------------------------------------------------------
// End of the values: the file continues with rows that have only a date.
// ---------------------------------------------------------------------------------------

TEST(EarthOrientationTest, StopsAtTheFirstRowWithoutValues) {
  const EarthOrientationTable table = load_fixture(kEndPath);
  EXPECT_EQ(table.row_count(), 3U);
  EXPECT_EQ(table.first_mjd(), 61726);
  EXPECT_EQ(table.last_mjd(), 61728);
  // 271119 61728.00 P  0.141040 ...  0.277527 ...  P-0.2452823
  expect_row(table.at(at(2027, 11, 19)), -0.2452823, 0.141040, 0.277527);
  EXPECT_FALSE(table.at(at(2027, 11, 20)).has_value());
}

TEST(EarthOrientationTest, AnyBlankValueEndsTheTable) {
  // Blank out each value of a second row in turn: only the first row remains.
  const std::string second_row = "261118 61362.00" + kRow20261117.substr(15);
  for (const std::size_t column : std::array<std::size_t, 3>{19, 38, 59}) {
    std::string blanked = second_row;
    const std::size_t width = column == 59 ? 10 : 9;
    blanked.replace(column - 1, width, std::string(width, ' '));
    std::string text = kRow20261117;
    text += "\n";
    text += blanked;
    text += "\n";
    const EarthOrientationLoadResult result = load_text(text);
    EXPECT_TRUE(result.table.has_value()) << "column " << column;
    EXPECT_EQ(result.table.value_or(EarthOrientationTable{}).row_count(), 1U)
        << "column " << column;
  }
}

TEST(EarthOrientationTest, AcceptsCrLfAndAShortLastLine) {
  const EarthOrientationLoadResult result = load_text(kRow20261117 + "\r\n\r\n");
  EXPECT_TRUE(result.table.has_value()) << result.error.value_or("");
  expect_row(result.table.value_or(EarthOrientationTable{}).at(at(2026, 11, 17)), -0.0714893,
             0.118407, 0.326797);
}

// ---------------------------------------------------------------------------------------
// Malformed input fails the whole load with a message naming the line.
// ---------------------------------------------------------------------------------------

// The 2026-11-17 row with columns [first, first + replacement.size()) replaced.
std::string with_columns(std::size_t first_column, const std::string& replacement) {
  std::string row = kRow20261117;
  row.replace(first_column - 1, replacement.size(), replacement);
  return row;
}

void expect_error(const std::string& text, const std::string& expected_fragment) {
  const EarthOrientationLoadResult result = load_text(text);
  EXPECT_FALSE(result.table.has_value());
  const std::string message = result.error.value_or("(no error)");
  EXPECT_NE(message.find(expected_fragment), std::string::npos) << "message: " << message;
}

TEST(EarthOrientationTest, RejectsMalformedRows) {
  expect_error(with_columns(19, " 0.11x407"), "line 1: polar motion x");
  expect_error(with_columns(38, "      nan"), "line 1: polar motion y");
  expect_error(with_columns(59, "-0.07148.3"), "line 1: UT1-UTC");
  std::string bad_second_row = with_columns(8, "61362.00");
  bad_second_row.replace(18, 9, " 0.1+8407");
  expect_error(kRow20261117 + "\n" + bad_second_row, "line 2: polar motion x");
  expect_error(with_columns(8, "61361.50"), "line 1: MJD");
  expect_error(with_columns(8, "        "), "line 1: MJD");
  expect_error(with_columns(8, "6136l.00"), "line 1: MJD");
  expect_error(with_columns(8, "-6136.00"), "line 1: MJD");
}

TEST(EarthOrientationTest, RejectsImplausibleValues) {
  expect_error(with_columns(19, " 2.118407"), "line 1: polar motion x");
  expect_error(with_columns(38, "-3.326797"), "line 1: polar motion y");
  expect_error(with_columns(59, " 1.0000000"), "line 1: UT1-UTC");
  expect_error(with_columns(59, "-1.0714893"), "line 1: UT1-UTC");
}

TEST(EarthOrientationTest, RejectsGapsAndDisorder) {
  const std::string next_day = with_columns(8, "61362.00");
  const std::string two_days_later = with_columns(8, "61363.00");
  expect_error(kRow20261117 + "\n" + two_days_later, "line 2: MJD 61363 does not follow");
  expect_error(kRow20261117 + "\n" + kRow20261117, "line 2: MJD 61361 does not follow");
  expect_error(next_day + "\n" + kRow20261117, "line 2: MJD 61361 does not follow");
}

TEST(EarthOrientationTest, RejectsEmptyInput) {
  expect_error("", "no row with polar motion and UT1-UTC values");
  expect_error("\n\n", "no row with polar motion and UT1-UTC values");
  // Only rows past the end of the values, as at the end of the real file.
  expect_error("271120 61729.00\n271121 61730.00\n", "no row with polar motion");
}

TEST(EarthOrientationTest, ReportsUnusableFiles) {
  const EarthOrientationLoadResult missing =
      load_earth_orientation(std::filesystem::path("tests/time/data/no_such_file.txt"));
  EXPECT_FALSE(missing.table.has_value());
  EXPECT_NE(missing.error.value_or("").find("cannot open tests/time/data/no_such_file.txt"),
            std::string::npos);

  const EarthOrientationLoadResult directory =
      load_earth_orientation(std::filesystem::path("tests/time/data"));
  EXPECT_FALSE(directory.table.has_value());
  EXPECT_NE(directory.error.value_or("").find("is a directory"), std::string::npos);

  std::istringstream failed_stream;
  failed_stream.setstate(std::ios::failbit);
  const EarthOrientationLoadResult unreadable = load_earth_orientation(failed_stream);
  EXPECT_FALSE(unreadable.table.has_value());
  EXPECT_EQ(unreadable.error, "input stream is not readable");
}

}  // namespace
