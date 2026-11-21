#include "failures/timeline.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <limits>
#include <optional>
#include <ostream>
#include <vector>

#include "failure/fixture.hpp"
#include "inputs/inputs.hpp"
#include "satsim/failure/schedule.hpp"
#include "satsim/topology/link_state.hpp"
#include "world.hpp"

namespace satsim::failures_app {

// Readable failure messages.
void PrintTo(const Detection& d, std::ostream* os) {
  *os << "{node " << d.node << " at " << d.t_s << "}";
}

}  // namespace satsim::failures_app

namespace {

using satsim::app::EndpointModel;
using satsim::failure::ElementKind;
using satsim::failure::testing::failure_at;
using satsim::failures_app::Detection;
using satsim::failures_app::Fact;
using satsim::failures_app::ForceChange;
using satsim::failures_app::Subject;
using satsim::failures_app::SubjectKind;
using satsim::failures_app::Timeline;
using satsim::failures_app::testing::hop_s;
using satsim::failures_app::testing::kDish0;
using satsim::failures_app::testing::kGateway0;
using satsim::failures_app::testing::kGateway1;
using satsim::failures_app::testing::World;
using satsim::topology::LinkForce;

constexpr double kInf = std::numeric_limits<double>::infinity();

Subject link(std::size_t i) { return {.kind = SubjectKind::kLink, .index = i}; }
Subject satellite(std::size_t i) { return {.kind = SubjectKind::kSatellite, .index = i}; }
Subject gateway(std::size_t i) { return {.kind = SubjectKind::kGateway, .index = i}; }

void expect_fact(const Fact& fact, Subject subject, bool down, double t_s,
                 const std::vector<Detection>& detectors, double planner_known_s) {
  EXPECT_EQ(fact.subject, subject);
  EXPECT_EQ(fact.down, down);
  EXPECT_EQ(fact.t_s, t_s);
  EXPECT_FALSE(fact.old);
  EXPECT_EQ(fact.detectors, detectors);
  EXPECT_EQ(fact.planner_known_s, planner_known_s);
}

void expect_old(const Fact& fact, Subject subject, bool down, double t_s) {
  EXPECT_EQ(fact.subject, subject);
  EXPECT_EQ(fact.down, down);
  EXPECT_EQ(fact.t_s, t_s);
  EXPECT_TRUE(fact.old);
  EXPECT_TRUE(fact.detectors.empty());
  EXPECT_EQ(fact.planner_known_s, 0.0);
}

TEST(TimelineTest, StepOfAnInstant) {
  using satsim::failures_app::first_step_at_or_after;
  using satsim::failures_app::step_of;
  EXPECT_EQ(step_of(-3.0, 1.0), 0);
  EXPECT_EQ(step_of(0.0, 1.0), 0);
  EXPECT_EQ(step_of(10.1, 1.0), 10);
  EXPECT_EQ(step_of(12.0, 1.0), 12);
  EXPECT_EQ(step_of(29.0, 15.0), 1);
  EXPECT_EQ(first_step_at_or_after(-3.0, 1.0), 0);
  EXPECT_EQ(first_step_at_or_after(0.0, 1.0), 0);
  EXPECT_EQ(first_step_at_or_after(10.1, 1.0), 11);
  EXPECT_EQ(first_step_at_or_after(12.0, 1.0), 12);
  EXPECT_EQ(first_step_at_or_after(15.0, 15.0), 1);
  EXPECT_EQ(first_step_at_or_after(15.5, 15.0), 2);
}

// Satellite 1 fails for good at 10 s, best case. Its links 0 (0-1), 1 (1-2) and 4 (1-5) are
// failed from 10 s. Facts, each detected at 10 + 0.1 s:
//  - satellite 1 down: city 0's dish (sees 1) and gateway 0 (sees 1); the planner at once from
//    gateway 0's own detection: (10.1 + 0) + 0;
//  - link 0 down: satellite 0. Its report goes 0 -> 4 -> 5 -> gateway 1 (links 2 and 3, then
//    800 km down): 10.1 + ((hop 800 + hop 1000) + hop 2000);
//  - link 1 down: satellite 2, straight to gateway 0 at 700 km: 10.1 + hop 700;
//  - link 4 down: satellite 5, straight to gateway 1 at 800 km: 10.1 + hop 800.
TEST(TimelineTest, PermanentSatelliteFailureBestCase) {
  World w;
  const Timeline t = w.timeline({failure_at(ElementKind::kSatellite, 1, 10.0)});
  ASSERT_EQ(t.links.size(), 6U);
  EXPECT_EQ(t.planned_count, 6U);
  const double d = 10.0 + 0.1;
  for (const std::size_t i : {0U, 1U, 4U}) {
    EXPECT_EQ(t.force[i], (std::vector<ForceChange>{{.from_s = -kInf, .force = LinkForce::kInForce},
                                                    {.from_s = 10.0, .force = LinkForce::kFailed}}))
        << i;
  }
  for (const std::size_t i : {2U, 3U, 5U}) {
    EXPECT_EQ(t.force[i].size(), 1U) << i;
  }
  EXPECT_EQ(t.force_at(9.999)[0], LinkForce::kInForce);
  EXPECT_EQ(t.force_at(10.0)[0], LinkForce::kFailed);
  EXPECT_EQ(t.satellite_down_at(10.0)[1], 1);
  EXPECT_EQ(t.satellite_down_at(9.0)[1], 0);
  ASSERT_EQ(t.facts.size(), 4U);
  expect_fact(t.facts[0], satellite(1), true, 10.0, {{kDish0, d}, {kGateway0, d}}, d);
  expect_fact(t.facts[1], link(0), true, 10.0, {{0, d}},
              d + ((hop_s(800.0) + hop_s(1000.0)) + hop_s(2000.0)));
  expect_fact(t.facts[2], link(1), true, 10.0, {{2, d}}, d + hop_s(700.0));
  expect_fact(t.facts[3], link(4), true, 10.0, {{5, d}}, d + hop_s(800.0));
  EXPECT_EQ(t.facts[0].cause, std::optional<std::size_t>(0));
  EXPECT_EQ(t.changes, (std::vector<double>{10.0}));
}

// Realistic model: no dish detects; the rest is the same.
TEST(TimelineTest, PermanentSatelliteFailureRealistic) {
  World w(EndpointModel::kRealistic);
  const Timeline t = w.timeline({failure_at(ElementKind::kSatellite, 1, 10.0)});
  ASSERT_EQ(t.facts.size(), 4U);
  const double d = 10.0 + 0.1;
  expect_fact(t.facts[0], satellite(1), true, 10.0, {{kGateway0, d}}, d);
  expect_fact(t.facts[3], link(4), true, 10.0, {{5, d}}, d + hop_s(800.0));
}

// Satellites 1 and 0 fail at the same instant. Link 0 joins them: neither end detects it. Then
// satellite 0's failure takes down only link 2 (0-4; link 0 is already failed), detected by 4,
// whose report goes 4 -> 5 -> gateway 1. Nobody on the ground sees satellite 0.
TEST(TimelineTest, ADownNeighbourDetectsNothing) {
  World w;
  const Timeline t = w.timeline(
      {failure_at(ElementKind::kSatellite, 1, 10.0), failure_at(ElementKind::kSatellite, 0, 10.0)});
  ASSERT_EQ(t.facts.size(), 6U);
  const double d = 10.0 + 0.1;
  expect_fact(t.facts[1], link(0), true, 10.0, {}, kInf);
  expect_fact(t.facts[2], link(1), true, 10.0, {{2, d}}, d + hop_s(700.0));
  expect_fact(t.facts[3], link(4), true, 10.0, {{5, d}}, d + hop_s(800.0));
  expect_fact(t.facts[4], satellite(0), true, 10.0, {}, kInf);
  expect_fact(t.facts[5], link(2), true, 10.0, {{4, d}}, d + (hop_s(800.0) + hop_s(1000.0)));
}

// Link 3 (4-5) fails for good at 10 s while out of sight at steps 10 to 12; it would have come
// back at step 13, so both ends detect at 13 + 0.1 s. Satellite 5 reports straight down to
// gateway 1 (800 km), sooner than satellite 4 (4 -> 0 -> 1 -> gateway 0).
TEST(TimelineTest, ALinkOutOfSightIsDetectedWhenItWouldComeBack) {
  World w;
  w.geometry.down_between(4, 5, 10, 13);
  const Timeline t = w.timeline({failure_at(ElementKind::kLaserLink, 3, 10.0)});
  ASSERT_EQ(t.facts.size(), 1U);
  const double d = 13.0 + 0.1;
  expect_fact(t.facts[0], link(3), true, 10.0, {{4, d}, {5, d}}, d + hop_s(800.0));
  EXPECT_EQ(t.force_at(10.0)[3], LinkForce::kFailed);
}

// The same link never comes back in the run: nobody detects it, the planner never knows.
TEST(TimelineTest, ALinkThatNeverComesBackIsNeverDetected) {
  World w;
  w.geometry.down_between(4, 5, 10, 100);
  const Timeline t = w.timeline({failure_at(ElementKind::kLaserLink, 3, 10.0)});
  ASSERT_EQ(t.facts.size(), 1U);
  expect_fact(t.facts[0], link(3), true, 10.0, {}, kInf);
}

// The failure lasts 2 s (ends at 12) and the link is out of sight until step 13: the failure
// ends first, so nobody detects it. At 12 s the link acquires again: in force at 12 + 30 s.
TEST(TimelineTest, ALinkFailureThatEndsBeforeTheLinkComesBackIsNotDetected) {
  World w;
  w.geometry.down_between(4, 5, 10, 13);
  const Timeline t = w.timeline({failure_at(ElementKind::kLaserLink, 3, 10.0, 2.0)});
  ASSERT_EQ(t.facts.size(), 2U);
  expect_fact(t.facts[0], link(3), true, 10.0, {}, kInf);
  const double up = 12.0 + 30.0;
  expect_fact(t.facts[1], link(3), false, up, {{4, up}, {5, up}}, up + hop_s(800.0));
  EXPECT_EQ(t.force[3], (std::vector<ForceChange>{{.from_s = -kInf, .force = LinkForce::kInForce},
                                                  {.from_s = 10.0, .force = LinkForce::kFailed},
                                                  {.from_s = 12.0, .force = LinkForce::kAcquiring},
                                                  {.from_s = up, .force = LinkForce::kInForce}}));
}

// Gateway 1 fails at 20 s, satellite 5 at the same instant. The satellites gateway 1 sees and
// that work (16; not 5) detect at 20 + 0.1 s; the planner knows by itself at
// (20 + 0.1) + 0.25 (planner processing 0.25 s). No laser link changes for the gateway.
TEST(TimelineTest, GatewayFailure) {
  World w;
  w.settings.times.planner_processing_s = 0.25;
  const Timeline t = w.timeline(
      {failure_at(ElementKind::kGateway, 1, 20.0), failure_at(ElementKind::kSatellite, 5, 20.0)});
  ASSERT_GE(t.facts.size(), 1U);
  const double d = 20.0 + 0.1;
  expect_fact(t.facts[0], gateway(1), true, 20.0, {{16, d}}, d + 0.25);
  EXPECT_EQ(t.gateway_down_at(20.0), (std::vector<char>{0, 1}));
  EXPECT_EQ(t.gateway_down_at(19.0), (std::vector<char>{0, 0}));
  // Satellite 5's fact: city 0's dish detects (gateway 1, which also sees it, is down).
  expect_fact(t.facts[1], satellite(5), true, 20.0, {{kDish0, d}},
              (d + (hop_s(600.0) + hop_s(500.0))) + 0.25);
}

// Satellite 5 is down from 10 s for 2.5 s. At 12.5 s (not a step) it is up again, known at
// once by city 0's dish and gateway 1 (they see it at step 12). Its links 3 and 4 acquire from
// 12.5 s and carry traffic from 12.5 + 30 s, when both ends know.
TEST(TimelineTest, TransientSatelliteFailure) {
  World w;
  const Timeline t = w.timeline({failure_at(ElementKind::kSatellite, 5, 10.0, 2.5)});
  ASSERT_EQ(t.facts.size(), 6U);
  const double d = 10.0 + 0.1;
  const double r = 10.0 + 2.5;
  const double up = r + 30.0;
  expect_fact(t.facts[0], satellite(5), true, 10.0, {{kDish0, d}, {kGateway1, d}}, d);
  expect_fact(t.facts[1], link(3), true, 10.0, {{4, d}},
              d + ((hop_s(600.0) + hop_s(1000.0)) + hop_s(2000.0)));
  expect_fact(t.facts[2], link(4), true, 10.0, {{1, d}}, d + hop_s(600.0));
  expect_fact(t.facts[3], satellite(5), false, r, {{kDish0, r}, {kGateway1, r}}, r);
  expect_fact(t.facts[4], link(3), false, up, {{4, up}, {5, up}}, up + hop_s(800.0));
  expect_fact(t.facts[5], link(4), false, up, {{1, up}, {5, up}}, up + hop_s(600.0));
  for (const std::size_t i : {3U, 4U}) {
    EXPECT_EQ(t.force[i], (std::vector<ForceChange>{{.from_s = -kInf, .force = LinkForce::kInForce},
                                                    {.from_s = 10.0, .force = LinkForce::kFailed},
                                                    {.from_s = r, .force = LinkForce::kAcquiring},
                                                    {.from_s = up, .force = LinkForce::kInForce}}))
        << i;
  }
  EXPECT_EQ(t.force_at(12.4)[3], LinkForce::kFailed);
  EXPECT_EQ(t.force_at(12.5)[3], LinkForce::kAcquiring);
  EXPECT_EQ(t.force_at(42.49)[3], LinkForce::kAcquiring);
  EXPECT_EQ(t.force_at(42.5)[3], LinkForce::kInForce);
  EXPECT_EQ(t.satellite_down_at(12.49)[5], 1);
  EXPECT_EQ(t.satellite_down_at(12.5)[5], 0);
  EXPECT_EQ(t.changes, (std::vector<double>{10.0, r, up}));
}

// A link failing again while it acquires: the acquisition never ends and no "up" fact is made.
TEST(TimelineTest, AFailureDuringAcquisitionCancelsIt) {
  World w;
  const Timeline t = w.timeline({failure_at(ElementKind::kLaserLink, 3, 10.0, 5.0),
                                 failure_at(ElementKind::kLaserLink, 3, 20.0)});
  ASSERT_EQ(t.facts.size(), 2U);
  EXPECT_TRUE(t.facts[0].down);
  EXPECT_TRUE(t.facts[1].down);
  EXPECT_EQ(t.facts[1].t_s, 20.0);
  EXPECT_EQ(t.force[3], (std::vector<ForceChange>{{.from_s = -kInf, .force = LinkForce::kInForce},
                                                  {.from_s = 10.0, .force = LinkForce::kFailed},
                                                  {.from_s = 15.0, .force = LinkForce::kAcquiring},
                                                  {.from_s = 20.0, .force = LinkForce::kFailed}}));
}

// Reports on the control graph. Link 5 (16-17) fails at 10 s: satellite 16 reports straight to
// gateway 1 (900 km); satellite 17's only link is the failed one, so it has no path and does
// not report.
TEST(TimelineTest, PlannerKnowsFromTheEarliestReport) {
  World w;
  const Timeline t = w.timeline({failure_at(ElementKind::kLaserLink, 5, 10.0)});
  ASSERT_EQ(t.facts.size(), 1U);
  const double d = 10.0 + 0.1;
  expect_fact(t.facts[0], link(5), true, 10.0, {{16, d}, {17, d}}, d + hop_s(900.0));
}

// With gateway 1 down since 5 s, neither end of link 5 has a path to a working gateway: the
// planner never learns of it. (Gateway 1's own fact is known by the planner by itself.)
TEST(TimelineTest, PlannerNeverKnowsWithoutAPath) {
  World w;
  const Timeline t = w.timeline(
      {failure_at(ElementKind::kGateway, 1, 5.0), failure_at(ElementKind::kLaserLink, 5, 10.0)});
  ASSERT_EQ(t.facts.size(), 2U);
  expect_fact(t.facts[0], gateway(1), true, 5.0, {{5, 5.0 + 0.1}, {16, 5.0 + 0.1}}, 5.0 + 0.1);
  const double d = 10.0 + 0.1;
  expect_fact(t.facts[1], link(5), true, 10.0, {{16, d}, {17, d}}, kInf);
}

// A dish's report: satellite 5 fails at 10 s with gateway 1 down. Only city 0's dish sees it
// fail; its report goes dish -> satellite 1 (500 km) -> gateway 0 (600 km). In the realistic
// model no dish detects and no dish edge exists: nobody on the ground knows.
TEST(TimelineTest, ADishReportsThroughAnotherSatellite) {
  const auto failures = {failure_at(ElementKind::kGateway, 1, 5.0),
                         failure_at(ElementKind::kSatellite, 5, 10.0)};
  const double d = 10.0 + 0.1;
  {
    World w;
    const Timeline t = w.timeline(failures);
    ASSERT_GE(t.facts.size(), 2U);
    expect_fact(t.facts[1], satellite(5), true, 10.0, {{kDish0, d}},
                d + (hop_s(600.0) + hop_s(500.0)));
  }
  {
    World w(EndpointModel::kRealistic);
    const Timeline t = w.timeline(failures);
    ASSERT_GE(t.facts.size(), 2U);
    expect_fact(t.facts[1], satellite(5), true, 10.0, {}, kInf);
  }
}

// The per-hop time and the planner processing time enter every report.
TEST(TimelineTest, ReportTimesUseHopProcessingAndPlannerProcessing) {
  World w;
  w.settings.times.hop_processing_s = 0.01;
  w.settings.times.planner_processing_s = 0.5;
  w.settings.times.detection_s = 1.0;
  const Timeline t = w.timeline({failure_at(ElementKind::kLaserLink, 5, 10.0)});
  ASSERT_EQ(t.facts.size(), 1U);
  const double d = 10.0 + 1.0;
  expect_fact(t.facts[0], link(5), true, 10.0, {{16, d}, {17, d}}, (d + hop_s(900.0, 0.01)) + 0.5);
}

// A failure dated before the run that is still in force: old news, known by everyone from the
// start (no detectors, planner at 0), with its links failed.
TEST(TimelineTest, OldFailureIsKnownFromTheStart) {
  World w;
  const Timeline t = w.timeline({failure_at(ElementKind::kSatellite, 1, -100.0)});
  ASSERT_EQ(t.facts.size(), 4U);
  expect_old(t.facts[0], satellite(1), true, -100.0);
  expect_old(t.facts[1], link(0), true, -100.0);
  expect_old(t.facts[2], link(1), true, -100.0);
  expect_old(t.facts[3], link(4), true, -100.0);
  EXPECT_EQ(t.force_at(0.0)[0], LinkForce::kFailed);
  EXPECT_EQ(t.force_at(0.0)[2], LinkForce::kInForce);
  EXPECT_EQ(t.satellite_down_at(0.0)[1], 1);
  EXPECT_TRUE(t.changes.empty());
}

// One that ended before the run (or exactly at its start) is ignored.
TEST(TimelineTest, OldFailureEndedBeforeTheStartIsIgnored) {
  for (const double length : {50.0, 100.0}) {
    World w;
    const Timeline t = w.timeline({failure_at(ElementKind::kSatellite, 1, -100.0, length)});
    EXPECT_TRUE(t.facts.empty()) << length;
    EXPECT_EQ(t.force_at(0.0)[0], LinkForce::kInForce) << length;
    EXPECT_EQ(t.satellite_down_at(0.0)[1], 0) << length;
    EXPECT_TRUE(t.changes.empty()) << length;
  }
}

// One that ends during the run returns as any other: at -100 + 150 = 50 s.
TEST(TimelineTest, OldFailureEndingDuringTheRunReturns) {
  World w;
  const Timeline t = w.timeline({failure_at(ElementKind::kSatellite, 1, -100.0, 150.0)});
  ASSERT_EQ(t.facts.size(), 8U);
  expect_old(t.facts[0], satellite(1), true, -100.0);
  expect_fact(t.facts[4], satellite(1), false, 50.0, {{kDish0, 50.0}, {kGateway0, 50.0}}, 50.0);
  expect_fact(t.facts[5], link(0), false, 80.0, {{0, 80.0}, {1, 80.0}}, 80.0 + hop_s(600.0));
  EXPECT_EQ(t.force_at(49.0)[0], LinkForce::kFailed);
  EXPECT_EQ(t.force_at(50.0)[0], LinkForce::kAcquiring);
  EXPECT_EQ(t.force_at(80.0)[0], LinkForce::kInForce);
  EXPECT_EQ(t.changes, (std::vector<double>{50.0, 80.0}));
}

// Without re-pointing nothing is ever added to the link list.
TEST(TimelineTest, NoRepairLinksWithoutRepointing) {
  World w;
  const Timeline t = w.timeline({failure_at(ElementKind::kSatellite, 1, 10.0),
                                 failure_at(ElementKind::kSatellite, 16, -50.0)});
  EXPECT_EQ(t.links.size(), 6U);
}

}  // namespace
