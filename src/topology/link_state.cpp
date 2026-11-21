#include "satsim/topology/link_state.hpp"

#include <Eigen/Core>
#include <algorithm>
#include <array>
#include <cassert>
#include <cmath>
#include <cstddef>
#include <numbers>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "satsim/frames/sun.hpp"
#include "satsim/time/utc_time.hpp"
#include "satsim/topology/geometry.hpp"
#include "satsim/topology/link_plan.hpp"
#include "satsim/topology/positions.hpp"

namespace satsim::topology {

namespace {

// Whether the unit vector `sun` is within `angle_rad` of the direction of `beam` (not
// normalised): cos(angle between) > cos(angle_rad). "Below the set angle" is strict.
bool sun_within(const Eigen::Vector3d& beam, const Eigen::Vector3d& sun, double angle_rad) {
  const double length = beam.norm();
  return sun.dot(beam) > std::cos(angle_rad) * length;
}

const std::optional<Eigen::Vector3d>& position_of(const StepPositions& positions,
                                                  std::size_t satellite) {
  static const std::optional<Eigen::Vector3d> kNone;
  return satellite < positions.position_ecef_km.size() ? positions.position_ecef_km[satellite]
                                                       : kNone;
}

}  // namespace

std::optional<std::string> check_limits(const LinkLimits& limits) {
  if (!std::isfinite(limits.atmosphere_km) || limits.atmosphere_km < 0.0) {
    return "atmosphere_km must be finite and at least 0";
  }
  if (limits.range_cap_km &&
      (!std::isfinite(*limits.range_cap_km) || *limits.range_cap_km <= 0.0)) {
    return "range_cap_km must be finite and positive";
  }
  if (limits.sun_exclusion_rad &&
      (!std::isfinite(*limits.sun_exclusion_rad) || *limits.sun_exclusion_rad <= 0.0 ||
       *limits.sun_exclusion_rad > std::numbers::pi)) {
    return "sun_exclusion_rad must be in (0, pi]";
  }
  if (!std::isfinite(limits.lock_on_s) || limits.lock_on_s < 0.0) {
    return "lock_on_s must be finite and at least 0";
  }
  return std::nullopt;
}

std::string_view name_of(LinkDownReason reason) noexcept {
  switch (reason) {
    case LinkDownReason::kEndNotPropagated:
      return "end_not_propagated";
    case LinkDownReason::kNoLineOfSight:
      return "no_line_of_sight";
    case LinkDownReason::kOverRangeCap:
      return "over_range_cap";
    case LinkDownReason::kSunExclusion:
      return "sun_exclusion";
    case LinkDownReason::kLockingOn:
      return "locking_on";
    case LinkDownReason::kFailed:
      return "failed";
    case LinkDownReason::kAcquiring:
      return "acquiring";
    case LinkDownReason::kNotInForce:
      return "not_in_force";
  }
  return "?";
}

std::string_view name_of(LinkForce force) noexcept {
  switch (force) {
    case LinkForce::kInForce:
      return "in_force";
    case LinkForce::kAcquiring:
      return "acquiring";
    case LinkForce::kFailed:
      return "failed";
    case LinkForce::kNotInForce:
      return "not_in_force";
  }
  return "?";
}

std::size_t count_up(const StepLinks& links) noexcept {
  std::size_t up = 0;
  for (const LinkState& state : links.states) {
    if (state.up()) {
      ++up;
    }
  }
  return up;
}

std::array<std::size_t, 5> count_down_by_reason(const StepLinks& links) noexcept {
  std::array<std::size_t, 5> counts{};
  for (const LinkState& state : links.states) {
    // The reasons of failures and repair links come after the five geometric ones and are
    // left out.
    if (state.down_reason && static_cast<std::size_t>(*state.down_reason) < counts.size()) {
      ++counts[static_cast<std::size_t>(*state.down_reason)];
    }
  }
  return counts;
}

std::array<std::size_t, 8> count_down_by_reason_with_failures(const StepLinks& links) noexcept {
  std::array<std::size_t, 8> counts{};
  for (const LinkState& state : links.states) {
    if (state.down_reason && static_cast<std::size_t>(*state.down_reason) < counts.size()) {
      ++counts[static_cast<std::size_t>(*state.down_reason)];
    }
  }
  return counts;
}

void apply_link_force(StepLinks& links, std::span<const LinkForce> force) noexcept {
  assert(force.size() == links.states.size());
  const std::size_t count = std::min(force.size(), links.states.size());
  for (std::size_t i = 0; i < count; ++i) {
    LinkState& state = links.states[i];
    switch (force[i]) {
      case LinkForce::kInForce:
        break;
      case LinkForce::kAcquiring:
        if (state.up()) {
          state.down_reason = LinkDownReason::kAcquiring;
        }
        break;
      case LinkForce::kFailed:
        state.down_reason = LinkDownReason::kFailed;
        break;
      case LinkForce::kNotInForce:
        state.down_reason = LinkDownReason::kNotInForce;
        break;
    }
  }
}

LinkState geometric_link_state(const std::optional<Eigen::Vector3d>& a_ecef_km,
                               const std::optional<Eigen::Vector3d>& b_ecef_km,
                               const std::optional<Eigen::Vector3d>& sun_direction_ecef,
                               const LinkLimits& limits) noexcept {
  if (!a_ecef_km || !b_ecef_km) {
    return LinkState{.length_km = std::nullopt, .down_reason = LinkDownReason::kEndNotPropagated};
  }
  const Eigen::Vector3d beam = *b_ecef_km - *a_ecef_km;
  const double length_km = beam.norm();
  LinkState state{.length_km = length_km, .down_reason = std::nullopt};
  if (!has_line_of_sight(*a_ecef_km, *b_ecef_km, limits.atmosphere_km)) {
    state.down_reason = LinkDownReason::kNoLineOfSight;
  } else if (limits.range_cap_km && length_km > *limits.range_cap_km) {
    state.down_reason = LinkDownReason::kOverRangeCap;
  } else if (limits.sun_exclusion_rad && sun_direction_ecef &&
             (sun_within(beam, *sun_direction_ecef, *limits.sun_exclusion_rad) ||
              sun_within(-beam, *sun_direction_ecef, *limits.sun_exclusion_rad))) {
    state.down_reason = LinkDownReason::kSunExclusion;
  }
  return state;
}

LinkStateEvaluator::LinkStateEvaluator(const LinkPlan& plan, const LinkLimits& limits)
    : links_(plan.links), limits_(limits), available_since_(plan.links.size()) {}

StepLinks LinkStateEvaluator::step(const StepPositions& positions) {
  assert(!previous_utc_ || positions.utc > *previous_utc_);
  previous_utc_ = positions.utc;
  std::optional<Eigen::Vector3d> sun;
  if (limits_.sun_exclusion_rad) {
    sun = frames::sun_direction_ecef(positions.utc, positions.earth_orientation);
  }
  StepLinks result{.utc = positions.utc, .states = {}};
  result.states.reserve(links_.size());
  for (std::size_t i = 0; i < links_.size(); ++i) {
    LinkState state =
        geometric_link_state(position_of(positions, links_[i].satellite_a),
                             position_of(positions, links_[i].satellite_b), sun, limits_);
    std::optional<time::UtcTime>& since = available_since_[i];
    if (!state.up()) {
      since.reset();
    } else {
      if (!since) {
        since = positions.utc;
      }
      if (positions.utc.seconds_since(*since) < limits_.lock_on_s) {
        state.down_reason = LinkDownReason::kLockingOn;
      }
    }
    result.states.push_back(state);
  }
  return result;
}

}  // namespace satsim::topology
