#pragma once

// The matching of lasers that plan_laser_links (link_plan.cpp) and match_free_lasers
// (link_repair.cpp) share, so that a repair matches by exactly the planner's rule. Private to
// the topology module.

#include <Eigen/Core>
#include <cstddef>
#include <functional>
#include <optional>
#include <span>

#include "satsim/constellation/classification.hpp"

namespace satsim::topology::detail {

/// One end of a candidate link, as plain doubles: matching runs over every pair of two planes,
/// and Eigen expressions dominate the cost in unoptimised sanitizer builds.
struct MatchEnd {
  std::size_t satellite = 0;
  double x = 0.0;
  double y = 0.0;
  double z = 0.0;
  double tangent_km = 0.0;  ///< Tangent length to the limiting sphere (0 if inside it).
};

/// The end of `satellite` at `position_ecef_km`, for a limiting sphere of radius `limit_km`
/// (line_of_sight_radius_km).
[[nodiscard]] MatchEnd match_end(std::size_t satellite, const Eigen::Vector3d& position_ecef_km,
                                 double limit_km) noexcept;

/// The distance between two ends, km, if the segment between them has line of sight (it stays
/// at or above `limit_km` from the Earth's centre), else nullopt. Two ends farther apart than
/// the sum of their tangent lengths are rejected first without the exact test; the exact test
/// is segment_min_radius_km's (the point of the segment closest to the centre, clamped to it).
[[nodiscard]] std::optional<double> in_sight_distance_km(const MatchEnd& a, const MatchEnd& b,
                                                         double limit_km) noexcept;

/// Whether a link between satellites a and b keeps line of sight for a whole orbit
/// (keeps_line_of_sight_for_an_orbit, from their OrbitGeometry). A satellite without geometry
/// has no permanent link.
[[nodiscard]] bool permanent(const constellation::Classification& classification, std::size_t a,
                             std::size_t b, double atmosphere_km);

/// Greedy shortest-first matching of the lasers of plane `from` facing plane `to` with the
/// lasers of plane `to` facing back (Phase 2 spec 4.3):
/// - candidates are the pairs (a in from_members, b in to_members) with a != b, both with a
///   position (position_ecef_km; a satellite beyond its end has none), from_free(a),
///   to_free(b), !linked(a, b), and line of sight now (in_sight_distance_km);
/// - they are taken in increasing distance, ties by a's then b's catalogue number;
/// - a pair is accepted, by calling accept(a, b), if from_free(a), to_free(b) and !linked(a, b)
///   still hold and the link is permanent. accept is expected to change what the predicates
///   answer (the lasers are no longer free, the pair is linked).
/// Deterministic: depends only on the arguments.
void match_across_planes(const constellation::Classification& classification,
                         std::span<const std::optional<Eigen::Vector3d>> position_ecef_km,
                         double atmosphere_km, std::span<const std::size_t> from_members,
                         std::span<const std::size_t> to_members,
                         const std::function<bool(std::size_t)>& from_free,
                         const std::function<bool(std::size_t)>& to_free,
                         const std::function<bool(std::size_t, std::size_t)>& linked,
                         const std::function<void(std::size_t, std::size_t)>& accept);

}  // namespace satsim::topology::detail
