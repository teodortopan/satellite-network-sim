#pragma once

#include <Eigen/Core>

#include "satsim/time/earth_orientation.hpp"
#include "satsim/time/utc_time.hpp"

namespace satsim::frames {

/// Rotates a TEME position (the frame SGP4 outputs) into PEF (pseudo Earth-fixed):
/// r_pef = R3(gmst) * r_teme, a rotation about the z-axis by GMST (IAU 1982) at UT1.
///
/// PEF is Earth-fixed except for polar motion, the few metres the rotation axis wanders
/// across the Earth's surface; teme_to_ecef applies it. `ut1` is a UT1 reading (see
/// satsim::time::ut1_from_utc); passing UTC instead rotates the result by
/// 7.29e-5 rad/s * (UT1 - UTC), up to about 450 m at LEO radius (36 m in November 2026).
/// Position only; units are preserved (km in, km out).
[[nodiscard]] Eigen::Vector3d teme_to_pef(const Eigen::Vector3d& position_teme_km,
                                          const time::UtcTime& ut1) noexcept;

/// Applies polar motion: the ITRF position of a PEF position, given the pole offsets
/// x_p, y_p in radians. Vallado, Crawford, Hujsak & Kelso, "Revisiting Spacetrack Report
/// #3" (AIAA 2006-6753), eq. C-2, builds polar motion as W = ROT1(y_p) ROT2(x_p) with
/// r_PEF = W r_ITRF, where ROTi(a) are frame rotations (the axes turn by +a about axis i).
/// Hence r_ITRF = W^T r_PEF = ROT2(-x_p) ROT1(-y_p) r_PEF. These signs reproduce the paper's
/// printed r_ITRF to 13 mm (tests/frames/teme_ecef_test.cpp); flipping either sign moves it
/// by about 10 m.
///
/// The angles are below about 3e-6 rad, so this moves a position by at most |r| times the
/// pole offset: about 12 m at LEO radius in 2026. The IERS form W = R3(-s') R2(x_p) R1(y_p)
/// differs from this order by second-order terms (x_p * y_p ~ 1e-12 rad, under 0.01 mm) and
/// by s' (the TIO locator, -47 microarcseconds per century from J2000: 6e-11 rad in 2026,
/// 0.4 mm at LEO radius), both neglected, as Vallado's TEME convention does.
[[nodiscard]] Eigen::Vector3d pef_to_itrf(const Eigen::Vector3d& position_pef_km,
                                          double polar_x_rad, double polar_y_rad) noexcept;

/// Rotates a TEME position into the Earth-fixed frame ITRF, in km: GMST (IAU 1982) at
/// UT1 = utc + earth_orientation.ut1_minus_utc_s, then polar motion with the sample's
/// x_p, y_p. `earth_orientation` is the sample for `utc` (EarthOrientationTable::at).
///
/// This is Vallado's TEME to ITRF convention (AIAA 2006-6753, Appendix C). Position only: no
/// Earth-fixed velocity (it would need the omega x r term, and nothing needs it yet).
[[nodiscard]] Eigen::Vector3d teme_to_ecef(
    const Eigen::Vector3d& position_teme_km, const time::UtcTime& utc,
    const time::EarthOrientation& earth_orientation) noexcept;

}  // namespace satsim::frames
