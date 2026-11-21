#pragma once

#include <Eigen/Core>
#include <cstddef>
#include <optional>
#include <span>
#include <utility>
#include <vector>

#include "satsim/constellation/classification.hpp"
#include "satsim/topology/geometry.hpp"
#include "satsim/topology/link_plan.hpp"

namespace satsim::topology {

// Re-matching lasers that lost their partner during a run (Phase 4 spec 4.8, "In a plane" and
// "Across planes"). Which lasers are free, which satellites are down and which links are in
// force are the caller's business; this is the rule that pairs the free lasers at one step.

/// The state of the network at the step where free lasers are matched. Every span is indexed
/// by satellite of the classification.
struct RepairInput {
  /// Earth-fixed positions at the matching step; nullopt for a satellite without one.
  std::span<const std::optional<Eigen::Vector3d>> position_ecef_km;
  /// Non-zero for a satellite that is down (failed).
  std::span<const char> satellite_down;
  /// laser_free[s][k]: whether laser k of satellite s (LinkPlan::lasers[s][k]) is free, as the
  /// caller decides it: a laser whose link failed, or one that never had a partner.
  std::span<const std::vector<char>> laser_free;
  /// Pairs of satellites already joined by a link in force or acquiring, in either order. No
  /// repair link joins a pair listed here. A link that failed is neither, so the two ends of a
  /// failed link may be matched to each other again unless the caller lists their pair here.
  std::span<const std::pair<std::size_t, std::size_t>> linked;
  /// Line of sight now and for a whole orbit is required, as in the plan.
  double atmosphere_km = kDefaultAtmosphereKm;
};

/// The repair links made by matching the free lasers of `input`, under the sides, plane
/// neighbours and pattern of `plan` (the original plan of `classification`).
///
/// - A laser takes part only if it is marked free and its satellite is not down and has a
///   position. An entry missing from a span counts as not free, or as down. A laser is used at
///   most once in the result.
/// - Shell by shell in the plan's shell order; within a shell first in-plane, then cross-plane
///   (east lasers with west lasers), then second cross-plane (second_east with second_west)
///   where the pattern has those lasers: the planner's order.
/// - In a plane: round the plane's ring in index order over the members that are not down
///   (a member without a position stays in the ring; it simply has no laser that takes part).
///   For each such member i and the next such member j ahead of it, wrapping (so one dead
///   satellite or a run of several is bridged), with i != j: if i's ahead laser and j's behind
///   laser take part, i and j are not linked (in input.linked or by an earlier link of the
///   result), the segment has line of sight now (the planner's test, with atmosphere_km) and
///   the link keeps line of sight for an orbit (keeps_line_of_sight_for_an_orbit), the link
///   i -> j is made: satellite_a = i (kAhead), satellite_b = j (kBehind), kInPlane.
/// - Across planes, for each plane p in increasing p with its east neighbour q (then likewise
///   two planes east), from plan.neighbours: exactly the planner's matching (plan_laser_links,
///   Phase 2 spec 4.3) on the lasers that take part. Candidates are the pairs (a in p facing
///   east, b in q facing west) with line of sight now that are not linked; taken shortest
///   first, ties to a's then b's lower catalogue number, a pair is linked if both lasers are
///   still unused, the two are still not linked and the link is permanent. satellite_a is the
///   satellite in the western plane, as in the plan.
///
/// Ordered as made: by shell, then in-plane links (plane, then ring order), then cross-plane
/// links (western plane, then acceptance order), then second-cross-plane links. Deterministic:
/// depends only on the arguments.
[[nodiscard]] std::vector<PlannedLink> match_free_lasers(
    const constellation::Classification& classification, const LinkPlan& plan,
    const RepairInput& input);

}  // namespace satsim::topology
