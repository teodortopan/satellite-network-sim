#include "inputs/step_source.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "common/csv.hpp"
#include "common/steps.hpp"
#include "inputs/inputs.hpp"
#include "satsim/ground/sites.hpp"
#include "satsim/time/earth_orientation.hpp"
#include "satsim/time/utc_time.hpp"
#include "satsim/topology/ground_links.hpp"
#include "satsim/topology/positions.hpp"

namespace satsim::app {

namespace {

std::vector<ground::GroundSite> sites_of(const Inputs& inputs) {
  std::vector<ground::GroundSite> sites;
  sites.reserve(inputs.cities.size() + inputs.gateways.size());
  for (const ground::City& city : inputs.cities) {
    sites.push_back(city.site);
  }
  for (const GatewaySite& gateway : inputs.gateways) {
    sites.push_back(gateway.site);
  }
  return sites;
}

}  // namespace

StepSource::StepSource(const Inputs& inputs, const InputOptions& options,
                       std::size_t position_window, std::size_t full_window, AfterLinks after_links)
    : inputs_(&inputs),
      start_(options.start),
      step_ns_(options.step_ns),
      position_window_(std::max({position_window, full_window, std::size_t{1}})),
      full_window_(std::max(full_window, std::size_t{1})),
      after_links_(std::move(after_links)),
      sites_(sites_of(inputs)),
      city_count_(inputs.cities.size()),
      visibility_(sites_, inputs.working, options.mask),
      link_states_(inputs.plan, options.limits) {}

std::optional<std::string> StepSource::push_positions(std::int64_t step) {
  const time::UtcTime utc = time_of_step(start_, step_ns_, step);
  const std::optional<time::EarthOrientation> eop = inputs_->earth_orientation.at(utc);
  if (!eop) {  // prepare_inputs checked every step and the look-ahead.
    return "no Earth-orientation values for " + format_utc_iso(utc, inputs_->fraction_digits);
  }
  positions_.push_back(topology::positions_at(inputs_->propagators, utc, *eop));
  return std::nullopt;
}

void StepSource::push_full(std::size_t offset) {
  topology::StepPositions& positions = positions_[offset];
  Full full{.links = link_states_.step(positions), .visible = {}};
  if (after_links_) {
    after_links_(positions, full.links);
  }
  full.visible = visibility_.at(positions);
  full_.push_back(std::move(full));
}

std::optional<std::string> StepSource::advance() {
  if (index_ < 0) {
    index_ = 0;
    for (std::size_t k = 0; k < position_window_; ++k) {
      if (auto error = push_positions(static_cast<std::int64_t>(k))) {
        return error;
      }
    }
    for (std::size_t k = 0; k < full_window_; ++k) {
      push_full(k);
    }
    return std::nullopt;
  }
  ++index_;
  positions_.erase(positions_.begin());
  full_.erase(full_.begin());
  if (auto error = push_positions(index_ + static_cast<std::int64_t>(position_window_) - 1)) {
    return error;
  }
  push_full(full_window_ - 1);
  return std::nullopt;
}

}  // namespace satsim::app
