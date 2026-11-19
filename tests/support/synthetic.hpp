#pragma once

// Synthetic constellations for the constellation and topology tests: ideal Walker shells as
// element sets, and the same orbit's element set at another epoch.

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "satsim/orbit/element_set.hpp"
#include "satsim/time/civil_time.hpp"
#include "satsim/time/utc_time.hpp"

namespace satsim::constellation::testing {

/// An ideal Walker delta shell: `planes` planes evenly spaced in RAAN, `per_plane` satellites
/// evenly spaced in each, satellites of neighbouring planes offset along the orbit by
/// phasing * 360 / (planes * per_plane) degrees.
struct WalkerShell {
  std::size_t planes = 1;
  std::size_t per_plane = 1;
  double inclination_deg = 53.0;
  /// Semi-major axis minus 6378.135 km (WGS-72 equatorial radius), from which the mean motion
  /// is computed with WGS-72's mu (Keplerian; SGP4 reads it as Kozai mean motion). The mean
  /// geodetic height SGP4 then gives differs by a few km (Earth's flattening).
  double height_km = 550.0;
  int phasing = 1;
  double first_raan_deg = 0.0;
  /// Catalogue number of plane 0, satellite 0; satellite s of plane p gets
  /// first_id + p * per_plane + s.
  std::int64_t first_id = 100000;
  /// OBJECT_ID of every satellite (a launch after the no-laser cutoff by default).
  std::string object_id = "2025-001A";
  time::CivilTime epoch{2026, 11, 18, 12, 0, 0.0};
};

/// The catalogue number of satellite `s` of plane `p`.
[[nodiscard]] std::int64_t walker_id(const WalkerShell& shell, std::size_t p, std::size_t s);

/// The shell's element sets: circular (eccentricity 1e-4), no drag (BSTAR 0), argument of
/// perigee 0, so the mean anomaly is the argument of latitude.
[[nodiscard]] std::vector<orbit::ElementSet> walker_element_sets(const WalkerShell& shell);

/// The element set of the same orbit at `new_epoch`: the mean anomaly, argument of perigee and
/// RAAN advanced by SGP4's own secular rates (read from Vallado's sgp4init). With BSTAR 0
/// these are SGP4's only secular terms, so both element sets give the same positions at any
/// time. Precondition: elements.bstar_per_earth_radius == 0 and a near-Earth orbit.
[[nodiscard]] orbit::ElementSet at_epoch(const orbit::ElementSet& elements,
                                         const time::CivilTime& new_epoch);

/// A UTC instant from an ISO text; aborts the test on a bad one.
[[nodiscard]] time::UtcTime utc(const time::CivilTime& civil);

}  // namespace satsim::constellation::testing
