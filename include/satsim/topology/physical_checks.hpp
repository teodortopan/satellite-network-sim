#pragma once

#include <cstddef>
#include <span>
#include <vector>

#include "satsim/constellation/classification.hpp"
#include "satsim/ground/sites.hpp"
#include "satsim/topology/geometry.hpp"
#include "satsim/topology/ground_links.hpp"
#include "satsim/topology/link_plan.hpp"
#include "satsim/topology/link_state.hpp"
#include "satsim/topology/positions.hpp"

namespace satsim::topology {

// The physical checks of spec 6.1 over one step's assembled links (all but the delay lower
// bound, which needs paths). Each is an independent recomputation from the classification, the
// positions and the sites, never a re-reading of a flag or value the builder set, so that a
// builder bug shows up as a count here.

/// Violations per rule at one step. Every count is 0 for a correct network.
struct PhysicalCheckCounts {
  /// Laser links marked up whose segment passes below the atmosphere layer (the minimum
  /// radius recomputed with a different formula from the builder's), or that have an end
  /// without a position.
  std::size_t line_of_sight = 0;
  /// Laser links marked up that are longer than the range cap (distance recomputed).
  std::size_t range_cap = 0;
  /// Satellites with more planned laser links than max_lasers_of(plan), the pattern's budget
  /// for a planned plan (counted from the link list itself).
  std::size_t laser_budget = 0;
  /// Laser links not recorded exactly once at each end (in LinkPlan::lasers) with matching
  /// sides, laser records naming a link that is out of range or does not have that satellite
  /// at that side, pairs of satellites linked more than once, and links joining a satellite
  /// to itself.
  std::size_t symmetry = 0;
  /// Laser links joining two shells (by the classification's shell_index; not counted when
  /// plan.cross_shell_links, a file's links), touching a satellite that is not a plane member,
  /// or whose recorded shell is not satellite_a's.
  std::size_t shells = 0;
  /// Ground links below their mask (elevation recomputed with a different formula from the
  /// builder's, mask recomputed from the shell's mean height and the site's latitude, or the
  /// override mask_settings.min_elevation_rad), beyond mask_settings.max_ground_range_km under
  /// the range rule (distance recomputed from the site's coordinates), to a satellite not in a
  /// plane, or to a satellite without a position.
  std::size_t mask = 0;
  /// The step's link states and the plan disagree in size, or the per-satellite tables do
  /// not cover the classification: nothing else can be trusted.
  std::size_t structure = 0;

  [[nodiscard]] std::size_t total() const noexcept {
    return line_of_sight + range_cap + laser_budget + symmetry + shells + mask + structure;
  }
};

/// Runs every check. `visible[i]` are the ground links of `sites[i]` (GroundVisibility::at);
/// pass empty spans to check lasers only. Elevations are compared with a tolerance of 1e-9 rad
/// and the line-of-sight radius and range cap with 1e-9 km, far below anything physical, so
/// that two correct formulas never disagree by rounding.
[[nodiscard]] PhysicalCheckCounts run_physical_checks(
    const constellation::Classification& classification, const LinkPlan& plan,
    const StepLinks& links, const StepPositions& positions, const LinkLimits& limits,
    std::span<const ground::GroundSite> sites,
    std::span<const std::vector<VisibleSatellite>> visible,
    const ElevationMaskSettings& mask_settings = {});

/// Violations per rule at one step of a run whose laser links change (Phase 4 spec 6.1, last
/// bullet). Every count is 0 for a correct network.
struct ForceCheckCounts {
  /// Lasers, identified by (satellite, side), in more than one link whose force is kInForce or
  /// kAcquiring: each link beyond the first at a laser counts once.
  std::size_t laser_reuse = 0;
  /// Repair links (index >= planned_count) in force or acquiring whose two ends are not both
  /// plane members of one shell (by the classification).
  std::size_t repair_shell = 0;
  /// Links whose state is up while one of their end satellites is down.
  std::size_t failed_satellite_link = 0;
  /// Links whose state is up without line of sight (the minimum radius recomputed with
  /// run_physical_checks' independent formula, with its 1e-9 km tolerance), or with an end
  /// without a position.
  std::size_t line_of_sight = 0;
  /// The spans disagree in size with the link list or the classification, or a link names a
  /// satellite out of range: nothing else can be trusted, and nothing else is counted.
  std::size_t structure = 0;

  [[nodiscard]] std::size_t total() const noexcept {
    return laser_reuse + repair_shell + failed_satellite_link + line_of_sight + structure;
  }
};

/// Checks one step of a run's fixed link list: `links` are the planned links (the first
/// `planned_count`) followed by every repair link of the run; `force[i]` and `states.states[i]`
/// are link i's force and state at the step (after apply_link_force); `satellite_down` is
/// non-zero for a satellite that is down, one per satellite of the classification, as are
/// `positions`. Only limits.atmosphere_km is used. The Phase 2 checks of run_physical_checks
/// are not repeated here: run them on the links in force.
[[nodiscard]] ForceCheckCounts run_force_checks(
    const constellation::Classification& classification, std::span<const PlannedLink> links,
    std::size_t planned_count, std::span<const LinkForce> force, const StepLinks& states,
    const StepPositions& positions, std::span<const char> satellite_down, const LinkLimits& limits);

}  // namespace satsim::topology
