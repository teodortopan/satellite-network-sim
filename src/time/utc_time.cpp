#include "satsim/time/utc_time.hpp"

#include <cassert>
#include <cmath>
#include <cstdint>
#include <optional>

#include "satsim/time/civil_time.hpp"

namespace satsim::time {

namespace {

constexpr std::int64_t kNanosecondsPerSecond = 1'000'000'000;
constexpr std::int64_t kNanosecondsPerMinute = 60 * kNanosecondsPerSecond;
constexpr std::int64_t kNanosecondsPerHour = 60 * kNanosecondsPerMinute;
constexpr std::int64_t kNanosecondsPerDay = 24 * kNanosecondsPerHour;
constexpr double kSecondsPerDay = 86400.0;
constexpr double kMaxOffsetSeconds = 1e11;  // plus_seconds precondition, about 3000 years.

// 1970-01-01 is MJD 40587; MJD 0 is JD 2400000.5.
constexpr std::int64_t kMjdOfUnixEpoch = 40587;
constexpr double kJdOfMjdZero = 2400000.5;

constexpr int kMinYear = 1;
constexpr int kMaxYear = 9999;

bool is_leap_year(int year) noexcept {
  return year % 4 == 0 && (year % 100 != 0 || year % 400 == 0);
}

int days_in_month(int year, int month) noexcept {
  switch (month) {
    case 2:
      return is_leap_year(year) ? 29 : 28;
    case 4:
    case 6:
    case 9:
    case 11:
      return 30;
    default:
      return 31;
  }
}

// Days from 1970-01-01 to a proleptic Gregorian date, and back. These are H. Hinnant's
// "days_from_civil" / "civil_from_days" (chrono-Compatible Low-Level Date Algorithms,
// https://howardhinnant.github.io/date_algorithms.html): the year is counted from March so
// the leap day falls at the end, and 400-year eras make the arithmetic exact for any year.
std::int64_t days_from_civil(std::int64_t year, std::int64_t month, std::int64_t day) noexcept {
  year -= month <= 2 ? 1 : 0;
  const std::int64_t era = (year >= 0 ? year : year - 399) / 400;
  const std::int64_t year_of_era = year - era * 400;                            // [0, 399]
  const std::int64_t month_from_march = (month + 9) % 12;                       // March = 0
  const std::int64_t day_of_year = (153 * month_from_march + 2) / 5 + day - 1;  // [0, 365]
  const std::int64_t day_of_era =
      year_of_era * 365 + year_of_era / 4 - year_of_era / 100 + day_of_year;  // [0, 146096]
  return era * 146097 + day_of_era - 719468;
}

struct Date {
  std::int64_t year;
  std::int64_t month;
  std::int64_t day;
};

Date civil_from_days(std::int64_t days_since_unix_epoch) noexcept {
  const std::int64_t z = days_since_unix_epoch + 719468;
  const std::int64_t era = (z >= 0 ? z : z - 146096) / 146097;
  const std::int64_t day_of_era = z - era * 146097;  // [0, 146096]
  const std::int64_t year_of_era =
      (day_of_era - day_of_era / 1460 + day_of_era / 36524 - day_of_era / 146096) / 365;
  const std::int64_t day_of_year =
      day_of_era - (365 * year_of_era + year_of_era / 4 - year_of_era / 100);  // [0, 365]
  const std::int64_t month_from_march = (5 * day_of_year + 2) / 153;           // [0, 11]
  const std::int64_t day = day_of_year - (153 * month_from_march + 2) / 5 + 1;
  const std::int64_t month = month_from_march < 10 ? month_from_march + 3 : month_from_march - 9;
  const std::int64_t year = year_of_era + era * 400 + (month <= 2 ? 1 : 0);
  return {.year = year, .month = month, .day = day};
}

// Floor division and the matching non-negative remainder, for a positive divisor.
std::int64_t floor_div(std::int64_t a, std::int64_t b) noexcept {
  const std::int64_t q = a / b;
  return (a % b != 0 && a < 0) ? q - 1 : q;
}

}  // namespace

UtcTime::UtcTime(std::int64_t mjd_day, std::int64_t nanoseconds_of_day) noexcept
    : mjd_day_(mjd_day + floor_div(nanoseconds_of_day, kNanosecondsPerDay)),
      nanoseconds_of_day_(nanoseconds_of_day -
                          floor_div(nanoseconds_of_day, kNanosecondsPerDay) * kNanosecondsPerDay) {}

std::optional<UtcTime> UtcTime::from_civil(const CivilTime& civil) noexcept {
  const bool date_exists = civil.year >= kMinYear && civil.year <= kMaxYear && civil.month >= 1 &&
                           civil.month <= 12 && civil.day >= 1 &&
                           civil.day <= days_in_month(civil.year, civil.month);
  // Written so that NaN fails the second check.
  const bool time_exists = civil.hour >= 0 && civil.hour <= 23 && civil.minute >= 0 &&
                           civil.minute <= 59 && civil.second >= 0.0 && civil.second < 60.0;
  if (!date_exists || !time_exists) {
    return std::nullopt;
  }
  const std::int64_t mjd_day =
      days_from_civil(civil.year, civil.month, civil.day) + kMjdOfUnixEpoch;
  // second < 60, so the product is below 6e10 and fits easily. Rounding 59.9999999996 s up
  // to 60e9 ns is carried into the next minute by the constructor's normalisation.
  const std::int64_t nanoseconds_of_day =
      civil.hour * kNanosecondsPerHour + civil.minute * kNanosecondsPerMinute +
      std::llround(civil.second * static_cast<double>(kNanosecondsPerSecond));
  return UtcTime(mjd_day, nanoseconds_of_day);
}

CivilTime UtcTime::to_civil() const noexcept {
  const Date date = civil_from_days(mjd_day_ - kMjdOfUnixEpoch);
  const std::int64_t nanoseconds_of_minute = nanoseconds_of_day_ % kNanosecondsPerMinute;
  // At most 59'999'999'999 ns, which converts to a double strictly below 60.
  return {
      .year = static_cast<int>(date.year),
      .month = static_cast<int>(date.month),
      .day = static_cast<int>(date.day),
      .hour = static_cast<int>(nanoseconds_of_day_ / kNanosecondsPerHour),
      .minute =
          static_cast<int>((nanoseconds_of_day_ % kNanosecondsPerHour) / kNanosecondsPerMinute),
      .second =
          static_cast<double>(nanoseconds_of_minute) / static_cast<double>(kNanosecondsPerSecond),
  };
}

double UtcTime::jd_whole() const noexcept {
  // Exact: the MJD is an integer far below 2^52.
  return static_cast<double>(mjd_day_) + kJdOfMjdZero;
}

double UtcTime::jd_fraction() const noexcept {
  // Both operands are exact in double (below 2^53); the division rounds once, and the
  // largest numerator 86400e9 - 1 still gives a result below 1.
  return static_cast<double>(nanoseconds_of_day_) / static_cast<double>(kNanosecondsPerDay);
}

UtcTime UtcTime::plus_seconds(double seconds) const noexcept {
  // Outside the precondition the integer conversions below would be undefined behaviour.
  assert(std::isfinite(seconds) && std::abs(seconds) <= kMaxOffsetSeconds);
  // Split into whole days and a remainder first: fmod is exact, so no precision of the
  // caller's value is lost, and the nanosecond product stays below 8.64e13.
  const double remainder_s = std::fmod(seconds, kSecondsPerDay);
  const auto whole_days = static_cast<std::int64_t>((seconds - remainder_s) / kSecondsPerDay);
  const std::int64_t remainder_ns =
      std::llround(remainder_s * static_cast<double>(kNanosecondsPerSecond));
  return {mjd_day_ + whole_days, nanoseconds_of_day_ + remainder_ns};
}

double UtcTime::seconds_since(const UtcTime& earlier) const noexcept {
  const std::int64_t day_difference = mjd_day_ - earlier.mjd_day_;
  // Below 2^53 in magnitude, so the conversion is exact.
  const std::int64_t nanosecond_difference = nanoseconds_of_day_ - earlier.nanoseconds_of_day_;
  return static_cast<double>(day_difference) * kSecondsPerDay +
         static_cast<double>(nanosecond_difference) / static_cast<double>(kNanosecondsPerSecond);
}

}  // namespace satsim::time
