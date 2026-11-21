#include "failures/view.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <limits>
#include <optional>
#include <vector>

#include "failure/fixture.hpp"
#include "failures/timeline.hpp"
#include "satsim/failure/schedule.hpp"
#include "satsim/topology/ground_links.hpp"
#include "satsim/topology/link_plan.hpp"
#include "satsim/topology/link_state.hpp"
#include "world.hpp"

namespace {

using satsim::failure::ElementKind;
using satsim::failure::testing::failure_at;
using satsim::failures_app::Belief;
using satsim::failures_app::ControlNodes;
using satsim::failures_app::Fact;
using satsim::failures_app::ForceChange;
using satsim::failures_app::OverlaidStep;
using satsim::failures_app::StepGeometry;
using satsim::failures_app::Subject;
using satsim::failures_app::SubjectKind;
using satsim::failures_app::Timeline;
using satsim::failures_app::ViewState;
using satsim::failures_app::testing::seen;
using satsim::failures_app::testing::World;
using satsim::topology::LinkDownReason;
using satsim::topology::LinkForce;
using satsim::topology::LinkState;
using satsim::topology::PlannedLink;
using satsim::topology::StepLinks;
using satsim::topology::VisibleSatellite;

constexpr double kInf = std::numeric_limits<double>::infinity();

PlannedLink link_of(std::size_t a, std::size_t b) {
  return PlannedLink{.satellite_a = a, .satellite_b = b};
}

Fact fact(SubjectKind kind, std::size_t index, bool down, double t_s, double planner_s = 0.0) {
  return Fact{.subject = Subject{.kind = kind, .index = index},
              .down = down,
              .t_s = t_s,
              .old = false,
              .detectors = {},
              .planner_known_s = planner_s,
              .cause = std::nullopt};
}

// A hand-made timeline: 4 satellites, 1 city, 2 gateways; planned links 0: 0-1, 1: 1-2,
// 2: 0-2, then repair link 3: 2-3. Satellite 3 has no planned link.
Timeline small_timeline(std::vector<Fact> facts) {
  Timeline t;
  t.links = {link_of(0, 1), link_of(1, 2), link_of(0, 2), link_of(2, 3)};
  t.planned_count = 3;
  t.force = {{{.from_s = -kInf, .force = LinkForce::kInForce}},
             {{.from_s = -kInf, .force = LinkForce::kInForce}},
             {{.from_s = -kInf, .force = LinkForce::kInForce}},
             {{.from_s = -kInf, .force = LinkForce::kNotInForce}}};
  t.nodes = ControlNodes{.satellites = 4, .cities = 1, .gateways = 2};
  t.facts = std::move(facts);
  return t;
}

std::vector<double> known_at(const Timeline& t) {
  std::vector<double> k;
  k.reserve(t.facts.size());
  for (const Fact& f : t.facts) {
    k.push_back(f.planner_known_s);
  }
  return k;
}

TEST(ViewTest, NothingKnownBeforeThePlannerKnows) {
  const Timeline t = small_timeline({fact(SubjectKind::kSatellite, 1, true, 10.0, 10.5),
                                     fact(SubjectKind::kLink, 0, true, 10.0, 11.0),
                                     fact(SubjectKind::kGateway, 1, true, 10.0, 12.0)});
  const std::vector<double> k = satsim::failures_app::planner_knowledge(t);
  EXPECT_EQ(k, (std::vector<double>{10.5, 11.0, 12.0}));
  const ViewState before = satsim::failures_app::view_at(t, k, 10.4);
  EXPECT_EQ(before.links, std::vector<Belief>(4, Belief::kNothing));
  EXPECT_EQ(before.satellite_down, std::vector<char>(4, 0));
  EXPECT_EQ(before.gateway_down, std::vector<char>(2, 0));
  const ViewState mid = satsim::failures_app::view_at(t, k, 10.5);
  EXPECT_EQ(mid.satellites[1], Belief::kDown);
  EXPECT_EQ(mid.satellite_down, (std::vector<char>{0, 1, 0, 0}));
  EXPECT_EQ(mid.links[0], Belief::kNothing);
  const ViewState after = satsim::failures_app::view_at(t, k, 12.0);
  EXPECT_EQ(after.links[0], Belief::kDown);
  EXPECT_EQ(after.gateway_down, (std::vector<char>{0, 1}));
  EXPECT_EQ(after.gateways[1], Belief::kDown);
}

// A later fact replaces an earlier one, whatever order the actor learnt them in; on equal
// instants the later in the list wins.
TEST(ViewTest, LaterFactReplacesEarlier) {
  const Timeline t = small_timeline({fact(SubjectKind::kLink, 1, false, 40.0, 41.0),
                                     fact(SubjectKind::kLink, 1, true, 10.0, 45.0),
                                     fact(SubjectKind::kLink, 2, true, 20.0, 21.0),
                                     fact(SubjectKind::kLink, 2, false, 20.0, 21.0)});
  const std::vector<double> k = known_at(t);
  EXPECT_EQ(satsim::failures_app::view_at(t, k, 42.0).links[1], Belief::kUp);
  // Learning the older "down" later does not undo the newer "up".
  EXPECT_EQ(satsim::failures_app::view_at(t, k, 50.0).links[1], Belief::kUp);
  EXPECT_EQ(satsim::failures_app::view_at(t, k, 30.0).links[2], Belief::kUp);
}

// Satellite 0's planned links are 0 (0-1) and 2 (0-2). Both held down: taken as down. Only
// one: not. Satellite 3 has no planned link and is never inferred; repair link 3 does not
// count.
TEST(ViewTest, InferenceNeedsEveryPlannedLinkDown) {
  const Timeline both = small_timeline({fact(SubjectKind::kLink, 0, true, 10.0, 11.0),
                                        fact(SubjectKind::kLink, 2, true, 10.0, 11.0),
                                        fact(SubjectKind::kLink, 3, true, 10.0, 11.0)});
  const ViewState v = satsim::failures_app::view_at(both, known_at(both), 11.0);
  EXPECT_EQ(v.satellite_down, (std::vector<char>{1, 0, 0, 0}));
  EXPECT_EQ(v.satellites[0], Belief::kNothing);
  const Timeline one = small_timeline({fact(SubjectKind::kLink, 0, true, 10.0, 11.0)});
  EXPECT_EQ(satsim::failures_app::view_at(one, known_at(one), 11.0).satellite_down,
            (std::vector<char>{0, 0, 0, 0}));
  // An "up" replacing one of the downs lifts the inference.
  const Timeline back = small_timeline({fact(SubjectKind::kLink, 0, true, 10.0, 11.0),
                                        fact(SubjectKind::kLink, 2, true, 10.0, 11.0),
                                        fact(SubjectKind::kLink, 2, false, 40.0, 41.0)});
  EXPECT_EQ(satsim::failures_app::view_at(back, known_at(back), 41.0).satellite_down,
            (std::vector<char>{0, 0, 0, 0}));
}

// Satellite 2's planned links are 1 (1-2) and 2 (0-2); its repair link 3 (2-3). Both planned
// links held down take it as down, unless the repair link is known up (spec 4.4): a laser of
// it still works.
TEST(ViewTest, RepairLinkKnownUpLiftsTheInference) {
  const Timeline down = small_timeline({fact(SubjectKind::kLink, 1, true, 10.0, 11.0),
                                        fact(SubjectKind::kLink, 2, true, 10.0, 11.0)});
  EXPECT_EQ(satsim::failures_app::view_at(down, known_at(down), 11.0).satellite_down,
            (std::vector<char>{0, 0, 1, 0}));
  const Timeline repaired = small_timeline({fact(SubjectKind::kLink, 1, true, 10.0, 11.0),
                                            fact(SubjectKind::kLink, 2, true, 10.0, 11.0),
                                            fact(SubjectKind::kLink, 3, false, 40.0, 41.0)});
  EXPECT_EQ(satsim::failures_app::view_at(repaired, known_at(repaired), 40.5).satellite_down,
            (std::vector<char>{0, 0, 1, 0}));
  EXPECT_EQ(satsim::failures_app::view_at(repaired, known_at(repaired), 41.0).satellite_down,
            (std::vector<char>{0, 0, 0, 0}));
  // The repair link known down again: down again.
  const Timeline dropped = small_timeline({fact(SubjectKind::kLink, 1, true, 10.0, 11.0),
                                           fact(SubjectKind::kLink, 2, true, 10.0, 11.0),
                                           fact(SubjectKind::kLink, 3, false, 40.0, 41.0),
                                           fact(SubjectKind::kLink, 3, true, 50.0, 51.0)});
  EXPECT_EQ(satsim::failures_app::view_at(dropped, known_at(dropped), 51.0).satellite_down,
            (std::vector<char>{0, 0, 1, 0}));
}

StepGeometry step_geometry() {
  StepGeometry g{.utc = satsim::failures_app::testing::some_time(),
                 .position_ecef_km = {},
                 .city_visible = {},
                 .gateway_visible = {}};
  g.position_ecef_km.assign(4, std::nullopt);
  g.city_visible = {{seen(0, 500.0), seen(1, 510.0), seen(3, 520.0)}};
  g.gateway_visible = {{seen(1, 600.0), seen(2, 610.0)}, {seen(0, 700.0), seen(1, 710.0)}};
  return g;
}

StepLinks geometric() {
  StepLinks links{.utc = satsim::failures_app::testing::some_time(), .states = {}};
  links.states = {LinkState{.length_km = 1000.0, .down_reason = std::nullopt},
                  LinkState{.length_km = 1100.0, .down_reason = std::nullopt},
                  LinkState{.length_km = 9000.0, .down_reason = LinkDownReason::kNoLineOfSight},
                  LinkState{.length_km = 1200.0, .down_reason = std::nullopt}};
  return links;
}

std::vector<std::size_t> satellites_of(const std::vector<VisibleSatellite>& visible) {
  std::vector<std::size_t> s;
  s.reserve(visible.size());
  for (const VisibleSatellite& v : visible) {
    s.push_back(v.satellite);
  }
  return s;
}

// The view holds satellite 1 down and gateway 0 down, and repair link 3 as known up.
// Planned links 0 and 1 touch satellite 1: failed. Link 2 is untouched (in force, its geometric
// reason kept). Repair link 3 known up: in force. Satellite 1 leaves every list; gateway 0's
// list is empty.
TEST(ViewTest, OverlayOfAView) {
  const Timeline t = small_timeline({fact(SubjectKind::kSatellite, 1, true, 10.0, 11.0),
                                     fact(SubjectKind::kGateway, 0, true, 10.0, 11.0),
                                     fact(SubjectKind::kLink, 3, false, 10.0, 11.0)});
  const ViewState v = satsim::failures_app::view_at(t, known_at(t), 11.0);
  const OverlaidStep o = satsim::failures_app::overlay_view(step_geometry(), geometric(), t, v);
  EXPECT_EQ(o.force, (std::vector<LinkForce>{LinkForce::kFailed, LinkForce::kFailed,
                                             LinkForce::kInForce, LinkForce::kInForce}));
  EXPECT_EQ(o.links.states[0].down_reason, LinkDownReason::kFailed);
  EXPECT_EQ(o.links.states[1].down_reason, LinkDownReason::kFailed);
  EXPECT_EQ(o.links.states[2].down_reason, LinkDownReason::kNoLineOfSight);
  EXPECT_TRUE(o.links.states[3].up());
  EXPECT_EQ(o.links.states[0].length_km, 1000.0);
  EXPECT_EQ(satellites_of(o.city_visible[0]), (std::vector<std::size_t>{0, 3}));
  EXPECT_TRUE(o.gateway_visible[0].empty());
  EXPECT_EQ(satellites_of(o.gateway_visible[1]), (std::vector<std::size_t>{0}));
  EXPECT_EQ(o.satellite_down, (std::vector<char>{0, 1, 0, 0}));
  EXPECT_EQ(o.gateway_down, (std::vector<char>{1, 0}));
  const auto ground = o.ground();
  EXPECT_EQ(ground.links, &o.links);
  EXPECT_EQ(ground.city_visible.size(), 1U);
  EXPECT_EQ(ground.gateway_visible.size(), 2U);
  EXPECT_EQ(ground.satellite_down.size(), 4U);
  EXPECT_EQ(ground.gateway_down.size(), 2U);
}

// Repair link 3 with nothing known, or known down, is not in force; known up but touching a
// satellite taken as down, failed. The view never shows a link acquiring, even when the truth
// has it acquiring.
TEST(ViewTest, OverlayOfAViewForRepairLinksAndNeverAcquiring) {
  Timeline t = small_timeline({});
  t.force[0].push_back({.from_s = 5.0, .force = LinkForce::kAcquiring});
  const ViewState none = satsim::failures_app::view_at(t, {}, 11.0);
  const OverlaidStep o = satsim::failures_app::overlay_view(step_geometry(), geometric(), t, none);
  EXPECT_EQ(o.force, (std::vector<LinkForce>{LinkForce::kInForce, LinkForce::kInForce,
                                             LinkForce::kInForce, LinkForce::kNotInForce}));
  EXPECT_EQ(o.links.states[3].down_reason, LinkDownReason::kNotInForce);
  EXPECT_EQ(satellites_of(o.city_visible[0]), (std::vector<std::size_t>{0, 1, 3}));

  const Timeline down = small_timeline({fact(SubjectKind::kLink, 3, true, 10.0, 11.0)});
  EXPECT_EQ(
      satsim::failures_app::overlay_view(step_geometry(), geometric(), down,
                                         satsim::failures_app::view_at(down, known_at(down), 11.0))
          .force[3],
      LinkForce::kNotInForce);
  const Timeline up_dead = small_timeline({fact(SubjectKind::kLink, 3, false, 10.0, 11.0),
                                           fact(SubjectKind::kSatellite, 3, true, 20.0, 21.0)});
  EXPECT_EQ(satsim::failures_app::overlay_view(
                step_geometry(), geometric(), up_dead,
                satsim::failures_app::view_at(up_dead, known_at(up_dead), 21.0))
                .force[3],
            LinkForce::kFailed);
}

// The truth on the transient world: satellite 5 down from 10 s for 2.5 s, its links 3 and 4
// acquiring from 12.5 s to 42.5 s.
TEST(ViewTest, OverlayOfTheTruthShowsAcquiring) {
  World w;
  const Timeline t = w.timeline({failure_at(ElementKind::kSatellite, 5, 10.0, 2.5)});
  {
    const StepLinks g = satsim::failures_app::geometric_states(w.geometry, t.links, 11);
    const OverlaidStep o = satsim::failures_app::overlay_truth(w.geometry.at(11), g, t, 11.0);
    EXPECT_EQ(o.force[3], LinkForce::kFailed);
    EXPECT_EQ(o.links.states[3].down_reason, LinkDownReason::kFailed);
    EXPECT_EQ(o.links.states[3].length_km, 1000.0);
    EXPECT_EQ(satellites_of(o.city_visible[0]), (std::vector<std::size_t>{1}));
    EXPECT_EQ(satellites_of(o.gateway_visible[1]), (std::vector<std::size_t>{16}));
    EXPECT_EQ(o.satellite_down[5], 1);
    EXPECT_EQ(o.gateway_down, (std::vector<char>{0, 0}));
  }
  {
    const StepLinks g = satsim::failures_app::geometric_states(w.geometry, t.links, 20);
    const OverlaidStep o = satsim::failures_app::overlay_truth(w.geometry.at(20), g, t, 20.0);
    EXPECT_EQ(o.force[3], LinkForce::kAcquiring);
    EXPECT_EQ(o.links.states[3].down_reason, LinkDownReason::kAcquiring);
    EXPECT_EQ(satellites_of(o.city_visible[0]), (std::vector<std::size_t>{1, 5}));
    EXPECT_EQ(o.satellite_down[5], 0);
    EXPECT_TRUE(o.links.states[0].up());
  }
  {
    // The planner's view at 20 s: it heard of the return (12.5 s) but not yet of the links
    // coming up; it still holds links 3 and 4 down, so satellite 5 is taken as down too.
    const ViewState v =
        satsim::failures_app::view_at(t, satsim::failures_app::planner_knowledge(t), 20.0);
    EXPECT_EQ(v.links[3], Belief::kDown);
    EXPECT_EQ(v.satellites[5], Belief::kUp);
    EXPECT_EQ(v.satellite_down[5], 1);
  }
}

// A gateway down in the truth gets an empty list.
TEST(ViewTest, OverlayOfTheTruthEmptiesADownGateway) {
  World w;
  const Timeline t = w.timeline({failure_at(ElementKind::kGateway, 0, 10.0)});
  const StepLinks g = satsim::failures_app::geometric_states(w.geometry, t.links, 10);
  const OverlaidStep o = satsim::failures_app::overlay_truth(w.geometry.at(10), g, t, 10.0);
  EXPECT_TRUE(o.gateway_visible[0].empty());
  EXPECT_EQ(satellites_of(o.gateway_visible[1]), (std::vector<std::size_t>{5, 16}));
  EXPECT_EQ(o.gateway_down, (std::vector<char>{1, 0}));
}

}  // namespace
