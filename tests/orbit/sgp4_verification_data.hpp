#pragma once

// Test helper: reads Vallado's SGP4 verification cases (tests/orbit/data/SGP4-VER.TLE) and
// his published expected output (tests/orbit/data/tcppver.out), and turns a TLE into one of
// our ElementSets.

#include <array>
#include <string>
#include <vector>

#include "satsim/orbit/element_set.hpp"

namespace satsim::orbit::test {

/// One line of tcppver.out: minutes since epoch, then TEME position and velocity. The other
/// columns (osculating elements and a date) are ignored.
struct ExpectedRow {
  double minutes = 0.0;
  std::array<double, 3> position_km{};
  std::array<double, 3> velocity_km_s{};
};

/// One satellite of the verification file with its expected output.
struct VerificationCase {
  std::string line1;   ///< TLE line 1, without the line ending.
  std::string line2;   ///< TLE line 2 with the three trailing numbers, without the line ending.
  std::string satnum;  ///< Columns 3-7 of line 1, e.g. "00005".
  /// The trailing numbers on line 2: the time span tcppver.out covers, in minutes from epoch.
  double start_min = 0.0;
  double stop_min = 0.0;
  double step_min = 0.0;
  std::vector<ExpectedRow> rows;  ///< This satellite's block of tcppver.out, in file order.
};

/// Reads both files (paths relative to the repository root, the tests' working directory).
/// The i-th TLE goes with the i-th block of tcppver.out. Returns an empty vector, after adding
/// a GoogleTest failure, if either file is missing or malformed.
std::vector<VerificationCase> load_verification_cases();

/// The times at which Vallado's test driver (testcpp.cpp) evaluated this case, in order: first
/// the epoch, then from start to stop in steps, with the step accumulated in a double exactly
/// as the driver does and the last step clipped to stop. tcppver.out has one row per time,
/// until the first failed propagation.
std::vector<double> driver_times(const VerificationCase& c);

/// The epoch year of a TLE, four digits (two-digit years 57-99 are 19xx, 00-56 are 20xx).
int tle_epoch_year(const std::string& line1);

/// The epoch day of year with its fraction (1.0 is 1 January 00:00), as one double, the way
/// Vallado's twoline2rv reads it.
double tle_epoch_day_of_year(const std::string& line1);

/// The element set this TLE describes, in our units, read from the fixed TLE columns.
///
/// The epoch: a TLE gives a two-digit year and a day of year with exactly 8 decimals, i.e. a
/// multiple of 1e-8 day = 864 microseconds, a whole number of nanoseconds. It is built as a
/// UtcTime from integers (whole days, then the fraction as integer nanoseconds), so it is
/// exact, and stored as a CivilTime whose second is exact to the nanosecond; UtcTime's
/// from_civil reads it back to the same nanosecond.
///
/// BSTAR and the second derivative of mean motion are read the way Vallado's twoline2rv reads
/// them (signed mantissa, then multiplied by pow(10, exponent)), so both readers produce the
/// same double. All other numbers are read with from_chars, which, like sscanf, rounds
/// correctly.
ElementSet element_set_from_tle(const std::string& line1, const std::string& line2);

}  // namespace satsim::orbit::test
