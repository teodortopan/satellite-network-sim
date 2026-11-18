#pragma once

#include <optional>
#include <string_view>

namespace satsim::time {

/// A UTC calendar date and time of day, as written in data files and on the command line.
/// Plain data with no validation: converting it to an instant (UtcTime) is what checks that
/// the date exists.
struct CivilTime {
  int year = 0;         ///< Full year, e.g. 2026.
  int month = 0;        ///< 1 to 12.
  int day = 0;          ///< 1 to 31.
  int hour = 0;         ///< 0 to 23.
  int minute = 0;       ///< 0 to 59.
  double second = 0.0;  ///< Seconds including the fraction, 0 up to (not including) 60.

  friend bool operator==(const CivilTime&, const CivilTime&) = default;
};

/// Reads the ISO 8601 form `YYYY-MM-DDTHH:MM:SS` with an optional fraction of 1 to 9 digits
/// after a '.', e.g. "2026-11-18T21:00:00.25": the form of CelesTrak's EPOCH and of the
/// propagate program's --start. No zone designator: the time is UTC.
///
/// Checks the form only (every field is all digits, every separator is in its place), not
/// whether the date and time exist: "2026-13-45T99:99:99" is returned as read, and
/// UtcTime::from_civil is what rejects it. Returns nullopt for any other text, including
/// surrounding spaces. With at most 11 significant digits in the seconds, the double second is
/// within 1e-14 s of the text, so UtcTime::from_civil (which rounds to the nanosecond) gives
/// exactly the written instant.
[[nodiscard]] std::optional<CivilTime> parse_iso_civil_time(std::string_view text) noexcept;

}  // namespace satsim::time
