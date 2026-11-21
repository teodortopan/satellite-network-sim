// The failures program's loop on hand-made worlds: every interval of the planner, local-repair
// and reference lanes worked out by hand for a satellite, a link and a gateway failure; the
// planner's stale plan; a backup that is itself dead; a dish repair shared by two directions;
// --replan-on-report; old and transient failures; the measures of one case; each new check
// made to fire through a seam.
//
// The world (best case unless said otherwise), on the failure tests' small network
// (tests/failure/fixture.hpp): links 0: 0-1 (1000 km), 1: 1-2 (1500 km), 2: 0-4 (2000 km),
// 3: 4-5 (1000 km), 4: 1-5 (1000 km), 5: 16-17. City A (0) sees satellite 0 at 500 km; city B
// (1) sees satellites 2 and 5 at 500 km. Gateway g0 sees satellite 4 at 600 km. 1 s steps, 40
// steps (the run ends at 39 s), re-plan every 10 s, start planner, lowest-delay, no hop delay,
// detection 0.1 s, per hop 0.001 s. Routes from A to B:
//   A 0 1 5 B   3000 km (the shortest)    A 0 1 2 B   3500 km    A 0 4 5 B   4000 km

#include "failures/loop.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "failure/fixture.hpp"
#include "failures/measures.hpp"
#include "failures/settings.hpp"
#include "failures/timeline.hpp"
#include "failures/view.hpp"
#include "fake_geometry.hpp"
#include "inputs/inputs.hpp"
#include "routing/engine.hpp"
#include "routing/settings.hpp"
#include "satsim/core/physical_constants.hpp"
#include "satsim/failure/schedule.hpp"
#include "satsim/routing/strategy.hpp"
#include "satsim/topology/ground_links.hpp"
#include "satsim/topology/link_state.hpp"

namespace {

using satsim::app::EndpointModel;
using satsim::failure::ElementKind;
using satsim::failure::Failure;
using satsim::failure::testing::failure_at;
using satsim::failures_app::DirectionState;
using satsim::failures_app::FailureLoop;
using satsim::failures_app::Interval;
using satsim::failures_app::LaneKind;
using satsim::failures_app::LaneMeasures;
using satsim::failures_app::LaneRow;
using satsim::failures_app::LoopHooks;
using satsim::failures_app::LoopSettings;
using satsim::failures_app::Reaction;
using satsim::failures_app::Status;
using satsim::failures_app::StepGeometry;
using satsim::failures_app::StepWorld;
using satsim::failures_app::Subject;
using satsim::failures_app::SubjectKind;
using satsim::failures_app::Timeline;
using satsim::failures_app::TimelineSettings;
using satsim::failures_app::testing::FakeGeometry;
using satsim::failures_app::testing::seen;
using satsim::routing_app::DropCause;
using satsim::topology::VisibleSatellite;
using Route = std::vector<std::size_t>;

constexpr double kC = satsim::kSpeedOfLightVacuumKmPerS;
constexpr double kHop = 0.001;
constexpr std::int64_t kSteps = 40;
constexpr double kEnd = 39.0;

// The StepWorld of a FakeGeometry: offset j is step k + j.
class FakeWorld final : public StepWorld {
 public:
  FakeWorld(FakeGeometry& geometry, const Timeline& timeline)
      : geometry_(&geometry), timeline_(&timeline) {}
  void at(std::int64_t k) { k_ = k; }
  [[nodiscard]] std::size_t known_steps() const override { return 1; }
  [[nodiscard]] const StepGeometry& geometry(std::size_t offset) const override {
    return geometry_->at(k_ + static_cast<std::int64_t>(offset));
  }
  [[nodiscard]] const satsim::topology::StepLinks& geometric(std::size_t offset) const override {
    const std::int64_t k = k_ + static_cast<std::int64_t>(offset);
    auto it = cache_.find(k);
    if (it == cache_.end()) {
      it =
          cache_.emplace(k, satsim::failures_app::geometric_states(*geometry_, timeline_->links, k))
              .first;
    }
    return it->second;
  }
  [[nodiscard]] std::vector<double> times_left_in_view_s(std::size_t,
                                                         std::span<const VisibleSatellite> visible,
                                                         double horizon_s) const override {
    return std::vector<double>(visible.size(), horizon_s);
  }
  [[nodiscard]] bool moving_north(std::size_t) const override { return false; }

