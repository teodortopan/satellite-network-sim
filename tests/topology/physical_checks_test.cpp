#include "satsim/topology/physical_checks.hpp"

#include <gtest/gtest.h>

#include <Eigen/Core>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <numbers>
#include <optional>
#include <utility>
#include <vector>

#include "ideal.hpp"
#include "satsim/constellation/classification.hpp"
#include "satsim/ground/sites.hpp"
#include "satsim/time/civil_time.hpp"
#include "satsim/topology/ground_links.hpp"
#include "satsim/topology/link_plan.hpp"
#include "satsim/topology/link_state.hpp"
#include "satsim/topology/positions.hpp"
#include "support/synthetic.hpp"

namespace {

using satsim::constellation::Classification;
using satsim::topology::LaserPattern;
using satsim::topology::LaserSide;
using satsim::topology::LinkDownReason;
using satsim::topology::LinkKind;
using satsim::topology::LinkLimits;
using satsim::topology::LinkPlan;
using satsim::topology::LinkState;
using satsim::topology::PhysicalCheckCounts;
using satsim::topology::PlannedLink;
using satsim::topology::StepLinks;
using satsim::topology::StepPositions;
using satsim::topology::VisibleSatellite;

constexpr double kRadPerDeg = std::numbers::pi / 180.0;
constexpr satsim::time::CivilTime kEpoch{2026, 11, 18, 21, 15, 0.0};

// A small network checked by hand:
//   satellites 0-3: shell 0, one plane, mean height 470 km;
//   satellites 4-5: shell 1, one plane, mean height 480 km;
//   satellite 6:    excluded (in shell 0's bands, in no plane).
// Satellite i is at 6900 km radius, longitude 5 i deg, on the equator: neighbours are 600 km
// apart and in sight of each other.
struct Network {
  Classification classification;
  LinkPlan plan;
  StepLinks links;
  StepPositions positions;
  std::vector<satsim::ground::GroundSite> sites;
  std::vector<std::vector<VisibleSatellite>> visible;
};

Classification classification() {
  Classification c;
  c.planes.resize(2);
  c.planes[0].resize(1);
  c.planes[1].resize(1);
  for (std::size_t i = 0; i < 7; ++i) {
    satsim::constellation::ClassifiedSatellite s;
    s.norad_cat_id = 70000 + static_cast<std::int64_t>(i);
    satsim::constellation::OrbitGeometry g;
    g.mean_normal_teme = Eigen::Vector3d::UnitZ();
    g.mean_height_km = i < 4 ? 470.0 : 480.0;
    s.geometry = g;
    const std::size_t shell = i < 4 || i == 6 ? 0 : 1;
    s.shell_index = shell;
    if (i == 6) {
      s.placement = satsim::constellation::ExclusionReason::kNoPlane;
    } else {
      s.placement = satsim::constellation::PlaneAssignment{
          .plane_index = 0, .index_in_plane = c.planes[shell][0].members.size()};
      c.planes[shell][0].members.push_back(i);
    }
    c.satellites.push_back(s);
  }
  return c;
}

void add_link(LinkPlan& plan, std::size_t a, std::size_t b, LaserSide side_a, LaserSide side_b,
              LinkKind kind, std::size_t shell) {
  const std::size_t index = plan.links.size();
  plan.links.push_back(PlannedLink{.satellite_a = a,
                                   .satellite_b = b,
                                   .side_a = side_a,
                                   .side_b = side_b,
                                   .kind = kind,
                                   .shell_index = shell});
  plan.lasers.at(a).push_back({.side = side_a, .link = index, .unassigned = {}});
  plan.lasers.at(b).push_back({.side = side_b, .link = index, .unassigned = {}});
}

void add_in_plane(LinkPlan& plan, std::size_t a, std::size_t b, std::size_t shell) {
  add_link(plan, a, b, LaserSide::kAhead, LaserSide::kBehind, LinkKind::kInPlane, shell);
}

// Every link up with its length.
StepLinks all_up(const LinkPlan& plan, const StepPositions& positions) {
  StepLinks links{.utc = positions.utc, .states = {}};
  for (const PlannedLink& link : plan.links) {
    const auto& a = positions.position_ecef_km[link.satellite_a];
    const auto& b = positions.position_ecef_km[link.satellite_b];
    links.states.push_back(
        LinkState{.length_km = a && b ? std::optional<double>((*a - *b).norm()) : std::nullopt,
                  .down_reason = std::nullopt});
  }
  return links;
}

Network valid_network() {
  std::vector<std::optional<Eigen::Vector3d>> points;
  for (std::size_t i = 0; i < 7; ++i) {
    const double lon = 5.0 * static_cast<double>(i) * kRadPerDeg;
    points.emplace_back(Eigen::Vector3d(6900.0 * std::cos(lon), 6900.0 * std::sin(lon), 0.0));
  }
  StepPositions positions = satsim::topology::testing::step_positions(
      satsim::constellation::testing::utc(kEpoch), points);
  LinkPlan plan;
  plan.pattern = LaserPattern::kPlusGrid;
  plan.lasers.resize(7);
  add_in_plane(plan, 0, 1, 0);
  add_in_plane(plan, 1, 2, 0);
  add_in_plane(plan, 2, 3, 0);
  add_in_plane(plan, 3, 0, 0);
  add_in_plane(plan, 4, 5, 1);
  StepLinks links = all_up(plan, positions);
  // A site on the equator below satellite 1 sees it overhead.
  std::vector<satsim::ground::GroundSite> sites = {
      satsim::ground::make_ground_site("below 1", 0.0, 5.0)};
  std::vector<std::vector<VisibleSatellite>> visible = {
      {VisibleSatellite{.satellite = 1,
                        .norad_cat_id = 70001,
                        .shell_index = 0,
                        .elevation_rad = std::numbers::pi / 2.0,
                        .slant_range_km = 6900.0 - 6378.137,
                        .mask_rad = 20.0 * kRadPerDeg}}};
  return Network{.classification = classification(),
                 .plan = std::move(plan),
                 .links = std::move(links),
                 .positions = std::move(positions),
                 .sites = std::move(sites),
                 .visible = std::move(visible)};
}

LinkLimits range_cap(double km) {
  LinkLimits limits;
  limits.range_cap_km = km;
  return limits;
}

PhysicalCheckCounts check(const Network& n, const LinkLimits& limits = {}) {
  return satsim::topology::run_physical_checks(n.classification, n.plan, n.links, n.positions,
                                               limits, n.sites, n.visible);
}

TEST(PhysicalChecksTest, ValidNetworkPasses) {
  const Network n = valid_network();
  EXPECT_EQ(check(n).total(), 0U);
  EXPECT_EQ(check(n, range_cap(2000.0)).total(), 0U);
}

// A link marked up whose segment passes through the Earth (satellite 2 moved to the far side),
// and one marked up with an end that has no position.
TEST(PhysicalChecksTest, LineOfSight) {
  Network n = valid_network();
  n.positions.position_ecef_km[3] =
      -n.positions.position_ecef_km[3].value_or(Eigen::Vector3d::Zero());
  EXPECT_EQ(check(n).line_of_sight, 2U);  // 2-3 and 3-0.
  // The same links marked down are fine.
  n.links.states[2].down_reason = LinkDownReason::kNoLineOfSight;
  n.links.states[3].down_reason = LinkDownReason::kNoLineOfSight;
  EXPECT_EQ(check(n).total(), 0U);

  Network m = valid_network();
  m.positions.position_ecef_km[5].reset();
  EXPECT_EQ(check(m).line_of_sight, 1U);
}

TEST(PhysicalChecksTest, RangeCap) {
  Network n = valid_network();
  // Neighbours are 2 * 6900 * sin(2.5 deg) = 601.9 km apart; 3 and 0 (15 deg) 1801.2 km.
  EXPECT_EQ(check(n, range_cap(600.0)).range_cap, 5U);
  EXPECT_EQ(check(n, range_cap(602.0)).range_cap, 1U);
  EXPECT_EQ(check(n, range_cap(1802.0)).range_cap, 0U);
  n.links.states[0].down_reason = LinkDownReason::kOverRangeCap;
  EXPECT_EQ(check(n, range_cap(600.0)).range_cap, 4U);
}

// Satellite 0 with four links (1, 3, 2, and 4 across shells) under a three-laser pattern.
TEST(PhysicalChecksTest, LaserBudget) {
  Network n = valid_network();
  add_link(n.plan, 0, 2, LaserSide::kEast, LaserSide::kWest, LinkKind::kCrossPlane, 0);
  n.links = all_up(n.plan, n.positions);
  n.plan.pattern = LaserPattern::kThree;
  EXPECT_EQ(check(n).total(), 0U);  // Three each at most.
  add_link(n.plan, 0, 4, LaserSide::kSecondEast, LaserSide::kSecondWest,
           LinkKind::kSecondCrossPlane, 0);
  n.links = all_up(n.plan, n.positions);
  EXPECT_EQ(check(n).laser_budget, 1U);
  n.plan.pattern = LaserPattern::kPlusGrid;
  EXPECT_EQ(check(n).laser_budget, 0U);
}

TEST(PhysicalChecksTest, Symmetry) {
  {  // A link recorded at one end only.
    Network n = valid_network();
    n.plan.lasers[1].erase(n.plan.lasers[1].begin());
    EXPECT_EQ(check(n).symmetry, 1U);
  }
  {  // A laser record naming a link that does not touch the satellite.
    Network n = valid_network();
    n.plan.lasers[4].push_back({.side = LaserSide::kEast, .link = 1, .unassigned = {}});
    EXPECT_EQ(check(n).symmetry, 1U);
  }
  {  // A laser record naming a link that does not exist.
    Network n = valid_network();
    n.plan.lasers[4].push_back({.side = LaserSide::kEast, .link = 99, .unassigned = {}});
    EXPECT_EQ(check(n).symmetry, 1U);
  }
  {  // The same pair linked twice (consistently recorded at both ends).
    Network n = valid_network();
    add_link(n.plan, 1, 0, LaserSide::kEast, LaserSide::kWest, LinkKind::kCrossPlane, 0);
    n.links = all_up(n.plan, n.positions);
    EXPECT_EQ(check(n).symmetry, 1U);
  }
  {  // A record with the wrong side.
    Network n = valid_network();
    n.plan.lasers[0][0].side = LaserSide::kWest;
    EXPECT_EQ(check(n).symmetry, 2U);  // Link 0 not found at its side, the record unmatched.
  }
}

TEST(PhysicalChecksTest, Shells) {
  {  // Across two shells.
    Network n = valid_network();
    add_link(n.plan, 3, 4, LaserSide::kEast, LaserSide::kWest, LinkKind::kCrossPlane, 0);
    n.links = all_up(n.plan, n.positions);
    EXPECT_EQ(check(n).shells, 1U);
  }
  {  // To an excluded satellite.
    Network n = valid_network();
    add_link(n.plan, 3, 6, LaserSide::kEast, LaserSide::kWest, LinkKind::kCrossPlane, 0);
    n.links = all_up(n.plan, n.positions);
    EXPECT_EQ(check(n).shells, 1U);
  }
  {  // Recorded in the wrong shell.
    Network n = valid_network();
    n.plan.links[4].shell_index = 0;
    EXPECT_EQ(check(n).shells, 1U);
  }
}

TEST(PhysicalChecksTest, GroundLinkMask) {
  {  // Satellite 3 is 15 deg of longitude from the site, far below 20 deg elevation; the
     // record claims otherwise.
    Network n = valid_network();
    n.visible[0].push_back(VisibleSatellite{.satellite = 3,
                                            .norad_cat_id = 70003,
                                            .shell_index = 0,
                                            .elevation_rad = 0.5,
                                            .slant_range_km = 1000.0,
                                            .mask_rad = 0.0});
    EXPECT_EQ(check(n).mask, 1U);
  }
  {  // An excluded satellite, overhead or not.
    Network n = valid_network();
    n.visible[0].push_back(VisibleSatellite{.satellite = 6});
    EXPECT_EQ(check(n).mask, 1U);
  }
  {  // A satellite with no position.
    Network n = valid_network();
    n.positions.position_ecef_km[1].reset();
    EXPECT_EQ(check(n).mask, 1U);
  }
  {  // Satellite 2 at 10 deg from the site with the high-latitude allowance's 5 deg mask is
     // fine only where the allowance applies: the site is on the equator.
    Network n = valid_network();
    const double lon = 5.0 * kRadPerDeg;
    const Eigen::Vector3d site = n.sites[0].position_ecef_km;
    const Eigen::Vector3d up(std::cos(lon), std::sin(lon), 0.0);
    const Eigen::Vector3d east(-std::sin(lon), std::cos(lon), 0.0);
    const double e = 10.0 * kRadPerDeg;
    n.positions.position_ecef_km[2] = site + (1500.0 * ((std::cos(e) * east) + (std::sin(e) * up)));
    n.links = all_up(n.plan, n.positions);
    n.visible[0].push_back(VisibleSatellite{.satellite = 2});
    const auto counts = satsim::topology::run_physical_checks(n.classification, n.plan, n.links,
                                                              n.positions, {}, n.sites, n.visible,
                                                              {.high_latitude_allowance = true});
    EXPECT_EQ(counts.mask, 1U);
  }
}

TEST(PhysicalChecksTest, Structure) {
  Network n = valid_network();
  n.links.states.pop_back();
  EXPECT_EQ(check(n).structure, 1U);
  Network m = valid_network();
  m.visible.clear();
  EXPECT_EQ(check(m).structure, 1U);
}

}  // namespace
