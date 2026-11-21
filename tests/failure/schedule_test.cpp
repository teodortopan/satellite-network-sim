#include "satsim/failure/schedule.hpp"

#include <gtest/gtest.h>

#include <cmath>
#include <cstddef>
#include <limits>
#include <optional>
#include <vector>

#include "fixture.hpp"
#include "satsim/topology/link_plan.hpp"

namespace {

using satsim::failure::Change;
using satsim::failure::DownAt;
using satsim::failure::ElementKind;
using satsim::failure::Failure;
using satsim::failure::FailureSchedule;
using satsim::failure::Transition;
using satsim::failure::testing::failure_at;
using satsim::failure::testing::small_plan;

constexpr double kInf = std::numeric_limits<double>::infinity();

double before(double t) { return std::nextafter(t, -kInf); }
double after(double t) { return std::nextafter(t, kInf); }

TEST(ScheduleTest, KindNames) {
  EXPECT_EQ(satsim::failure::name_of(ElementKind::kSatellite), "satellite");
  EXPECT_EQ(satsim::failure::name_of(ElementKind::kLaserLink), "laser_link");
  EXPECT_EQ(satsim::failure::name_of(ElementKind::kGateway), "gateway");
  for (const ElementKind kind : satsim::failure::kAllElementKinds) {
    EXPECT_EQ(satsim::failure::parse_element_kind(satsim::failure::name_of(kind)), kind);
  }
  EXPECT_EQ(satsim::failure::parse_element_kind("Satellite"), std::nullopt);
  EXPECT_EQ(satsim::failure::parse_element_kind("link"), std::nullopt);
  EXPECT_EQ(satsim::failure::parse_element_kind(""), std::nullopt);
}

// A transient failure from 10 s for 5 s: down from 10 (included) to 15 (excluded).
TEST(ScheduleTest, ATransientFailureIsInForceFromItsStartToJustBeforeItsEnd) {
  const Failure f = failure_at(ElementKind::kSatellite, 1, 10.0, 5.0);
  EXPECT_EQ(satsim::failure::end_s(f), std::optional<double>(15.0));
  EXPECT_FALSE(satsim::failure::in_force(f, 0.0));
  EXPECT_FALSE(satsim::failure::in_force(f, before(10.0)));
  EXPECT_TRUE(satsim::failure::in_force(f, 10.0));
  EXPECT_TRUE(satsim::failure::in_force(f, after(10.0)));
  EXPECT_TRUE(satsim::failure::in_force(f, 12.5));
  EXPECT_TRUE(satsim::failure::in_force(f, before(15.0)));
  EXPECT_FALSE(satsim::failure::in_force(f, 15.0));
  EXPECT_FALSE(satsim::failure::in_force(f, after(15.0)));
  EXPECT_FALSE(satsim::failure::in_force(f, 1e9));
}

TEST(ScheduleTest, APermanentFailureNeverEnds) {
  const Failure f = failure_at(ElementKind::kLaserLink, 2, 30.0);
  EXPECT_EQ(satsim::failure::end_s(f), std::nullopt);
  EXPECT_FALSE(satsim::failure::in_force(f, before(30.0)));
  EXPECT_TRUE(satsim::failure::in_force(f, 30.0));
  EXPECT_TRUE(satsim::failure::in_force(f, 1e9));
}

// Failures dated before the run's start (negative instants): in force at the start if they
// have not ended by then.
TEST(ScheduleTest, OldFailuresAreInForceAtTheStartUnlessTheyEnded) {
  const Failure permanent = failure_at(ElementKind::kSatellite, 0, -3600.0);
  EXPECT_TRUE(satsim::failure::in_force(permanent, 0.0));
  EXPECT_TRUE(satsim::failure::in_force(permanent, -3600.0));
  EXPECT_FALSE(satsim::failure::in_force(permanent, before(-3600.0)));

  const Failure ended = failure_at(ElementKind::kSatellite, 0, -3600.0, 3000.0);  // Ends at -600.
  EXPECT_TRUE(satsim::failure::in_force(ended, before(-600.0)));
  EXPECT_FALSE(satsim::failure::in_force(ended, -600.0));
  EXPECT_FALSE(satsim::failure::in_force(ended, 0.0));

  const Failure lasting = failure_at(ElementKind::kGateway, 0, -3600.0, 3700.0);  // Ends at 100.
  EXPECT_TRUE(satsim::failure::in_force(lasting, 0.0));
  EXPECT_TRUE(satsim::failure::in_force(lasting, before(100.0)));
  EXPECT_FALSE(satsim::failure::in_force(lasting, 100.0));
}

// small_plan()'s links by classification index: 0: 0-1, 1: 1-2, 2: 0-4, 3: 4-5, 4: 1-5,
// 5: 16-17. Satellite 1 is down over [10, 15), link 3 from 20 on, gateway 1 from the start
// (an hour old), gateway 2 over [5, 6).
FailureSchedule mixed() {
  return FailureSchedule{.failures = {failure_at(ElementKind::kSatellite, 1, 10.0, 5.0),
                                      failure_at(ElementKind::kLaserLink, 3, 20.0),
                                      failure_at(ElementKind::kGateway, 1, -3600.0),
                                      failure_at(ElementKind::kGateway, 2, 5.0, 1.0)}};
}

TEST(ScheduleTest, SatellitesAndGatewaysDown) {
  const FailureSchedule s = mixed();
  EXPECT_FALSE(satsim::failure::satellite_down(s, 1, before(10.0)));
  EXPECT_TRUE(satsim::failure::satellite_down(s, 1, 10.0));
  EXPECT_TRUE(satsim::failure::satellite_down(s, 1, before(15.0)));
  EXPECT_FALSE(satsim::failure::satellite_down(s, 1, 15.0));
  EXPECT_FALSE(satsim::failure::satellite_down(s, 0, 12.0));
  // Index 1 of a gateway is not satellite 1, and the other way round.
  EXPECT_FALSE(satsim::failure::satellite_down(s, 1, 0.0));
  EXPECT_TRUE(satsim::failure::gateway_down(s, 1, 0.0));
  EXPECT_FALSE(satsim::failure::gateway_down(s, 1, before(-3600.0)));
  EXPECT_FALSE(satsim::failure::gateway_down(s, 2, before(5.0)));
  EXPECT_TRUE(satsim::failure::gateway_down(s, 2, 5.0));
  EXPECT_FALSE(satsim::failure::gateway_down(s, 2, 6.0));
  EXPECT_FALSE(satsim::failure::gateway_down(s, 0, 5.5));
  EXPECT_FALSE(satsim::failure::satellite_down(FailureSchedule{}, 1, 10.0));
}

// A down satellite takes down every link it ends (0, 1 and 4 for satellite 1) and no other.
TEST(ScheduleTest, ADownSatelliteFailsItsLinks) {
  const FailureSchedule s = mixed();
  const satsim::topology::LinkPlan& plan = small_plan();
  const auto failed = [&](double t_s) {
    std::vector<std::size_t> links;
    for (std::size_t l = 0; l < plan.links.size(); ++l) {
      if (satsim::failure::link_failed(s, plan, l, t_s)) {
        links.push_back(l);
      }
    }
    return links;
  };
  using Links = std::vector<std::size_t>;
  EXPECT_EQ(failed(0.0), Links{});
  EXPECT_EQ(failed(before(10.0)), Links{});
  EXPECT_EQ(failed(10.0), (Links{0, 1, 4}));
  EXPECT_EQ(failed(before(15.0)), (Links{0, 1, 4}));
  EXPECT_EQ(failed(15.0), Links{});
  EXPECT_EQ(failed(before(20.0)), Links{});
  EXPECT_EQ(failed(20.0), Links{3});
  EXPECT_EQ(failed(1e6), Links{3});
  // Both at once: satellite 4 down over [20, 30) adds links 2 and 3 (3 already failed).
  FailureSchedule both = s;
  both.failures.push_back(failure_at(ElementKind::kSatellite, 4, 20.0, 10.0));
  EXPECT_TRUE(satsim::failure::link_failed(both, plan, 2, 25.0));
  EXPECT_FALSE(satsim::failure::link_failed(both, plan, 2, 30.0));
  EXPECT_TRUE(satsim::failure::link_failed(both, plan, 3, 30.0));
}

// down_at gives, in one pass, what the single queries give, at instants around every start
// and end.
TEST(ScheduleTest, DownAtAgreesWithTheSingleQueries) {
  FailureSchedule s = mixed();
  s.failures.push_back(failure_at(ElementKind::kSatellite, 16, 12.0, 0.25));
  const satsim::topology::LinkPlan& plan = small_plan();
  const std::size_t satellites = plan.lasers.size();
  const std::size_t gateways = 5;
  ASSERT_EQ(satellites, 22U);
  for (const double t :
       {-4000.0, -3600.0, 0.0, 5.0, 5.5, 6.0, 10.0, 12.0, 12.1, 12.25, 14.9, 15.0, 20.0, 100.0}) {
    for (const double instant : {before(t), t, after(t)}) {
      const DownAt down = satsim::failure::down_at(s, plan, satellites, gateways, instant);
      ASSERT_EQ(down.satellites.size(), satellites);
      ASSERT_EQ(down.links.size(), plan.links.size());
      ASSERT_EQ(down.gateways.size(), gateways);
      for (std::size_t i = 0; i < satellites; ++i) {
        EXPECT_EQ(down.satellites[i], satsim::failure::satellite_down(s, i, instant)) << instant;
      }
      for (std::size_t l = 0; l < plan.links.size(); ++l) {
        EXPECT_EQ(down.links[l], satsim::failure::link_failed(s, plan, l, instant)) << instant;
      }
      for (std::size_t g = 0; g < gateways; ++g) {
        EXPECT_EQ(down.gateways[g], satsim::failure::gateway_down(s, g, instant)) << instant;
      }
    }
  }
  // At 12.1 s, worked by hand: satellites 1 and 16; links 0, 1, 4 (satellite 1), 5 (satellite
  // 16), not 3 (from 20 s); gateway 1.
  const DownAt at = satsim::failure::down_at(s, plan, satellites, gateways, 12.1);
  std::vector<bool> sats(satellites, false);
  sats[1] = true;
  sats[16] = true;
  EXPECT_EQ(at, (DownAt{.satellites = sats,
                        .links = {true, true, false, false, true, true},
                        .gateways = {false, true, false, false, false}}));
}

// Failures: 0 satellite 1 over [10, 15); 1 link 3 from 15; 2 gateway 1 over [-3600, 0);
// 3 satellite 2 over [0, 10). Instants: -3600 (2 begins), 0 (2 ends, 3 begins), 10 (0 begins,
// 3 ends), 15 (0 ends, 1 begins).
TEST(ScheduleTest, TransitionsListEveryStartAndEndOnce) {
  const FailureSchedule s{.failures = {failure_at(ElementKind::kSatellite, 1, 10.0, 5.0),
                                       failure_at(ElementKind::kLaserLink, 3, 15.0),
                                       failure_at(ElementKind::kGateway, 1, -3600.0, 3600.0),
                                       failure_at(ElementKind::kSatellite, 2, 0.0, 10.0)}};
  const std::vector<Transition> expected = {
      {.t_s = -3600.0, .changes = {Change{.failure = 2, .begins = true}}},
      {.t_s = 0.0,
       .changes = {Change{.failure = 2, .begins = false}, Change{.failure = 3, .begins = true}}},
      {.t_s = 10.0,
       .changes = {Change{.failure = 0, .begins = true}, Change{.failure = 3, .begins = false}}},
      {.t_s = 15.0,
       .changes = {Change{.failure = 0, .begins = false}, Change{.failure = 1, .begins = true}}},
  };
  EXPECT_EQ(satsim::failure::transitions(s), expected);
  EXPECT_TRUE(satsim::failure::transitions(FailureSchedule{}).empty());
  // A transient end between two steps is its own instant.
  const FailureSchedule short_one{.failures = {failure_at(ElementKind::kLaserLink, 0, 3.0, 0.5)}};
  EXPECT_EQ(
      satsim::failure::transitions(short_one),
      (std::vector<Transition>{{.t_s = 3.0, .changes = {Change{.failure = 0, .begins = true}}},
                               {.t_s = 3.5, .changes = {Change{.failure = 0, .begins = false}}}}));
}

TEST(ScheduleTest, LinksOfASatelliteAndLinksTakenDown) {
  const satsim::topology::LinkPlan& plan = small_plan();
  using Links = std::vector<std::size_t>;
  EXPECT_EQ(satsim::failure::links_of(plan, 0), (Links{0, 2}));
  EXPECT_EQ(satsim::failure::links_of(plan, 1), (Links{0, 1, 4}));
  EXPECT_EQ(satsim::failure::links_of(plan, 5), (Links{3, 4}));
  EXPECT_EQ(satsim::failure::links_of(plan, 17), (Links{5}));
  EXPECT_EQ(satsim::failure::links_of(plan, 3), Links{});  // Excluded.
  EXPECT_EQ(satsim::failure::links_of(plan, 9), Links{});  // A member without links.
  EXPECT_EQ(satsim::failure::links_taken_down(failure_at(ElementKind::kSatellite, 4, 0.0), plan),
            (Links{2, 3}));
  EXPECT_EQ(satsim::failure::links_taken_down(failure_at(ElementKind::kLaserLink, 4, 0.0), plan),
            Links{4});
  EXPECT_EQ(satsim::failure::links_taken_down(failure_at(ElementKind::kGateway, 1, 0.0), plan),
            Links{});
}

}  // namespace
