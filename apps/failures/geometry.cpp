#include "failures/geometry.hpp"

#include <Eigen/Core>
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "common/csv.hpp"
#include "common/steps.hpp"
#include "inputs/inputs.hpp"
#include "satsim/frames/sun.hpp"
#include "satsim/ground/sites.hpp"
#include "satsim/time/earth_orientation.hpp"
#include "satsim/topology/ground_links.hpp"
#include "satsim/topology/link_state.hpp"
#include "satsim/topology/positions.hpp"

namespace satsim::failures_app {

namespace {

// Every city, then every gateway: app::StepSource's sites.
std::vector<ground::GroundSite> sites_of(const app::Inputs& inputs) {
  std::vector<ground::GroundSite> sites;
  sites.reserve(inputs.cities.size() + inputs.gateways.size());
  for (const ground::City& city : inputs.cities) {
    sites.push_back(city.site);
  }
  for (const app::GatewaySite& gateway : inputs.gateways) {
    sites.push_back(gateway.site);
  }
  return sites;
}

const std::optional<Eigen::Vector3d>& position_of(const StepGeometry& step, std::size_t satellite) {
  static const std::optional<Eigen::Vector3d> kNone;
  return satellite < step.position_ecef_km.size() ? step.position_ecef_km[satellite] : kNone;
}

}  // namespace

SnapshotGeometry::SnapshotGeometry(const app::Inputs& inputs, const app::InputOptions& options,
                                   std::size_t cache_steps)
    : inputs_(&inputs),
      start_(options.start),
      step_ns_(options.step_ns),
      limits_(options.limits),
      sites_(sites_of(inputs)),
      city_count_(inputs.cities.size()),
      visibility_(sites_, inputs.working, options.mask),
      cache_steps_(std::max(cache_steps, std::size_t{1})) {}

const SnapshotGeometry::Cached& SnapshotGeometry::cached(std::int64_t step) {
  const auto hit = std::find_if(cache_.begin(), cache_.end(),
                                [&](const std::unique_ptr<Cached>& c) { return c->step == step; });
  if (hit != cache_.end()) {
    std::rotate(hit, std::next(hit), cache_.end());
    return *cache_.back();
  }
  const time::UtcTime utc = app::time_of_step(start_, step_ns_, step);
  auto entry = std::make_unique<Cached>(
      Cached{.step = step,
             .geometry =
                 StepGeometry{
                     .utc = utc, .position_ecef_km = {}, .city_visible = {}, .gateway_visible = {}},
             .sun_direction_ecef = std::nullopt});
  const std::optional<time::EarthOrientation> eop = inputs_->earth_orientation.at(utc);
  if (!eop) {
    if (!error_) {
      error_ =
          "no Earth-orientation values for " + app::format_utc_iso(utc, inputs_->fraction_digits);
    }
    entry->geometry.position_ecef_km.assign(inputs_->propagators.size(), std::nullopt);
    entry->geometry.city_visible.assign(city_count_, {});
    entry->geometry.gateway_visible.assign(sites_.size() - city_count_, {});
  } else {
    topology::StepPositions positions = topology::positions_at(inputs_->propagators, utc, *eop);
    std::vector<std::vector<topology::VisibleSatellite>> visible = visibility_.at(positions);
    const auto split = visible.begin() + static_cast<std::ptrdiff_t>(city_count_);
    entry->geometry.city_visible.assign(std::make_move_iterator(visible.begin()),
                                        std::make_move_iterator(split));
    entry->geometry.gateway_visible.assign(std::make_move_iterator(split),
                                           std::make_move_iterator(visible.end()));
    entry->geometry.position_ecef_km = std::move(positions.position_ecef_km);
    if (limits_.sun_exclusion_rad) {
      entry->sun_direction_ecef = frames::sun_direction_ecef(utc, *eop);
    }
  }
  if (cache_.size() >= cache_steps_) {
    cache_.erase(cache_.begin());
  }
  cache_.push_back(std::move(entry));
  return *cache_.back();
}

const StepGeometry& SnapshotGeometry::at(std::int64_t step) { return cached(step).geometry; }

topology::LinkState SnapshotGeometry::link_state(std::int64_t step, std::size_t a, std::size_t b) {
  const Cached& c = cached(step);
  return topology::geometric_link_state(position_of(c.geometry, a), position_of(c.geometry, b),
                                        c.sun_direction_ecef, limits_);
}

}  // namespace satsim::failures_app