 private:
  FakeGeometry* geometry_;
  const Timeline* timeline_;
  std::int64_t k_ = 0;
  mutable std::map<std::int64_t, satsim::topology::StepLinks> cache_;
};

struct Scene {
  FakeGeometry geometry;
  EndpointModel model;
  satsim::failures_app::ReactionTimes times{.detection_s = 0.1,
                                            .hop_processing_s = kHop,
                                            .compute_s = 0.1,
                                            .planner_processing_s = 0.0,
                                            .acquisition_s = 30.0,
                                            .repoint = false};
  bool replan_on_report = false;
  satsim::routing_app::EngineNetwork network;
  std::vector<satsim::routing::Strategy> strategies = {satsim::routing::Strategy{}};
  Timeline timeline;
  std::unique_ptr<FailureLoop> loop;
  std::vector<std::vector<LaneRow>> rows;

  explicit Scene(EndpointModel m = EndpointModel::kBestCase) : model(m) {
    geometry.base.position_ecef_km.assign(22, std::nullopt);
    geometry.base.city_visible = {{seen(0, 500.0)}, {seen(2, 500.0), seen(5, 500.0)}};
    geometry.base.gateway_visible = {{seen(4, 600.0)}};
    geometry.set_length(0, 1, 1000.0);
    geometry.set_length(1, 2, 1500.0);
    geometry.set_length(0, 4, 2000.0);
    geometry.set_length(4, 5, 1000.0);
    geometry.set_length(1, 5, 1000.0);
    geometry.set_length(16, 17, 1000.0);
    network.model = m;
    network.satellite_count = 22;
    network.city_count = 2;
    network.gateway_count = 1;
    network.directions = {{.from = 0, .to = 1, .lower_bound_s = 0.001, .has_home_pop = true}};
    network.end_delays_s = {0.001};
  }

  void run(const std::vector<Failure>& failures, const LoopHooks& hooks = {}) {
    const TimelineSettings settings{.times = times,
                                    .model = model,
                                    .hop_delay_s = 0.0,
                                    .step_s = 1.0,
                                    .step_count = kSteps,
                                    .gateway_count = network.gateway_count,
                                    .atmosphere_km = 80.0};
    timeline = satsim::failures_app::build_timeline(
        satsim::failure::testing::small().classification, satsim::failure::testing::small_plan(),
        satsim::failure::FailureSchedule{.failures = failures}, settings, geometry);
    LoopSettings loop_settings{
        .engine =
            satsim::routing_app::EngineSettings{.strategies = strategies,
                                                .step_ns = 1'000'000'000,
                                                .replan_ns = 10'000'000'000,
                                                .slot_ns = 10'000'000'000,
                                                .planner = satsim::routing_app::Planner::kStart,
                                                .serving = {},
                                                .backup = false,
                                                .hop_delay_s = 0.0},
        .reactions = {Reaction::kPlanner, Reaction::kLocalRepair},
        .replan_on_report = replan_on_report,
        .times = times,
        .step_count = kSteps};
    loop = std::make_unique<FailureLoop>(timeline, network, loop_settings, hooks);
    FakeWorld world(geometry, timeline);
    rows.clear();
    for (std::int64_t k = 0; k < kSteps; ++k) {
      world.at(k);
      std::vector<LaneRow> step_rows;
      const auto error = loop->step(k, world, step_rows);
      ASSERT_FALSE(error.has_value()) << error.value_or("");
      rows.push_back(std::move(step_rows));
    }
    loop->finish();
  }

  [[nodiscard]] std::size_t engine_of(LaneKind kind) const {
    for (std::size_t e = 0; e < loop->engine_count(); ++e) {
      if (loop->kind(e) == kind) {
        return e;
      }
    }
    ADD_FAILURE() << "no lane";
    return 0;
  }

  [[nodiscard]] std::vector<Interval> lane(LaneKind kind, std::size_t d = 0,
                                           std::size_t s = 0) const {
    std::vector<Interval> result;
    for (const Interval& iv : loop->intervals()) {
      if (iv.engine == engine_of(kind) && iv.direction == d && iv.strategy == s) {
        result.push_back(iv);
      }
    }
    return result;
  }

