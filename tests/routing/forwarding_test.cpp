#include "satsim/routing/forwarding.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <vector>

#include "satsim/network/graph.hpp"
#include "satsim/network/paths_to.hpp"
#include "satsim/network/shortest_path.hpp"
#include "satsim/routing/planner.hpp"
#include "satsim/routing/strategy.hpp"

namespace {

using satsim::network::Graph;
using satsim::network::kNoNode;
using satsim::network::PathsTo;
using satsim::network::shortest_paths_to;
using satsim::routing::name_of;
using satsim::routing::next_hop_from_views;
using satsim::routing::NextHop;
using satsim::routing::Route;
using satsim::routing::RoutePlanner;
using satsim::routing::Strategy;
using satsim::routing::StrategyKind;
using satsim::routing::Walk;
using satsim::routing::walk;
using satsim::routing::WalkOutcome;

// Node 0 is the sending dish, node 1 the receiving dish, 2 .. 5 satellites. Ground edges are
// directed, lasers undirected. Delays are binary fractions, so every sum is exact.
//
//   0 -> 2 (0.25), 0 -> 3 (0.25)      2 - 4 (0.25), 3 - 5 (0.5), 4 - 5 (0.125), 2 - 3 (0.125)
//   4 -> 1 (0.25), 5 -> 1 (0.25)
//
// The full view, towards 1:
//   4: 0.25 (next 1)            5: 0.25 (next 1)
//   2: 2-4-1 0.5 (next 4)       3: 3-2-4-1 0.625, not 3-5-1 0.75 (next 2)
//   0: 0-2-4-1 0.75, not 0-3-2-4-1 0.875 nor 0-3-5-1 1.0 (next 2). No ties anywhere.
// The view without laser 2 - 4 ("view B"), towards 1:
//   4: 0.25 (next 1)            5: 0.25 (next 1)
//   3: 3-5-1 0.75 (next 5)      2: 2-3-5-1 0.875 (next 3)
//   0: 0-3-5-1 1.0, not 0-2-3-5-1 1.125 (next 3).
constexpr std::size_t kSource = 0;
constexpr std::size_t kTarget = 1;

Graph network(bool with_2_4) {
  Graph g(6);
  const auto ground = [&](std::size_t a, std::size_t b, double delay_s) {
    EXPECT_FALSE(g.add_edge(a, b, delay_s).has_value());
  };
  const auto laser = [&](std::size_t a, std::size_t b, double delay_s) {
    EXPECT_FALSE(g.add_undirected_edge(a, b, delay_s).has_value());
  };
  ground(0, 2, 0.25);
  ground(0, 3, 0.25);
  if (with_2_4) {
    laser(2, 4, 0.25);
  }
  laser(3, 5, 0.5);
  laser(4, 5, 0.125);
  laser(2, 3, 0.125);
  ground(4, 1, 0.25);
  ground(5, 1, 0.25);
  return g;
}

TEST(ForwardingTest, OutcomeNames) {
  EXPECT_EQ(name_of(WalkOutcome::kDelivered), "delivered");
  EXPECT_EQ(name_of(WalkOutcome::kDeadEnd), "dead_end");
  EXPECT_EQ(name_of(WalkOutcome::kLoop), "loop");
}

void expect_walk(const Walk& w, WalkOutcome outcome, const Route& path) {
  EXPECT_EQ(name_of(w.outcome), name_of(outcome));
  EXPECT_EQ(w.path, path);
}

// Every node on one view: the walk is the tree's path, and the lowest-delay planner's route.
TEST(ForwardingTest, OneViewEverywhereGivesTheTreesPathAndThePlannersRoute) {
  for (const bool with_2_4 : {true, false}) {
    const Graph g = network(with_2_4);
    const PathsTo tree = shortest_paths_to(g, kTarget);
    const NextHop next = next_hop_from_views([&](std::size_t) -> const PathsTo& { return tree; });
    const Route expected = with_2_4 ? Route{0, 2, 4, 1} : Route{0, 3, 5, 1};
    expect_walk(walk(kSource, kTarget, next), WalkOutcome::kDelivered, expected);
    EXPECT_EQ(tree.path_from(kSource), expected);

    RoutePlanner planner(Strategy{.kind = StrategyKind::kLowestDelay});
    static_cast<void>(planner.plan(g, kSource, kTarget));
    EXPECT_EQ(planner.route(), expected) << "with 2-4 " << with_2_4;

    // From every node, the walk is the tree's path.
    for (std::size_t v = 0; v < g.node_count(); ++v) {
      expect_walk(walk(v, kTarget, next), WalkOutcome::kDelivered, tree.path_from(v));
    }
  }
}

// Node 2 holds view B (it has heard that 2 - 4 is down), everyone else the full view:
// 0 sends to 2 (full: next 2), 2 to 3 (B: next 3), 3 back to 2 (full: next 2). A loop.
TEST(ForwardingTest, TwoNodesWithDifferentViewsPointingAtEachOtherLoop) {
  const PathsTo full = shortest_paths_to(network(true), kTarget);
  const PathsTo b = shortest_paths_to(network(false), kTarget);
  ASSERT_EQ(full.next[3], 2U);
  ASSERT_EQ(b.next[2], 3U);
  const NextHop next =
      next_hop_from_views([&](std::size_t node) -> const PathsTo& { return node == 2 ? b : full; });
  expect_walk(walk(kSource, kTarget, next), WalkOutcome::kLoop, Route{0, 2, 3, 2});
  expect_walk(walk(3, kTarget, next), WalkOutcome::kLoop, Route{3, 2, 3});
}

// Nodes 2 and 3 both hold view B: 0 -> 2 (full), 2 -> 3 (B), 3 -> 5 (B), 5 -> 1 (full).
// Mixed views, still delivered.
TEST(ForwardingTest, MixedViewsThatStillDeliver) {
  const PathsTo full = shortest_paths_to(network(true), kTarget);
  const PathsTo b = shortest_paths_to(network(false), kTarget);
  const NextHop next = next_hop_from_views(
      [&](std::size_t node) -> const PathsTo& { return node == 2 || node == 3 ? b : full; });
  expect_walk(walk(kSource, kTarget, next), WalkOutcome::kDelivered, Route{0, 2, 3, 5, 1});
}

// Node 4 holds a view in which it has no edge at all, so no way on: 0 -> 2 -> 4, dead end.
TEST(ForwardingTest, ANodeWhoseViewHasNoWayOnIsADeadEnd) {
  const PathsTo full = shortest_paths_to(network(true), kTarget);
  Graph alone(6);  // Node 4's view: nothing works.
  const PathsTo nothing = shortest_paths_to(alone, kTarget);
  ASSERT_EQ(nothing.next[4], kNoNode);
  const NextHop next = next_hop_from_views(
      [&](std::size_t node) -> const PathsTo& { return node == 4 ? nothing : full; });
  expect_walk(walk(kSource, kTarget, next), WalkOutcome::kDeadEnd, Route{0, 2, 4});
  expect_walk(walk(4, kTarget, next), WalkOutcome::kDeadEnd, Route{4});
}

// The source is the target: delivered at once, and no node is asked.
TEST(ForwardingTest, SourceEqualToTargetIsDeliveredWithoutAsking) {
  std::size_t asked = 0;
  const NextHop next = [&](std::size_t) {
    ++asked;
    return kNoNode;
  };
  expect_walk(walk(7, 7, next), WalkOutcome::kDelivered, Route{7});
  EXPECT_EQ(asked, 0U);
}

// Plain callbacks: a loop back to the source, a node that points at itself, and each node
// asked once, in the order passed.
TEST(ForwardingTest, LoopsAreShownWithTheRepeatedNodeOnce) {
  const std::vector<std::size_t> back{2, kNoNode, 0};  // 0 -> 2 -> 0.
  expect_walk(walk(0, 1, [&](std::size_t n) { return back[n]; }), WalkOutcome::kLoop,
              Route{0, 2, 0});

  const std::vector<std::size_t> self{3, kNoNode, kNoNode, 3};  // 0 -> 3 -> 3.
  expect_walk(walk(0, 1, [&](std::size_t n) { return self[n]; }), WalkOutcome::kLoop,
              Route{0, 3, 3});

  // 0 -> 4 -> 2 -> 5 -> 1, each node asked once in that order.
  const std::vector<std::size_t> line{4, kNoNode, 5, kNoNode, 2, 1};
  std::vector<std::size_t> asked;
  const Walk w = walk(0, 1, [&](std::size_t n) {
    asked.push_back(n);
    return line[n];
  });
  expect_walk(w, WalkOutcome::kDelivered, Route{0, 4, 2, 5, 1});
  EXPECT_EQ(asked, (std::vector<std::size_t>{0, 4, 2, 5}));
}

}  // namespace
