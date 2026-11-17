#pragma once

#include <cstdint>
#include <string>

#include "satsim/time/civil_time.hpp"

namespace satsim::orbit {

/// One GP element set (one OMM record), converted to engine units. These are SGP4 mean
/// elements: they only give correct positions through SGP4, never through a two-body formula.
struct ElementSet {
  std::string object_name;        ///< OBJECT_NAME, e.g. "STARLINK-1008". Empty if absent.
  std::string object_id;          ///< OBJECT_ID, the international designator. Empty if absent.
  std::int64_t norad_cat_id = 0;  ///< NORAD_CAT_ID, 1 to 999999999 (OMM allows nine digits).

  time::CivilTime epoch;  ///< EPOCH, UTC.

  double mean_motion_rev_per_day = 0.0;  ///< MEAN_MOTION (Kozai mean motion, as SGP4 expects).
  double eccentricity = 0.0;             ///< ECCENTRICITY, in [0, 1).
  double inclination_rad = 0.0;          ///< INCLINATION, in [0, pi].
  double raan_rad = 0.0;                 ///< RA_OF_ASC_NODE, as published (normally [0, 2*pi)).
  double arg_perigee_rad = 0.0;          ///< ARG_OF_PERICENTER, as published.
  double mean_anomaly_rad = 0.0;         ///< MEAN_ANOMALY, as published.
  double bstar_per_earth_radius = 0.0;   ///< BSTAR drag term; may be negative.

  /// MEAN_MOTION_DOT and MEAN_MOTION_DDOT as published. They follow the TLE convention:
  /// the first is half the first derivative of mean motion and the second one sixth of the
  /// second derivative. SGP4 does not use them to propagate. Default 0 when absent.
  double mean_motion_dot_rev_per_day2 = 0.0;
  double mean_motion_ddot_rev_per_day3 = 0.0;

  friend bool operator==(const ElementSet&, const ElementSet&) = default;
};

}  // namespace satsim::orbit
