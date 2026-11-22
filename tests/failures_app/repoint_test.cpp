#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "failures/arguments.hpp"
#include "failures/geometry.hpp"
#include "failures/loop.hpp"
#include "failures/reports.hpp"
#include "failures/run.hpp"
#include "failures/timeline.hpp"
#include "failures/view.hpp"
#include "fake_geometry.hpp"
#include "inputs/inputs.hpp"
#include "routing/engine.hpp"
#include "routing/settings.hpp"
#include "satsim/core/physical_constants.hpp"
#include "satsim/failure/schedule.hpp"
#include "satsim/routing/strategy.hpp"
#include "satsim/time/civil_time.hpp"
#include "satsim/time/utc_time.hpp"
#include "satsim/topology/ground_links.hpp"
#include "satsim/topology/link_plan.hpp"
#include "satsim/topology/link_state.hpp"
#include "satsim/topology/physical_checks.hpp"
#include "satsim/topology/positions.hpp"
#include "step_world.hpp"
#include "support/synthetic.hpp"
#include "test_support.hpp"
#include "topology/ideal.hpp"

// Re-matching on an ideal shell (as tests/topology/link_repair_test.cpp): plus-grid, 12 planes
// 30 deg apart in RAAN, 30 satellites a plane 12 deg apart, 550 km, 53 deg. One dead satellite
// is bridged in its plane by its two in-plane neighbours (24 deg apart, in sight for ever); its
// cross-plane partners face its plane, where no laser is free, and find nobody.

