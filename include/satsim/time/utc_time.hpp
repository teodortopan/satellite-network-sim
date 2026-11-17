#pragma once

#include <compare>
#include <cstdint>
#include <optional>

#include "satsim/time/civil_time.hpp"

namespace satsim::time {

/// An instant on the UTC time scale.
///
/// Stored as a whole day (Modified Julian Date at 0h UTC) plus integer nanoseconds into that
/// day, never as one double Julian date: a double JD near 2.46e6 has a resolution of about
/// 40 microseconds, which is 0.3 m of satellite motion. Here the resolution is 1 ns for any
/// date, and comparisons are exact.
///
/// Every day has exactly 86400 s. Leap seconds are not represented: a simulated window is
/// assumed not to contain one (none has been inserted since the end of 2016).
class UtcTime {
 public:
  /// The instant a UTC calendar date and time names, in the proleptic Gregorian calendar.
  /// Returns nullopt if the date or time does not exist: year outside 1 to 9999, month
  /// outside 1 to 12, day outside the month (29 February only in leap years), hour outside
  /// 0 to 23, minute outside 0 to 59, or second not in [0, 60) (including NaN).
  /// A leap second (23:59:60) is deliberately rejected: simulated windows are assumed not to
  /// contain one. The second is rounded to the nearest nanosecond.
  [[nodiscard]] static std::optional<UtcTime> from_civil(const CivilTime& civil) noexcept;

  /// The Gregorian calendar date and time of this instant. The second is exact to the
  /// nanosecond and always strictly below 60.
  [[nodiscard]] CivilTime to_civil() const noexcept;

  /// Julian date at 0h UTC of this instant's day (always ends in .5). With jd_fraction()
  /// this is the two-part date SGP4 and Vallado's code use: JD = jd_whole() + jd_fraction().
  [[nodiscard]] double jd_whole() const noexcept;

  /// Fraction of the UTC day elapsed since 0h, in [0, 1).
  [[nodiscard]] double jd_fraction() const noexcept;

  /// This instant moved by `seconds` (negative moves back), rounded to the nearest
  /// nanosecond; crosses day, month and year boundaries with 86400 s per day.
  /// Precondition: `seconds` is finite and |seconds| <= 1e11 (about 3000 years).
  [[nodiscard]] UtcTime plus_seconds(double seconds) const noexcept;

  /// Seconds from `earlier` to this instant (negative if `earlier` is actually later).
  [[nodiscard]] double seconds_since(const UtcTime& earlier) const noexcept;

  friend constexpr std::strong_ordering operator<=>(const UtcTime&, const UtcTime&) = default;
  friend constexpr bool operator==(const UtcTime&, const UtcTime&) = default;

 private:
  UtcTime(std::int64_t mjd_day, std::int64_t nanoseconds_of_day) noexcept;

  // Member order matters: the defaulted comparisons are lexicographic, and
  // nanoseconds_of_day_ is always normalised to [0, 86400e9).
  std::int64_t mjd_day_;
  std::int64_t nanoseconds_of_day_;
};

}  // namespace satsim::time
