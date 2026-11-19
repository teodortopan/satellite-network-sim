#include "satsim/frames/sun.hpp"

#include <Eigen/Core>
#include <cmath>
#include <numbers>

#include "rotation.hpp"
#include "satsim/frames/precession_nutation.hpp"
#include "satsim/frames/teme_ecef.hpp"
#include "satsim/time/earth_orientation.hpp"
#include "satsim/time/time_scales.hpp"
#include "satsim/time/utc_time.hpp"

namespace satsim::frames {

namespace {

constexpr double kRadPerDeg = std::numbers::pi / 180.0;
constexpr double kJ2000JulianDate = 2451545.0;

}  // namespace

Eigen::Vector3d sun_direction_mod(const time::UtcTime& tt) noexcept {
  // Days of TT since J2000.0, from the two-part Julian date (the whole part first, so the
  // difference is exact).
  const double n = (tt.jd_whole() - kJ2000JulianDate) + tt.jd_fraction();
  const double mean_longitude_rad = (280.460 + 0.9856474 * n) * kRadPerDeg;
  const double mean_anomaly_rad = (357.528 + 0.9856003 * n) * kRadPerDeg;
  const double ecliptic_longitude_rad =
      mean_longitude_rad +
      (1.915 * std::sin(mean_anomaly_rad) + 0.020 * std::sin(2.0 * mean_anomaly_rad)) * kRadPerDeg;
  const double obliquity_rad = (23.439 - 0.0000004 * n) * kRadPerDeg;
  // Ecliptic (lambda, beta = 0) to equatorial: a rotation by -epsilon about the x axis.
  const double sin_longitude = std::sin(ecliptic_longitude_rad);
  return {std::cos(ecliptic_longitude_rad), std::cos(obliquity_rad) * sin_longitude,
          std::sin(obliquity_rad) * sin_longitude};
}

Eigen::Vector3d sun_direction_teme(const time::UtcTime& tt) noexcept {
  const double mean_obliquity = mean_obliquity_iau1980_rad(tt);
  const Nutation nutation = nutation_iau1980(tt);
  // r_TOD = N r_MOD and r_TOD = R3(-eqe) r_TEME (teme_j2000.hpp), so r_TEME = R3(eqe) N r_MOD.
  return detail::rot3(teme_equation_of_equinoxes_rad(mean_obliquity, nutation)) *
         (nutation_matrix(mean_obliquity, nutation) * sun_direction_mod(tt));
}

Eigen::Vector3d sun_direction_ecef(const time::UtcTime& utc,
                                   const time::EarthOrientation& earth_orientation) noexcept {
  return teme_to_ecef(sun_direction_teme(time::tt_from_utc(utc)), utc, earth_orientation);
}

}  // namespace satsim::frames
