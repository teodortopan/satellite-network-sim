#include "satsim/failure/detection.hpp"

#include <gtest/gtest.h>

#include <vector>

#include "fixture.hpp"
#include "satsim/failure/schedule.hpp"
#include "satsim/topology/link_plan.hpp"

namespace {

using satsim::failure::Detector;
using satsim::failure::ElementKind;
using satsim::failure::FailureSchedule;
using satsim::failure::testing::failure_at;
using satsim::failure::testing::small_plan;
using Detectors = std::vector<Detector>;

// small_plan()'s links by classification index: 0: 0-1, 1: 1-2, 2: 0-4, 3: 4-5, 4: 1-5,
// 5: 16-17.

TEST(DetectionTest, ALaserLinkIsDetectedByItsTwoEnds) {
  const satsim::topology::LinkPlan& plan = small_plan();
  EXPECT_EQ(satsim::failure::possible_detectors(failure_at(ElementKind::kLaserLink, 2, 0.0), plan),
            (Detectors{{.satellite = 0, .link = 2}, {.satellite = 4, .link = 2}}));
  EXPECT_EQ(satsim::failure::possible_detectors(failure_at(ElementKind::kLaserLink, 5, 0.0), plan),
            (Detectors{{.satellite = 16, .link = 5}, {.satellite = 17, .link = 5}}));
}

// Satellite 1's links are 0 (to 0), 1 (to 2) and 4 (to 5); satellite 5's are 3 (to 4) and 4
// (to 1), with 5 at the b end of both.
TEST(DetectionTest, ASatelliteIsDetectedByTheSatellitesAtTheOtherEndOfItsLinks) {
  const satsim::topology::LinkPlan& plan = small_plan();
  EXPECT_EQ(
      satsim::failure::possible_detectors(failure_at(ElementKind::kSatellite, 1, 0.0), plan),
      (Detectors{
          {.satellite = 0, .link = 0}, {.satellite = 2, .link = 1}, {.satellite = 5, .link = 4}}));
  EXPECT_EQ(satsim::failure::possible_detectors(failure_at(ElementKind::kSatellite, 5, 0.0), plan),
            (Detectors{{.satellite = 4, .link = 3}, {.satellite = 1, .link = 4}}));
  // A plane member without planned links has no laser neighbour to detect it.
  EXPECT_EQ(satsim::failure::possible_detectors(failure_at(ElementKind::kSatellite, 9, 0.0), plan),
            Detectors{});
}

TEST(DetectionTest, AGatewayHasNoDetectorInThePlan) {
  EXPECT_EQ(
      satsim::failure::possible_detectors(failure_at(ElementKind::kGateway, 0, 0.0), small_plan()),
      Detectors{});
}

// A neighbour that is itself down at the instant detects nothing; one whose failure has ended,
// or not begun, does.
TEST(DetectionTest, OnlyWorkingNeighboursDetect) {
  const satsim::topology::LinkPlan& plan = small_plan();
  const FailureSchedule schedule{
      .failures = {failure_at(ElementKind::kSatellite, 1, 10.0),
                   failure_at(ElementKind::kSatellite, 2, 5.0, 10.0),  // Down over [5, 15).
                   failure_at(ElementKind::kSatellite, 0, 12.0),
                   failure_at(ElementKind::kLaserLink, 0, 10.0)}};
  const auto& satellite_1 = schedule.failures[0];
  EXPECT_EQ(satsim::failure::working_detectors(satellite_1, schedule, plan, 10.0),
            (Detectors{{.satellite = 0, .link = 0}, {.satellite = 5, .link = 4}}));
  EXPECT_EQ(satsim::failure::working_detectors(satellite_1, schedule, plan, 12.0),
            (Detectors{{.satellite = 5, .link = 4}}));
  EXPECT_EQ(satsim::failure::working_detectors(satellite_1, schedule, plan, 15.0),
            (Detectors{{.satellite = 2, .link = 1}, {.satellite = 5, .link = 4}}));
  EXPECT_EQ(
      satsim::failure::working_detectors(satellite_1, schedule, plan, 4.0),
      (Detectors{
          {.satellite = 0, .link = 0}, {.satellite = 2, .link = 1}, {.satellite = 5, .link = 4}}));
  // Link 0 (0-1) at 10 s: satellite 1 is down, so only 0 detects; from 12 s nobody does.
  const auto& link_0 = schedule.failures[3];
  EXPECT_EQ(satsim::failure::working_detectors(link_0, schedule, plan, 10.0),
            (Detectors{{.satellite = 0, .link = 0}}));
  EXPECT_EQ(satsim::failure::working_detectors(link_0, schedule, plan, 12.0), Detectors{});
}

}  // namespace
