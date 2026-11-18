#pragma once

// Internal to satsim_orbit (not installed, not under include/): the conversion from an
// ElementSet to the numbers Vallado's sgp4init takes. Separate from propagator.cpp so the
// tests can check every converted value against what Vallado's own TLE reader produces.

#include <optional>

#include "satsim/orbit/element_set.hpp"

namespace satsim::orbit::detail {

/// The inputs of sgp4init, in SGP4's own units.
struct Sgp4Inputs {
  /// Epoch as days since 1949 December 31 00:00 UT (JD 2433281.5), sgp4init's `epoch`,
  /// computed and rounded exactly as Vallado's twoline2rv does (see sgp4_inputs.cpp).
  double epoch_days_since_1949_dec_31 = 0.0;
  /// Epoch as a two-part Julian date (whole day at 0h ending in .5, plus fraction of day), the
  /// elsetrec fields jdsatepoch and jdsatepochF.
  double epoch_jd_whole = 0.0;
  double epoch_jd_fraction = 0.0;

  double no_kozai_rad_per_min = 0.0;  ///< Kozai mean motion.
  double ndot_rad_per_min2 = 0.0;     ///< As twoline2rv stores it; SGP4 does not use it.
  double nddot_rad_per_min3 = 0.0;    ///< As twoline2rv stores it; SGP4 does not use it.
  double bstar_per_earth_radius = 0.0;
  double eccentricity = 0.0;
  double inclination_rad = 0.0;
  double raan_rad = 0.0;
  double arg_perigee_rad = 0.0;
  double mean_anomaly_rad = 0.0;
};

/// Converts and validates. Returns nullopt for the element sets Propagator::create rejects with
/// PropagationError::kInvalidElements: a non-finite number, mean motion not positive,
/// eccentricity outside [0, 1), inclination outside [0, pi], or an epoch that is not a valid
/// UTC date and time.
[[nodiscard]] std::optional<Sgp4Inputs> sgp4_inputs(const ElementSet& elements) noexcept;

}  // namespace satsim::orbit::detail
