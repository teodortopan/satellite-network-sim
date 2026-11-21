#include "fake.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <numbers>
#include <span>
#include <utility>
#include <vector>

#include "routing/engine.hpp"
#include "routing/graph.hpp"
#include "satsim/time/utc_time.hpp"
#include "satsim/topology/ground_links.hpp"
#include "satsim/topology/link_plan.hpp"
#include "satsim/topology/link_state.hpp"

namespace satsim::routing_app::testing {

time::UtcTime some_time() {
  const auto t = time::UtcTime::from_civil({2026, 11, 18, 21, 15, 0.0});
  if (!t) {
    ADD_FAILURE() << "bad time";
    std::abort();
  }
  return *t;
}

topology::VisibleSatellite seen(std::size_t satellite, double elevation_deg, double slant_km) {
  return topology::VisibleSatellite{.satellite = satellite,
                                    .norad_cat_id = 1000 + static_cast<std::int64_t>(satellite),
                                    .shell_index = 0,
                                    .elevation_rad = elevation_deg * std::numbers::pi / 180.0,
                                    .slant_range_km = slant_km,
                                    .mask_rad = 0.0};
}

topology::VisibleSatellite seen_in(std::size_t shell, std::size_t satellite, double elevation_deg,
                                   double slant_km) {
  topology::VisibleSatellite v = seen(satellite, elevation_deg, slant_km);
  v.shell_index = shell;
  return v;
}

topology::LinkState up(double length_km) {
  return topology::LinkState{.length_km = length_km, .down_reason = std::nullopt};
}

topology::LinkState down(topology::LinkDownReason reason) {
  return topology::LinkState{.length_km = std::nullopt, .down_reason = reason};
}

topology::LinkPlan plan_of(std::span<const std::pair<std::size_t, std::size_t>> links) {
  topology::LinkPlan plan;
  for (const auto& [a, b] : links) {
    plan.links.push_back(topology::PlannedLink{.satellite_a = a, .satellite_b = b});
  }
  return plan;
}

FakeWorld::FakeWorld(std::vector<FakeStep> steps, double step_s, std::size_t known_steps)
    : steps_(std::move(steps)), step_s_(step_s), known_steps_(known_steps) {
  for (const FakeStep& s : steps_) {
    links_.push_back(topology::StepLinks{.utc = some_time(), .states = s.links});
  }
}

std::size_t FakeWorld::View::known_steps() const {
  return std::min(world_->known_steps_, world_->steps_.size() - k_);
}

StepGround FakeWorld::View::step(std::size_t offset) const {
  const FakeStep& s = world_->steps_.at(k_ + offset);
  return StepGround{.links = &world_->links_.at(k_ + offset),
                    .city_visible = s.cities,
                    .gateway_visible = s.gateways};
}

std::vector<double> FakeWorld::View::times_left_in_view_s(
    std::size_t site, std::span<const topology::VisibleSatellite> visible, double horizon_s) const {
  const std::size_t n = topology::view_steps(world_->step_s_, horizon_s);
  std::vector<double> times;
  for (const topology::VisibleSatellite& v : visible) {
    std::size_t j = 0;
    for (; j < n; ++j) {
      const std::size_t k = k_ + j;
      if (k >= world_->steps_.size()) {
        break;
      }
      const FakeStep& s = world_->steps_[k];
      const std::size_t cities = s.cities.size();
      const auto& list = site < cities ? s.cities[site] : s.gateways.at(site - cities);
      if (find_visible(list, v.satellite) == nullptr) {
        break;
      }
    }
    times.push_back(j == n ? horizon_s
                           : std::min(static_cast<double>(j) * world_->step_s_, horizon_s));
  }
  return times;
}

bool FakeWorld::View::moving_north(std::size_t satellite) const {
  const auto& north = world_->steps_.at(k_).north;
  return std::find(north.begin(), north.end(), satellite) != north.end();
}

std::vector<FakeStep> repeat(const FakeStep& step, std::size_t n) {
  return std::vector<FakeStep>(n, step);
}

std::vector<std::vector<RouteRow>> run(Engine& engine, const FakeWorld& world) {
  std::vector<std::vector<RouteRow>> all;
  for (std::size_t k = 0; k < world.size(); ++k) {
    std::vector<RouteRow> rows;
    const auto error = engine.step(static_cast<std::int64_t>(k), world.view(k), rows);
    EXPECT_FALSE(error.has_value()) << error.value_or("");
    all.push_back(std::move(rows));
  }
  engine.finish();
  return all;
}

}  // namespace satsim::routing_app::testing
