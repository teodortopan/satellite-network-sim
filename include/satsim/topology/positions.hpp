#pragma once

#include <Eigen/Core>
#include <cstddef>
#include <optional>
#include <span>
#include <vector>

#include "satsim/constellation/classification.hpp"
#include "satsim/orbit/element_set.hpp"
#include "satsim/orbit/propagator.hpp"
#include "satsim/time/earth_orientation.hpp"
#include "satsim/time/utc_time.hpp"

namespace satsim::topology {

// Satellite positions at one instant, in the Earth-fixed frame (ITRF). All link geometry of a
// time step (lasers and ground links) is computed from one StepPositions, so every distance
// and angle compares positions at the same instant in the same frame.
//
// Satellites are identified everywhere in this module by their index in
// constellation::Classification::satellites (ascending catalogue number).

/// Whether satellite `index` of `classification` is a member of a plane (has a
/// PlaneAssignment). Only plane members carry traffic: excluded satellites get no laser and no
/// ground link.
[[nodiscard]] bool is_plane_member(const constellation::Classification& classification,
                                   std::size_t index) noexcept;

/// One propagator per classified satellite (same index), built from the element set with the
/// same catalogue number, for plane members only; nullopt for every other satellite, for a
/// member whose element set is missing from `element_sets`, or whose propagator cannot be
/// built (classification would then already have excluded it). `element_sets` are the sets
/// the classification was made from, in any order.
[[nodiscard]] std::vector<std::optional<orbit::Propagator>> plane_member_propagators(
    const constellation::Classification& classification,
    std::span<const orbit::ElementSet> element_sets);

/// Positions of every satellite at one UTC instant.
struct StepPositions {
  time::UtcTime utc;
  /// The Earth-orientation sample used for TEME to ITRF (EarthOrientationTable::at(utc)). Kept
  /// so that anything else rotated to ITRF for this step (the Sun) uses the same values.
  time::EarthOrientation earth_orientation;
  /// One per satellite. Set when the satellite was propagated successfully; nullopt when it
  /// has no propagator (not asked for) or its propagation failed (then propagation_error says
  /// why). A failed propagation never gives a position.
  std::vector<std::optional<Eigen::Vector3d>> position_ecef_km;
  /// One per satellite: set exactly when propagation was attempted and failed.
  std::vector<std::optional<orbit::PropagationError>> propagation_error;
  /// Number of set propagation_error entries.
  std::size_t failed_count = 0;
};

/// Propagates every satellite that has a propagator to `utc` (SGP4, TEME) and rotates it to
/// ITRF with frames::teme_to_ecef and `earth_orientation` (the sample for `utc`).
[[nodiscard]] StepPositions positions_at(
    std::span<const std::optional<orbit::Propagator>> propagators, const time::UtcTime& utc,
    const time::EarthOrientation& earth_orientation);

}  // namespace satsim::topology
