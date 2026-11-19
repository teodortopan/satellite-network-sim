#pragma once

#include <Eigen/Core>

#include "satsim/time/earth_orientation.hpp"
#include "satsim/time/utc_time.hpp"

namespace satsim::frames {

// The direction of the Sun from the Earth's centre, to about 0.01 degrees: enough to tell
// whether a laser beam points close to the Sun (Sun exclusion), not for astronomy.
//
// Formula: the Astronomical Almanac's low-precision formulas for the Sun (The Astronomical
// Almanac, section C, "Low precision formulas for the Sun"; also in the Explanatory Supplement
// to the Astronomical Almanac). With n = days of TT from J2000.0 (JD 2451545.0):
//   mean longitude, corrected for aberration   L = 280.460 deg + 0.9856474 deg * n
//   mean anomaly                               g = 357.528 deg + 0.9856003 deg * n
//   ecliptic longitude                         lambda = L + 1.915 deg sin g + 0.020 deg sin 2g
//   ecliptic latitude                          beta = 0
//   obliquity of the ecliptic                  epsilon = 23.439 deg - 0.0000004 deg * n
// The Almanac gives the precision as 0.01 deg between 1950 and 2050. Measured against ERFA
// (python/gen_sun_reference.py: the Earth's heliocentric position from erfa.epv00, aberrated
// with the Earth's barycentric velocity, precessed with IAU 1976), the angle error is at most
// 0.0106 deg over 1950-2050 and 0.011 deg in 2026.
//
// Frame: the longitude's rate includes general precession and its constant includes annual
// aberration but not nutation, so the result is the apparent direction referred to the mean
// equator and equinox of date (MOD); it agrees with ERFA best in that frame (0.0106 deg against
// 0.0144 deg in the true-of-date frame). Time argument: TT. The Almanac's n is a day count
// on a uniform scale; UT1 instead of TT would move the Sun by 69 s of its motion, under
// 0.001 deg.
//
// The Sun is about 23 000 Earth radii away, so the direction from any satellite differs from
// the direction from the Earth's centre by at most 7000 km / 1.5e8 km = 5e-5 rad (0.003 deg):
// one direction serves every satellite at one instant.

/// Unit vector from the Earth's centre to the Sun at TT `tt` (a TT reading, see
/// satsim::time::tt_from_utc), in the mean equator and equinox of date (MOD).
[[nodiscard]] Eigen::Vector3d sun_direction_mod(const time::UtcTime& tt) noexcept;

/// The same direction in TEME at TT `tt`: r_TEME = R3(eqe) N r_MOD, with the IAU 1980
/// nutation N and TEME's equation of the equinoxes eqe (see teme_j2000.hpp).
[[nodiscard]] Eigen::Vector3d sun_direction_teme(const time::UtcTime& tt) noexcept;

/// The same direction in the Earth-fixed frame (ITRF) at UTC `utc`: TT = UTC + 69.184 s for
/// the formula, then TEME to ITRF with teme_to_ecef (GMST at UT1 and polar motion from
/// `earth_orientation`, the sample for `utc`). A unit vector.
[[nodiscard]] Eigen::Vector3d sun_direction_ecef(
    const time::UtcTime& utc, const time::EarthOrientation& earth_orientation) noexcept;

}  // namespace satsim::frames