namespace {

using satsim::failure::ElementKind;
using satsim::failure::Failure;
using satsim::failures_app::Fact;
using satsim::failures_app::ForceChange;
using satsim::failures_app::SubjectKind;
using satsim::failures_app::Timeline;
using satsim::failures_app::TimelineSettings;
using satsim::failures_app::testing::IdealGeometry;
using satsim::topology::LaserPattern;
using satsim::topology::LaserSide;
using satsim::topology::LinkForce;
using satsim::topology::LinkKind;
using satsim::topology::LinkPlan;
using satsim::topology::PlannedLink;
using satsim::topology::testing::Ideal;
using satsim::topology::testing::WalkerShell;

constexpr double kInf = std::numeric_limits<double>::infinity();
constexpr satsim::time::CivilTime kEpoch{2026, 11, 18, 21, 15, 0.0};

const Ideal& twelve_planes() {
  static const Ideal kIdeal = [] {
    const std::array<WalkerShell, 1> shells = {WalkerShell{.planes = 12,
                                                           .per_plane = 30,
                                                           .inclination_deg = 53.0,
                                                           .height_km = 550.0,
                                                           .phasing = 1,
                                                           .epoch = kEpoch}};
    return satsim::topology::testing::ideal_shells(shells);
  }();
  return kIdeal;
}

const LinkPlan& plan() {
  static const LinkPlan kPlan = satsim::topology::plan_laser_links(
      twelve_planes().classification, twelve_planes().positions_ecef_km,
      satsim::topology::LinkPlanSettings{.pattern = LaserPattern::kPlusGrid});
  return kPlan;
}

std::size_t member(std::size_t p, std::size_t index) {
  const auto& members = twelve_planes().classification.planes.at(0).at(p).members;
  return members.at(index % members.size());
}

std::size_t link_between(std::size_t a, std::size_t b) {
  for (std::size_t i = 0; i < plan().links.size(); ++i) {
    const PlannedLink& l = plan().links[i];
    if ((l.satellite_a == a && l.satellite_b == b) || (l.satellite_a == b && l.satellite_b == a)) {
      return i;
    }
  }
  ADD_FAILURE() << "no link " << a << "-" << b;
  return 0;
}

PlannedLink in_plane(std::size_t a, std::size_t b) {
  return PlannedLink{.satellite_a = a,
                     .satellite_b = b,
                     .side_a = LaserSide::kAhead,
                     .side_b = LaserSide::kBehind,
                     .kind = LinkKind::kInPlane,
                     .shell_index = 0};
}

Failure failure(ElementKind kind, std::size_t element, double start_s,
                std::optional<double> length_s = std::nullopt) {
  const satsim::time::UtcTime start = satsim::constellation::testing::utc(kEpoch);
  return Failure{.kind = kind,
                 .element = element,
                 .start_utc = start.plus_seconds(start_s),
                 .start_s = start_s,
                 .length_s = length_s};
}

Timeline run_on(const LinkPlan& on, const std::vector<Failure>& failures, bool repoint = true,
                double detection_s = 0.1) {
  IdealGeometry geometry(twelve_planes(), satsim::constellation::testing::utc(kEpoch), 1.0);
  TimelineSettings settings;
  settings.times.repoint = repoint;
  settings.times.detection_s = detection_s;
  settings.step_s = 1.0;
  settings.step_count = 120;
  settings.gateway_count = 0;
  return satsim::failures_app::build_timeline(
      twelve_planes().classification, on, satsim::failure::FailureSchedule{.failures = failures},
      settings, geometry);
}

// plan() without link `removed`: its two lasers never had a partner (kNoFreePartner).
LinkPlan without_link(std::size_t removed) {
  LinkPlan p = plan();
  p.links.erase(p.links.begin() + static_cast<std::ptrdiff_t>(removed));
  for (auto& lasers : p.lasers) {
    for (satsim::topology::Laser& laser : lasers) {
      if (laser.link == removed) {
        laser.link.reset();
        laser.unassigned = satsim::topology::UnassignedReason::kNoFreePartner;
      } else if (laser.link && *laser.link > removed) {
        --*laser.link;
      }
    }
  }
  return p;
}

Timeline run(const std::vector<Failure>& failures, bool repoint = true, double detection_s = 0.1) {
  IdealGeometry geometry(twelve_planes(), satsim::constellation::testing::utc(kEpoch), 1.0);
  TimelineSettings settings;
  settings.times.repoint = repoint;
  settings.times.detection_s = detection_s;
  settings.step_s = 1.0;
  settings.step_count = 120;
  settings.gateway_count = 0;
  return satsim::failures_app::build_timeline(
      twelve_planes().classification, plan(),
      satsim::failure::FailureSchedule{.failures = failures}, settings, geometry);
}

std::vector<const Fact*> facts_of_link(const Timeline& t, std::size_t link) {
  std::vector<const Fact*> result;
  for (const Fact& f : t.facts) {
    if (f.subject.kind == SubjectKind::kLink && f.subject.index == link) {
      result.push_back(&f);
    }
  }
  return result;
}

TEST(RepointTest, PlanHasNoLaserWithoutPartner) {
  EXPECT_EQ(satsim::topology::count_unassigned_lasers(plan()),
            (std::array<std::size_t, 3>{0, 0, 0}));
}

// Satellite 7 of plane 4 dies for good at 10 s. Its neighbours 6 and 8 detect at 10.1 s, so
// their freed lasers are eligible at step 11, when 6 -> 8 is made: acquiring from 11 s, in
// force from 11 + 30 s, when both ends know.
TEST(RepointTest, DeadSatelliteIsBridgedAtTheFirstStepAfterDetection) {
  const std::size_t s6 = member(4, 6);
  const std::size_t s7 = member(4, 7);
  const std::size_t s8 = member(4, 8);
  const Timeline t = run({failure(ElementKind::kSatellite, s7, 10.0)});
  ASSERT_EQ(t.links.size(), plan().links.size() + 1);
  EXPECT_EQ(t.links.back(), in_plane(s6, s8));
  const std::size_t r = t.links.size() - 1;
  const double up = 11.0 + 30.0;
  EXPECT_EQ(t.force[r],
            (std::vector<ForceChange>{{.from_s = -kInf, .force = LinkForce::kNotInForce},
                                      {.from_s = 11.0, .force = LinkForce::kAcquiring},
                                      {.from_s = up, .force = LinkForce::kInForce}}));
  const auto facts = facts_of_link(t, r);
  ASSERT_EQ(facts.size(), 1U);
  EXPECT_FALSE(facts[0]->down);
  EXPECT_EQ(facts[0]->t_s, up);
  ASSERT_EQ(facts[0]->detectors.size(), 2U);
  EXPECT_EQ(facts[0]->detectors[0].t_s, up);
  EXPECT_EQ(facts[0]->detectors[1].t_s, up);
  EXPECT_EQ(facts[0]->planner_known_s, kInf);  // No gateway in this world.
}

// Detection 2 s: the lasers are eligible at 12 s exactly, a step: matched at step 12.
TEST(RepointTest, DetectionOnAStepIsMatchedAtThatStep) {
  const Timeline t = run({failure(ElementKind::kSatellite, member(4, 7), 10.0)}, true, 2.0);
  ASSERT_EQ(t.links.size(), plan().links.size() + 1);
  const std::size_t r = t.links.size() - 1;
  EXPECT_EQ(t.force[r],
            (std::vector<ForceChange>{{.from_s = -kInf, .force = LinkForce::kNotInForce},
                                      {.from_s = 12.0, .force = LinkForce::kAcquiring},
                                      {.from_s = 12.0 + 30.0, .force = LinkForce::kInForce}}));
}

// Without re-pointing nothing is made.
TEST(RepointTest, NothingWithoutRepointing) {
  const Timeline t = run({failure(ElementKind::kSatellite, member(4, 7), 10.0)}, false);
  EXPECT_EQ(t.links.size(), plan().links.size());
}

// The same satellite returns at 10 + 50 s: the repair link 6 -> 8, in force since 41 s, uses
// 6's ahead laser and 8's behind laser, whose original partner is back: it is dropped at 60 s
// (both ends know at once), and the dead satellite's four planned links acquire again from
// 60 s, in force from 90 s.
TEST(RepointTest, AReturnDropsTheRepairLinkAndReacquiresTheOriginals) {
  const std::size_t s6 = member(4, 6);
  const std::size_t s7 = member(4, 7);
  const std::size_t s8 = member(4, 8);
  const Timeline t = run({failure(ElementKind::kSatellite, s7, 10.0, 50.0)});
  ASSERT_EQ(t.links.size(), plan().links.size() + 1);
  const std::size_t r = t.links.size() - 1;
  EXPECT_EQ(t.force[r],
            (std::vector<ForceChange>{{.from_s = -kInf, .force = LinkForce::kNotInForce},
                                      {.from_s = 11.0, .force = LinkForce::kAcquiring},
                                      {.from_s = 41.0, .force = LinkForce::kInForce},
                                      {.from_s = 60.0, .force = LinkForce::kNotInForce}}));
  const auto repair_facts = facts_of_link(t, r);
  ASSERT_EQ(repair_facts.size(), 2U);
  EXPECT_TRUE(repair_facts[1]->down);
  EXPECT_EQ(repair_facts[1]->t_s, 60.0);
  ASSERT_EQ(repair_facts[1]->detectors.size(), 2U);
  EXPECT_EQ(repair_facts[1]->detectors[0].t_s, 60.0);
  for (const std::size_t i : {link_between(s6, s7), link_between(s7, s8)}) {
    EXPECT_EQ(t.force[i],
              (std::vector<ForceChange>{{.from_s = -kInf, .force = LinkForce::kInForce},
                                        {.from_s = 10.0, .force = LinkForce::kFailed},
                                        {.from_s = 60.0, .force = LinkForce::kAcquiring},
                                        {.from_s = 90.0, .force = LinkForce::kInForce}}))
        << i;
    const auto facts = facts_of_link(t, i);
    ASSERT_EQ(facts.size(), 2U);
    EXPECT_FALSE(facts[1]->down);
    EXPECT_EQ(facts[1]->t_s, 90.0);
  }
  EXPECT_EQ(t.force_at(90.0).size(), t.links.size());
}

// A laser link (6 -> 7 of plane 4) fails for good: both ends detect and their lasers are free,
// but they may not simply be joined again, and nobody else faces them: nothing is made.
TEST(RepointTest, TheEndsOfAFailedLinkAreNotJoinedAgain) {
  const std::size_t link = link_between(member(4, 6), member(4, 7));
  const Timeline t = run({failure(ElementKind::kLaserLink, link, 10.0)});
  EXPECT_EQ(t.links.size(), plan().links.size());
}

// An old failure an hour before the run: its repair link was matched and acquired long ago:
// in force from the start, with an old "up" fact at -3600 + 30 s.
TEST(RepointTest, OldFailureRepairLinkIsInForceFromTheStart) {
  const Timeline t = run({failure(ElementKind::kSatellite, member(4, 7), -3600.0)});
  ASSERT_EQ(t.links.size(), plan().links.size() + 1);
  const std::size_t r = t.links.size() - 1;
  EXPECT_EQ(t.links.back(), in_plane(member(4, 6), member(4, 8)));
  EXPECT_EQ(t.force_at(0.0)[r], LinkForce::kInForce);
  const auto facts = facts_of_link(t, r);
  ASSERT_EQ(facts.size(), 1U);
  EXPECT_TRUE(facts[0]->old);
  EXPECT_FALSE(facts[0]->down);
  EXPECT_EQ(facts[0]->t_s, -3600.0 + 30.0);
  EXPECT_EQ(facts[0]->planner_known_s, 0.0);
}

// Only 10 s old: matched on step 0's geometry, acquiring from the start until -10 + 30 s, then
// an ordinary "up" fact known at once by both ends.
TEST(RepointTest, YoungOldFailureRepairLinkIsStillAcquiring) {
  const Timeline t = run({failure(ElementKind::kSatellite, member(4, 7), -10.0)});
  ASSERT_EQ(t.links.size(), plan().links.size() + 1);
  const std::size_t r = t.links.size() - 1;
  const double up = -10.0 + 30.0;
  EXPECT_EQ(t.force[r],
            (std::vector<ForceChange>{{.from_s = -kInf, .force = LinkForce::kNotInForce},
                                      {.from_s = 0.0, .force = LinkForce::kAcquiring},
                                      {.from_s = up, .force = LinkForce::kInForce}}));
  const auto facts = facts_of_link(t, r);
  ASSERT_EQ(facts.size(), 1U);
  EXPECT_FALSE(facts[0]->old);
  EXPECT_EQ(facts[0]->t_s, up);
  ASSERT_EQ(facts[0]->detectors.size(), 2U);
  EXPECT_EQ(facts[0]->detectors[0].t_s, up);
}

// Two lasers that never had a partner (a cross-plane link taken out of the plan) are matched
// by the step-0 call an old failure elsewhere makes: that link carries traffic from the first
// step, with an old "up" fact (spec 4.8 Details), not one acquisition time later.
TEST(RepointTest, OldCallLinkBetweenNeverPartneredLasersIsInForceFromTheStart) {
  std::optional<std::size_t> cross;
  for (std::size_t i = 0; i < plan().links.size() && !cross; ++i) {
    const PlannedLink& l = plan().links[i];
    const auto& placement = twelve_planes().classification.satellites[l.satellite_a].placement;
    if (l.kind == LinkKind::kCrossPlane &&
        std::get<satsim::constellation::PlaneAssignment>(placement).plane_index == 0) {
      cross = i;
    }
  }
  ASSERT_TRUE(cross.has_value());
  const PlannedLink removed = plan().links[*cross];
  const LinkPlan p = without_link(*cross);
  const Timeline t = run_on(p, {failure(ElementKind::kSatellite, member(6, 7), -3600.0)});
  std::optional<std::size_t> made;
  for (std::size_t i = p.links.size(); i < t.links.size(); ++i) {
    const PlannedLink& l = t.links[i];
    if ((l.satellite_a == removed.satellite_a && l.satellite_b == removed.satellite_b) ||
        (l.satellite_a == removed.satellite_b && l.satellite_b == removed.satellite_a)) {
      made = i;
    }
  }
  ASSERT_TRUE(made.has_value());
  EXPECT_EQ(t.force[*made],
            (std::vector<ForceChange>{{.from_s = -kInf, .force = LinkForce::kInForce}}));
  const auto facts = facts_of_link(t, *made);
  ASSERT_EQ(facts.size(), 1U);
  EXPECT_TRUE(facts[0]->old);
  EXPECT_FALSE(facts[0]->down);
  EXPECT_EQ(facts[0]->t_s, 0.0);
  EXPECT_EQ(facts[0]->planner_known_s, 0.0);
}

// The ideal geometry with hand-made ground: every step sees the same cities and gateways.
class SitedGeometry final : public satsim::failures_app::Geometry {
 public:
  SitedGeometry(IdealGeometry& ideal,
                std::vector<std::vector<satsim::topology::VisibleSatellite>> cities,
                std::vector<std::vector<satsim::topology::VisibleSatellite>> gateways)
      : ideal_(&ideal), cities_(std::move(cities)), gateways_(std::move(gateways)) {}

