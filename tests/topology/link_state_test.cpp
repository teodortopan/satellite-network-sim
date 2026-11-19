#include "satsim/topology/link_state.hpp"

#include <gtest/gtest.h>

#include <Eigen/Core>
#include <Eigen/Geometry>
#include <array>
#include <cmath>
#include <cstddef>
#include <limits>
#include <numbers>
#include <optional>
#include <variant>
#include <vector>

#include "ideal.hpp"
#include "satsim/frames/sun.hpp"
#include "satsim/orbit/element_set.hpp"
#include "satsim/orbit/propagator.hpp"
#include "satsim/time/civil_time.hpp"
#include "satsim/time/utc_time.hpp"
#include "satsim/topology/link_plan.hpp"
#include "satsim/topology/positions.hpp"
#include "support/synthetic.hpp"

namespace {

using satsim::time::UtcTime;
using satsim::topology::check_limits;
using satsim::topology::geometric_link_state;
using satsim::topology::LinkDownReason;
using satsim::topology::LinkLimits;
using satsim::topology::LinkPlan;
using satsim::topology::LinkState;
using satsim::topology::LinkStateEvaluator;
using satsim::topology::PlannedLink;
using satsim::topology::StepLinks;
using satsim::topology::testing::step_positions;

constexpr double kRadPerDeg = std::numbers::pi / 180.0;
constexpr satsim::time::CivilTime kEpoch{2026, 11, 18, 21, 15, 0.0};

using Positions = std::vector<std::optional<Eigen::Vector3d>>;

// A plan with one link between satellites 0 and 1.
LinkPlan one_link_plan() {
  LinkPlan plan;
  plan.links.push_back(PlannedLink{});
  plan.links.back().satellite_a = 0;
  plan.links.back().satellite_b = 1;
  return plan;
}

// Two satellites at 7000 km radius, 1000 km apart: line of sight.
const Eigen::Vector3d kA(7000.0, -500.0, 0.0);
const Eigen::Vector3d kB(7000.0, 500.0, 0.0);
// Two satellites on opposite sides of the Earth: no line of sight.
const Eigen::Vector3d kFar(-7000.0, 500.0, 0.0);

TEST(LinkLimitsTest, DefaultsAndChecks) {
  const LinkLimits defaults;
  EXPECT_EQ(defaults.atmosphere_km, 80.0);
  EXPECT_FALSE(defaults.range_cap_km.has_value());
  EXPECT_FALSE(defaults.sun_exclusion_rad.has_value());
  EXPECT_EQ(defaults.lock_on_s, 0.0);
  EXPECT_FALSE(check_limits(defaults).has_value());
  EXPECT_TRUE(check_limits({.atmosphere_km = -1.0}).has_value());
  EXPECT_TRUE(check_limits({.range_cap_km = 0.0}).has_value());
  EXPECT_TRUE(check_limits({.range_cap_km = std::numeric_limits<double>::quiet_NaN()}).has_value());
  EXPECT_TRUE(check_limits({.sun_exclusion_rad = 0.0}).has_value());
  EXPECT_TRUE(check_limits({.sun_exclusion_rad = 4.0}).has_value());
  EXPECT_TRUE(check_limits({.lock_on_s = -1.0}).has_value());
  EXPECT_FALSE(check_limits({.range_cap_km = 3000.0, .sun_exclusion_rad = 0.1, .lock_on_s = 30.0})
                   .has_value());
}

TEST(GeometricLinkStateTest, LengthLineOfSightAndMissingEnd) {
  const LinkState up = geometric_link_state(kA, kB, std::nullopt, {});
  EXPECT_TRUE(up.up());
  EXPECT_DOUBLE_EQ(up.length_km.value_or(-1.0), 1000.0);
  const LinkState hidden = geometric_link_state(kA, kFar, std::nullopt, {});
  EXPECT_EQ(hidden.down_reason, LinkDownReason::kNoLineOfSight);
  EXPECT_TRUE(hidden.length_km.has_value());
  const LinkState missing = geometric_link_state(kA, std::nullopt, std::nullopt, {});
  EXPECT_EQ(missing.down_reason, LinkDownReason::kEndNotPropagated);
  EXPECT_FALSE(missing.length_km.has_value());
}

TEST(GeometricLinkStateTest, RangeCap) {
  EXPECT_EQ(geometric_link_state(kA, kB, std::nullopt, {.range_cap_km = 999.0}).down_reason,
            LinkDownReason::kOverRangeCap);
  EXPECT_TRUE(geometric_link_state(kA, kB, std::nullopt, {.range_cap_km = 1000.0}).up());
  EXPECT_TRUE(geometric_link_state(kA, kB, std::nullopt, {.range_cap_km = 1001.0}).up());
  // Line of sight is tested first.
  EXPECT_EQ(geometric_link_state(kA, kFar, std::nullopt, {.range_cap_km = 10.0}).down_reason,
            LinkDownReason::kNoLineOfSight);
}

// The beam from A to B points 10 deg from the Sun; seen from B, the beam back to A points
// 170 deg from it. Only one end looks at the Sun, and that is enough.
TEST(GeometricLinkStateTest, SunExclusionFromEitherEnd) {
  const Eigen::Vector3d sun(1.0, 0.0, 0.0);
  const Eigen::Vector3d a(0.0, 0.0, 7000.0);
  const Eigen::Vector3d b =
      a + (1500.0 * Eigen::Vector3d(std::cos(10.0 * kRadPerDeg), std::sin(10.0 * kRadPerDeg), 0.0));
  const LinkLimits wide{.sun_exclusion_rad = 15.0 * kRadPerDeg};
  const LinkLimits narrow{.sun_exclusion_rad = 5.0 * kRadPerDeg};
  EXPECT_EQ(geometric_link_state(a, b, sun, wide).down_reason, LinkDownReason::kSunExclusion);
  EXPECT_EQ(geometric_link_state(b, a, sun, wide).down_reason, LinkDownReason::kSunExclusion);
  EXPECT_TRUE(geometric_link_state(a, b, sun, narrow).up());
  EXPECT_TRUE(geometric_link_state(b, a, sun, narrow).up());
  // Off: the Sun does not matter.
  EXPECT_TRUE(geometric_link_state(a, b, sun, {}).up());
  // A beam across the Sun direction is never excluded.
  const Eigen::Vector3d across = a + Eigen::Vector3d(0.0, 1500.0, 0.0);
  EXPECT_TRUE(geometric_link_state(a, across, sun, wide).up());
}

// Through the evaluator, with the Sun's real direction at the step: a beam pointing at it is
// down, the same beam rotated 90 deg is up.
TEST(LinkStateEvaluatorTest, SunExclusionUsesTheSunOfTheStep) {
  const UtcTime t = satsim::constellation::testing::utc(kEpoch);
  const Eigen::Vector3d sun = satsim::frames::sun_direction_ecef(t, {});
  const Eigen::Vector3d side = sun.unitOrthogonal();
  const Eigen::Vector3d a = 6900.0 * side;
  const LinkLimits limits{.sun_exclusion_rad = 3.0 * kRadPerDeg};
  LinkStateEvaluator toward(one_link_plan(), limits);
  const StepLinks at_sun = toward.step(step_positions(t, Positions{a, a + (800.0 * sun)}));
  EXPECT_EQ(at_sun.states[0].down_reason, LinkDownReason::kSunExclusion);
  LinkStateEvaluator across(one_link_plan(), limits);
  const Eigen::Vector3d other = sun.cross(side);
  const StepLinks away = across.step(step_positions(t, Positions{a, a + (800.0 * other)}));
  EXPECT_TRUE(away.states[0].up());
}

// Lock-on of 30 s with 15 s steps. The link is available at every step except t = 45 s.
//   t:      0        15       30   45            60       75       90
//   state:  locking  locking  up   no sight      locking  locking  up
TEST(LinkStateEvaluatorTest, LockOnOverSeveralStepsIncludingTheFirst) {
  const UtcTime t0 = satsim::constellation::testing::utc(kEpoch);
  LinkStateEvaluator evaluator(one_link_plan(), LinkLimits{.lock_on_s = 30.0});
  const std::array<bool, 7> available = {true, true, true, false, true, true, true};
  const std::array<std::optional<LinkDownReason>, 7> expected = {LinkDownReason::kLockingOn,
                                                                 LinkDownReason::kLockingOn,
                                                                 std::nullopt,
                                                                 LinkDownReason::kNoLineOfSight,
                                                                 LinkDownReason::kLockingOn,
                                                                 LinkDownReason::kLockingOn,
                                                                 std::nullopt};
  for (std::size_t k = 0; k < available.size(); ++k) {
    const UtcTime t = t0.plus_seconds(15.0 * static_cast<double>(k));
    const StepLinks links =
        evaluator.step(step_positions(t, Positions{kA, available[k] ? kB : kFar}));
    ASSERT_EQ(links.states.size(), 1U);
    EXPECT_EQ(links.states[0].down_reason, expected[k]) << "step " << k;
    EXPECT_EQ(links.utc, t);
  }
}

TEST(LinkStateEvaluatorTest, LockOnZeroIsUpAtOnceAndShorterLockOn) {
  const UtcTime t0 = satsim::constellation::testing::utc(kEpoch);
  LinkStateEvaluator immediate(one_link_plan(), {});
  EXPECT_TRUE(immediate.step(step_positions(t0, Positions{kA, kB})).states[0].up());

  LinkStateEvaluator fifteen(one_link_plan(), LinkLimits{.lock_on_s = 15.0});
  EXPECT_EQ(fifteen.step(step_positions(t0, Positions{kA, kB})).states[0].down_reason,
            LinkDownReason::kLockingOn);
  EXPECT_TRUE(
      fifteen.step(step_positions(t0.plus_seconds(15.0), Positions{kA, kB})).states[0].up());
  // A missing end interrupts availability like any other reason.
  EXPECT_EQ(fifteen.step(step_positions(t0.plus_seconds(30.0), Positions{kA, std::nullopt}))
                .states[0]
                .down_reason,
            LinkDownReason::kEndNotPropagated);
  EXPECT_EQ(
      fifteen.step(step_positions(t0.plus_seconds(45.0), Positions{kA, kB})).states[0].down_reason,
      LinkDownReason::kLockingOn);
}

// positions_at: a satellite that has decayed by the step gives an error, never a position; a
// satellite without a propagator gives neither. A link to the decayed one is down.
TEST(PositionsTest, FailedPropagationIsCountedNotAPosition) {
  satsim::constellation::testing::WalkerShell shell;
  shell.planes = 1;
  shell.per_plane = 2;
  shell.epoch = kEpoch;
  std::vector<satsim::orbit::ElementSet> sets =
      satsim::constellation::testing::walker_element_sets(shell);
  sets[1].mean_motion_rev_per_day = 16.2;  // About 270 km with heavy drag: decays within a day.
  sets[1].bstar_per_earth_radius = 0.025;
  std::vector<std::optional<satsim::orbit::Propagator>> propagators;
  propagators.reserve(sets.size() + 1);
  for (const auto& set : sets) {
    propagators.emplace_back(
        std::get<satsim::orbit::Propagator>(satsim::orbit::Propagator::create(set)));
  }
  propagators.emplace_back(std::nullopt);
  const UtcTime later = satsim::constellation::testing::utc(kEpoch).plus_seconds(86400.0);
  const auto positions = satsim::topology::positions_at(propagators, later, {});
  ASSERT_EQ(positions.position_ecef_km.size(), 3U);
  EXPECT_TRUE(positions.position_ecef_km[0].has_value());
  EXPECT_FALSE(positions.propagation_error[0].has_value());
  EXPECT_FALSE(positions.position_ecef_km[1].has_value());
  EXPECT_EQ(positions.propagation_error[1], satsim::orbit::PropagationError::kDecayed);
  EXPECT_FALSE(positions.position_ecef_km[2].has_value());
  EXPECT_FALSE(positions.propagation_error[2].has_value());
  EXPECT_EQ(positions.failed_count, 1U);
  EXPECT_NEAR(positions.position_ecef_km[0].value_or(Eigen::Vector3d::Zero()).norm(),
              6378.0 + 550.0, 15.0);

  LinkStateEvaluator evaluator(one_link_plan(), {});
  EXPECT_EQ(evaluator.step(positions).states[0].down_reason, LinkDownReason::kEndNotPropagated);
}

// Propagators are built for plane members only, matched by catalogue number.
TEST(PositionsTest, PropagatorsForPlaneMembersOnly) {
  const std::array<satsim::topology::testing::WalkerShell, 1> shells = {
      satsim::topology::testing::WalkerShell{
          .planes = 2, .per_plane = 3, .phasing = 0, .epoch = kEpoch}};
  const auto ideal = satsim::topology::testing::ideal_shells(
      shells, {.drop = {}, .exclude = [](std::size_t, std::size_t p, std::size_t s) {
                 return p == 1 && s == 2;
               }});
  // Element sets in reverse order: matching is by catalogue number, not position.
  std::vector<satsim::orbit::ElementSet> reversed(ideal.element_sets.rbegin(),
                                                  ideal.element_sets.rend());
  const auto propagators =
      satsim::topology::plane_member_propagators(ideal.classification, reversed);
  ASSERT_EQ(propagators.size(), 6U);
  const auto positions =
      satsim::topology::positions_at(propagators, satsim::constellation::testing::utc(kEpoch), {});
  for (std::size_t i = 0; i < 6; ++i) {
    EXPECT_EQ(propagators[i].has_value(), i != 5) << i;
    if (i != 5) {
      EXPECT_LT((positions.position_ecef_km[i].value_or(Eigen::Vector3d::Zero()) -
                 ideal.positions_ecef_km[i].value_or(Eigen::Vector3d::Ones()))
                    .norm(),
                1e-9);
    }
  }
}

}  // namespace
