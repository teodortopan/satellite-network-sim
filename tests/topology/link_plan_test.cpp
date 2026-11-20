#include "satsim/topology/link_plan.hpp"

#include <gtest/gtest.h>

#include <Eigen/Core>
#include <Eigen/Geometry>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <numbers>
#include <numeric>
#include <optional>
#include <random>
#include <set>
#include <utility>
#include <variant>
#include <vector>

#include "ideal.hpp"
#include "satsim/constellation/classification.hpp"
#include "satsim/constellation/geometry.hpp"
#include "satsim/orbit/element_set.hpp"
#include "satsim/orbit/propagator.hpp"
#include "satsim/time/civil_time.hpp"
#include "satsim/time/earth_orientation.hpp"
#include "satsim/time/utc_time.hpp"
#include "satsim/topology/geometry.hpp"
#include "satsim/topology/link_state.hpp"
#include "satsim/topology/physical_checks.hpp"
#include "satsim/topology/positions.hpp"
#include "support/synthetic.hpp"

namespace {

using satsim::constellation::Classification;
using satsim::constellation::PlaneAssignment;
using satsim::topology::count_links_by_kind;
using satsim::topology::count_unassigned_lasers;
using satsim::topology::Laser;
using satsim::topology::laser_budget;
using satsim::topology::laser_sides;
using satsim::topology::LaserPattern;
using satsim::topology::LaserSide;
using satsim::topology::LinkKind;
using satsim::topology::LinkPlan;
using satsim::topology::LinkPlanSettings;
using satsim::topology::PlannedLink;
using satsim::topology::UnassignedReason;
using satsim::topology::testing::Ideal;
using satsim::topology::testing::ideal_shells;
using satsim::topology::testing::IdealOptions;
using satsim::topology::testing::WalkerShell;

constexpr satsim::time::CivilTime kEpoch{2026, 11, 18, 21, 15, 0.0};

constexpr std::size_t kInPlane = 0;
constexpr std::size_t kCross = 1;
constexpr std::size_t kSecondCross = 2;
constexpr std::size_t kTooSmall = 0;
constexpr std::size_t kNoNeighbour = 1;
constexpr std::size_t kNoPartner = 2;

LinkPlan plan(const Ideal& ideal, LaserPattern pattern) {
  return satsim::topology::plan_laser_links(ideal.classification, ideal.positions_ecef_km,
                                            LinkPlanSettings{.pattern = pattern});
}

// Planned links touching each satellite, counted from the link list.
std::vector<std::size_t> links_per_satellite(const LinkPlan& p, std::size_t satellites) {
  std::vector<std::size_t> counts(satellites, 0);
  for (const PlannedLink& link : p.links) {
    ++counts.at(link.satellite_a);
    ++counts.at(link.satellite_b);
  }
  return counts;
}

// Number of connected components of the graph of planned links over `satellites` nodes.
std::size_t components(const LinkPlan& p, std::size_t satellites) {
  std::vector<std::size_t> parent(satellites);
  std::iota(parent.begin(), parent.end(), 0);
  const auto find = [&](std::size_t x) {
    while (parent[x] != x) {
      parent[x] = parent[parent[x]];
      x = parent[x];
    }
    return x;
  };
  for (const PlannedLink& link : p.links) {
    parent[find(link.satellite_a)] = find(link.satellite_b);
  }
  std::set<std::size_t> roots;
  for (std::size_t i = 0; i < satellites; ++i) {
    roots.insert(find(i));
  }
  return roots.size();
}

// Every physical check at the reference time, lasers only: all must pass for any plan.
std::size_t violations(const Ideal& ideal, const LinkPlan& p) {
  const auto positions = satsim::topology::testing::step_positions(
      satsim::constellation::testing::utc(kEpoch), ideal.positions_ecef_km);
  satsim::topology::LinkStateEvaluator evaluator(p, {});
  const auto links = evaluator.step(positions);
  return satsim::topology::run_physical_checks(ideal.classification, p, links, positions, {}, {},
                                               {})
      .total();
}

// No unordered pair of satellites is linked twice.
bool no_duplicate_pairs(const LinkPlan& p) {
  std::set<std::pair<std::size_t, std::size_t>> pairs;
  for (const PlannedLink& link : p.links) {
    if (!pairs.insert(std::minmax(link.satellite_a, link.satellite_b)).second) {
      return false;
    }
  }
  return true;
}

std::size_t plane_of(const Classification& c, std::size_t satellite) {
  return std::get<PlaneAssignment>(c.satellites[satellite].placement).plane_index;
}

const Laser& laser_at(const LinkPlan& p, std::size_t satellite, LaserSide side) {
  for (const Laser& l : p.lasers.at(satellite)) {
    if (l.side == side) {
      return l;
    }
  }
  ADD_FAILURE() << "no laser " << static_cast<int>(side) << " on satellite " << satellite;
  return p.lasers.at(satellite).front();
}

// Hypatia's first Starlink shell (spec 6.2): 72 planes of 22 at 550 km and 53 deg. Built once
// for all the tests that use it.
const Ideal& hypatia_shell() {
  static const Ideal kIdeal = [] {
    const std::array<WalkerShell, 1> shells = {WalkerShell{.planes = 72,
                                                           .per_plane = 22,
                                                           .inclination_deg = 53.0,
                                                           .height_km = 550.0,
                                                           .phasing = 17,
                                                           .epoch = kEpoch}};
    return ideal_shells(shells);
  }();
  return kIdeal;
}

TEST(LaserPatternTest, NamesBudgetsAndSides) {
  for (const LaserPattern pattern : satsim::topology::kAllLaserPatterns) {
    EXPECT_EQ(satsim::topology::parse_laser_pattern(satsim::topology::name_of(pattern)), pattern);
    EXPECT_EQ(laser_sides(pattern, 0).size(), laser_budget(pattern));
    EXPECT_EQ(laser_sides(pattern, 7).size(), laser_budget(pattern));
  }
  EXPECT_FALSE(satsim::topology::parse_laser_pattern("plus_grid").has_value());
  EXPECT_EQ(laser_budget(LaserPattern::kThree), 3U);
  EXPECT_EQ(laser_budget(LaserPattern::kPlusGrid), 4U);
  EXPECT_EQ(laser_budget(LaserPattern::kSix), 6U);
  EXPECT_EQ(laser_sides(LaserPattern::kThree, 4).back(), LaserSide::kEast);
  EXPECT_EQ(laser_sides(LaserPattern::kThree, 5).back(), LaserSide::kWest);
}

// Two satellites in one circular plane keep a fixed angle between them, so the permanence test
// is exact there: at r = 6928.135 km and R = 6458.137 km the widest angle with line of sight
// is 2 acos(R / r) = 42.45 deg.
TEST(PermanentLinkTest, SamePlaneHoldsUpToTheLineOfSightAngle) {
  const auto at = [](double argument_of_latitude_deg) {
    satsim::constellation::OrbitGeometry g;
    g.mean_normal_teme = Eigen::Vector3d(0.0, -std::sin(0.925), std::cos(0.925));
    g.inclination_rad = 0.925;
    g.raan_teme_rad = 0.0;
    g.argument_of_latitude_rad = argument_of_latitude_deg * std::numbers::pi / 180.0;
    g.mean_radius_km = 6928.135;
    return g;
  };
  using satsim::topology::keeps_line_of_sight_for_an_orbit;
  EXPECT_TRUE(keeps_line_of_sight_for_an_orbit(at(10.0), at(52.4)));
  EXPECT_FALSE(keeps_line_of_sight_for_an_orbit(at(10.0), at(52.5)));
  EXPECT_TRUE(keeps_line_of_sight_for_an_orbit(at(350.0), at(20.0)));  // Across the node.
  // A lower atmosphere layer lets the same pair through.
  EXPECT_TRUE(keeps_line_of_sight_for_an_orbit(at(10.0), at(52.5), 70.0));
}

// Two planes at 53 deg whose RAANs differ by 30 deg, the first satellite at argument of
// latitude 10 deg. Which way the satellites move decides the answer: with the second 30 deg
// behind (-20 deg) the link's closest approach to the Earth's centre over one orbit is 285 km
// above the 6458.137 km limit, with the second 30 deg ahead (40 deg) it is 273 km below
// (computed apart from this code, at 3600 points of the orbit). Satellites moving the wrong
// way round would swap the two answers.
TEST(PermanentLinkTest, TwoPlanesDependOnWhichSatelliteLeads) {
  const auto at = [](double raan_deg, double argument_of_latitude_deg) {
    const double inclination_rad = 53.0 * std::numbers::pi / 180.0;
    const double raan_rad = raan_deg * std::numbers::pi / 180.0;
    satsim::constellation::OrbitGeometry g;
    const double sin_i = std::sin(inclination_rad);
    g.mean_normal_teme = Eigen::Vector3d(sin_i * std::sin(raan_rad), -sin_i * std::cos(raan_rad),
                                         std::cos(inclination_rad));
    g.inclination_rad = inclination_rad;
    g.raan_teme_rad = raan_rad;
    g.argument_of_latitude_rad = argument_of_latitude_deg * std::numbers::pi / 180.0;
    g.mean_radius_km = 6928.135;
    return g;
  };
  using satsim::topology::keeps_line_of_sight_for_an_orbit;
  EXPECT_TRUE(keeps_line_of_sight_for_an_orbit(at(0.0, 10.0), at(30.0, -20.0)));
  EXPECT_FALSE(keeps_line_of_sight_for_an_orbit(at(0.0, 10.0), at(30.0, 40.0)));
}

// The function may skip the 360 points where the widest angle decides the answer. On random
// pairs of orbits (any tilt, RAAN and start, radii 300 to 1200 km up) its answer must be the
// one the 360 points give, visited here one by one. Both answers occur many times.
TEST(PermanentLinkTest, AgreesWithVisitingEveryPointOnRandomOrbits) {
  std::mt19937 rng(20261120);
  std::uniform_real_distribution<double> turn(0.0, 2.0 * std::numbers::pi);
  std::uniform_real_distribution<double> tilt(0.0, std::numbers::pi);
  std::uniform_real_distribution<double> radius(6678.0, 7578.0);
  std::uniform_real_distribution<double> near(-0.9, 0.9);
  const auto orbit = [](double inclination_rad, double raan_rad, double u_rad, double radius_km) {
    satsim::constellation::OrbitGeometry g;
    const double sin_i = std::sin(inclination_rad);
    g.mean_normal_teme = Eigen::Vector3d(sin_i * std::sin(raan_rad), -sin_i * std::cos(raan_rad),
                                         std::cos(inclination_rad));
    g.inclination_rad = inclination_rad;
    g.raan_teme_rad = raan_rad;
    g.argument_of_latitude_rad = u_rad;
    g.mean_radius_km = radius_km;
    return g;
  };
  const auto position = [](const satsim::constellation::OrbitGeometry& g, double advance_rad) {
    const Eigen::Vector3d node(std::cos(g.raan_teme_rad), std::sin(g.raan_teme_rad), 0.0);
    const Eigen::Vector3d along = g.mean_normal_teme.cross(node);
    const double u = g.argument_of_latitude_rad + advance_rad;
    return Eigen::Vector3d(g.mean_radius_km * ((std::cos(u) * node) + (std::sin(u) * along)));
  };
  std::size_t permanent = 0;
  std::size_t not_permanent = 0;
  for (int trial = 0; trial < 600; ++trial) {
    const auto a = orbit(tilt(rng), turn(rng), turn(rng), radius(rng));
    // Half the pairs are close to a (planes and starts within 0.9 rad), so that many links
    // sit near the limit; the other half are anywhere.
    const auto b = trial % 2 == 0
                       ? orbit(a.inclination_rad + (0.3 * near(rng)), a.raan_teme_rad + near(rng),
                               a.argument_of_latitude_rad + near(rng), radius(rng))
                       : orbit(tilt(rng), turn(rng), turn(rng), radius(rng));
    bool every_point = true;
    const std::size_t points = satsim::topology::kPermanenceSamplesPerOrbit;
    for (std::size_t k = 0; k < points; ++k) {
      const double advance_rad =
          2.0 * std::numbers::pi * static_cast<double>(k) / static_cast<double>(points);
      every_point = every_point && satsim::topology::has_line_of_sight(position(a, advance_rad),
                                                                       position(b, advance_rad));
    }
    EXPECT_EQ(satsim::topology::keeps_line_of_sight_for_an_orbit(a, b), every_point) << trial;
    ++(every_point ? permanent : not_permanent);
  }
  EXPECT_GT(permanent, 50U);
  EXPECT_GT(not_permanent, 50U);
}

TEST(LinkPlanTest, HypatiaShellPlusGridHasExactly3168Links) {
  const Ideal& ideal = hypatia_shell();
  const std::size_t n = ideal.classification.satellites.size();
  ASSERT_EQ(n, 1584U);
  const LinkPlan p = plan(ideal, LaserPattern::kPlusGrid);
  EXPECT_EQ(p.links.size(), 3168U);
  const auto kinds = count_links_by_kind(p);
  EXPECT_EQ(kinds[kInPlane], 1584U);
  EXPECT_EQ(kinds[kCross], 1584U);
  EXPECT_EQ(kinds[kSecondCross], 0U);
  for (const std::size_t count : links_per_satellite(p, n)) {
    EXPECT_EQ(count, 4U);
  }
  EXPECT_EQ(count_unassigned_lasers(p), (std::array<std::size_t, 3>{0, 0, 0}));
  // Cross-plane links join neighbouring planes, west end first.
  for (const PlannedLink& link : p.links) {
    const std::size_t pa = plane_of(ideal.classification, link.satellite_a);
    const std::size_t pb = plane_of(ideal.classification, link.satellite_b);
    if (link.kind == LinkKind::kCrossPlane) {
      EXPECT_EQ(pb, (pa + 1) % 72);
      EXPECT_EQ(link.side_a, LaserSide::kEast);
      EXPECT_EQ(link.side_b, LaserSide::kWest);
    } else {
      EXPECT_EQ(pa, pb);
    }
  }
  EXPECT_TRUE(no_duplicate_pairs(p));
  EXPECT_EQ(components(p, n), 1U);
  EXPECT_EQ(violations(ideal, p), 0U);
}

TEST(LinkPlanTest, HypatiaShellThreeGivesEverySatelliteThreeAndOneComponent) {
  const Ideal& ideal = hypatia_shell();
  const std::size_t n = ideal.classification.satellites.size();
  const LinkPlan p = plan(ideal, LaserPattern::kThree);
  EXPECT_EQ(p.links.size(), 2376U);
  const auto kinds = count_links_by_kind(p);
  EXPECT_EQ(kinds[kInPlane], 1584U);
  EXPECT_EQ(kinds[kCross], 792U);
  for (const std::size_t count : links_per_satellite(p, n)) {
    EXPECT_EQ(count, 3U);
  }
  // Even positions face east, odd west.
  for (const PlannedLink& link : p.links) {
    if (link.kind == LinkKind::kCrossPlane) {
      const auto& a =
          std::get<PlaneAssignment>(ideal.classification.satellites[link.satellite_a].placement);
      const auto& b =
          std::get<PlaneAssignment>(ideal.classification.satellites[link.satellite_b].placement);
      EXPECT_EQ(a.index_in_plane % 2, 0U);
      EXPECT_EQ(b.index_in_plane % 2, 1U);
    }
  }
  EXPECT_TRUE(no_duplicate_pairs(p));
  EXPECT_EQ(components(p, n), 1U);
  EXPECT_EQ(violations(ideal, p), 0U);
}

TEST(LinkPlanTest, HypatiaShellSixGivesEverySatelliteSix) {
  const Ideal& ideal = hypatia_shell();
  const std::size_t n = ideal.classification.satellites.size();
  const LinkPlan p = plan(ideal, LaserPattern::kSix);
  EXPECT_EQ(p.links.size(), 4752U);
  const auto kinds = count_links_by_kind(p);
  EXPECT_EQ(kinds[kInPlane], 1584U);
  EXPECT_EQ(kinds[kCross], 1584U);
  EXPECT_EQ(kinds[kSecondCross], 1584U);
  for (const std::size_t count : links_per_satellite(p, n)) {
    EXPECT_EQ(count, 6U);
  }
  for (const PlannedLink& link : p.links) {
    if (link.kind == LinkKind::kSecondCrossPlane) {
      EXPECT_EQ(plane_of(ideal.classification, link.satellite_b),
                (plane_of(ideal.classification, link.satellite_a) + 2) % 72);
    }
  }
  EXPECT_TRUE(no_duplicate_pairs(p));
  EXPECT_EQ(violations(ideal, p), 0U);
}

// 5 planes of 5: the planes are 72 deg apart in RAAN and the satellites 72 deg apart along
// the orbit, more than the 42 deg a link at 550 km can span.
// - In-plane: 5 rings of 5, 25 links, all planned although none has line of sight.
// - three: positions 0, 2, 4 face east, 1 and 3 west, so each plane offers 3 east and 2 west
//   lasers. A few east-west pairs are in sight of each other at the reference time, but planes
//   72 deg apart meet at gamma = 56.0 deg (see the hand computation below), so once an orbit
//   every pair is at least 6504 km apart, over the 5016.6 km limit: no cross link is planned,
//   and all 25 cross-plane lasers have no partner. Nothing is up.
TEST(LinkPlanTest, FivePlanesOfFiveWithThree) {
  const std::array<WalkerShell, 1> shells = {WalkerShell{.planes = 5,
                                                         .per_plane = 5,
                                                         .inclination_deg = 53.0,
                                                         .height_km = 550.0,
                                                         .phasing = 2,
                                                         .epoch = kEpoch}};
  const Ideal ideal = ideal_shells(shells);
  const LinkPlan p = plan(ideal, LaserPattern::kThree);
  const auto kinds = count_links_by_kind(p);
  const auto unassigned = count_unassigned_lasers(p);
  EXPECT_EQ(kinds[kInPlane], 25U);
  EXPECT_EQ(kinds[kCross], 0U);
  EXPECT_EQ(unassigned[kTooSmall], 0U);
  EXPECT_EQ(unassigned[kNoNeighbour], 0U);
  // 15 east and 10 west lasers.
  EXPECT_EQ(unassigned[kNoPartner], 25U);
  // None of the in-plane links (72 deg) is up.
  const auto positions = satsim::topology::testing::step_positions(
      satsim::constellation::testing::utc(kEpoch), ideal.positions_ecef_km);
  satsim::topology::LinkStateEvaluator evaluator(p, {});
  EXPECT_EQ(satsim::topology::count_up(evaluator.step(positions)), 0U);
  for (const std::size_t count : links_per_satellite(p, 25)) {
    EXPECT_EQ(count, 2U);
  }
  EXPECT_EQ(violations(ideal, p), 0U);
}

// A plane with two missing satellites: the ring closes over the 90 deg gap, a link that has no
// line of sight. It is planned, and down (counted) at every step.
TEST(LinkPlanTest, GapTooLongToBridgeIsPlannedButDown) {
  const std::array<WalkerShell, 1> shells = {WalkerShell{.planes = 6,
                                                         .per_plane = 12,
                                                         .inclination_deg = 53.0,
                                                         .height_km = 550.0,
                                                         .phasing = 1,
                                                         .epoch = kEpoch}};
  const Ideal ideal = ideal_shells(
      shells, IdealOptions{.drop = [](std::size_t, std::size_t p,
                                      std::size_t s) { return p == 0 && (s == 5 || s == 6); },
                           .exclude = {}});
  const LinkPlan p = plan(ideal, LaserPattern::kPlusGrid);
  EXPECT_EQ(count_links_by_kind(p)[kInPlane], 70U);
  const auto& members = ideal.classification.planes[0][0].members;
  ASSERT_EQ(members.size(), 10U);
  // Satellites 4 and 7 of the Walker plane are now neighbours, 4 and 5 in the plane.
  std::optional<std::size_t> gap_link;
  for (std::size_t i = 0; i < p.links.size(); ++i) {
    if (p.links[i].satellite_a == members[4] && p.links[i].satellite_b == members[5]) {
      gap_link = i;
    }
  }
  ASSERT_TRUE(gap_link.has_value());
  const auto positions = satsim::topology::testing::step_positions(
      satsim::constellation::testing::utc(kEpoch), ideal.positions_ecef_km);
  satsim::topology::LinkStateEvaluator evaluator(p, {});
  const auto links = evaluator.step(positions);
  EXPECT_EQ(links.states[gap_link.value_or(0)].down_reason,
            satsim::topology::LinkDownReason::kNoLineOfSight);
  const auto down = satsim::topology::count_down_by_reason(links);
  EXPECT_EQ(down[static_cast<std::size_t>(satsim::topology::LinkDownReason::kNoLineOfSight)], 1U);
  EXPECT_EQ(satsim::topology::count_up(links), p.links.size() - 1);
  EXPECT_EQ(violations(ideal, p), 0U);
}

// The neighbouring-planes rule (spec 4.3): the next plane in RAAN order is always the
// neighbour, and a cross-plane link is planned only if it keeps line of sight for a whole
// orbit. The hand computation for the tests below: at 550 km above WGS-72's radius
// (r = 6928.135 km) and a limit of R = 6458.137 km, the longest link is 2 sqrt(r^2 - R^2) =
// 5016.6 km. Two planes of tilt i whose RAANs differ by g meet at an angle gamma with
// cos gamma = cos^2 i + sin^2 i cos g. Once an orbit, each satellite is 90 deg from the line
// where the planes cross, and then at least gamma from every point of the other plane: a
// link between them is at least 2 r sin(gamma / 2) long at that moment.

// Hypatia's shell with only planes 0 to 9 and 15 to 24 kept: the 30 deg gap between them is
// 6 times the median gap (5 deg), and 240 deg are empty after plane 24. Across 30 deg
// (gamma = 23.9 deg, at least 2864 km) the groups are joined; across 240 deg (gamma =
// 87.5 deg, 9584 km) nothing is.
TEST(LinkPlanTest, GroupsBeyondThreeTimesTheMedianGapAreJoinedByPermanentLinks) {
  const std::array<WalkerShell, 1> shells = {WalkerShell{.planes = 72,
                                                         .per_plane = 22,
                                                         .inclination_deg = 53.0,
                                                         .height_km = 550.0,
                                                         .phasing = 17,
                                                         .epoch = kEpoch}};
  const Ideal ideal = ideal_shells(
      shells, IdealOptions{.drop = [](std::size_t, std::size_t p,
                                      std::size_t) { return p > 9 && (p < 15 || p > 24); },
                           .exclude = {}});
  const auto& planes = ideal.classification.planes[0];
  ASSERT_EQ(planes.size(), 20U);
  const std::size_t n = ideal.classification.satellites.size();
  const LinkPlan p = plan(ideal, LaserPattern::kPlusGrid);
  EXPECT_EQ(p.neighbours[0][9].east, 10U);
  EXPECT_EQ(p.neighbours[0][19].east, 0U);
  std::size_t across_30 = 0;
  std::size_t across_240 = 0;
  for (const PlannedLink& link : p.links) {
    if (link.kind != LinkKind::kCrossPlane) {
      continue;
    }
    const std::size_t pa = plane_of(ideal.classification, link.satellite_a);
    if (pa == 9) {
      ++across_30;
    } else if (pa == 19) {
      ++across_240;
    }
  }
  EXPECT_EQ(across_30, 22U);
  EXPECT_EQ(across_240, 0U);
  // The other 18 plane pairs are 5 deg apart and fully linked; plane 19's east and plane 0's
  // west lasers are left without a partner.
  EXPECT_EQ(count_links_by_kind(p)[kCross], 19U * 22U);
  const auto unassigned = count_unassigned_lasers(p);
  EXPECT_EQ(unassigned[kNoNeighbour], 0U);
  EXPECT_EQ(unassigned[kNoPartner], 2U * 22U);
  for (const std::size_t s : planes[19].members) {
    EXPECT_EQ(laser_at(p, s, LaserSide::kEast).unassigned, UnassignedReason::kNoFreePartner);
  }
  EXPECT_EQ(components(p, n), 1U);
  EXPECT_TRUE(no_duplicate_pairs(p));
  EXPECT_EQ(violations(ideal, p), 0U);
}

// 16 planes 22.5 deg apart with planes 7, 8 and 9 removed: plane 6's east neighbour is plane
// 10 (now 7), 90 deg away. gamma = 68.8 deg, so a link between them is at least 7825 km long
// once an orbit, over the 5016.6 km limit: no cross-plane link is planned across the gap.
TEST(LinkPlanTest, NoCrossPlaneLinkAcrossAGapTooWideForAPermanentLink) {
  const std::array<WalkerShell, 1> shells = {WalkerShell{.planes = 16,
                                                         .per_plane = 12,
                                                         .inclination_deg = 53.0,
                                                         .height_km = 550.0,
                                                         .phasing = 1,
                                                         .epoch = kEpoch}};
  const Ideal ideal = ideal_shells(
      shells,
      IdealOptions{.drop = [](std::size_t, std::size_t p, std::size_t) { return p >= 7 && p <= 9; },
                   .exclude = {}});
  ASSERT_EQ(ideal.classification.planes[0].size(), 13U);
  const LinkPlan p = plan(ideal, LaserPattern::kPlusGrid);
  const auto& n = p.neighbours[0];
  EXPECT_EQ(n[6].east, 7U);
  EXPECT_EQ(n[7].west, 6U);
  EXPECT_EQ(n[5].east, 6U);
  EXPECT_EQ(n[7].east, 8U);
  EXPECT_EQ(n[12].east, 0U);  // Across RAAN 0.
  EXPECT_EQ(n[0].west, 12U);
  for (const PlannedLink& link : p.links) {
    if (link.kind == LinkKind::kCrossPlane) {
      EXPECT_NE(plane_of(ideal.classification, link.satellite_a), 6U);
    }
  }
  const auto unassigned = count_unassigned_lasers(p);
  EXPECT_EQ(unassigned[kNoNeighbour], 0U);
  for (const std::size_t s : ideal.classification.planes[0][6].members) {
    EXPECT_EQ(laser_at(p, s, LaserSide::kEast).unassigned, UnassignedReason::kNoFreePartner);
  }
  for (const std::size_t s : ideal.classification.planes[0][7].members) {
    EXPECT_EQ(laser_at(p, s, LaserSide::kWest).unassigned, UnassignedReason::kNoFreePartner);
  }
  // Every east laser is linked or has no partner (each link uses one east laser).
  EXPECT_EQ(count_links_by_kind(p)[kCross] + (unassigned[kNoPartner] / 2), 13U * 12U);
  EXPECT_EQ(violations(ideal, p), 0U);
}

// Two planes 60 deg apart (planes 0 and 1 of a 6-plane shell): near the northern and southern
// turns the planes come close and some pairs see each other at the reference time, but
// gamma = 47.1 deg, so once an orbit every pair is at least 5533 km apart, over the
// 5016.6 km limit. No cross-plane link is planned.
TEST(LinkPlanTest, LineOfSightAtTheStartButNotForAWholeOrbitIsRefused) {
  const std::array<WalkerShell, 1> shells = {WalkerShell{.planes = 6,
                                                         .per_plane = 24,
                                                         .inclination_deg = 53.0,
                                                         .height_km = 550.0,
                                                         .phasing = 1,
                                                         .epoch = kEpoch}};
  const Ideal ideal = ideal_shells(
      shells, IdealOptions{.drop = [](std::size_t, std::size_t p, std::size_t) { return p > 1; },
                           .exclude = {}});
  const auto& planes = ideal.classification.planes[0];
  ASSERT_EQ(planes.size(), 2U);
  std::size_t in_sight = 0;
  for (const std::size_t a : planes[0].members) {
    for (const std::size_t b : planes[1].members) {
      const std::optional<Eigen::Vector3d>& pa = ideal.positions_ecef_km[a];
      const std::optional<Eigen::Vector3d>& pb = ideal.positions_ecef_km[b];
      if (pa.has_value() && pb.has_value() && satsim::topology::has_line_of_sight(*pa, *pb)) {
        ++in_sight;
      }
    }
  }
  ASSERT_GT(in_sight, 0U);
  for (const LaserPattern pattern : satsim::topology::kAllLaserPatterns) {
    const LinkPlan p = plan(ideal, pattern);
    EXPECT_EQ(count_links_by_kind(p)[kCross], 0U) << satsim::topology::name_of(pattern);
    EXPECT_EQ(count_links_by_kind(p)[kSecondCross], 0U) << satsim::topology::name_of(pattern);
    EXPECT_EQ(components(p, 48), 2U) << satsim::topology::name_of(pattern);
  }
}

// Two shells at different heights and tilts: no link between them; each shell's links are the
// ones it gets alone.
TEST(LinkPlanTest, TwoShellsAreNeverLinked) {
  const WalkerShell low{.planes = 12,
                        .per_plane = 12,
                        .inclination_deg = 53.0,
                        .height_km = 470.0,
                        .phasing = 1,
                        .first_id = 100000,
                        .epoch = kEpoch};
  const WalkerShell high{.planes = 10,
                         .per_plane = 14,
                         .inclination_deg = 70.0,
                         .height_km = 486.0,
                         .phasing = 3,
                         .first_id = 200000,
                         .epoch = kEpoch};
  const std::array<WalkerShell, 2> both = {low, high};
  const Ideal ideal = ideal_shells(both);
  const LinkPlan p = plan(ideal, LaserPattern::kSix);
  for (const PlannedLink& link : p.links) {
    EXPECT_EQ(ideal.classification.satellites[link.satellite_a].shell_index,
              ideal.classification.satellites[link.satellite_b].shell_index);
    EXPECT_EQ(ideal.classification.satellites[link.satellite_a].shell_index, link.shell_index);
  }
  const std::array<WalkerShell, 1> only_low = {low};
  const std::array<WalkerShell, 1> only_high = {high};
  EXPECT_EQ(p.links.size(), plan(ideal_shells(only_low), LaserPattern::kSix).links.size() +
                                plan(ideal_shells(only_high), LaserPattern::kSix).links.size());
  EXPECT_EQ(violations(ideal, p), 0U);
}

// An excluded satellite has no laser and no link; its plane's ring closes around it.
TEST(LinkPlanTest, ExcludedSatelliteGetsNothing) {
  const std::array<WalkerShell, 1> shells = {WalkerShell{.planes = 8,
                                                         .per_plane = 12,
                                                         .inclination_deg = 53.0,
                                                         .height_km = 550.0,
                                                         .phasing = 1,
                                                         .epoch = kEpoch}};
  const Ideal ideal = ideal_shells(
      shells, IdealOptions{.drop = {}, .exclude = [](std::size_t, std::size_t p, std::size_t s) {
                             return p == 2 && s == 3;
                           }});
  std::size_t excluded = 0;
  for (std::size_t i = 0; i < ideal.classification.satellites.size(); ++i) {
    if (!satsim::topology::is_plane_member(ideal.classification, i)) {
      excluded = i;
    }
  }
  ASSERT_EQ(ideal.classification.satellites[excluded].norad_cat_id, 100000 + (2 * 12) + 3);
  for (const LaserPattern pattern : satsim::topology::kAllLaserPatterns) {
    const LinkPlan p = plan(ideal, pattern);
    EXPECT_TRUE(p.lasers[excluded].empty());
    for (const PlannedLink& link : p.links) {
      EXPECT_NE(link.satellite_a, excluded);
      EXPECT_NE(link.satellite_b, excluded);
    }
    EXPECT_EQ(count_links_by_kind(p)[kInPlane], 8U * 12U - 1U);
    EXPECT_EQ(violations(ideal, p), 0U);
  }
}

// Planes of 1 and 2 satellites: a plane of 2 has one in-plane link, not two; a plane of 1
// none. Their unused in-plane lasers say why.
TEST(LinkPlanTest, PlanesOfOneAndTwoSatellites) {
  const std::array<WalkerShell, 1> shells = {WalkerShell{.planes = 12,
                                                         .per_plane = 12,
                                                         .inclination_deg = 53.0,
                                                         .height_km = 550.0,
                                                         .phasing = 0,
                                                         .epoch = kEpoch}};
  // Plane 0 keeps satellite 0, plane 1 keeps 0 and 1.
  const Ideal ideal = ideal_shells(
      shells,
      IdealOptions{.drop = [](std::size_t, std::size_t p,
                              std::size_t s) { return (p == 0 && s != 0) || (p == 1 && s > 1); },
                   .exclude = {}});
  const auto& planes = ideal.classification.planes[0];
  ASSERT_EQ(planes[0].members.size(), 1U);
  ASSERT_EQ(planes[1].members.size(), 2U);
  const LinkPlan p = plan(ideal, LaserPattern::kPlusGrid);
  EXPECT_EQ(count_links_by_kind(p)[kInPlane], 0U + 1U + (10U * 12U));
  const std::size_t lone = planes[0].members[0];
  EXPECT_EQ(laser_at(p, lone, LaserSide::kAhead).unassigned, UnassignedReason::kPlaneTooSmall);
  EXPECT_EQ(laser_at(p, lone, LaserSide::kBehind).unassigned, UnassignedReason::kPlaneTooSmall);
  const std::size_t first = planes[1].members[0];
  const std::size_t second = planes[1].members[1];
  EXPECT_TRUE(laser_at(p, first, LaserSide::kAhead).link.has_value());
  EXPECT_EQ(laser_at(p, first, LaserSide::kAhead).link,
            laser_at(p, second, LaserSide::kBehind).link);
  EXPECT_EQ(laser_at(p, first, LaserSide::kBehind).unassigned, UnassignedReason::kPlaneTooSmall);
  EXPECT_EQ(laser_at(p, second, LaserSide::kAhead).unassigned, UnassignedReason::kPlaneTooSmall);
  EXPECT_EQ(count_unassigned_lasers(p)[kTooSmall], 4U);
  EXPECT_TRUE(no_duplicate_pairs(p));
  EXPECT_EQ(violations(ideal, p), 0U);
}

// A shell of one plane: no cross-plane neighbour at all.
TEST(LinkPlanTest, ShellOfOnePlane) {
  const std::array<WalkerShell, 1> shells = {WalkerShell{.planes = 1,
                                                         .per_plane = 15,
                                                         .inclination_deg = 53.0,
                                                         .height_km = 550.0,
                                                         .phasing = 0,
                                                         .epoch = kEpoch}};
  const Ideal ideal = ideal_shells(shells);
  const LinkPlan p = plan(ideal, LaserPattern::kSix);
  EXPECT_EQ(p.links.size(), 15U);
  EXPECT_EQ(p.neighbours[0][0], satsim::topology::PlaneNeighbours{});
  EXPECT_EQ(count_unassigned_lasers(p)[kNoNeighbour], 15U * 4U);
}

// Two planes 20 deg apart (the rest of an 18-plane shell removed): each is the other's east
// and west neighbour, so they are matched on both sides (the planes are 20 deg apart seen from
// either side, so permanent links exist on both), but no pair is linked twice. Two planes east
// would be the plane itself: none.
TEST(LinkPlanTest, ShellOfTwoPlanes) {
  const std::array<WalkerShell, 1> shells = {WalkerShell{.planes = 18,
                                                         .per_plane = 12,
                                                         .inclination_deg = 53.0,
                                                         .height_km = 550.0,
                                                         .phasing = 0,
                                                         .epoch = kEpoch}};
  const Ideal ideal = ideal_shells(
      shells, IdealOptions{.drop = [](std::size_t, std::size_t p, std::size_t) { return p > 1; },
                           .exclude = {}});
  for (const LaserPattern pattern : satsim::topology::kAllLaserPatterns) {
    const LinkPlan p = plan(ideal, pattern);
    const auto& n = p.neighbours[0];
    EXPECT_EQ(n[0].east, 1U);
    EXPECT_EQ(n[0].west, 1U);
    EXPECT_EQ(n[1].east, 0U);
    EXPECT_EQ(n[1].west, 0U);
    EXPECT_FALSE(n[0].second_east.has_value());
    EXPECT_FALSE(n[1].second_west.has_value());
    EXPECT_TRUE(no_duplicate_pairs(p)) << satsim::topology::name_of(pattern);
    EXPECT_EQ(count_links_by_kind(p)[kSecondCross], 0U);
    EXPECT_GT(count_links_by_kind(p)[kCross], 0U);
    for (const std::size_t count : links_per_satellite(p, 24)) {
      EXPECT_LE(count, laser_budget(pattern));
    }
    EXPECT_EQ(violations(ideal, p), 0U);
  }
}

// Three and four planes with six: two planes east is also a neighbour (3 planes) or is
// reached from both sides (4 planes); still no pair is linked twice.
TEST(LinkPlanTest, SixOnThreeAndFourPlanes) {
  for (const std::size_t planes : {std::size_t{3}, std::size_t{4}}) {
    // Keep the first `planes` of a 24-plane shell (15 deg apart).
    const std::array<WalkerShell, 1> shells = {WalkerShell{.planes = 24,
                                                           .per_plane = 12,
                                                           .inclination_deg = 53.0,
                                                           .height_km = 550.0,
                                                           .phasing = 0,
                                                           .epoch = kEpoch}};
    const Ideal ideal =
        ideal_shells(shells, IdealOptions{.drop = [planes](std::size_t, std::size_t p,
                                                           std::size_t) { return p >= planes; },
                                          .exclude = {}});
    const LinkPlan p = plan(ideal, LaserPattern::kSix);
    EXPECT_TRUE(no_duplicate_pairs(p)) << planes;
    for (const std::size_t count : links_per_satellite(p, planes * 12)) {
      EXPECT_LE(count, 6U);
    }
    EXPECT_EQ(violations(ideal, p), 0U);
  }
}

// The plan depends only on the classification and positions; run through
// classify_constellation with the element sets in two orders, it comes out identical.
TEST(LinkPlanTest, DeterministicUnderShuffledInput) {
  const WalkerShell shell{.planes = 12,
                          .per_plane = 10,
                          .inclination_deg = 53.0,
                          .height_km = 550.0,
                          .phasing = 1,
                          .epoch = kEpoch};
  std::vector<satsim::orbit::ElementSet> sets =
      satsim::constellation::testing::walker_element_sets(shell);
  const auto reference = satsim::constellation::testing::utc(kEpoch);
  satsim::constellation::ClassificationConfig config;
  config.shells.push_back(satsim::constellation::ShellDefinition{.name = "test",
                                                                 .inclination_deg = 53.0,
                                                                 .inclination_tolerance_deg = 0.5,
                                                                 .min_height_km = 500.0,
                                                                 .max_height_km = 600.0,
                                                                 .plane_gap_threshold_deg = 2.0,
                                                                 .min_plane_size = 1});
  config.last_launch_without_lasers = {.year = 2021, .number = 44};
  const auto plan_for = [&](const std::vector<satsim::orbit::ElementSet>& input) {
    auto result = satsim::constellation::classify_constellation(input, reference, config);
    const auto& c = std::get<Classification>(result);
    const auto propagators = satsim::topology::plane_member_propagators(c, input);
    const auto positions = satsim::topology::positions_at(propagators, reference, {});
    EXPECT_EQ(positions.failed_count, 0U);
    return satsim::topology::plan_laser_links(c, positions.position_ecef_km,
                                              {.pattern = LaserPattern::kThree});
  };
  const LinkPlan first = plan_for(sets);
  std::mt19937 rng(12345);
  std::ranges::shuffle(sets, rng);
  const LinkPlan second = plan_for(sets);
  EXPECT_EQ(first, second);
  // 12 rings of 10; each of the 12 plane pairs joins 5 east-facing to 5 west-facing lasers.
  EXPECT_EQ(count_links_by_kind(first)[kInPlane], 120U);
  EXPECT_EQ(count_links_by_kind(first)[kCross], 60U);
}

}  // namespace
