#pragma once

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

}  // namespace satsim::time
