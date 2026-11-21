#include "fake_geometry.hpp"

#include <gtest/gtest.h>

#include <Eigen/Core>
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <utility>
#include <variant>
#include <vector>

#include "satsim/frames/teme_ecef.hpp"
#include "satsim/orbit/propagator.hpp"
#include "satsim/time/earth_orientation.hpp"
#include "satsim/topology/ground_links.hpp"
#include "satsim/topology/link_state.hpp"
#include "support/synthetic.hpp"

namespace satsim::failures_app::testing {

time::UtcTime some_time() { return constellation::testing::utc({2026, 11, 18, 21, 15, 0.0}); }

topology::VisibleSatellite seen(std::size_t satellite, double slant_km) {
  return topology::VisibleSatellite{.satellite = satellite,
                                    .norad_cat_id = 1000 + static_cast<std::int64_t>(satellite),
                                    .shell_index = 0,
                                    .elevation_rad = 0.5,
                                    .slant_range_km = slant_km,
                                    .mask_rad = 0.3};
}

void FakeGeometry::down_between(std::size_t a, std::size_t b, std::int64_t from, std::int64_t to) {
  for (std::int64_t k = from; k < to; ++k) {
    states[{k, std::min(a, b), std::max(a, b)}] = topology::LinkState{
        .length_km = 9000.0, .down_reason = topology::LinkDownReason::kNoLineOfSight};
  }
}

void FakeGeometry::set_length(std::size_t a, std::size_t b, double km) {
  lengths_km[{std::min(a, b), std::max(a, b)}] = km;
}

const StepGeometry& FakeGeometry::at(std::int64_t step) {
  const auto it = steps.find(step);
  return it != steps.end() ? it->second : base;
}

topology::LinkState FakeGeometry::link_state(std::int64_t step, std::size_t a, std::size_t b) {
  const std::size_t lo = std::min(a, b);
  const std::size_t hi = std::max(a, b);
  if (const auto it = states.find({step, lo, hi}); it != states.end()) {
    return it->second;
  }
  const auto length = lengths_km.find({lo, hi});
  return topology::LinkState{.length_km = length != lengths_km.end() ? length->second : 1000.0,
                             .down_reason = std::nullopt};
}

IdealGeometry::IdealGeometry(const topology::testing::Ideal& ideal, const time::UtcTime& reference,
                             double step_s)
    : reference_(reference), step_s_(step_s) {
  for (const orbit::ElementSet& set : ideal.element_sets) {
    orbit::PropagatorOrError built = orbit::Propagator::create(set);
    if (auto* p = std::get_if<orbit::Propagator>(&built)) {
      propagators_.push_back(std::move(*p));
    } else {
      ADD_FAILURE() << "cannot propagate " << set.norad_cat_id;
    }
  }
}

const StepGeometry& IdealGeometry::at(std::int64_t step) {
  std::unique_ptr<StepGeometry>& entry = cache_[step];
  if (!entry) {
    entry = std::make_unique<StepGeometry>(
        StepGeometry{.utc = reference_.plus_seconds(static_cast<double>(step) * step_s_),
                     .position_ecef_km = {},
                     .city_visible = {},
                     .gateway_visible = {}});
    for (const orbit::Propagator& p : propagators_) {
      const orbit::StateOrError state = p.propagate(entry->utc);
      if (const auto* teme = std::get_if<orbit::TemeState>(&state)) {
        entry->position_ecef_km.emplace_back(
            frames::teme_to_ecef(teme->position_km, entry->utc, time::EarthOrientation{}));
      } else {
        entry->position_ecef_km.emplace_back(std::nullopt);
      }
    }
  }
  return *entry;
}

topology::LinkState IdealGeometry::link_state(std::int64_t step, std::size_t a, std::size_t b) {
  const StepGeometry& g = at(step);
  return topology::geometric_link_state(g.position_ecef_km.at(a), g.position_ecef_km.at(b),
                                        std::nullopt, topology::LinkLimits{});
}

}  // namespace satsim::failures_app::testing