  [[nodiscard]] const satsim::failures_app::StepGeometry& at(std::int64_t step) override {
    std::unique_ptr<satsim::failures_app::StepGeometry>& entry = cache_[step];
    if (!entry) {
      entry = std::make_unique<satsim::failures_app::StepGeometry>(ideal_->at(step));
      entry->city_visible = cities_;
      entry->gateway_visible = gateways_;
    }
    return *entry;
  }
  [[nodiscard]] satsim::topology::LinkState link_state(std::int64_t step, std::size_t a,
                                                       std::size_t b) override {
    return ideal_->link_state(step, a, b);
  }

 private:
  IdealGeometry* ideal_;
  std::vector<std::vector<satsim::topology::VisibleSatellite>> cities_;
  std::vector<std::vector<satsim::topology::VisibleSatellite>> gateways_;
  std::map<std::int64_t, std::unique_ptr<satsim::failures_app::StepGeometry>> cache_;
};

// Re-pointing through the loop (best case, start planner, re-plan every 5 s, 60 steps of 1 s).
// City A sees only satellite 6 of plane 4, city B only satellite 8, both at 600 km, so the
// baseline route is A 6 7 8 B over two in-plane hops (12 deg apart). Gateway g0 sees 6 at
// 700 km. Satellite 7 dies at 10 s; 6 and 8 detect at 10.1 s and are bridged at step 11 (24 deg
// apart, in sight for ever, shorter than the two hops it replaces): acquiring until 11 + 30 s.
//  - The truth carries traffic over the bridge only from 41 s: acquiring before (down with the
//    reason acquiring), up from then.
//  - The reference, which plans on the truth, takes A 6 8 B at its first plan after that, 45 s,
//    and not at 40 s.
//  - The planner learns "6-8 up" from 6's report to g0, plus its processing time of 5 s: at
//    41 + (700 km / c + 0.001) + 5, about 46.003 s; so it keeps its detour at 45 s and takes
//    A 6 8 B at 50 s.
//  - The force checks of spec 6.1 find nothing at any step; the loop's checks neither.
//  - The facts CSV shows the bridge's "up" fact and the events CSV the routes over it.
TEST(RepointTest, ThroughTheLoopABridgeCarriesTrafficOnceAcquiredAndKnown) {
  namespace fa = satsim::failures_app;
  namespace ra = satsim::routing_app;
  const std::size_t s6 = member(4, 6);
  const std::size_t s7 = member(4, 7);
  const std::size_t s8 = member(4, 8);
  const auto& classification = twelve_planes().classification;
  const std::size_t n = classification.satellites.size();
  IdealGeometry ideal(twelve_planes(), satsim::constellation::testing::utc(kEpoch), 1.0);
  SitedGeometry geometry(ideal, {{fa::testing::seen(s6, 600.0)}, {fa::testing::seen(s8, 600.0)}},
                         {{fa::testing::seen(s6, 700.0)}});
  fa::ReactionTimes times;
  times.repoint = true;
  times.planner_processing_s = 5.0;
  constexpr std::int64_t kRunSteps = 60;
  const fa::TimelineSettings settings{.times = times,
                                      .model = satsim::app::EndpointModel::kBestCase,
                                      .hop_delay_s = 0.0,
                                      .step_s = 1.0,
                                      .step_count = kRunSteps,
                                      .gateway_count = 1,
                                      .atmosphere_km = satsim::topology::kDefaultAtmosphereKm};
  const Timeline timeline = satsim::failures_app::build_timeline(
      classification, plan(),
      satsim::failure::FailureSchedule{.failures = {failure(ElementKind::kSatellite, s7, 10.0)}},
      settings, geometry);
  ASSERT_EQ(timeline.links.size(), plan().links.size() + 1);
  const std::size_t bridge = timeline.links.size() - 1;
  EXPECT_EQ(timeline.links[bridge], in_plane(s6, s8));
  const auto up_facts = facts_of_link(timeline, bridge);
  ASSERT_EQ(up_facts.size(), 1U);
  EXPECT_NEAR(up_facts[0]->planner_known_s,
              41.0 + (700.0 / satsim::kSpeedOfLightVacuumKmPerS + 0.001) + 5.0, 1e-12);

  ra::EngineNetwork network{.model = satsim::app::EndpointModel::kBestCase,
                            .satellite_count = n,
                            .city_count = 2,
                            .gateway_count = 1,
                            .plan = nullptr,
                            .directions = {{.from = 0, .to = 1, .lower_bound_s = 0.001}},
                            .end_delays_s = {}};
  const fa::LoopSettings loop_settings{
      .engine = ra::EngineSettings{.strategies = {satsim::routing::Strategy{}},
                                   .step_ns = 1'000'000'000,
                                   .replan_ns = 5'000'000'000,
                                   .slot_ns = 5'000'000'000,
                                   .planner = ra::Planner::kStart,
                                   .serving = {},
                                   .backup = false,
                                   .hop_delay_s = 0.0},
      .reactions = {fa::Reaction::kPlanner},
      .replan_on_report = false,
      .times = times,
      .step_count = kRunSteps};
  fa::FailureLoop loop(timeline, network, loop_settings);
  fa::testing::GeometryWorld world(geometry, timeline);
  const std::size_t a = n;      // A's open dish.
  const std::size_t b = n + 3;  // B's receiving dish.
  const std::vector<std::size_t> over_bridge = {a, s6, s8, b};
  std::map<std::int64_t, std::vector<fa::LaneRow>> rows;
  std::size_t force_failures = 0;
  for (std::int64_t k = 0; k < kRunSteps; ++k) {
    world.at(k);
    std::vector<fa::LaneRow> step_rows;
    const auto error = loop.step(k, world, step_rows);
    ASSERT_FALSE(error.has_value()) << error.value_or("");
    const fa::OverlaidStep& truth = loop.truth();
    const satsim::topology::LinkState& state = truth.links.states.at(bridge);
    if (k >= 11 && k < 41) {
      EXPECT_EQ(state.down_reason, satsim::topology::LinkDownReason::kAcquiring) << k;
    } else if (k >= 41) {
      EXPECT_TRUE(state.up()) << k;
    }
    const satsim::topology::StepPositions positions{
        .utc = ideal.at(k).utc,
        .earth_orientation = {},
        .position_ecef_km = ideal.at(k).position_ecef_km,
        .propagation_error = std::vector<std::optional<satsim::orbit::PropagationError>>(n),
        .failed_count = 0};
    force_failures +=
        satsim::topology::run_force_checks(classification, timeline.links, timeline.planned_count,
                                           truth.force, truth.links, positions,
                                           truth.satellite_down, satsim::topology::LinkLimits{})
            .total();
    rows[k] = std::move(step_rows);
  }
  loop.finish();
  EXPECT_EQ(force_failures, 0U);
  EXPECT_EQ(loop.checks().total(), 0U);
  // Rows: baseline, reference, planner.
  EXPECT_EQ(rows.at(5)[0].row.route, (std::vector<std::size_t>{a, s6, s7, s8, b}));
  EXPECT_TRUE(rows.at(40)[1].row.ok);
  EXPECT_NE(rows.at(40)[1].row.route, over_bridge);
  EXPECT_TRUE(rows.at(45)[1].row.ok);
  EXPECT_EQ(rows.at(45)[1].row.route, over_bridge);
  EXPECT_TRUE(rows.at(45)[2].row.ok);
  EXPECT_NE(rows.at(45)[2].row.route, over_bridge);
  EXPECT_TRUE(rows.at(50)[2].row.ok);
  EXPECT_EQ(rows.at(50)[2].row.route, over_bridge);

  // The CSVs, written for a run made of this world.
  const std::vector<std::string_view> arguments = {"--snapshot",   "unused",
                                                   "--ground",     "unused",
                                                   "--cities",     "unused",
                                                   "--start",      "2026-11-18T21:15:00",
                                                   "--duration-s", "60",
                                                   "--step-s",     "1",
                                                   "--pattern",    "plus-grid",
                                                   "--model",      "best-case",
                                                   "--out",        "-",
                                                   "--reactions",  "planner",
                                                   "--strategies", "lowest-delay",
                                                   "--replan-s",   "5"};
  const fa::ParsedArguments parsed = fa::parse_arguments(arguments);
  ASSERT_TRUE(parsed.options.has_value()) << parsed.error.value_or("");
  fa::FailureRun r{.run = ra::PreparedRun{satsim::app::Inputs{}, parsed.options->routing},
                   .options = *parsed.options,
                   .schedule = {},
                   .timeline_settings = settings,
                   .timeline = timeline,
                   .reports = fa::fact_reports(timeline, settings, geometry),
                   .floods = {}};
  r.run.working = classification;
  r.run.cities.resize(2);
  r.run.cities[0].site.name = "A";
  r.run.cities[1].site.name = "B";
  r.run.gateways.resize(1);
  r.run.gateways[0].site.name = "G";
  r.run.directions = {{.from = 0, .to = 1}};
  const std::string n6 = std::to_string(classification.satellites[s6].norad_cat_id);
  const std::string n8 = std::to_string(classification.satellites[s8].norad_cat_id);
  ASSERT_LT(s6, s8);
  ASSERT_LT(classification.satellites[s6].norad_cat_id, classification.satellites[s8].norad_cat_id);
  const std::string subject = std::string("link:").append(n6).append("-").append(n8);
  const std::string detectors =
      std::string("satellite:").append(n6).append("@41;satellite:").append(n8).append("@41");
  const std::string bridge_route = std::string(n6).append(" ").append(n8);
  std::ostringstream facts_csv;
  ASSERT_TRUE(fa::write_facts(r, facts_csv));
  bool fact_row = false;
  for (const auto& row : satsim::propagate::testing::parse_csv(facts_csv.str())) {
    if (row.at(1) == subject) {
      fact_row = true;
      EXPECT_EQ(row.at(2), "up");
      EXPECT_EQ(row.at(3), "41");
      EXPECT_EQ(row.at(5), detectors);
      EXPECT_NE(row.at(6), "never");
    }
  }
  EXPECT_TRUE(fact_row);
  fa::FailureStatistics stats;
  for (std::size_t e = 0; e < loop.engine_count(); ++e) {
    stats.kinds.push_back(loop.kind(e));
  }
  stats.intervals = loop.intervals();
  std::ostringstream events_csv;
  ASSERT_TRUE(fa::write_events(r, stats, events_csv));
  bool reference_row = false;
  bool planner_row = false;
  for (const auto& row : satsim::propagate::testing::parse_csv(events_csv.str())) {
    if (row.at(13) == bridge_route && row.at(9) == "delivered") {
      reference_row = reference_row || (row.at(8) == "reference" && row.at(0) == "45");
      planner_row = planner_row || (row.at(8) == "planner" && row.at(0) == "50");
    }
  }
  EXPECT_TRUE(reference_row);
  EXPECT_TRUE(planner_row);
}

}  // namespace
