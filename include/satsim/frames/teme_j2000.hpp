#pragma once

#include <Eigen/Core>

#include "satsim/frames/precession_nutation.hpp"
#include "satsim/time/utc_time.hpp"

namespace satsim::frames {

// TEME to J2000 (FK5): the mean equator and equinox of J2000.0 of the classical IAU 1976 /
// 1980 theory. It is not GCRF/ICRF: those differ from it by the frame bias (about 0.02",
// fixed) plus this nutation model's error (the IERS celestial-pole offsets, about 0.12" in
// d_psi in 2026; that alone moves a position by up to 1.6 m at 6928 km). Together that is
// 1 to 1.6 m at Starlink radius, measured against Skyfield's GCRS. A GCRF position needs the
// IAU 2006/2000A chain instead.
//
// Chain (frame rotations acting on column vectors, all evaluated at TT `tt`):
//   r_TOD  = R3(-eqe) r_TEME       true equator and equinox of date; eqe is the TEME
//                                  equation of the equinoxes, d_psi cos(eps_mean)
//   r_TOD  = N r_MOD               N = nutation_matrix (mean of date to true of date)
//   r_MOD  = P r_J2000             P = precession_matrix_iau1976 (J2000 to mean of date)
// so r_J2000 = P^T N^T R3(-eqe) r_TEME, and r_TEME = R3(eqe) N P r_J2000.
//
// TEME "of date": the rotation is evaluated at the instant of the vector, as Vallado et al.
// recommend (AIAA 2006-6753, section II.D).
//
// Both frames are inertial (non-rotating apart from precession and nutation), so a velocity
// is rotated with the same matrix as a position. The matrix itself changes by at most
// 1.1e-11 rad/s (precession plus nutation, measured over 2026-2027), which would add under
// 8e-8 km/s at 7000 km; that term is neglected, as in Vallado's example (AIAA 2006-6753,
// Appendix C), whose J2000 velocity the tests reproduce.

/// The rotation matrix M with v_J2000 = M v_TEME at TT `tt`, using the full IAU 1980
/// nutation series. Orthonormal; its inverse is its transpose.
[[nodiscard]] Eigen::Matrix3d teme_to_j2000_matrix(const time::UtcTime& tt) noexcept;

/// As above with a given nutation, for reproducing published examples computed with a
/// truncated series (see nutation_iau1980_largest_terms).
[[nodiscard]] Eigen::Matrix3d teme_to_j2000_matrix(const time::UtcTime& tt,
                                                   const Nutation& nutation) noexcept;

/// A TEME position (km) or velocity (km/s) at TT `tt`, expressed in J2000 (FK5).
[[nodiscard]] Eigen::Vector3d teme_to_j2000(const Eigen::Vector3d& vector_teme,
                                            const time::UtcTime& tt) noexcept;

/// A J2000 (FK5) position (km) or velocity (km/s), expressed in TEME of date at TT `tt`.
/// Inverse of teme_to_j2000.
[[nodiscard]] Eigen::Vector3d j2000_to_teme(const Eigen::Vector3d& vector_j2000,
                                            const time::UtcTime& tt) noexcept;

}  // namespace satsim::frames
