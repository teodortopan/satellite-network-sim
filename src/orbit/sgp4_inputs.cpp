#include "sgp4_inputs.hpp"

#include <cmath>
#include <numbers>

#include "satsim/time/utc_time.hpp"

namespace satsim::orbit::detail {

namespace {

constexpr double kMinutesPerDay = 1440.0;

// Revolutions per day to radians per minute is a division by this number of minutes per
// radian, 1440 / (2*pi) = 229.18... Written exactly as Vallado's twoline2rv writes it
// (xpdotp), so a mean motion read from an OMM record gives the same double as the same value
// read from a TLE: the verification tests compare the two bit for bit.
constexpr double kMinutesPerDayPerRadian = kMinutesPerDay / (2.0 * std::numbers::pi);

// sgp4init counts its epoch in days from 1949 December 31 00:00 UT ("0 January 1950"),
// which is this Julian date.
constexpr double kSgp4EpochJd = 2433281.5;

}  // namespace

std::optional<Sgp4Inputs> sgp4_inputs(const ElementSet& elements) noexcept {
  const ElementSet& e = elements;
  if (!std::isfinite(e.mean_motion_rev_per_day) || !std::isfinite(e.eccentricity) ||
      !std::isfinite(e.inclination_rad) || !std::isfinite(e.raan_rad) ||
      !std::isfinite(e.arg_perigee_rad) || !std::isfinite(e.mean_anomaly_rad) ||
      !std::isfinite(e.bstar_per_earth_radius) || !std::isfinite(e.mean_motion_dot_rev_per_day2) ||
      !std::isfinite(e.mean_motion_ddot_rev_per_day3)) {
    return std::nullopt;
  }
  if (e.mean_motion_rev_per_day <= 0.0 || e.eccentricity < 0.0 || e.eccentricity >= 1.0 ||
      e.inclination_rad < 0.0 || e.inclination_rad > std::numbers::pi) {
    return std::nullopt;
  }
  const auto epoch = time::UtcTime::from_civil(e.epoch);
  if (!epoch) {
    return std::nullopt;
  }

  Sgp4Inputs in;
  in.epoch_jd_whole = epoch->jd_whole();
  in.epoch_jd_fraction = epoch->jd_fraction();
  // Deliberately the same expression as Vallado's twoline2rv: the two Julian date parts are
  // added first, which rounds the sum to about 5e-10 day (40 microseconds) near JD 2.46e6.
  // Near-Earth SGP4 (period under 225 minutes, all of Starlink) never uses this value, so the
  // rounding has no effect there. Deep-space SGP4 uses it for the Sun's and Moon's positions,
  // and mirroring the rounding is what makes the output match Vallado's published cases: the
  // unrounded (jd_whole - 2433281.5) + jd_fraction moves case 23333 (WIND) by 4.1e-6 km.
  // Time since epoch never goes through this number (see Propagator::propagate).
  in.epoch_days_since_1949_dec_31 = (in.epoch_jd_whole + in.epoch_jd_fraction) - kSgp4EpochJd;

  in.no_kozai_rad_per_min = e.mean_motion_rev_per_day / kMinutesPerDayPerRadian;
  // MEAN_MOTION_DOT and MEAN_MOTION_DDOT follow the TLE convention (half the first derivative,
  // one sixth of the second). twoline2rv converts them to radians and minutes without undoing
  // those factors; this mirrors it, so the record matches one built from a TLE. SGP4 itself
  // never reads either value.
  in.ndot_rad_per_min2 =
      e.mean_motion_dot_rev_per_day2 / (kMinutesPerDayPerRadian * kMinutesPerDay);
  in.nddot_rad_per_min3 =
      e.mean_motion_ddot_rev_per_day3 / (kMinutesPerDayPerRadian * kMinutesPerDay * kMinutesPerDay);
  in.bstar_per_earth_radius = e.bstar_per_earth_radius;
  in.eccentricity = e.eccentricity;
  in.inclination_rad = e.inclination_rad;
  in.raan_rad = e.raan_rad;
  in.arg_perigee_rad = e.arg_perigee_rad;
  in.mean_anomaly_rad = e.mean_anomaly_rad;
  return in;
}

}  // namespace satsim::orbit::detail
