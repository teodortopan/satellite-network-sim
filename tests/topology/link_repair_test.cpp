#include "satsim/topology/link_repair.hpp"

#include <gtest/gtest.h>

#include <Eigen/Core>
#include <algorithm>
#include <array>
#include <cstddef>
#include <optional>
#include <random>
#include <set>
#include <utility>
#include <vector>

#include "ideal.hpp"
#include "satsim/constellation/classification.hpp"
#include "satsim/time/civil_time.hpp"
#include "satsim/topology/geometry.hpp"
#include "satsim/topology/link_plan.hpp"
#include "support/synthetic.hpp"

namespace {

using satsim::topology::Laser;
using satsim::topology::LaserPattern;
using satsim::topology::LaserSide;
using satsim::topology::LinkKind;
using satsim::topology::LinkPlan;
using satsim::topology::LinkPlanSettings;
using satsim::topology::PlannedLink;
using satsim::topology::RepairInput;
using satsim::topology::UnassignedReason;
using satsim::topology::testing::Ideal;
using satsim::topology::testing::ideal_shells;
using satsim::topology::testing::IdealOptions;
using satsim::topology::testing::WalkerShell;

constexpr satsim::time::CivilTime kEpoch{2026, 11, 18, 21, 15, 0.0};

// 12 planes 30 deg apart in RAAN, `per_plane` satellites 360 / per_plane deg apart, 550 km,
// 53 deg. At r = 6928 km and R = 6458.137 km two satellites of one plane see each other up to
// 2 acos(R / r) = 42.5 deg apart, and a link in one plane keeps that angle for ever.
Ideal twelve_planes(std::size_t per_plane) {
  const std::array<WalkerShell, 1> shells = {WalkerShell{.planes = 12,
                                                         .per_plane = per_plane,
                                                         .inclination_deg = 53.0,
                                                         .height_km = 550.0,
                                                         .phasing = 1,
                                                         .epoch = kEpoch}};
  return ideal_shells(shells);
}

LinkPlan plan_of(const Ideal& ideal, LaserPattern pattern) {
  return satsim::topology::plan_laser_links(ideal.classification, ideal.positions_ecef_km,
                                            LinkPlanSettings{.pattern = pattern});
}

std::size_t member(const Ideal& ideal, std::size_t plane, std::size_t index) {
  const auto& members = ideal.classification.planes.at(0).at(plane).members;
  return members.at(index % members.size());
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

// The planned partner of `satellite` through its laser facing `side`.
std::size_t partner(const LinkPlan& p, std::size_t satellite, LaserSide side) {
  const PlannedLink& link = p.links.at(laser_at(p, satellite, side).link.value_or(0));
  return link.satellite_a == satellite ? link.satellite_b : link.satellite_a;
}

// What the caller of match_free_lasers gives it after the satellites in `down` failed: a laser
// is free when its planned link touches a down satellite or it never had a partner
// (no_free_partner); the planned links not touching a down satellite are linked.
struct Failure {
  std::vector<char> down;
  std::vector<std::vector<char>> free;
  std::vector<std::pair<std::size_t, std::size_t>> linked;

  [[nodiscard]] RepairInput input(const Ideal& ideal) const {
    return RepairInput{.position_ecef_km = ideal.positions_ecef_km,
                       .satellite_down = down,
                       .laser_free = free,
                       .linked = linked,
                       .atmosphere_km = satsim::topology::kDefaultAtmosphereKm};
  }
};

Failure fail(const Ideal& ideal, const LinkPlan& p, const std::set<std::size_t>& down) {
  const std::size_t n = ideal.classification.satellites.size();
  Failure f;
  f.down.assign(n, 0);
  for (const std::size_t s : down) {
    f.down.at(s) = 1;
  }
  const auto touches_down = [&](const PlannedLink& link) {
    return down.contains(link.satellite_a) || down.contains(link.satellite_b);
  };
  f.free.resize(n);
  for (std::size_t s = 0; s < n; ++s) {
    for (const Laser& l : p.lasers[s]) {
      f.free[s].push_back(
          static_cast<char>((l.link.has_value() && touches_down(p.links[*l.link])) ||
                            l.unassigned == UnassignedReason::kNoFreePartner));
    }
  }
  for (const PlannedLink& link : p.links) {
    if (!touches_down(link)) {
      f.linked.emplace_back(link.satellite_a, link.satellite_b);
    }
  }
  return f;
}

std::vector<PlannedLink> repair(const Ideal& ideal, const LinkPlan& p, const Failure& f) {
  return satsim::topology::match_free_lasers(ideal.classification, p, f.input(ideal));
}

PlannedLink in_plane(std::size_t a, std::size_t b) {
  return PlannedLink{.satellite_a = a,
                     .satellite_b = b,
                     .side_a = LaserSide::kAhead,
                     .side_b = LaserSide::kBehind,
                     .kind = LinkKind::kInPlane,
                     .shell_index = 0};
}

PlannedLink cross(std::size_t west, std::size_t east) {
  return PlannedLink{.satellite_a = west,
                     .satellite_b = east,
                     .side_a = LaserSide::kEast,
                     .side_b = LaserSide::kWest,
                     .kind = LinkKind::kCrossPlane,
                     .shell_index = 0};
}

bool sees_for_ever(const Ideal& ideal, std::size_t a, std::size_t b) {
  const auto& ga = ideal.classification.satellites.at(a).geometry;
  const auto& gb = ideal.classification.satellites.at(b).geometry;
  return ga.has_value() && gb.has_value() &&
         satsim::topology::has_line_of_sight(
             ideal.positions_ecef_km.at(a).value_or(Eigen::Vector3d::Zero()),
             ideal.positions_ecef_km.at(b).value_or(Eigen::Vector3d::Zero())) &&
         satsim::topology::keeps_line_of_sight_for_an_orbit(*ga, *gb);
}

double distance_km(const Ideal& ideal, std::size_t a, std::size_t b) {
  return (ideal.positions_ecef_km.at(a).value_or(Eigen::Vector3d::Zero()) -
          ideal.positions_ecef_km.at(b).value_or(Eigen::Vector3d::Zero()))
      .norm();
}

// plus-grid, 12 planes of 30 (12 deg apart in a plane). Satellite 7 of plane 4 fails; its
// four lasers' partners are freed:
// - in plane 4, satellites 6 (its ahead laser) and 8 (its behind laser), now 24 deg apart:
//   in sight and for ever (24 < 42.5 deg), so they are linked across the gap, 6 -> 8;
// - its east partner E in plane 5 (west laser free) and its west partner W in plane 3 (east
//   laser free). W's east laser faces plane 4, E's west laser faces plane 4 too, and plane 4
//   has no other free laser; E and W are in planes two apart and do not face each other.
// So the result is exactly the one in-plane link. The dead satellite's own lasers are marked
// free as well and must not be used: E and W would pair with it again.
TEST(LinkRepairTest, PlusGridOneDeadSatelliteIsBridgedInItsPlaneOnly) {
  const Ideal ideal = twelve_planes(30);
  const LinkPlan p = plan_of(ideal, LaserPattern::kPlusGrid);
  const std::size_t dead = member(ideal, 4, 7);
  ASSERT_TRUE(laser_at(p, dead, LaserSide::kEast).link.has_value());
  ASSERT_TRUE(laser_at(p, dead, LaserSide::kWest).link.has_value());
  Failure f = fail(ideal, p, {dead});
  for (char& c : f.free.at(dead)) {
    c = 1;
  }
  const std::vector<PlannedLink> links = repair(ideal, p, f);
  EXPECT_EQ(links, (std::vector<PlannedLink>{in_plane(member(ideal, 4, 6), member(ideal, 4, 8))}));
}

// Satellites 7 and 8 of plane 4 fail: 6 and 9 are 36 deg apart, still in sight for ever, and
// one link bridges both. The four freed cross-plane lasers (in planes 3 and 5) face plane 4,
// which has none: nothing else.
TEST(LinkRepairTest, TwoNeighbouringDeadSatellitesAreBridgedByOneLink) {
  const Ideal ideal = twelve_planes(30);
  const LinkPlan p = plan_of(ideal, LaserPattern::kPlusGrid);
  const Failure f = fail(ideal, p, {member(ideal, 4, 7), member(ideal, 4, 8)});
  EXPECT_EQ(repair(ideal, p, f),
            (std::vector<PlannedLink>{in_plane(member(ideal, 4, 6), member(ideal, 4, 9))}));
}

// Satellites 7, 8 and 9 of plane 4 fail: 6 and 10 are 48 deg apart, beyond the 42.5 deg of
// line of sight. The gap stays open.
TEST(LinkRepairTest, AGapWithoutLineOfSightIsNotBridged) {
  const Ideal ideal = twelve_planes(30);
  const LinkPlan p = plan_of(ideal, LaserPattern::kPlusGrid);
  const Failure f = fail(ideal, p, {member(ideal, 4, 7), member(ideal, 4, 8), member(ideal, 4, 9)});
  ASSERT_FALSE(satsim::topology::has_line_of_sight(
      ideal.positions_ecef_km.at(member(ideal, 4, 6)).value_or(Eigen::Vector3d::Zero()),
      ideal.positions_ecef_km.at(member(ideal, 4, 10)).value_or(Eigen::Vector3d::Zero())));
  EXPECT_TRUE(repair(ideal, p, f).empty());
}

// Two planes 60 deg apart (planes 0 and 1 of a 6-plane shell of 24): some pairs are in sight
// at the reference time but none is for a whole orbit (Phase 2's plan makes no cross-plane
// link here, link_plan_test). Every cross-plane laser is free (no_free_partner); the repair
// refuses every pair as the planner did, so nothing is made.
TEST(LinkRepairTest, InSightNowButNotPermanentIsRefused) {
  const std::array<WalkerShell, 1> shells = {WalkerShell{.planes = 6,
                                                         .per_plane = 24,
                                                         .inclination_deg = 53.0,
                                                         .height_km = 550.0,
                                                         .phasing = 1,
                                                         .epoch = kEpoch}};
  const Ideal ideal = ideal_shells(
      shells, IdealOptions{.drop = [](std::size_t, std::size_t p, std::size_t) { return p > 1; },
                           .exclude = {}});
  const LinkPlan p = plan_of(ideal, LaserPattern::kPlusGrid);
  const Failure f = fail(ideal, p, {});
  std::size_t free_in_sight = 0;
  for (std::size_t i = 0; i < 24; ++i) {
    for (std::size_t j = 0; j < 24; ++j) {
      const std::size_t a = member(ideal, 0, i);
      const std::size_t b = member(ideal, 1, j);
      ASSERT_EQ(laser_at(p, a, LaserSide::kEast).unassigned, UnassignedReason::kNoFreePartner);
      ASSERT_EQ(laser_at(p, b, LaserSide::kWest).unassigned, UnassignedReason::kNoFreePartner);
      if (satsim::topology::has_line_of_sight(
              ideal.positions_ecef_km.at(a).value_or(Eigen::Vector3d::Zero()),
              ideal.positions_ecef_km.at(b).value_or(Eigen::Vector3d::Zero()))) {
        ++free_in_sight;
      }
    }
  }
  ASSERT_GT(free_in_sight, 0U);
  EXPECT_TRUE(repair(ideal, p, f).empty());
}

// three, 12 planes of 30: 15 east and 15 west lasers per plane, and the plan pairs them all
// (no laser without a partner). Satellite 6 of plane 4 (even: it faces east) fails. Its
// partner Y in plane 5 has a free west laser, but plane 4 has no free east laser left: Y finds
// nobody, and only the in-plane bridge 5 -> 7 is made.
TEST(LinkRepairTest, ThreeCrossPartnerFindsNobodyWhereNoLaserIsFree) {
  const Ideal ideal = twelve_planes(30);
  const LinkPlan p = plan_of(ideal, LaserPattern::kThree);
  ASSERT_EQ(satsim::topology::count_unassigned_lasers(p), (std::array<std::size_t, 3>{0, 0, 0}));
  const std::size_t dead = member(ideal, 4, 6);
  ASSERT_TRUE(laser_at(p, dead, LaserSide::kEast).link.has_value());
  EXPECT_EQ(repair(ideal, p, fail(ideal, p, {dead})),
            (std::vector<PlannedLink>{in_plane(member(ideal, 4, 5), member(ideal, 4, 7))}));
}

// three, 12 planes of 29: 15 east lasers (even positions 0 to 28) and 14 west per plane, so
// each plane's matching with its east neighbour leaves exactly one east laser without a
// partner, Z (no_free_partner). In plane 4 the east-facing satellite X two places from Z
// fails; its partner Y in plane 5 is freed. The free lasers are then: Z of every plane (east)
// and Y (west). The only facing pair is Z of plane 4 with Y, in sight now and for ever
// (asserted), so the result is the bridge across X, then Z -> Y.
TEST(LinkRepairTest, ThreeCrossPartnerFindsTheLaserThatNeverHadOne) {
  const Ideal ideal = twelve_planes(29);
  const LinkPlan p = plan_of(ideal, LaserPattern::kThree);
  ASSERT_EQ(satsim::topology::count_unassigned_lasers(p)[2], 12U);
  std::optional<std::size_t> z;
  for (std::size_t k = 0; k < 29; ++k) {
    const std::size_t s = member(ideal, 4, k);
    if (k % 2 == 0 && laser_at(p, s, LaserSide::kEast).unassigned) {
      ASSERT_FALSE(z.has_value());
      z = k;
    }
  }
  ASSERT_TRUE(z.has_value());
  const std::size_t zk = z.value_or(0);
  // X: the even position two places from Z (28 + 2 wraps to 1, odd, so the other side then).
  const std::size_t xk = (zk + 2) % 29 % 2 == 0 ? (zk + 2) % 29 : (zk + 27) % 29;
  const std::size_t x = member(ideal, 4, xk);
  ASSERT_TRUE(laser_at(p, x, LaserSide::kEast).link.has_value());
  const std::size_t y = partner(p, x, LaserSide::kEast);
  ASSERT_TRUE(sees_for_ever(ideal, member(ideal, 4, zk), y));
  const std::vector<PlannedLink> links = repair(ideal, p, fail(ideal, p, {x}));
  EXPECT_EQ(links,
            (std::vector<PlannedLink>{in_plane(member(ideal, 4, xk + 28), member(ideal, 4, xk + 1)),
                                      cross(member(ideal, 4, zk), y)}));
}

// plus-grid, 12 planes of 30. In plane 4, satellites 6 and 8 fail; in plane 5, the east
// partner D of satellite 7 (of plane 4) fails. Then plane 4 has one free east laser, 7's, and
// plane 5 two free west lasers, those of E6 and E8 (the east partners of 6 and 8). 7 can take
// only one of them: the nearer at this step. Also bridged: 5 -> 7 and 7 -> 9 in plane 4, and
// the gap left by D in plane 5. Lasers facing planes 3 and 6 find nobody.
TEST(LinkRepairTest, ALaserIsUsedOnceAndTheShortestPairWins) {
  const Ideal ideal = twelve_planes(30);
  const LinkPlan p = plan_of(ideal, LaserPattern::kPlusGrid);
  const std::size_t s6 = member(ideal, 4, 6);
  const std::size_t s7 = member(ideal, 4, 7);
  const std::size_t s8 = member(ideal, 4, 8);
  const std::size_t d = partner(p, s7, LaserSide::kEast);
  const std::size_t e6 = partner(p, s6, LaserSide::kEast);
  const std::size_t e8 = partner(p, s8, LaserSide::kEast);
  ASSERT_TRUE(sees_for_ever(ideal, s7, e6));
  ASSERT_TRUE(sees_for_ever(ideal, s7, e8));
  ASSERT_NE(distance_km(ideal, s7, e6), distance_km(ideal, s7, e8));
  const std::size_t nearer = distance_km(ideal, s7, e6) < distance_km(ideal, s7, e8) ? e6 : e8;
  // D's place in plane 5, for its bridge.
  const auto& plane5 = ideal.classification.planes[0][5].members;
  const std::size_t dk = static_cast<std::size_t>(std::ranges::find(plane5, d) - plane5.begin());
  ASSERT_LT(dk, 30U);
  const std::vector<PlannedLink> links = repair(ideal, p, fail(ideal, p, {s6, s8, d}));
  EXPECT_EQ(links,
            (std::vector<PlannedLink>{
                in_plane(member(ideal, 4, 5), s7), in_plane(s7, member(ideal, 4, 9)),
                in_plane(member(ideal, 5, dk + 29), member(ideal, 5, dk + 1)), cross(s7, nearer)}));
}

TEST(LinkRepairTest, NothingFreeGivesNothing) {
  const Ideal ideal = twelve_planes(30);
  const LinkPlan p = plan_of(ideal, LaserPattern::kSix);
  Failure f = fail(ideal, p, {member(ideal, 4, 7)});
  for (auto& lasers : f.free) {
    std::ranges::fill(lasers, 0);
  }
  EXPECT_TRUE(repair(ideal, p, f).empty());
}

// The result depends only on the input: twice the same call, and the same call with the
// linked pairs listed in another order and the other way round, give the same links in the
// same order (in-plane before cross-plane, as checked by the tests above).
TEST(LinkRepairTest, Deterministic) {
  const Ideal ideal = twelve_planes(30);
  const LinkPlan p = plan_of(ideal, LaserPattern::kPlusGrid);
  const Failure f = fail(ideal, p,
                         {member(ideal, 4, 6), member(ideal, 4, 8),
                          partner(p, member(ideal, 4, 7), LaserSide::kEast), member(ideal, 9, 0)});
  const std::vector<PlannedLink> first = repair(ideal, p, f);
  EXPECT_EQ(first.size(), 5U);
  EXPECT_EQ(repair(ideal, p, f), first);
  Failure shuffled = f;
  std::mt19937 rng(4);
  std::ranges::shuffle(shuffled.linked, rng);
  for (auto& [a, b] : shuffled.linked) {
    std::swap(a, b);
  }
  EXPECT_EQ(repair(ideal, p, shuffled), first);
}

}  // namespace
