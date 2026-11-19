#pragma once

#include <Eigen/Core>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "satsim/time/utc_time.hpp"
#include "satsim/topology/geometry.hpp"
#include "satsim/topology/link_plan.hpp"
#include "satsim/topology/positions.hpp"

namespace satsim::topology {

// Whether each planned laser link is up at a time step (spec 4.3, 4.5).

/// Limits on laser links (spec 4.5). SpaceX publishes none; these are parameters.
struct LinkLimits {
  /// Height of the layer a beam must clear above the WGS-84 equatorial radius, km.
  double atmosphere_km = kDefaultAtmosphereKm;
  /// A maximum link length, km. None by default (only line of sight limits the length).
  std::optional<double> range_cap_km = std::nullopt;
  /// When set, a link is down while the angle between the beam and the direction to the Sun,
  /// seen from either end, is below this angle (radians). Off by default. The Earth hiding the
  /// Sun is not modelled: a beam pointing at the Sun through the Earth is still excluded,
  /// which only errs on the side of more links down.
  std::optional<double> sun_exclusion_rad = std::nullopt;
  /// A link that becomes geometrically available carries nothing until it has been
  /// continuously available for this long, seconds. 0 by default (see LinkStateEvaluator).
  double lock_on_s = 0.0;
};

/// nullopt if usable, else what is wrong: atmosphere_km finite and >= 0, range_cap_km finite
/// and > 0, sun_exclusion_rad finite and in (0, pi], lock_on_s finite and >= 0.
[[nodiscard]] std::optional<std::string> check_limits(const LinkLimits& limits);

/// Why a planned link is down, in the order they are tested (the first that applies is the
/// one reported).
enum class LinkDownReason : std::uint8_t {
  kEndNotPropagated,  ///< One end has no position at this step (propagation failed).
  kNoLineOfSight,     ///< The segment passes below the atmosphere layer.
  kOverRangeCap,      ///< Longer than LinkLimits::range_cap_km.
  kSunExclusion,      ///< The Sun is within LinkLimits::sun_exclusion_rad of the beam.
  kLockingOn,         ///< Available, but not yet for LinkLimits::lock_on_s.
};

inline constexpr std::array<LinkDownReason, 5> kAllLinkDownReasons = {
    LinkDownReason::kEndNotPropagated, LinkDownReason::kNoLineOfSight,
    LinkDownReason::kOverRangeCap, LinkDownReason::kSunExclusion, LinkDownReason::kLockingOn};

/// "end_not_propagated", "no_line_of_sight", "over_range_cap", "sun_exclusion", "locking_on".
[[nodiscard]] std::string_view name_of(LinkDownReason reason) noexcept;

/// One planned link at one step.
struct LinkState {
  /// Distance between the two ends, km; nullopt exactly when an end has no position.
  std::optional<double> length_km = std::nullopt;
  /// nullopt when the link is up.
  std::optional<LinkDownReason> down_reason = std::nullopt;

  [[nodiscard]] bool up() const noexcept { return !down_reason.has_value(); }

  friend bool operator==(const LinkState&, const LinkState&) = default;
};

/// Every planned link at one step.
struct StepLinks {
  time::UtcTime utc;
  /// states[i] is LinkPlan::links[i].
  std::vector<LinkState> states;
};

/// Links up at the step.
[[nodiscard]] std::size_t count_up(const StepLinks& links) noexcept;

/// Links down at the step, indexed by LinkDownReason.
[[nodiscard]] std::array<std::size_t, 5> count_down_by_reason(const StepLinks& links) noexcept;

/// The geometric state of one link (everything but lock-on): kEndNotPropagated if an end has no
/// position, else kNoLineOfSight, kOverRangeCap or kSunExclusion, else up. `sun_direction_ecef`
/// is the unit vector to the Sun in the same frame; it is used only when
/// limits.sun_exclusion_rad is set; if it is nullopt then, the Sun is not tested (the
/// evaluator always passes it when the exclusion is on).
[[nodiscard]] LinkState geometric_link_state(
    const std::optional<Eigen::Vector3d>& a_ecef_km,
    const std::optional<Eigen::Vector3d>& b_ecef_km,
    const std::optional<Eigen::Vector3d>& sun_direction_ecef, const LinkLimits& limits) noexcept;

/// Steps every planned link through time, in time order, applying the lock-on rule.
///
/// Lock-on: a link is "available" at a step when geometric_link_state says it is up. Each
/// link remembers the step from which it has been available at every step since (its
/// "available since" time). At a step where it is available and its available-since time is
/// at least lock_on_s ago, it is up; available but more recent, it is down with kLockingOn;
/// not available, it is down with the geometric reason and its available-since time is
/// cleared.
///
/// Availability is known only at steps, so a link down at one step and available at the next
/// counts as available from that next step (not from some instant between them), and one
/// available at two consecutive steps counts as available between them.
///
/// The first step: nothing is known before it, so every link available at the first step
/// starts locking on at the first step. With lock_on_s > 0 no link is up at the first step;
/// with steps of 15 s and lock_on_s = 30 s, a link available from the first step is up from
/// the third. With lock_on_s = 0 every available link is up at once, and the evaluator's
/// result equals geometric_link_state's at every step.
class LinkStateEvaluator {
 public:
  /// Evaluates the links of `plan` (copied: the evaluator does not refer to `plan` later)
  /// with `limits` (precondition: check_limits(limits) is nullopt).
  LinkStateEvaluator(const LinkPlan& plan, const LinkLimits& limits);

  /// The state of every planned link at `positions.utc`. When the Sun exclusion is on, the
  /// Sun's direction is computed for this step (frames::sun_direction_ecef with
  /// positions.earth_orientation). Precondition: positions.utc is strictly later than the
  /// previous call's, and positions has one entry per satellite of the plan's classification.
  [[nodiscard]] StepLinks step(const StepPositions& positions);

 private:
  std::vector<PlannedLink> links_;
  LinkLimits limits_;
  std::vector<std::optional<time::UtcTime>> available_since_;
  std::optional<time::UtcTime> previous_utc_;
};

}  // namespace satsim::topology
