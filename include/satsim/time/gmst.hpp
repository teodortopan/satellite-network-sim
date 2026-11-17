#pragma once

#include "satsim/time/utc_time.hpp"

namespace satsim::time {

/// Greenwich Mean Sidereal Time in radians, in [0, 2*pi): the angle the Earth has turned
/// between the mean equinox and the Greenwich meridian. IAU 1982 model (Aoki et al. 1982;
/// the formula Vallado's SGP4 companion code calls gstime). TEME is defined against this
/// angle, so it is the one to use for TEME to Earth-fixed, not a newer sidereal-time model.
///
/// The argument is formally UT1. Callers currently pass UTC, since |UT1 - UTC| < 0.9 s.
/// The Earth turns at 7.292115e-5 rad/s, so that approximation rotates the result by up to
/// 6.6e-5 rad: about 0.5 km/s * 0.9 s, i.e. up to roughly 450 m along-track at a 6928 km
/// orbit radius (550 km altitude), and correspondingly less for today's smaller |UT1 - UTC|.
[[nodiscard]] double gmst_rad(const UtcTime& ut1) noexcept;

}  // namespace satsim::time
