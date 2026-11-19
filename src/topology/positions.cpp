#include "satsim/topology/positions.hpp"

#include <Eigen/Core>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

#include "satsim/constellation/classification.hpp"
#include "satsim/frames/teme_ecef.hpp"
#include "satsim/orbit/element_set.hpp"
#include "satsim/orbit/propagator.hpp"
#include "satsim/time/earth_orientation.hpp"
#include "satsim/time/utc_time.hpp"

namespace satsim::topology {

bool is_plane_member(const constellation::Classification& classification,
                     std::size_t index) noexcept {
  return index < classification.satellites.size() &&
         std::holds_alternative<constellation::PlaneAssignment>(
             classification.satellites[index].placement);
}

std::vector<std::optional<orbit::Propagator>> plane_member_propagators(
    const constellation::Classification& classification,
    std::span<const orbit::ElementSet> element_sets) {
  std::unordered_map<std::int64_t, const orbit::ElementSet*> by_id;
  by_id.reserve(element_sets.size());
  for (const orbit::ElementSet& set : element_sets) {
    by_id.emplace(set.norad_cat_id, &set);
  }
  std::vector<std::optional<orbit::Propagator>> propagators(classification.satellites.size());
  for (std::size_t i = 0; i < classification.satellites.size(); ++i) {
    if (!is_plane_member(classification, i)) {
      continue;
    }
    const auto found = by_id.find(classification.satellites[i].norad_cat_id);
    if (found == by_id.end()) {
      continue;
    }
    orbit::PropagatorOrError built = orbit::Propagator::create(*found->second);
    if (auto* propagator = std::get_if<orbit::Propagator>(&built)) {
      propagators[i].emplace(std::move(*propagator));
    }
  }
  return propagators;
}

StepPositions positions_at(std::span<const std::optional<orbit::Propagator>> propagators,
                           const time::UtcTime& utc,
                           const time::EarthOrientation& earth_orientation) {
  StepPositions step{
      .utc = utc,
      .earth_orientation = earth_orientation,
      .position_ecef_km = std::vector<std::optional<Eigen::Vector3d>>(propagators.size()),
      .propagation_error = std::vector<std::optional<orbit::PropagationError>>(propagators.size()),
      .failed_count = 0};
  for (std::size_t i = 0; i < propagators.size(); ++i) {
    const std::optional<orbit::Propagator>& propagator = propagators[i];
    if (!propagator.has_value()) {
      continue;
    }
    const orbit::StateOrError state = propagator->propagate(utc);
    if (const auto* teme = std::get_if<orbit::TemeState>(&state)) {
      step.position_ecef_km[i] = frames::teme_to_ecef(teme->position_km, utc, earth_orientation);
    } else {
      step.propagation_error[i] = std::get<orbit::PropagationError>(state);
      ++step.failed_count;
    }
  }
  return step;
}

}  // namespace satsim::topology
