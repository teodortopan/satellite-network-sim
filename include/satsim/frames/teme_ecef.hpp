#pragma once

#include <Eigen/Core>

#include "satsim/time/utc_time.hpp"

namespace satsim::frames {

/// Rotates a TEME position (the frame SGP4 outputs) into the Earth-fixed frame:
/// r_ecef = R3(gmst) * r_teme, a rotation about the z-axis by GMST (IAU 1982) at `t`.
///
/// The result is what the literature calls PEF (pseudo Earth-fixed): polar motion, the few
/// metres the rotation axis wanders across the Earth's surface, is not applied. It is a tilt
/// of under about 0.6 arcsec (3e-6 rad), so neglecting it moves a position by up to roughly
/// 17 m at the surface and 20 m at LEO radius, far below SGP4's ~1 km accuracy.
/// `t` is used as UT1 (see satsim::time::gmst_rad for the size of passing UTC).
/// Position only; units are preserved (km in, km out).
[[nodiscard]] Eigen::Vector3d teme_to_ecef(const Eigen::Vector3d& position_teme_km,
                                           const time::UtcTime& t) noexcept;

}  // namespace satsim::frames
