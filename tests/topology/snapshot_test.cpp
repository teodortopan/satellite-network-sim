// The topology of the committed Starlink snapshot at the start of the Phase 2 run, with the
// default classification. Built only without sanitizers and not run under Valgrind, like the
// other snapshot checks (parsing the snapshot under ASan takes minutes). Asserts robust facts
// and prints the numbers the report needs.

#include "satsim/orbit/snapshot.hpp"

#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <numbers>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "satsim/constellation/classification.hpp"
#include "satsim/ground/loaders.hpp"
#include "satsim/ground/sites.hpp"
#include "satsim/orbit/element_set.hpp"
#include "satsim/time/earth_orientation.hpp"
#include "satsim/time/utc_time.hpp"
#include "satsim/topology/geometry.hpp"
#include "satsim/topology/ground_links.hpp"
#include "satsim/topology/link_plan.hpp"
#include "satsim/topology/link_state.hpp"
#include "satsim/topology/physical_checks.hpp"
#include "satsim/topology/positions.hpp"

namespace {

namespace topology = satsim::topology;
using satsim::constellation::Classification;
using satsim::time::UtcTime;

const std::filesystem::path kSnapshotDir = "data/starlink/2026-11-18";
const std::filesystem::path kCities = "data/ground/cities.json";
const std::filesystem::path kGateways = "data/ground/2026-11-19/gateways.json";

struct Snapshot {
  std::vector<satsim::orbit::ElementSet> sets;
  Classification classification;
  satsim::time::EarthOrientationTable earth_orientation;
  UtcTime reference;
};

// Loaded and classified once for every test here.
const Snapshot& snapshot() {
  static const Snapshot kSnapshot = [] {
    auto loaded = satsim::orbit::load_snapshot_elements(kSnapshotDir);
    if (!std::holds_alternative<satsim::orbit::SnapshotElements>(loaded)) {
      ADD_FAILURE() << std::get<std::string>(loaded);
      std::abort();
    }
    std::vector<satsim::orbit::ElementSet> sets;
    for (const auto& s : std::get<satsim::orbit::SnapshotElements>(loaded).selection.selected) {
      sets.push_back(s.elements);
    }
    const auto reference = UtcTime::from_civil({2026, 11, 18, 21, 15, 0.0});
    auto classified = satsim::constellation::classify_constellation(
        sets, *reference, satsim::constellation::default_config());
    auto eop = satsim::time::load_earth_orientation(kSnapshotDir / "finals2000A.all");
    if (!std::holds_alternative<Classification>(classified) || !eop.table) {
      ADD_FAILURE() << "cannot classify the snapshot or load its Earth orientation";
      std::abort();
    }
    return Snapshot{.sets = std::move(sets),
                    .classification = std::move(std::get<Classification>(classified)),
                    .earth_orientation = std::move(*eop.table),
                    .reference = *reference};
  }();
  return kSnapshot;
}

std::vector<satsim::ground::GroundSite> city_sites() {
  const auto cities = satsim::ground::load_cities(kCities);
  EXPECT_FALSE(cities.file_error.has_value());
  std::vector<satsim::ground::GroundSite> sites;
  sites.reserve(cities.cities.size());
  for (const auto& city : cities.cities) {
    sites.push_back(city.site);
  }
  return sites;
}

std::vector<satsim::ground::GroundSite> gateway_sites() {
  const auto loaded = satsim::ground::load_gateways(kGateways);
  EXPECT_FALSE(loaded.file_error.has_value());
  std::vector<satsim::ground::GroundSite> sites;
  for (const auto& gateway : satsim::ground::select_gateways(loaded.records)) {
    sites.push_back(gateway.site);
  }
  return sites;
}

TEST(TopologySnapshotTest, EveryPatternPassesThePhysicalChecks) {
  const Snapshot& s = snapshot();
  const Classification& c = s.classification;
  const auto propagators = topology::plane_member_propagators(c, s.sets);
  const auto eop = s.earth_orientation.at(s.reference);
  ASSERT_TRUE(eop.has_value());
  const topology::StepPositions positions = topology::positions_at(propagators, s.reference, *eop);
  EXPECT_EQ(positions.failed_count, 0U);

  std::size_t members = 0;
  for (std::size_t i = 0; i < c.satellites.size(); ++i) {
    if (topology::is_plane_member(c, i)) {
      ++members;
      EXPECT_TRUE(positions.position_ecef_km[i].has_value());
    }
  }
  std::cout << "plane members: " << members << "\n";
  EXPECT_GT(members, 8000U);

  const auto heights = topology::shell_mean_heights_km(c);
  for (std::size_t shell = 0; shell < heights.size(); ++shell) {
    ASSERT_TRUE(heights[shell].has_value());
    std::cout << "shell " << shell << ": mean height " << *heights[shell] << " km, mask "
              << topology::elevation_mask_rad(*heights[shell], 0.0) * 180.0 / std::numbers::pi
              << " deg, longest link "
              << topology::max_link_length_km(6378.137 + *heights[shell],
                                              6378.137 + *heights[shell])
              << " km\n";
  }

  const std::vector<satsim::ground::GroundSite> cities = city_sites();
  EXPECT_EQ(cities.size(), 14U);
  const topology::GroundVisibility city_visibility(cities, c);
  const auto visible = city_visibility.at(positions);
  std::size_t cities_seeing = 0;
  for (std::size_t i = 0; i < cities.size(); ++i) {
    const auto serving = topology::pick_serving_satellite(visible[i]);
    std::cout << "  " << cities[i].name << ": " << visible[i].size() << " satellites"
              << (serving ? ", serving " + std::to_string(serving->norad_cat_id) + " at " +
                                std::to_string(serving->elevation_rad * 180.0 / std::numbers::pi) +
                                " deg"
                          : std::string())
              << "\n";
    if (!visible[i].empty()) {
      ++cities_seeing;
    }
  }
  std::cout << "cities with at least one satellite: " << cities_seeing << " of " << cities.size()
            << "\n";
  EXPECT_GE(cities_seeing, 12U);

  for (const topology::LaserPattern pattern : topology::kAllLaserPatterns) {
    const topology::LinkPlan plan =
        topology::plan_laser_links(c, positions.position_ecef_km, {.pattern = pattern});
    topology::LinkStateEvaluator evaluator(plan, {});
    const topology::StepLinks links = evaluator.step(positions);
    const auto counts =
        topology::run_physical_checks(c, plan, links, positions, {}, cities, visible);
    EXPECT_EQ(counts.total(), 0U) << topology::name_of(pattern);

    std::vector<std::size_t> per_satellite(c.satellites.size(), 0);
    for (const auto& link : plan.links) {
      ++per_satellite[link.satellite_a];
      ++per_satellite[link.satellite_b];
    }
    for (const std::size_t n : per_satellite) {
      EXPECT_LE(n, topology::laser_budget(pattern));
    }

    std::cout << "pattern " << topology::name_of(pattern) << ": " << plan.links.size() << " links;";
    const auto kinds = topology::count_links_by_kind(plan);
    for (const auto kind : topology::kAllLinkKinds) {
      std::cout << " " << topology::name_of(kind) << " " << kinds[static_cast<std::size_t>(kind)];
    }
    std::cout << "\n  lasers unassigned:";
    const auto unassigned = topology::count_unassigned_lasers(plan);
    for (const auto reason : topology::kAllUnassignedReasons) {
      std::cout << " " << topology::name_of(reason) << " "
                << unassigned[static_cast<std::size_t>(reason)];
    }
    std::cout << "\n  up " << topology::count_up(links) << ", down:";
    const auto down = topology::count_down_by_reason(links);
    for (const auto reason : topology::kAllLinkDownReasons) {
      std::cout << " " << topology::name_of(reason) << " "
                << down[static_cast<std::size_t>(reason)];
    }
    std::cout << "\n  physical-check violations: " << counts.total() << "\n";
  }
}

// How long one step takes for the full run's work: about 230 ground sites (14 cities and the
// operational gateways), positions of every plane member, and the link states of the main
// pattern. Measured over 40 steps of 15 s and extrapolated to the 2,881 steps of the run.
// Every step also passes the physical checks (not timed).
TEST(TopologySnapshotTest, StepCost) {
  const Snapshot& s = snapshot();
  const Classification& c = s.classification;
  const auto propagators = topology::plane_member_propagators(c, s.sets);
  std::vector<satsim::ground::GroundSite> sites = city_sites();
  for (auto& g : gateway_sites()) {
    sites.push_back(std::move(g));
  }
  const auto eop0 = s.earth_orientation.at(s.reference);
  ASSERT_TRUE(eop0.has_value());
  const auto first = topology::positions_at(propagators, s.reference, *eop0);
  const topology::LinkPlan plan = topology::plan_laser_links(c, first.position_ecef_km, {});
  topology::LinkStateEvaluator evaluator(plan, {});
  const topology::GroundVisibility visibility(sites, c);

  using Clock = std::chrono::steady_clock;
  Clock::duration in_positions{};
  Clock::duration in_links{};
  Clock::duration in_ground{};
  constexpr int kSteps = 40;
  std::size_t ground_links = 0;
  for (int k = 0; k < kSteps; ++k) {
    const UtcTime t = s.reference.plus_seconds(15.0 * k);
    const auto eop = s.earth_orientation.at(t);
    ASSERT_TRUE(eop.has_value());
    const auto t0 = Clock::now();
    const auto positions = topology::positions_at(propagators, t, *eop);
    const auto t1 = Clock::now();
    const auto links = evaluator.step(positions);
    const auto t2 = Clock::now();
    const auto visible = visibility.at(positions);
    const auto t3 = Clock::now();
    in_positions += t1 - t0;
    in_links += t2 - t1;
    in_ground += t3 - t2;
    for (const auto& v : visible) {
      ground_links += v.size();
    }
    EXPECT_EQ(links.states.size(), plan.links.size());
    EXPECT_EQ(topology::run_physical_checks(c, plan, links, positions, {}, sites, visible).total(),
              0U)
        << "step " << k;
  }
  const auto per_run_s = [](Clock::duration d) {
    return std::chrono::duration<double>(d).count() / kSteps * 2881.0;
  };
  std::cout << sites.size() << " sites, " << ground_links / kSteps
            << " ground links per step on average\n"
            << "extrapolated to 2,881 steps: positions " << per_run_s(in_positions)
            << " s, link states " << per_run_s(in_links) << " s, ground links "
            << per_run_s(in_ground) << " s\n";
  EXPECT_GE(sites.size(), 200U);
}

}  // namespace