  // Best case node numbers: satellites, then per city its open and receiving dish.
  [[nodiscard]] static std::size_t open(std::size_t city) { return 22 + 2 * city; }
  [[nodiscard]] static std::size_t receiving(std::size_t city) { return 22 + 2 * city + 1; }
};

// The expected interval: [start, end) in a state, with a route, an element, a delay.
struct Expect {
  double start_s;
  double end_s;
  DirectionState state;
  Route route;
  std::optional<Subject> element = std::nullopt;
  std::optional<double> delay_s = std::nullopt;
};

void expect_intervals(const std::vector<Interval>& got, const std::vector<Expect>& want) {
  ASSERT_EQ(got.size(), want.size());
  for (std::size_t i = 0; i < want.size(); ++i) {
    SCOPED_TRACE(i);
    EXPECT_NEAR(got[i].start_s, want[i].start_s, 1e-12);
    EXPECT_NEAR(got[i].end_s, want[i].end_s, 1e-12);
    EXPECT_EQ(got[i].status.state, want[i].state);
    EXPECT_EQ(got[i].status.route, want[i].route);
    EXPECT_EQ(got[i].status.element, want[i].element);
    if (want[i].delay_s) {
      ASSERT_TRUE(got[i].status.delay_s.has_value());
      EXPECT_NEAR(got[i].status.delay_s.value_or(-1.0), want[i].delay_s.value_or(-1.0), 1e-15);
    }
  }
}

Subject satellite(std::size_t i) { return {.kind = SubjectKind::kSatellite, .index = i}; }
Subject link(std::size_t i) { return {.kind = SubjectKind::kLink, .index = i}; }
Subject gateway(std::size_t i) { return {.kind = SubjectKind::kGateway, .index = i}; }

const Route kA015B = {22, 0, 1, 5, 25};
const Route kA012B = {22, 0, 1, 2, 25};
const Route kA045B = {22, 0, 4, 5, 25};
const double k015 = 3000.0 / kC;
const double k045 = 4000.0 / kC;
const double k012 = 3500.0 / kC;

// The planner's knowing instant of a fact: the first matching fact's.
double known(const Timeline& t, Subject subject, bool down = true) {
  for (const auto& f : t.facts) {
    if (f.subject == subject && f.down == down) {
      return f.planner_known_s;
    }
  }
  ADD_FAILURE() << "no fact";
  return 0.0;
}

// Satellite 1 fails for good at 5 s. Its neighbours 0, 2 and 5 detect the dead links at 5.1 s.
// The planner learns 1-5 from satellite 5 (5-4-g0, 1600 km, 2 hops), 0-1 from satellite 0
// (0-4-g0, 2600 km) and 1-2 from satellite 2 through city B's dish (2-B-5-4-g0, 2600 km,
// 4 hops); never "satellite 1 down" (nobody sees it from the ground).
//  - planner and reference: black hole from 5 s until the plan at 10 s, then A 0 4 5 B;
//  - local repair: satellite 0, before the dead one, detected at 5.1 s: then its backup from 0
//    avoiding 1 (and A): A 0 4 5 B. At 10 s the planning graph has lost 0-1: planned as usual.
TEST(LoopTest, SatelliteFailureEveryInterval) {
  Scene scene;
  scene.run({failure_at(ElementKind::kSatellite, 1, 5.0)});
  const Timeline& t = scene.timeline;
  EXPECT_DOUBLE_EQ(known(t, link(4)), 5.1 + (600.0 / kC + kHop) + (1000.0 / kC + kHop));
  EXPECT_DOUBLE_EQ(known(t, link(0)), 5.1 + (600.0 / kC + kHop) + (2000.0 / kC + kHop));
  EXPECT_EQ(known(t, satellite(1)), std::numeric_limits<double>::infinity());
  for (const LaneKind kind : {LaneKind::kPlanner, LaneKind::kReference}) {
    SCOPED_TRACE(static_cast<int>(kind));
    expect_intervals(scene.lane(kind),
                     {{0.0, 5.0, DirectionState::kDelivered, kA015B, std::nullopt, k015},
                      {5.0, 10.0, DirectionState::kBlackHole, kA015B, satellite(1)},
                      {10.0, kEnd, DirectionState::kDelivered, kA045B, std::nullopt, k045}});
  }
  expect_intervals(scene.lane(LaneKind::kLocalRepair),
                   {{0.0, 5.0, DirectionState::kDelivered, kA015B, std::nullopt, k015},
                    {5.0, 5.0 + 0.1, DirectionState::kBlackHole, kA015B, satellite(1)},
                    {5.0 + 0.1, kEnd, DirectionState::kDelivered, kA045B}});
  expect_intervals(scene.lane(LaneKind::kBaseline),
                   {{0.0, kEnd, DirectionState::kDelivered, kA015B, std::nullopt, k015}});
  EXPECT_EQ(scene.loop->checks().total(), 0U);
  EXPECT_EQ(scene.loop->repairs(), 1U);
  // The rows at the step instants: the plan at 10 s replaced the repaired route by the same.
  const auto& at10 = scene.rows.at(10);
  ASSERT_EQ(at10.size(), 4U);
  EXPECT_EQ(at10[3].row.plan, satsim::routing::PlanOutcome::kKept);
  EXPECT_EQ(at10[2].row.plan, satsim::routing::PlanOutcome::kReplaced);
  EXPECT_EQ(scene.rows.at(5)[2].status.cause, DropCause::kFailedSatellite);
  EXPECT_FALSE(scene.rows.at(5)[2].row.ok);
  EXPECT_TRUE(scene.rows.at(6)[3].row.ok);
}

// The measures of that run (spec 4.7), t_f = 5 s.
TEST(LoopTest, SatelliteFailureMeasures) {
  Scene scene;
  scene.run({failure_at(ElementKind::kSatellite, 1, 5.0)});
  const auto& loop = *scene.loop;
  const std::vector<LaneMeasures> m = satsim::failures_app::lane_measures(
      loop.intervals(), loop.step_delays(), loop.engine_count(), 1, 1, 1.0, loop.end_s(), 5.0);
  ASSERT_EQ(m.size(), 4U);
  const LaneMeasures& planner = m[scene.engine_of(LaneKind::kPlanner)];
  EXPECT_DOUBLE_EQ(planner.black_hole_s.value_or(-1.0), 5.0);
  EXPECT_DOUBLE_EQ(planner.lost_s.value_or(-1.0), 5.0);
  EXPECT_DOUBLE_EQ(planner.restore_s.value_or(-1.0), 5.0);
  EXPECT_DOUBLE_EQ(planner.settle_s.value_or(-1.0), 0.0);  // The reference does the same.
  ASSERT_EQ(planner.extra_ms.size(), 30U);                 // Steps 10 to 39.
  EXPECT_NEAR(planner.extra_ms.front(), (k045 - k015) * 1000.0, 1e-12);
  EXPECT_FALSE(planner.extra_settled_ms.has_value());  // Not delivered at 5 s.
  const LaneMeasures& repair = m[scene.engine_of(LaneKind::kLocalRepair)];
  EXPECT_NEAR(repair.black_hole_s.value_or(-1.0), 0.1, 1e-12);
  EXPECT_NEAR(repair.lost_s.value_or(-1.0), 0.1, 1e-12);
  EXPECT_NEAR(repair.restore_s.value_or(-1.0), 0.1, 1e-12);
  EXPECT_DOUBLE_EQ(repair.settle_s.value_or(-1.0),
                   5.0);                             // Differs from the reference over (5.1, 10).
  ASSERT_EQ(repair.extra_ms.size(), 34U);            // Steps 6 to 39.
  ASSERT_TRUE(repair.extra_settled_ms.has_value());  // At 10 s.
  EXPECT_NEAR(repair.extra_settled_ms.value_or(-1.0), (k045 - k015) * 1000.0, 1e-12);
  // No failure instant: nothing.
  const std::vector<LaneMeasures> none =
      satsim::failures_app::lane_measures(loop.intervals(), loop.step_delays(), loop.engine_count(),
                                          1, 1, 1.0, loop.end_s(), std::nullopt);
  EXPECT_FALSE(none[2].black_hole_s.has_value());
  EXPECT_FALSE(none[2].settle_s.has_value());
  EXPECT_TRUE(none[2].extra_ms.empty());
}

// Link 1-5 fails at 5 s; satellite 1, its near end, detects at 5.1 s and repairs onto its
// backup avoiding 5 (and A, 0): A 0 1 2 B. The planner learns it (from 1 or 5) and plans
// A 0 1 2 B at 10 s.
TEST(LoopTest, LinkFailureEveryInterval) {
  Scene scene;
  scene.run({failure_at(ElementKind::kLaserLink, 4, 5.0)});
  for (const LaneKind kind : {LaneKind::kPlanner, LaneKind::kReference}) {
    expect_intervals(scene.lane(kind),
                     {{0.0, 5.0, DirectionState::kDelivered, kA015B},
                      {5.0, 10.0, DirectionState::kBlackHole, kA015B, link(4)},
                      {10.0, kEnd, DirectionState::kDelivered, kA012B, std::nullopt, k012}});
  }
  expect_intervals(scene.lane(LaneKind::kLocalRepair),
                   {{0.0, 5.0, DirectionState::kDelivered, kA015B},
                    {5.0, 5.1, DirectionState::kBlackHole, kA015B, link(4)},
                    {5.1, kEnd, DirectionState::kDelivered, kA012B, std::nullopt, k012}});
  EXPECT_EQ(scene.loop->checks().total(), 0U);
}

// The planner hears late (processing 10 s): at 10 s it still believes satellite 1 works and
// plans A 0 1 5 B again (a dead element); it learns at about 15.1 s and plans A 0 4 5 B at 20.
// Local repair keeps its repaired route at 10 s (the planning graph still has 0-1, the dead
// hop; the plan is written kept) and is planned as usual at 20 s.
TEST(LoopTest, StalePlanChoosesTheDeadElementAndRepairIsKept) {
  Scene scene;
  scene.times.planner_processing_s = 10.0;
  scene.run({failure_at(ElementKind::kSatellite, 1, 5.0)});
  expect_intervals(scene.lane(LaneKind::kPlanner),
                   {{0.0, 5.0, DirectionState::kDelivered, kA015B},
                    {5.0, 20.0, DirectionState::kBlackHole, kA015B, satellite(1)},
                    {20.0, kEnd, DirectionState::kDelivered, kA045B}});
  expect_intervals(scene.lane(LaneKind::kLocalRepair),
                   {{0.0, 5.0, DirectionState::kDelivered, kA015B},
                    {5.0, 5.1, DirectionState::kBlackHole, kA015B, satellite(1)},
                    {5.1, kEnd, DirectionState::kDelivered, kA045B}});
  EXPECT_EQ(scene.rows.at(10)[2].row.plan, satsim::routing::PlanOutcome::kKept);
  EXPECT_EQ(scene.rows.at(10)[3].row.plan, satsim::routing::PlanOutcome::kKept);
  EXPECT_EQ(scene.rows.at(10)[3].row.route, kA045B);
  EXPECT_EQ(scene.loop->checks().total(), 0U);
}

// Satellite 4 died at 3 s (unnoticed by the plan of 0 s, whose graph still has it); satellite 1
// dies at 5 s. The repair at 5.1 s takes the backup A 0 4 5 B, itself dead: a black hole on 4,
// with no second repair. Nobody can report satellite 1's links (g0 sees only 4), so the
// planner's graph at 10, 20 and 30 s still has 0-1, the dead hop: the repaired route is kept
// to the end, a black hole until the planner acts (here, never).
TEST(LoopTest, BackupThatIsItselfDeadStaysABlackHole) {
  Scene scene;
  scene.run(
      {failure_at(ElementKind::kSatellite, 4, 3.0), failure_at(ElementKind::kSatellite, 1, 5.0)});
  expect_intervals(scene.lane(LaneKind::kLocalRepair),
                   {{0.0, 5.0, DirectionState::kDelivered, kA015B},
                    {5.0, 5.1, DirectionState::kBlackHole, kA015B, satellite(1)},
                    {5.1, kEnd, DirectionState::kBlackHole, kA045B, satellite(4)}});
  EXPECT_EQ(scene.rows.at(10)[3].row.plan, satsim::routing::PlanOutcome::kKept);
  // The planner's own lane takes its stale shortest path, through the dead satellite 1.
  expect_intervals(scene.lane(LaneKind::kPlanner),
                   {{0.0, 5.0, DirectionState::kDelivered, kA015B},
                    {5.0, kEnd, DirectionState::kBlackHole, kA015B, satellite(1)}});
  EXPECT_EQ(scene.loop->repairs(), 1U);
  EXPECT_EQ(scene.loop->checks().total(), 0U);
}

// --replan-on-report: the planner plans the instant it learns a fact. First 1-5 (at t_a): its
// view still has 0-1 and 1-2, so A 0 1 2 B, through the dead satellite; then 0-1 (at t_b):
// A 0 4 5 B. The reference keeps to its regular plans.
TEST(LoopTest, ReplanOnReport) {
  Scene scene;
  scene.replan_on_report = true;
  scene.run({failure_at(ElementKind::kSatellite, 1, 5.0)});
  const double t_a = known(scene.timeline, link(4));
  const double t_b = known(scene.timeline, link(0));
  ASSERT_LT(t_a, t_b);
  ASSERT_LT(t_b, known(scene.timeline, link(1)));
  expect_intervals(scene.lane(LaneKind::kPlanner),
                   {{0.0, 5.0, DirectionState::kDelivered, kA015B},
                    {5.0, t_a, DirectionState::kBlackHole, kA015B, satellite(1)},
                    {t_a, t_b, DirectionState::kBlackHole, kA012B, satellite(1)},
                    {t_b, kEnd, DirectionState::kDelivered, kA045B}});
  expect_intervals(scene.lane(LaneKind::kReference),
                   {{0.0, 5.0, DirectionState::kDelivered, kA015B},
                    {5.0, 10.0, DirectionState::kBlackHole, kA015B, satellite(1)},
                    {10.0, kEnd, DirectionState::kDelivered, kA045B}});
  // Local repair repaired at 5.1 < t_a; the plans on report find the dead hop 0-1 at t_a (still
  // believed up): kept; at t_b it is gone: planned, the same route.
  expect_intervals(scene.lane(LaneKind::kLocalRepair),
                   {{0.0, 5.0, DirectionState::kDelivered, kA015B},
                    {5.0, 5.1, DirectionState::kBlackHole, kA015B, satellite(1)},
                    {5.1, kEnd, DirectionState::kDelivered, kA045B}});
  EXPECT_EQ(scene.loop->checks().total(), 0U);
}

// An old failure (satellite 1, an hour before the run): known by everyone from the first
// step; every lane takes A 0 4 5 B from 0 s and nothing else happens.
TEST(LoopTest, OldFailureKnownFromTheFirstStep) {
  Scene scene;
  scene.run({failure_at(ElementKind::kSatellite, 1, -3600.0)});
  for (const LaneKind kind : {LaneKind::kReference, LaneKind::kPlanner, LaneKind::kLocalRepair}) {
    expect_intervals(scene.lane(kind), {{0.0, kEnd, DirectionState::kDelivered, kA045B}});
  }
  expect_intervals(scene.lane(LaneKind::kBaseline),
                   {{0.0, kEnd, DirectionState::kDelivered, kA015B}});
  EXPECT_EQ(scene.loop->checks().total(), 0U);
}

// Satellite 1 fails at 5 s for 9.5 s: back at 14.5 s, between steps; its links acquire for 2 s
// and carry traffic from 16.5 s, when both ends know. The planner learns the "up" facts through
// reports and plans A 0 1 5 B again at 20 s, as the reference.
TEST(LoopTest, TransientFailure) {
  Scene scene;
  scene.times.acquisition_s = 2.0;
  scene.run({failure_at(ElementKind::kSatellite, 1, 5.0, 9.5)});
  EXPECT_TRUE(
      std::binary_search(scene.timeline.changes.begin(), scene.timeline.changes.end(), 14.5 + 2.0));
  for (const LaneKind kind : {LaneKind::kPlanner, LaneKind::kReference}) {
    expect_intervals(scene.lane(kind),
                     {{0.0, 5.0, DirectionState::kDelivered, kA015B},
                      {5.0, 10.0, DirectionState::kBlackHole, kA015B, satellite(1)},
                      {10.0, 20.0, DirectionState::kDelivered, kA045B},
                      {20.0, kEnd, DirectionState::kDelivered, kA015B}});
  }
  expect_intervals(scene.lane(LaneKind::kLocalRepair),
                   {{0.0, 5.0, DirectionState::kDelivered, kA015B},
                    {5.0, 5.1, DirectionState::kBlackHole, kA015B, satellite(1)},
                    {5.1, 20.0, DirectionState::kDelivered, kA045B},
                    {20.0, kEnd, DirectionState::kDelivered, kA015B}});
  EXPECT_EQ(scene.loop->checks().total(), 0U);
}

// A realistic world: city A's dish serves from satellite 0 (both 0 and 4 seen; equal
// elevations, the lower catalogue number); gateway g0 sees 5 at 500 km, gateway g1 sees 4 at
// 900 km; every end delay 1 ms. Node numbers: satellites 0-21, open and receiving dishes 22-27,
// gateways 28 and 29, ends 30 and 31 (two directions, A to B and A to C), serving dishes 32-34
// (with one direction: end 30, serving dishes 31-33).
void make_realistic(Scene& scene) {
  scene.geometry.base.city_visible = {{seen(0, 500.0), seen(4, 500.0)}, {}, {}};
  scene.geometry.base.gateway_visible = {{seen(5, 500.0)}, {seen(4, 900.0)}};
  scene.network.city_count = 3;
  scene.network.gateway_count = 2;
  scene.network.directions = {{.from = 0, .to = 1, .lower_bound_s = 0.001, .has_home_pop = true},
                              {.from = 0, .to = 2, .lower_bound_s = 0.001, .has_home_pop = true}};
  scene.network.end_delays_s = {0.001, 0.001, 0.001, 0.001};
}

// Satellite 0, the dish's serving satellite, fails at 5 s. In local repair the dish (no
// detector in the realistic model) picks again at 5.1 s among what its city sees, 0 left out:
// satellite 4; BOTH directions of city A continue from 4 (4 g1, 1400 km against 4 5 g0's
// 2000 km), one repair of the dish. The
// planner's dish waits for the slot start at 10 s, when the planner (who learnt 0-1 and 0-4
// down, so satellite 0 down) gives it 4.
TEST(LoopTest, RealisticDishRepairSharedByTwoDirections) {
  Scene scene(EndpointModel::kRealistic);
  make_realistic(scene);
  scene.run({failure_at(ElementKind::kSatellite, 0, 5.0)});
  const Route before_b = {32, 0, 1, 5, 28, 30};
  const Route before_c = {32, 0, 1, 5, 28, 31};
  const Route after_b = {32, 4, 29, 30};
  const Route after_c = {32, 4, 29, 31};
  for (std::size_t d = 0; d < 2; ++d) {
    SCOPED_TRACE(d);
    const Route& before = d == 0 ? before_b : before_c;
    const Route& after = d == 0 ? after_b : after_c;
    expect_intervals(scene.lane(LaneKind::kLocalRepair, d),
                     {{0.0, 5.0, DirectionState::kDelivered, before},
                      {5.0, 5.1, DirectionState::kBlackHole, before, satellite(0)},
                      {5.1, kEnd, DirectionState::kDelivered, after}});
    expect_intervals(scene.lane(LaneKind::kPlanner, d),
                     {{0.0, 5.0, DirectionState::kDelivered, before},
                      {5.0, 10.0, DirectionState::kBlackHole, before, satellite(0)},
                      {10.0, kEnd, DirectionState::kDelivered, after}});
  }
  EXPECT_EQ(scene.lane(LaneKind::kLocalRepair, 0)[2].serving, std::optional<std::size_t>(4));
  EXPECT_EQ(scene.loop->repairs(), 2U);
  EXPECT_EQ(scene.loop->checks().total(), 0U);
}

// Gateway g0 fails at 5 s. Satellite 5, which sees it, detects at 5.1 s and repairs onto its
// backup avoiding g0 (and the dish, 0, 1): 5 4 g1. The planner knows the gateway at 5.1 s and
// plans 0 4 g1 at 10 s.
TEST(LoopTest, GatewayFailureEveryInterval) {
  Scene scene(EndpointModel::kRealistic);
  make_realistic(scene);
  scene.network.directions.resize(1);
  scene.network.end_delays_s.resize(2);
  scene.run({failure_at(ElementKind::kGateway, 0, 5.0)});
  const Route before = {31, 0, 1, 5, 28, 30};
  expect_intervals(scene.lane(LaneKind::kPlanner),
                   {{0.0, 5.0, DirectionState::kDelivered, before},
                    {5.0, 10.0, DirectionState::kBlackHole, before, gateway(0)},
                    {10.0, kEnd, DirectionState::kDelivered, {31, 0, 4, 29, 30}}});
  expect_intervals(scene.lane(LaneKind::kLocalRepair),
                   {{0.0, 5.0, DirectionState::kDelivered, before},
                    {5.0, 5.1, DirectionState::kBlackHole, before, gateway(0)},
                    {5.1, 10.0, DirectionState::kDelivered, {31, 0, 1, 5, 4, 29, 30}},
                    {10.0, kEnd, DirectionState::kDelivered, {31, 0, 4, 29, 30}}});
  EXPECT_EQ(scene.loop->checks().total(), 0U);
}

// Each new check of spec 6.1, made to fire through a seam.
TEST(LoopTest, ChecksFire) {
  const auto planner = [](const Scene& s) { return s.engine_of(LaneKind::kPlanner); };
  {
    // A delivered route with a hop the truth has not (the dead satellite put in it).
    Scene scene;
    LoopHooks hooks;
    hooks.before_checks = [&](std::size_t e, std::size_t, std::size_t, Status& status) {
      if (e == planner(scene) && status.state == DirectionState::kDelivered &&
          status.route == kA045B) {
        status.route = kA015B;
      }
    };
    scene.run({failure_at(ElementKind::kSatellite, 1, 5.0)}, hooks);
    EXPECT_GT(scene.loop->checks().delivered_not_up, 0U);
  }
  {
    // A black hole on an element that works.
    Scene scene;
    LoopHooks hooks;
    hooks.before_checks = [&](std::size_t e, std::size_t, std::size_t, Status& status) {
      if (e == planner(scene) && status.state == DirectionState::kBlackHole) {
        status.element = satellite(0);
      }
    };
    scene.run({failure_at(ElementKind::kSatellite, 1, 5.0)}, hooks);
    EXPECT_GT(scene.loop->checks().black_hole_not_failed, 0U);
  }
  {
    // The planner knowing a fact before its report can arrive.
    Scene scene;
    LoopHooks hooks;
    hooks.planner_knowledge = [](satsim::failures_app::Knowledge& k) {
      for (double& t : k) {
        t = std::min(t, 5.0);
      }
    };
    scene.times.planner_processing_s = 10.0;
    scene.run({failure_at(ElementKind::kSatellite, 1, 5.0)}, hooks);
    EXPECT_GT(scene.loop->checks().fact_used_early, 0U);
  }
  {
    // A repair before its node can know.
    Scene scene;
    LoopHooks hooks;
    hooks.repair_instant = [](double t) { return t - 0.05; };
    scene.run({failure_at(ElementKind::kSatellite, 1, 5.0)}, hooks);
    EXPECT_GT(scene.loop->checks().repair_too_early, 0U);
  }
  {
    // Once settled (every fact of a link failure is learnt by 10 s), a route other than the
    // reference's (and a delay below the baseline's ideal).
    Scene scene;
    LoopHooks hooks;
    hooks.before_checks = [&](std::size_t e, std::size_t, std::size_t, Status& status) {
      if (e == planner(scene) && status.route == kA012B) {
        status.route = kA015B;
        status.delay_s = 0.0;
      }
    };
    scene.run({failure_at(ElementKind::kLaserLink, 4, 5.0)}, hooks);
    EXPECT_GT(scene.loop->checks().settled_differs, 0U);
    EXPECT_GT(scene.loop->checks().below_baseline_ideal, 0U);
  }
}

}  // namespace
