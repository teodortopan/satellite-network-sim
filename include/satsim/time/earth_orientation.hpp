#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <iosfwd>
#include <optional>
#include <string>
#include <vector>

#include "satsim/time/utc_time.hpp"

namespace satsim::time {

/// Earth orientation parameters (EOP) at one instant: what the published daily
/// measurements say about the Earth's rotation angle and the position of its rotation axis.
struct EarthOrientation {
  /// UT1 - UTC in seconds: how far the Earth's rotation is ahead of UTC (|value| < 0.9 s).
  /// Ignoring it rotates Earth-fixed positions by 7.29e-5 rad/s times this value: about
  /// 36 m at a 6928 km orbit radius for the -0.0715 s of November 2026.
  double ut1_minus_utc_s = 0.0;
  /// Polar motion x: the rotation pole's offset from the ITRF pole along the Greenwich
  /// meridian, in radians (published in arcseconds; about 0.1" to 0.3", i.e. 1.6e-6 rad).
  double polar_x_rad = 0.0;
  /// Polar motion y: the same offset along the 90 deg west meridian, in radians.
  double polar_y_rad = 0.0;
};

/// The daily Earth orientation values of an IERS `finals2000A` file (IERS Rapid Service /
/// Prediction Centre, https://maia.usno.navy.mil/ser7/readme.finals2000A), interpolated in
/// time. The file holds measured values followed by predictions about a year ahead; this
/// table keeps both, and stops at the first row whose Bulletin A values are not all given.
///
/// Rows are at 0h UTC of consecutive days. Lookups interpolate linearly between the two
/// rows that bracket the instant. Size of that approximation, measured on the 2020-2027 rows
/// of the file of 2026-11-18 as linear minus 4-point cubic interpolation at mid-day: at most
/// 49 us of UT1 - UTC (2.5 cm at a 6928 km orbit radius) and 0.11 mas of polar motion
/// (4 mm). Sub-daily (tidal) variations, which the daily values do not contain, are not
/// modelled either.
class EarthOrientationTable {
 public:
  /// The values at a UTC instant, or nullopt when they are not available:
  ///  - before the first row or after the last one: never extrapolated;
  ///  - strictly between two rows across which UT1 - UTC jumps by more than 0.5 s. That jump
  ///    is a leap second: UTC stopped for one second during the first row's day, so
  ///    interpolating UT1 - UTC across it would be wrong by up to 1 s (about 460 m at the
  ///    equator). The project assumes a simulated window contains no leap second; none has
  ///    been inserted after 2016-12-31. The rows themselves, at 0h UTC, are still returned.
  [[nodiscard]] std::optional<EarthOrientation> at(const UtcTime& utc) const noexcept;

  /// Number of daily rows.
  [[nodiscard]] std::size_t row_count() const noexcept { return rows_.size(); }

  /// Modified Julian Date of the first row (0h UTC of that day). Precondition: row_count() > 0,
  /// which every table returned by load_earth_orientation satisfies.
  [[nodiscard]] std::int64_t first_mjd() const noexcept { return first_mjd_; }

  /// Modified Julian Date of the last row. Same precondition as first_mjd().
  [[nodiscard]] std::int64_t last_mjd() const noexcept {
    return first_mjd_ + static_cast<std::int64_t>(rows_.size()) - 1;
  }

 private:
  friend struct EarthOrientationLoader;

  std::int64_t first_mjd_ = 0;
  std::vector<EarthOrientation> rows_;  // rows_[i] is at MJD first_mjd_ + i, 0h UTC.
};

/// The outcome of loading a finals2000A file: either a non-empty table or an error message.
struct EarthOrientationLoadResult {
  /// Set on success. Holds at least one row.
  std::optional<EarthOrientationTable> table;
  /// Set on failure: why the file could not be used, naming the line (from 1) at fault.
  std::optional<std::string> error;
};

/// Reads the fixed-width `finals2000A` format. Per row (1-based columns, from the readme):
/// MJD in 8-15, Bulletin A polar motion x in 19-27 and y in 38-46 (arcseconds), and
/// Bulletin A UT1 - UTC in 59-68 (seconds). Other columns are ignored, and a trailing
/// carriage return is accepted.
///
/// Reading stops, without error, at the first row (or end of input) where any of the three
/// values is blank: the end of the predictions. The whole load fails, and no table is
/// returned, if: a field is non-blank but not a number; the MJD is missing, not a whole
/// day, or not exactly one day after the previous row's; a value is outside a plausible
/// range (|x|, |y| > 2 arcseconds or |UT1 - UTC| >= 1 s); or no row has all three values
/// (including an empty file). Never throws.
[[nodiscard]] EarthOrientationLoadResult load_earth_orientation(std::istream& in) noexcept;

/// Opens path and loads it as above; errors are prefixed with the path.
[[nodiscard]] EarthOrientationLoadResult load_earth_orientation(
    const std::filesystem::path& path) noexcept;

}  // namespace satsim::time
