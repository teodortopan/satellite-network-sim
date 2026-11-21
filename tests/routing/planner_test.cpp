#include "satsim/routing/planner.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <optional>
#include <random>
#include <string_view>
#include <variant>
#include <vector>

#include "satsim/network/graph.hpp"
#include "satsim/network/shortest_path.hpp"
#include "satsim/routing/strategy.hpp"

namespace {

using satsim::network::Graph;
using satsim::routing::AccessCandidates;
using satsim::routing::name_of;
using satsim::routing::parse_strategy;
using satsim::routing::PlanOutcome;
using satsim::routing::PlanResult;
using satsim::routing::Route;
using satsim::routing::RoutePlanner;
using satsim::routing::Strategy;
using satsim::routing::StrategyKind;

// Every graph here: node 0 is the sending dish (the source), node 1 the receiving dish (the
// target), and the others satellites. Ground edges are directed (dish -> satellite, satellite
// -> dish) as in the program's graph; lasers are undirected. Delays are binary fractions, so
// every sum is exact and every comparison is the one worked out by hand.
constexpr std::size_t kSource = 0;
constexpr std::size_t kTarget = 1;

void ground(Graph& g, std::size_t from, std::size_t to, double delay_s) {
  ASSERT_FALSE(g.add_edge(from, to, delay_s).has_value());
}

void laser(Graph& g, std::size_t a, std::size_t b, double delay_s) {
  ASSERT_FALSE(g.add_undirected_edge(a, b, delay_s).has_value());
}

// Two routes 0 -> 2 -> 1 (each ground edge via2_s) and 0 -> 3 -> 1 (each via3_s); a
// non-positive delay leaves that route's edges out.
Graph two_routes(double via2_s, double via3_s) {
  Graph g(4);
  if (via2_s > 0.0) {
    ground(g, kSource, 2, via2_s);
    ground(g, 2, kTarget, via2_s);
  }
  if (via3_s > 0.0) {
    ground(g, kSource, 3, via3_s);
    ground(g, 3, kTarget, via3_s);
  }
  return g;
}

Strategy strategy(std::string_view name) {
  const auto parsed = parse_strategy(name);
  EXPECT_TRUE(std::holds_alternative<Strategy>(parsed)) << name;
  return std::holds_alternative<Strategy>(parsed) ? std::get<Strategy>(parsed) : Strategy{};
}

void expect_plan(const PlanResult& result, const RoutePlanner& planner, PlanOutcome outcome,
                 const Route& route, std::optional<double> delay_s) {
  EXPECT_EQ(name_of(result.outcome), name_of(outcome));
  EXPECT_EQ(planner.route(), route);
  EXPECT_EQ(result.delay_s, delay_s);
  EXPECT_EQ(planner.has_route(), !route.empty());
}

TEST(PlannerTest, OutcomeNames) {
  EXPECT_EQ(name_of(PlanOutcome::kKept), "kept");
  EXPECT_EQ(name_of(PlanOutcome::kReplaced), "replaced");
  EXPECT_EQ(name_of(PlanOutcome::kNone), "none");
}

// lowest-delay re-takes the shortest path at every plan.
//   plan 1: via 2 = 0.25 + 0.25 = 0.5, via 3 = 1.0: [0, 2, 1], replaced (nothing held).
//   plan 2: the same graph: [0, 2, 1] again, kept.
//   plan 3: via 2 = 1.0, via 3 = 0.25: [0, 3, 1], replaced.
TEST(PlannerTest, LowestDelaySwitchesToTheShortestPath) {
  RoutePlanner planner(strategy("lowest-delay"));
  EXPECT_FALSE(planner.has_route());
  expect_plan(planner.plan(two_routes(0.25, 0.5), kSource, kTarget), planner,
              PlanOutcome::kReplaced, Route{0, 2, 1}, 0.5);
  expect_plan(planner.plan(two_routes(0.25, 0.5), kSource, kTarget), planner, PlanOutcome::kKept,
              Route{0, 2, 1}, 0.5);
  expect_plan(planner.plan(two_routes(0.5, 0.125), kSource, kTarget), planner,
              PlanOutcome::kReplaced, Route{0, 3, 1}, 0.25);
}

// Every strategy: no route while the target is unreachable, the shortest path once it is.
//   plan 1: no edge into 1: none.
//   plan 2: only via 2 (0.5): [0, 2, 1], replaced.
//   plan 3: unreachable again: none, the held route is dropped.
TEST(PlannerTest, UnreachableThenReachableForEveryStrategy) {
  for (const std::string_view name :
       {"lowest-delay", "keep-until-broken", "longest-lived", "threshold:1ms", "threshold:5%"}) {
    SCOPED_TRACE(name);
    RoutePlanner planner(strategy(name));
    Graph cut(4);
    ground(cut, kSource, 2, 0.25);
    const AccessCandidates candidates{.heads = {{0, 2}}, .tails = {{2, 1}}};
    expect_plan(planner.plan(cut, kSource, kTarget, candidates), planner, PlanOutcome::kNone,
                Route{}, std::nullopt);
    expect_plan(planner.plan(two_routes(0.25, 0.0), kSource, kTarget, candidates), planner,
                PlanOutcome::kReplaced, Route{0, 2, 1}, 0.5);
    expect_plan(planner.plan(cut, kSource, kTarget, candidates), planner, PlanOutcome::kNone,
                Route{}, std::nullopt);
    EXPECT_FALSE(planner.held_route_is_up(cut));
    EXPECT_EQ(planner.held_delay_s(cut), std::nullopt);
  }
}

// keep-until-broken holds a route while it is up, however much better another one gets.
//   plan 1: via 2 = 0.5, via 3 = 1.0: [0, 2, 1].
//   plan 2: via 2 = 1.0, via 3 = 0.25: [0, 2, 1] is up: kept, at its new delay 1.0.
//   step:   2 -> 1 gone: the held route is down, no delay.
//   plan 3: that graph: [0, 3, 1] (0.25), replaced.
TEST(PlannerTest, KeepUntilBrokenHoldsTheRouteUntilItBreaks) {
  RoutePlanner planner(strategy("keep-until-broken"));
  expect_plan(planner.plan(two_routes(0.25, 0.5), kSource, kTarget), planner,
              PlanOutcome::kReplaced, Route{0, 2, 1}, 0.5);
  const Graph better3 = two_routes(0.5, 0.125);
  expect_plan(planner.plan(better3, kSource, kTarget), planner, PlanOutcome::kKept, Route{0, 2, 1},
              1.0);
  EXPECT_TRUE(planner.held_route_is_up(better3));
  EXPECT_EQ(planner.held_delay_s(better3), std::optional(1.0));

  Graph broken = two_routes(0.0, 0.125);
  ground(broken, kSource, 2, 0.5);  // 0 -> 2 stays, 2 -> 1 is gone.
  EXPECT_FALSE(planner.held_route_is_up(broken));
  EXPECT_EQ(planner.held_delay_s(broken), std::nullopt);
  expect_plan(planner.plan(broken, kSource, kTarget), planner, PlanOutcome::kReplaced,
              Route{0, 3, 1}, 0.25);
}

// A held route whose ends are not this plan's source and target is not usable: replaced.
TEST(PlannerTest, AHeldRouteBetweenOtherNodesIsNotKept) {
  RoutePlanner planner(strategy("keep-until-broken"));
  Graph g = two_routes(0.25, 0.5);
  ground(g, 2, 3, 0.25);
  expect_plan(planner.plan(g, kSource, kTarget), planner, PlanOutcome::kReplaced, Route{0, 2, 1},
              0.5);
  expect_plan(planner.plan(g, kSource, 3), planner, PlanOutcome::kReplaced, Route{0, 3}, 0.5);
}

// Threshold in milliseconds. "threshold:1.953125ms" is 2^-9 s exactly (an exact quotient is
// rounded to itself), so "at the limit" is exact.
//   plan 1: only via 2, each edge 2^-6 + 2^-10: H = 2^-5 + 2^-9 = 0.033203125: [0, 2, 1].
//   plan 2: via 3 added with S = H - 2^-9 + delta, i.e. each edge (S / 2):
//     delta = 0       (H - S = 2^-9, at the limit):       kept.
//     delta = +2^-12  (H - S < 2^-9, under the limit):     kept.
//     delta = -2^-12  (H - S > 2^-9, over the limit):      [0, 3, 1], replaced.
TEST(PlannerTest, ThresholdInMillisecondsKeepsAtAndUnderTheLimitAndSwitchesOver) {
  const Strategy s = strategy("threshold:1.953125ms");
  ASSERT_EQ(s.threshold_s, 0.001953125);
  constexpr double kEdgeH = 0.015625 + 0.0009765625;  // 2^-6 + 2^-10
  constexpr double kH = 2 * kEdgeH;                   // 0.033203125
  constexpr double kLimit = 0.001953125;              // 2^-9
  for (const double delta : {0.0, 0.000244140625, -0.000244140625}) {
    SCOPED_TRACE(delta);
    RoutePlanner planner(s);
    expect_plan(planner.plan(two_routes(kEdgeH, 0.0), kSource, kTarget), planner,
                PlanOutcome::kReplaced, Route{0, 2, 1}, kH);
    const double shortest_s = kH - kLimit + delta;
    const Graph both = two_routes(kEdgeH, shortest_s / 2);
    if (delta >= 0.0) {
      expect_plan(planner.plan(both, kSource, kTarget), planner, PlanOutcome::kKept, Route{0, 2, 1},
                  kH);
    } else {
      expect_plan(planner.plan(both, kSource, kTarget), planner, PlanOutcome::kReplaced,
                  Route{0, 3, 1}, shortest_s);
    }
  }
}

// Threshold as a fraction: 12.5% of the held delay H = 0.25 (each edge 0.125) is 0.03125.
//   S = 0.21875           (H - S = 0.03125, at the limit):   kept.
//   S = 0.21875 + 2^-10   (under the limit):                  kept.
//   S = 0.21875 - 2^-10   (over the limit):                   [0, 3, 1], replaced.
TEST(PlannerTest, ThresholdAsAFractionKeepsAtAndUnderTheLimitAndSwitchesOver) {
  const Strategy s = strategy("threshold:12.5%");
  ASSERT_EQ(s.threshold_fraction, 0.125);
  for (const double delta : {0.0, 0.0009765625, -0.0009765625}) {
    SCOPED_TRACE(delta);
    RoutePlanner planner(s);
    expect_plan(planner.plan(two_routes(0.125, 0.0), kSource, kTarget), planner,
                PlanOutcome::kReplaced, Route{0, 2, 1}, 0.25);
    const double shortest_s = 0.21875 + delta;
    const Graph both = two_routes(0.125, shortest_s / 2);
    if (delta >= 0.0) {
      expect_plan(planner.plan(both, kSource, kTarget), planner, PlanOutcome::kKept, Route{0, 2, 1},
                  0.25);
    } else {
      expect_plan(planner.plan(both, kSource, kTarget), planner, PlanOutcome::kReplaced,
                  Route{0, 3, 1}, shortest_s);
    }
  }
}

// A broken held route is replaced by the shortest path even when that is much worse, and a
// realistic threshold (1 ms) holds the route against a gain of 0.5 ms but not of 2 ms.
//   plan 1: via 2 = 0.5, via 3 = 1.0: [0, 2, 1].
//   plan 2: 2 -> 1 gone: [0, 3, 1] (1.0), replaced although 0.5 worse than before.
//   plan 3: via 2 back at 0.9995 s (0.5 ms better than the held 1.0): kept.
//   plan 4: via 2 at 0.998 s (2 ms better): [0, 2, 1], replaced.
TEST(PlannerTest, ThresholdTakesTheShortestPathWhenTheHeldRouteBreaks) {
  RoutePlanner planner(strategy("threshold:1ms"));
  expect_plan(planner.plan(two_routes(0.25, 0.5), kSource, kTarget), planner,
              PlanOutcome::kReplaced, Route{0, 2, 1}, 0.5);
  Graph broken = two_routes(0.0, 0.5);
  ground(broken, kSource, 2, 0.25);
  expect_plan(planner.plan(broken, kSource, kTarget), planner, PlanOutcome::kReplaced,
              Route{0, 3, 1}, 1.0);
  const PlanResult small_gain = planner.plan(two_routes(0.49975, 0.5), kSource, kTarget);
  EXPECT_EQ(small_gain.outcome, PlanOutcome::kKept);
  EXPECT_EQ(planner.route(), (Route{0, 3, 1}));
  const PlanResult large_gain = planner.plan(two_routes(0.499, 0.5), kSource, kTarget);
  EXPECT_EQ(large_gain.outcome, PlanOutcome::kReplaced);
  EXPECT_EQ(planner.route(), (Route{0, 2, 1}));
}

// The 2 x 2 longest-lived graph. Heads [0, 2] and [0, 3], tails [4, 1] and [5, 1], each
// ground edge 0.125. Lasers: 2 - 4 (0.5) and 3 - 5 (0.125), nothing else between 2..5, so
// (2, 5) and (3, 4) are not joined. A satellite 6 that is no candidate gives the true shortest
// path 0 -> 6 -> 1 (0.0625 + 0.0625).
//   (2, 4): 0.125 + 0.5 + 0.125 = 0.75; (3, 5): 0.125 + 0.125 + 0.125 = 0.375.
Graph two_by_two() {
  Graph g(7);
  for (const std::size_t s : {2U, 3U}) {
    ground(g, kSource, s, 0.125);
  }
  for (const std::size_t s : {4U, 5U}) {
    ground(g, s, kTarget, 0.125);
  }
  laser(g, 2, 4, 0.5);
  laser(g, 3, 5, 0.125);
  ground(g, kSource, 6, 0.0625);
  ground(g, 6, kTarget, 0.0625);
  return g;
}

const AccessCandidates kTwoByTwo{.heads = {{0, 2}, {0, 3}}, .tails = {{4, 1}, {5, 1}}};

// The lowest-delay joined pair is the second head with the second tail; two pairs are not
// joined; the shortest path through 6 is not a candidate.
TEST(PlannerTest, LongestLivedTakesTheLowestDelayJoinedPair) {
  RoutePlanner planner(strategy("longest-lived"));
  expect_plan(planner.plan(two_by_two(), kSource, kTarget, kTwoByTwo), planner,
              PlanOutcome::kReplaced, Route{0, 3, 5, 1}, 0.375);
  RoutePlanner lowest(strategy("lowest-delay"));
  EXPECT_EQ(lowest.plan(two_by_two(), kSource, kTarget).delay_s, std::optional(0.125));
}

// Equal delays: laser 2 - 4 at 0.125 makes (2, 4) 0.375 too, and the earlier head wins:
// [0, 2, 4, 1]. With one head and two equally joined tails (laser 2 - 5 at 0.125 too), the
// earlier tail wins: [0, 2, 4, 1].
TEST(PlannerTest, LongestLivedTiesGoToTheEarlierHeadThenTheEarlierTail) {
  Graph g = two_by_two();
  g.clear_edges();
  for (const std::size_t s : {2U, 3U}) {
    ground(g, kSource, s, 0.125);
  }
  for (const std::size_t s : {4U, 5U}) {
    ground(g, s, kTarget, 0.125);
  }
  laser(g, 2, 4, 0.125);
  laser(g, 3, 5, 0.125);
  RoutePlanner by_head(strategy("longest-lived"));
  expect_plan(by_head.plan(g, kSource, kTarget, kTwoByTwo), by_head, PlanOutcome::kReplaced,
              Route{0, 2, 4, 1}, 0.375);

  laser(g, 2, 5, 0.125);
  const AccessCandidates one_head{.heads = {{0, 2}}, .tails = {{4, 1}, {5, 1}}};
  RoutePlanner by_tail(strategy("longest-lived"));
  expect_plan(by_tail.plan(g, kSource, kTarget, one_head), by_tail, PlanOutcome::kReplaced,
              Route{0, 2, 4, 1}, 0.375);
  const AccessCandidates swapped{.heads = {{0, 2}}, .tails = {{5, 1}, {4, 1}}};
  RoutePlanner by_tail_swapped(strategy("longest-lived"));
  expect_plan(by_tail_swapped.plan(g, kSource, kTarget, swapped), by_tail_swapped,
              PlanOutcome::kReplaced, Route{0, 2, 5, 1}, 0.375);
}

// A head that is not up (no edge 0 -> 3) is skipped: only (2, 4) remains, 0.75.
TEST(PlannerTest, LongestLivedSkipsAHeadOrTailThatIsNotUp) {
  Graph g = two_by_two();
  g.clear_edges();
  ground(g, kSource, 2, 0.125);
  for (const std::size_t s : {4U, 5U}) {
    ground(g, s, kTarget, 0.125);
  }
  laser(g, 2, 4, 0.5);
  laser(g, 3, 5, 0.125);
  RoutePlanner planner(strategy("longest-lived"));
  expect_plan(planner.plan(g, kSource, kTarget, kTwoByTwo), planner, PlanOutcome::kReplaced,
              Route{0, 2, 4, 1}, 0.75);

  // And a tail that is not up: with 4 -> 1 gone too, nothing is joined.
  Graph h(7);
  ground(h, kSource, 2, 0.125);
  ground(h, 5, kTarget, 0.125);
  laser(h, 2, 4, 0.5);
  laser(h, 3, 5, 0.125);
  RoutePlanner none(strategy("longest-lived"));
  expect_plan(none.plan(h, kSource, kTarget, kTwoByTwo), none, PlanOutcome::kNone, Route{},
              std::nullopt);
}

// head.back() == tail.front(): satellite 2 sees both dishes; the middle is the one node.
//   [0, 2] + [2, 1] = [0, 2, 1], 0.125 + 0.125. The pair (2, 4) costs 0.75 and loses.
TEST(PlannerTest, LongestLivedJoinsAHeadAndTailThatMeetAtOneSatellite) {
  Graph g = two_by_two();
  ground(g, 2, kTarget, 0.125);
  const AccessCandidates candidates{.heads = {{0, 2}}, .tails = {{4, 1}, {2, 1}}};
  RoutePlanner planner(strategy("longest-lived"));
  expect_plan(planner.plan(g, kSource, kTarget, candidates), planner, PlanOutcome::kReplaced,
              Route{0, 2, 1}, 0.25);
}

// The middle never enters a node of the head or tail other than its two ends. Head [0, 2],
// tail [4, 6, 1] (satellite 4, gateway 6, end 1). Laser 2 - 4 (0.5); 2 - 6 and 6 -> 4 at
// 0.0625 each: 2 -> 6 -> 4 would be cheaper (0.125) but 6 is in the tail, so the middle is
// 2 -> 4 and the route [0, 2, 4, 6, 1]: 0.125 + 0.5 + 0.125 + 0.125 = 0.875.
// A pair that shares a node besides the joint is skipped: head [0, 2], tail [4, 2, 1].
TEST(PlannerTest, LongestLivedMiddleAvoidsTheHeadAndTail) {
  Graph g(7);
  ground(g, kSource, 2, 0.125);
  laser(g, 2, 4, 0.5);
  laser(g, 2, 6, 0.0625);
  ground(g, 6, 4, 0.0625);
  ground(g, 4, 6, 0.125);
  ground(g, 6, kTarget, 0.125);
  ground(g, 2, kTarget, 0.125);
  RoutePlanner planner(strategy("longest-lived"));
  const AccessCandidates via_gateway{.heads = {{0, 2}}, .tails = {{4, 6, 1}}};
  expect_plan(planner.plan(g, kSource, kTarget, via_gateway), planner, PlanOutcome::kReplaced,
              Route{0, 2, 4, 6, 1}, 0.875);

  RoutePlanner shared(strategy("longest-lived"));
  const AccessCandidates sharing{.heads = {{0, 2}}, .tails = {{4, 2, 1}}};
  expect_plan(shared.plan(g, kSource, kTarget, sharing), shared, PlanOutcome::kNone, Route{},
              std::nullopt);
}

// longest-lived keeps its route while it is up, whatever the candidates, then re-chooses.
//   plan 1: [0, 3, 5, 1] (0.375), as above.
//   plan 2: no candidates at all, a much shorter path via 6: kept.
//   plan 3: laser 3 - 5 down: the only joined pair is (2, 4): [0, 2, 4, 1] (0.75).
//   plan 4: everything down and no candidates: none.
TEST(PlannerTest, LongestLivedKeepsItsRouteWhileItIsUp) {
  RoutePlanner planner(strategy("longest-lived"));
  expect_plan(planner.plan(two_by_two(), kSource, kTarget, kTwoByTwo), planner,
              PlanOutcome::kReplaced, Route{0, 3, 5, 1}, 0.375);
  expect_plan(planner.plan(two_by_two(), kSource, kTarget, {}), planner, PlanOutcome::kKept,
              Route{0, 3, 5, 1}, 0.375);
  Graph no35 = two_by_two();
  no35.clear_edges();
  for (const std::size_t s : {2U, 3U}) {
    ground(no35, kSource, s, 0.125);
  }
  for (const std::size_t s : {4U, 5U}) {
    ground(no35, s, kTarget, 0.125);
  }
  laser(no35, 2, 4, 0.5);
  expect_plan(planner.plan(no35, kSource, kTarget, kTwoByTwo), planner, PlanOutcome::kReplaced,
              Route{0, 2, 4, 1}, 0.75);
  expect_plan(planner.plan(Graph(7), kSource, kTarget, {}), planner, PlanOutcome::kNone, Route{},
              std::nullopt);
}

// Random small graphs that change at every plan: at each plan, lowest-delay's delay is never
// above any other strategy's, both measured in that planning graph. Delays are real numbers,
// so this also checks that the sums are formed the same way (path order) everywhere.
TEST(PlannerTest, LowestDelayIsNeverAboveAnyOtherStrategyAtAPlan) {
  constexpr std::size_t kNodes = 12;  // 0 source, 1 target, 2..11 satellites.
  constexpr int kPlans = 300;
  std::mt19937_64 random(20261120);
  std::uniform_real_distribution<double> delay(0.001, 0.02);
  std::bernoulli_distribution ground_up(0.3);
  std::bernoulli_distribution laser_up(0.25);

  std::vector<RoutePlanner> others;
  for (const std::string_view name : {"keep-until-broken", "longest-lived", "threshold:1ms",
                                      "threshold:5ms", "threshold:2%", "threshold:10%"}) {
    others.emplace_back(strategy(name));
  }
  RoutePlanner lowest(Strategy{.kind = StrategyKind::kLowestDelay});
  std::size_t compared = 0;
  std::size_t worse = 0;
  for (int k = 0; k < kPlans; ++k) {
    Graph g(kNodes);
    AccessCandidates candidates;
    for (std::size_t s = 2; s < kNodes; ++s) {
      if (ground_up(random)) {
        ground(g, kSource, s, delay(random));
        if (candidates.heads.size() < 2) {
          candidates.heads.push_back({kSource, s});
        }
      }
      if (ground_up(random)) {
        ground(g, s, kTarget, delay(random));
        if (candidates.tails.size() < 2) {
          candidates.tails.push_back({s, kTarget});
        }
      }
      for (std::size_t t = s + 1; t < kNodes; ++t) {
        if (laser_up(random)) {
          laser(g, s, t, delay(random));
        }
      }
    }
    const PlanResult best = lowest.plan(g, kSource, kTarget);
    for (RoutePlanner& other : others) {
      const PlanResult result = other.plan(g, kSource, kTarget, candidates);
      if (best.delay_s.has_value() && result.delay_s.has_value()) {
        ++compared;
        worse += *best.delay_s > *result.delay_s ? 1U : 0U;
      }
      // Any route a strategy holds after a plan is up in the planning graph.
      if (other.has_route()) {
        worse += other.held_route_is_up(g) ? 0U : 1U;
      }
    }
  }
  EXPECT_EQ(worse, 0U);
  EXPECT_GT(compared, 500U);  // Enough plans had routes to test something.
}

// plan_with_shortest, given the shortest path, gives exactly what plan gives (outcome, delay
// and held route), for every strategy, on the same random graphs changing at every plan.
TEST(PlannerTest, PlanWithAGivenShortestPathEqualsPlan) {
  constexpr std::size_t kNodes = 12;
  constexpr int kPlans = 100;
  std::mt19937_64 random(20261121);
  std::uniform_real_distribution<double> delay(0.001, 0.02);
  std::bernoulli_distribution ground_up(0.3);
  std::bernoulli_distribution laser_up(0.25);
  std::vector<RoutePlanner> searching;
  std::vector<RoutePlanner> given;
  for (const std::string_view name :
       {"lowest-delay", "keep-until-broken", "longest-lived", "threshold:1ms", "threshold:2%"}) {
    searching.emplace_back(strategy(name));
    given.emplace_back(strategy(name));
  }
  std::size_t routes = 0;
  for (int k = 0; k < kPlans; ++k) {
    Graph g(kNodes);
    AccessCandidates candidates;
    for (std::size_t s = 2; s < kNodes; ++s) {
      if (ground_up(random)) {
        ground(g, kSource, s, delay(random));
        candidates.heads.push_back({kSource, s});
      }
      if (ground_up(random)) {
        ground(g, s, kTarget, delay(random));
        candidates.tails.push_back({s, kTarget});
      }
      for (std::size_t t = s + 1; t < kNodes; ++t) {
        if (laser_up(random)) {
          laser(g, s, t, delay(random));
        }
      }
    }
    const Route shortest = satsim::network::shortest_paths(g, kSource).path_to(kTarget);
    for (std::size_t i = 0; i < searching.size(); ++i) {
      const PlanResult a = searching[i].plan(g, kSource, kTarget, candidates);
      const PlanResult b = given[i].plan_with_shortest(g, kSource, kTarget, shortest, candidates);
      EXPECT_EQ(name_of(a.outcome), name_of(b.outcome));
      EXPECT_EQ(a.delay_s, b.delay_s);
      EXPECT_EQ(searching[i].route(), given[i].route());
      routes += given[i].has_route() ? 1U : 0U;
    }
  }
  EXPECT_GT(routes, 150U);
}

// An empty given path means the target is unreachable: lowest-delay then holds no route.
TEST(PlannerTest, PlanWithAnEmptyShortestPathHasNoRoute) {
  const Graph g = two_routes(0.25, 0.5);
  RoutePlanner planner(Strategy{.kind = StrategyKind::kLowestDelay});
  expect_plan(planner.plan_with_shortest(g, kSource, kTarget, {}), planner, PlanOutcome::kNone, {},
              std::nullopt);
}

// hold replaces the held route without planning; the next plan treats it as any held route.
//   two_routes(0.25, 0.5): via 2 = 0.5, via 3 = 1.0.
//   plan 1 (keep-until-broken): [0, 2, 1].
//   hold [0, 3, 1]: held, up, delay 1.0.
//   plan 2: [0, 3, 1] is usable: kept at 1.0, although via 2 is shorter.
TEST(PlannerTest, HoldReplacesTheHeldRouteAndKeepUntilBrokenKeepsIt) {
  const Graph g = two_routes(0.25, 0.5);
  RoutePlanner planner(strategy("keep-until-broken"));
  expect_plan(planner.plan(g, kSource, kTarget), planner, PlanOutcome::kReplaced, Route{0, 2, 1},
              0.5);
  planner.hold(Route{0, 3, 1});
  EXPECT_EQ(planner.route(), (Route{0, 3, 1}));
  EXPECT_TRUE(planner.has_route());
  EXPECT_TRUE(planner.held_route_is_up(g));
  EXPECT_EQ(planner.held_delay_s(g), std::optional(1.0));
  expect_plan(planner.plan(g, kSource, kTarget), planner, PlanOutcome::kKept, Route{0, 3, 1}, 1.0);
}

// A held route that is not up: it is reported down, and the next plan replaces it.
//   two_routes(0.25, 0) has no edges via 3.
TEST(PlannerTest, HoldOfARouteThatIsNotUpIsReplacedAtTheNextPlan) {
  const Graph g = two_routes(0.25, 0.0);
  RoutePlanner planner(strategy("keep-until-broken"));
  planner.hold(Route{0, 3, 1});
  EXPECT_EQ(planner.route(), (Route{0, 3, 1}));
  EXPECT_FALSE(planner.held_route_is_up(g));
  EXPECT_EQ(planner.held_delay_s(g), std::nullopt);
  expect_plan(planner.plan(g, kSource, kTarget), planner, PlanOutcome::kReplaced, Route{0, 2, 1},
              0.5);
}

// Holding an empty route means no route; holding the route a plan would choose makes that
// plan "kept".
TEST(PlannerTest, HoldEmptyIsNoRouteAndOutcomesCompareAgainstTheHeldRoute) {
  const Graph g = two_routes(0.25, 0.5);
  RoutePlanner planner(strategy("keep-until-broken"));
  expect_plan(planner.plan(g, kSource, kTarget), planner, PlanOutcome::kReplaced, Route{0, 2, 1},
              0.5);
  planner.hold({});
  EXPECT_FALSE(planner.has_route());
  EXPECT_TRUE(planner.route().empty());
  EXPECT_FALSE(planner.held_route_is_up(g));
  EXPECT_EQ(planner.held_delay_s(g), std::nullopt);
  expect_plan(planner.plan(g, kSource, kTarget), planner, PlanOutcome::kReplaced, Route{0, 2, 1},
              0.5);

  RoutePlanner lowest(strategy("lowest-delay"));
  lowest.hold(Route{0, 2, 1});
  expect_plan(lowest.plan(g, kSource, kTarget), lowest, PlanOutcome::kKept, Route{0, 2, 1}, 0.5);
  lowest.hold(Route{0, 3, 1});
  expect_plan(lowest.plan(g, kSource, kTarget), lowest, PlanOutcome::kReplaced, Route{0, 2, 1},
              0.5);
}

}  // namespace
