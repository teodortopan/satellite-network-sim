#pragma once

#include <Eigen/Core>
#include <cstddef>

#include "satsim/time/utc_time.hpp"

namespace satsim::frames {

// The classical (FK5) models that connect TEME with the J2000 inertial frame: IAU 1976
// precession, IAU 1980 nutation, IAU 1980 mean obliquity. Every function takes TT, as a TT
// reading in a UtcTime (see satsim::time::tt_from_utc), and evaluates it from the two-part
// Julian date (jd_whole + jd_fraction), never one double. These follow the formulas of the
// IAU SOFA / ERFA routines Prec76, Nut80, Obl80, Pmat76 and Numat; the tests compare them
// with pyerfa values from python/gen_frame_reference.py.
//
// Matrix convention: frame rotations, applied to column vectors, mapping the earlier frame
// into the later one: r_mean_of_date = P r_J2000 and r_true_of_date = N r_mean_of_date.

/// IAU 1976 precession Euler angles from J2000.0 to the date (Lieske 1979, A&A 73, 282,
/// eqs. 6 and 7; the ERFA eraPrec76 form with the start epoch at J2000.0), in radians.
struct PrecessionAngles {
  double zeta_rad = 0.0;
  double theta_rad = 0.0;
  double z_rad = 0.0;
};

/// Nutation: the periodic wobble of the true pole about the mean pole, in radians.
struct Nutation {
  double longitude_rad = 0.0;  ///< Delta psi: nutation in longitude, along the ecliptic.
  double obliquity_rad = 0.0;  ///< Delta epsilon: nutation in obliquity.
};

/// Number of terms in the IAU 1980 nutation series.
inline constexpr std::size_t kNutationIau1980TermCount = 106;

/// The IAU 1976 precession angles at TT `tt`. Each is a cubic in T, the Julian centuries of
/// TT since J2000.0 (zeta and z grow by 2306.2181 arcsec per century, theta by 2004.3109).
[[nodiscard]] PrecessionAngles precession_iau1976(const time::UtcTime& tt) noexcept;

/// The IAU 1976 precession matrix P (J2000 mean equator and equinox to mean of date):
/// P = R3(-z) R2(theta) R3(-zeta). Equals eraPmat76.
[[nodiscard]] Eigen::Matrix3d precession_matrix_iau1976(const time::UtcTime& tt) noexcept;

/// Mean obliquity of the ecliptic, IAU 1980, in radians (Explanatory Supplement to the
/// Astronomical Almanac, 1992, expression 3.222-1):
/// 84381.448" - 46.8150" T - 0.00059" T^2 + 0.001813" T^3.
[[nodiscard]] double mean_obliquity_iau1980_rad(const time::UtcTime& tt) noexcept;

/// IAU 1980 nutation, the full 106-term series with its fundamental arguments (Explanatory
/// Supplement to the Astronomical Almanac, 1992, section 3.222; coefficients from ERFA
/// nut80.c). No celestial-pole corrections (the IERS dPsi, dEps) are applied: SGP4's TEME is
/// defined without them. They measure this model's error against the real pole: about
/// -0.054" and -0.006" in 2000, -0.12" and -0.009" (predicted) in November 2026. A d_psi
/// error tilts TEME to J2000 by d_psi sin(eps), so the 2026 values amount to about 1.6 m at
/// a 6928 km radius.
[[nodiscard]] Nutation nutation_iau1980(const time::UtcTime& tt) noexcept;

/// The IAU 1980 series truncated to its `term_count` largest terms by longitude amplitude
/// (at most 106; 106 equals nutation_iau1980 up to summation order). Production code uses
/// the full series. This exists to reproduce published examples computed with a truncated
/// series, as Vallado's software offers (4 terms are "most common", AIAA 2006-6753 section
/// II.D). Truncating to 4 terms changes d_psi by up to 0.32" and d_eps by up to 0.08"
/// (1950-2100), several metres at 6928 km.
[[nodiscard]] Nutation nutation_iau1980_largest_terms(const time::UtcTime& tt,
                                                      std::size_t term_count) noexcept;

/// The nutation matrix N (mean of date to true of date):
/// N = R1(-(eps_mean + d_eps)) R3(-d_psi) R1(eps_mean). Equals eraNumat.
[[nodiscard]] Eigen::Matrix3d nutation_matrix(double mean_obliquity_rad,
                                              const Nutation& nutation) noexcept;

/// The equation of the equinoxes as SGP4's TEME uses it, in radians: d_psi cos(eps_mean),
/// the angle along the true equator from the mean equinox (TEME's x-axis) to the true
/// equinox. Deliberately without the 1997 "kinematic" terms (0.00264" sin(Omega) +
/// 0.000063" sin(2 Omega)) of the IAU 1994 equation of the equinoxes: TEME predates them
/// (AIAA 2006-6753, Appendix C).
[[nodiscard]] double teme_equation_of_equinoxes_rad(double mean_obliquity_rad,
                                                    const Nutation& nutation) noexcept;

}  // namespace satsim::frames
