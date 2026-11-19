#include "satsim/network/shortest_path.hpp"

#include <gtest/gtest.h>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <random>
#include <tuple>
#include <vector>

#include "satsim/network/graph.hpp"

namespace {

using satsim::network::Graph;
using satsim::network::kNoNode;
using satsim::network::shortest_paths;
using satsim::network::ShortestPaths;
using Path = std::vector<std::size_t>;

constexpr double kInf = std::numeric_limits<double>::infinity();

void add(Graph& g, std::size_t a, std::size_t b, double delay_s) {
  ASSERT_FALSE(g.add_undirected_edge(a, b, delay_s).has_value());
}

void add_directed(Graph& g, std::size_t a, std::size_t b, double delay_s) {
  ASSERT_FALSE(g.add_edge(a, b, delay_s).has_value());
}

// Worked by hand (undirected, delays in seconds):
//   0 -1- 1 -2- 3        shortest: 0:0, 1:1, 2:3 (0-1-2: 1+2, not 0-2: 4), 3:3 (0-1-3),
//   |      \    |                  4:4 (0-1-3-4: 1+2+1, not 0-1-2-4: 1+2+2)
//   4      2    1        5 is unconnected.
//   (0-2)   \   |
//            2 -2- 4
TEST(ShortestPathTest, HandWorkedGraph) {
  Graph g(6);
  add(g, 0, 1, 1.0);
  add(g, 0, 2, 4.0);
  add(g, 1, 2, 2.0);
  add(g, 1, 3, 2.0);
  add(g, 3, 4, 1.0);
  add(g, 2, 4, 2.0);
  const ShortestPaths p = shortest_paths(g, 0);
  EXPECT_EQ(p.source, 0U);
  EXPECT_EQ(p.delay_s, (std::vector<double>{0.0, 1.0, 3.0, 3.0, 4.0, kInf}));
  EXPECT_EQ(p.hops, (std::vector<std::size_t>{0, 1, 2, 2, 3, 0}));
  EXPECT_EQ(p.predecessor, (std::vector<std::size_t>{kNoNode, 0, 1, 1, 3, kNoNode}));
  EXPECT_EQ(p.path_to(4), (Path{0, 1, 3, 4}));
  EXPECT_EQ(p.path_to(0), (Path{0}));
  EXPECT_TRUE(p.reachable(0));
  EXPECT_TRUE(p.reachable(4));
}

TEST(ShortestPathTest, NoPath) {
  Graph g(4);
  add(g, 0, 1, 1.0);
  add(g, 2, 3, 1.0);
  const ShortestPaths p = shortest_paths(g, 0);
  for (const std::size_t n : {2U, 3U}) {
    EXPECT_FALSE(p.reachable(n));
    EXPECT_EQ(p.delay_s[n], kInf);
    EXPECT_EQ(p.predecessor[n], kNoNode);
    EXPECT_TRUE(p.path_to(n).empty());
  }
  EXPECT_EQ(p.path_to(1), (Path{0, 1}));
}

TEST(ShortestPathTest, SingleNode) {
  Graph g(1);
  const ShortestPaths p = shortest_paths(g, 0);
  EXPECT_EQ(p.delay_s, (std::vector<double>{0.0}));
  EXPECT_EQ(p.path_to(0), (Path{0}));
  EXPECT_TRUE(p.reachable(0));
}

TEST(ShortestPathTest, DirectedEdgesGoOneWay) {
  Graph g(3);
  add_directed(g, 0, 1, 1.0);
  add_directed(g, 2, 1, 1.0);
  const ShortestPaths from0 = shortest_paths(g, 0);
  EXPECT_TRUE(from0.reachable(1));
  EXPECT_FALSE(from0.reachable(2));
  const ShortestPaths from1 = shortest_paths(g, 1);
  EXPECT_FALSE(from1.reachable(0));
  EXPECT_FALSE(from1.reachable(2));
}

// Equal delays: fewest hops first, then the lowest predecessor. Delays are binary fractions,
// so the sums are exact and the ties are real.
TEST(ShortestPathTest, EqualDelaysGoToFewestHopsThenLowestPredecessor) {
  // Diamond 0 -> {2, 1} -> 3, both 0.5 + 0.25: predecessor 1, the lower, whatever the order
  // of insertion.
  for (const bool reversed : {false, true}) {
    Graph g(4);
    if (reversed) {
      add(g, 2, 3, 0.25);
      add(g, 1, 3, 0.25);
      add(g, 0, 2, 0.5);
      add(g, 0, 1, 0.5);
    } else {
      add(g, 0, 1, 0.5);
      add(g, 0, 2, 0.5);
      add(g, 1, 3, 0.25);
      add(g, 2, 3, 0.25);
    }
    const ShortestPaths p = shortest_paths(g, 0);
    EXPECT_EQ(p.delay_s[3], 0.75);
    EXPECT_EQ(p.path_to(3), (Path{0, 1, 3})) << "reversed " << reversed;
  }
  {
    // 0 -> 3 directly (0.75, one hop) against 0 -> 1 -> 3 (0.5 + 0.25, two hops): direct,
    // although its predecessor 0 would win anyway; and 0 -> 1 -> 2 -> 4 (three hops) against
    // 0 -> 3 -> 4 (two hops), same delay: through 3 although 2 is lower than 3.
    Graph g(5);
    add(g, 0, 1, 0.5);
    add(g, 1, 3, 0.25);
    add(g, 0, 3, 0.75);
    add(g, 1, 2, 0.125);
    add(g, 2, 4, 0.125);
    add(g, 3, 4, 0.0);
    const ShortestPaths p = shortest_paths(g, 0);
    EXPECT_EQ(p.path_to(3), (Path{0, 3}));
    EXPECT_EQ(p.delay_s[4], 0.75);
    EXPECT_EQ(p.path_to(4), (Path{0, 3, 4}));
  }
  {
    // Zero-delay edges: 0 -0- 1 -0- 2 and 0 -0- 2: 2 is reached directly (fewer hops).
    Graph g(3);
    add(g, 0, 1, 0.0);
    add(g, 1, 2, 0.0);
    add(g, 0, 2, 0.0);
    const ShortestPaths p = shortest_paths(g, 0);
    EXPECT_EQ(p.delay_s, (std::vector<double>{0.0, 0.0, 0.0}));
    EXPECT_EQ(p.path_to(2), (Path{0, 2}));
  }
}

TEST(ShortestPathTest, ParallelEdgesAndSelfLoopsDoNotMatter) {
  Graph g(2);
  add(g, 0, 0, 0.0);
  add(g, 0, 1, 2.0);
  add(g, 0, 1, 1.0);
  const ShortestPaths p = shortest_paths(g, 0);
  EXPECT_EQ(p.delay_s[1], 1.0);
  EXPECT_EQ(p.hops[1], 1U);
}

// A simple O(n^2) Dijkstra with the same documented rule: settle the unsettled node with the
// smallest (delay, hops, node), relax with (delay, hops, predecessor) compared
// lexicographically.
ShortestPaths reference(const Graph& g, std::size_t source) {
  const std::size_t n = g.node_count();
  ShortestPaths r{.source = source,
                  .delay_s = std::vector<double>(n, kInf),
                  .hops = std::vector<std::size_t>(n, 0),
                  .predecessor = std::vector<std::size_t>(n, kNoNode)};
  std::vector<bool> done(n, false);
  r.delay_s[source] = 0.0;
  for (std::size_t round = 0; round < n; ++round) {
    std::size_t best = kNoNode;
    for (std::size_t v = 0; v < n; ++v) {
      if (done[v] || std::isinf(r.delay_s[v])) {
        continue;
      }
      // (delay, hops, node) lexicographically; v > best always, so a tie keeps best.
      if (best == kNoNode || r.delay_s[v] < r.delay_s[best] ||
          (r.delay_s[v] == r.delay_s[best] && r.hops[v] < r.hops[best])) {
        best = v;
      }
    }
    if (best == kNoNode) {
      break;
    }
    done[best] = true;
    for (const auto& e : g.edges_from(best)) {
      if (done[e.to]) {
        continue;
      }
      const double d = r.delay_s[best] + e.delay_s;
      const std::size_t h = r.hops[best] + 1;
      if (std::tie(d, h, best) < std::tie(r.delay_s[e.to], r.hops[e.to], r.predecessor[e.to])) {
        r.delay_s[e.to] = d;
        r.hops[e.to] = h;
        r.predecessor[e.to] = best;
      }
    }
  }
  return r;
}

// Random sparse graphs, once with small integer delays (many exact ties) and once with real
// delays, every fourth node as the source. The delays are also checked to be the true minimum:
// no edge can improve any label (the Bellman optimality condition). The per-node checks are
// counted and asserted once per source: thousands of separate assertions take minutes under
// GCC's ASan.
TEST(ShortestPathTest, RandomGraphsAgreeWithASimpleReference) {
  constexpr std::size_t kNodes = 100;
  constexpr std::size_t kEdges = 270;
  std::mt19937_64 random(20261119);
  for (const bool integer_delays : {true, false}) {
    Graph g(kNodes);
    std::uniform_int_distribution<std::size_t> node(0, kNodes - 1);
    std::uniform_int_distribution<int> small(0, 3);
    std::uniform_real_distribution<double> real(0.0, 0.05);
    for (std::size_t e = 0; e < kEdges; ++e) {
      const double delay = integer_delays ? small(random) : real(random);
      if (e % 4 == 0) {
        add_directed(g, node(random), node(random), delay);
      } else {
        add(g, node(random), node(random), delay);
      }
    }
    std::size_t reached = 0;
    for (std::size_t source = 0; source < kNodes; source += 4) {
      const ShortestPaths p = shortest_paths(g, source);
      const ShortestPaths r = reference(g, source);
      ASSERT_EQ(p.delay_s, r.delay_s) << "source " << source;
      ASSERT_EQ(p.hops, r.hops) << "source " << source;
      ASSERT_EQ(p.predecessor, r.predecessor) << "source " << source;
      std::size_t improvable = 0;
      std::size_t bad_paths = 0;
      for (std::size_t u = 0; u < kNodes; ++u) {
        if (!p.reachable(u)) {
          continue;
        }
        ++reached;
        for (const auto& e : g.edges_from(u)) {
          improvable += p.delay_s[e.to] > p.delay_s[u] + e.delay_s ? 1U : 0U;
        }
        const Path path = p.path_to(u);
        const bool good =
            path.size() == p.hops[u] + 1 && path.front() == source && path.back() == u;
        bad_paths += good ? 0U : 1U;
      }
      ASSERT_EQ(improvable, 0U) << "source " << source;
      ASSERT_EQ(bad_paths, 0U) << "source " << source;
    }
    EXPECT_GT(reached, kNodes);  // The graphs are connected enough to test something.
  }
}

}  // namespace
