#include "satsim/network/many_sources.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <limits>
#include <vector>

#include "satsim/network/graph.hpp"
#include "satsim/network/shortest_path.hpp"

namespace {

using satsim::network::Edge;
using satsim::network::Graph;
using satsim::network::kNoNode;
using satsim::network::ManySourcePaths;
using satsim::network::shortest_paths;
using satsim::network::shortest_paths_from_many;
using satsim::network::ShortestPaths;
using satsim::network::Start;
using Path = std::vector<std::size_t>;

constexpr double kInf = std::numeric_limits<double>::infinity();

void add(Graph& g, std::size_t a, std::size_t b, double delay_s) {
  ASSERT_FALSE(g.add_undirected_edge(a, b, delay_s).has_value());
}

void add_directed(Graph& g, std::size_t a, std::size_t b, double delay_s) {
  ASSERT_FALSE(g.add_edge(a, b, delay_s).has_value());
}

// Every delay here is a binary fraction, so every sum is exact and every tie is real.

// A line 0 - 1 - 2 - 3 - 4, every edge 1 s, plus 0.25 s per edge: each edge costs 1.25.
// Starts: 0 at 0 s and 4 at 0.5 s.
//   from 0: 0, 1.25, 2.5, 3.75, 5     from 4: 5.5, 4.25, 3, 1.75, 0.5
// So 0, 1 and 2 descend from 0 (2: 2.5 < 3), 3 and 4 from 4 (3: 1.75 < 3.75).
TEST(ManySourcesTest, HandWorkedLineWithExtraPerEdge) {
  Graph g(5);
  for (std::size_t i = 0; i + 1 < 5; ++i) {
    add(g, i, i + 1, 1.0);
  }
  const std::vector<Start> starts{{.node = 0, .delay_s = 0.0}, {.node = 4, .delay_s = 0.5}};
  const ManySourcePaths p = shortest_paths_from_many(g, starts, 0.25);
  EXPECT_EQ(p.delay_s, (std::vector<double>{0.0, 1.25, 2.5, 1.75, 0.5}));
  EXPECT_EQ(p.hops, (std::vector<std::size_t>{0, 1, 2, 1, 0}));
  EXPECT_EQ(p.predecessor, (std::vector<std::size_t>{kNoNode, 0, 1, 4, kNoNode}));
  EXPECT_EQ(p.start, (std::vector<std::size_t>{0, 0, 0, 4, 4}));
  EXPECT_EQ(p.path_to(2), (Path{0, 1, 2}));
  EXPECT_EQ(p.path_to(3), (Path{4, 3}));
  EXPECT_EQ(p.path_to(4), (Path{4}));
  for (std::size_t v = 0; v < 5; ++v) {
    EXPECT_TRUE(p.reachable(v));
  }
}

// Without the extra the same line gives 0, 1, 2 from 0 and 1.5, 0.5 from 4; node 2 is 2 from
// 0 and 2.5 from 4.
TEST(ManySourcesTest, NoExtraByDefault) {
  Graph g(5);
  for (std::size_t i = 0; i + 1 < 5; ++i) {
    add(g, i, i + 1, 1.0);
  }
  const std::vector<Start> starts{{.node = 0, .delay_s = 0.0}, {.node = 4, .delay_s = 0.5}};
  const ManySourcePaths p = shortest_paths_from_many(g, starts);
  EXPECT_EQ(p.delay_s, (std::vector<double>{0.0, 1.0, 2.0, 1.5, 0.5}));
  EXPECT_EQ(p.start, (std::vector<std::size_t>{0, 0, 0, 4, 4}));
}

// 0 -1- 1 -1- 2, starts 0 at 0 s and 2 at 5 s: 2 is reached from 0 at 2 s, before its own
// start, so it descends from 0 with 2 hops.
TEST(ManySourcesTest, AStartLaterThanAPathIsReached) {
  Graph g(3);
  add(g, 0, 1, 1.0);
  add(g, 1, 2, 1.0);
  const std::vector<Start> starts{{.node = 0, .delay_s = 0.0}, {.node = 2, .delay_s = 5.0}};
  const ManySourcePaths p = shortest_paths_from_many(g, starts);
  EXPECT_EQ(p.delay_s, (std::vector<double>{0.0, 1.0, 2.0}));
  EXPECT_EQ(p.hops[2], 2U);
  EXPECT_EQ(p.predecessor[2], 1U);
  EXPECT_EQ(p.start[2], 0U);
  EXPECT_EQ(p.path_to(2), (Path{0, 1, 2}));
}

// The same line with 2 starting at exactly 2 s, the total it is reached at: it stays a start
// (0 hops beat 2). Node 1 is 1 from 0 and 3 from 2: from 0.
TEST(ManySourcesTest, AStartAtTheReachedTotalStaysAStart) {
  Graph g(3);
  add(g, 0, 1, 1.0);
  add(g, 1, 2, 1.0);
  const std::vector<Start> starts{{.node = 2, .delay_s = 2.0}, {.node = 0, .delay_s = 0.0}};
  const ManySourcePaths p = shortest_paths_from_many(g, starts);
  EXPECT_EQ(p.delay_s, (std::vector<double>{0.0, 1.0, 2.0}));
  EXPECT_EQ(p.hops, (std::vector<std::size_t>{0, 1, 0}));
  EXPECT_EQ(p.predecessor, (std::vector<std::size_t>{kNoNode, 0, kNoNode}));
  EXPECT_EQ(p.start, (std::vector<std::size_t>{0, 0, 2}));
  EXPECT_EQ(p.path_to(2), (Path{2}));
}

// Two starts reach node 1 at the same total and hops: the lower predecessor decides, whatever
// the order of the starts or the edges.
TEST(ManySourcesTest, EqualTotalAndHopsGoToTheLowerPredecessor) {
  for (const bool reversed : {false, true}) {
    Graph g(4);
    if (reversed) {
      add(g, 3, 1, 0.5);
      add(g, 2, 1, 0.5);
    } else {
      add(g, 2, 1, 0.5);
      add(g, 3, 1, 0.5);
    }
    std::vector<Start> starts{{.node = 3, .delay_s = 0.25}, {.node = 2, .delay_s = 0.25}};
    if (reversed) {
      std::reverse(starts.begin(), starts.end());
    }
    const ManySourcePaths p = shortest_paths_from_many(g, starts);
    EXPECT_EQ(p.delay_s[1], 0.75);
    EXPECT_EQ(p.hops[1], 1U);
    EXPECT_EQ(p.predecessor[1], 2U) << "reversed " << reversed;
    EXPECT_EQ(p.start[1], 2U) << "reversed " << reversed;
    EXPECT_FALSE(p.reachable(0));
  }
}

// Node 2 at 1 s: 0 -0.5- 1 -0.5- 2 (two hops from 0) or 3 -0.5- 2 (one hop from 3, which
// starts at 0.5 s). Fewest hops: from 3, although predecessor 1 is lower than 3.
TEST(ManySourcesTest, EqualTotalsGoToFewestHopsAcrossStarts) {
  Graph g(4);
  add(g, 0, 1, 0.5);
  add(g, 1, 2, 0.5);
  add(g, 3, 2, 0.5);
  const std::vector<Start> starts{{.node = 0, .delay_s = 0.0}, {.node = 3, .delay_s = 0.5}};
  const ManySourcePaths p = shortest_paths_from_many(g, starts);
  EXPECT_EQ(p.delay_s[2], 1.0);
  EXPECT_EQ(p.hops[2], 1U);
  EXPECT_EQ(p.predecessor[2], 3U);
  EXPECT_EQ(p.start[2], 3U);
  EXPECT_EQ(p.path_to(2), (Path{3, 2}));
}

// Fewest hops wins even when the path with fewer hops is found later: node 3 at 1 s through
// 0 -0.125- 1 -0.125- 2 -0.75- 3 (three hops; 2 is settled at 0.25 s) or through 4 -0.5- 3
// (4 is settled at 0.5 s, as a start at 0.5 s or reached by 0 -0.5- 4).
TEST(ManySourcesTest, FewestHopsWinEvenWhenFoundLater) {
  for (const bool four_starts : {true, false}) {
    Graph g(5);
    add(g, 0, 1, 0.125);
    add(g, 1, 2, 0.125);
    add(g, 2, 3, 0.75);
    add(g, 4, 3, 0.5);
    std::vector<Start> starts{{.node = 0, .delay_s = 0.0}};
    if (four_starts) {
      starts.push_back({.node = 4, .delay_s = 0.5});
    } else {
      add(g, 0, 4, 0.5);
    }
    const ManySourcePaths p = shortest_paths_from_many(g, starts);
    EXPECT_EQ(p.delay_s[3], 1.0);
    EXPECT_EQ(p.predecessor[3], 4U) << "4 starts " << four_starts;
    EXPECT_EQ(p.hops[3], four_starts ? 1U : 2U);
    EXPECT_EQ(p.start[3], four_starts ? 4U : 0U);
  }
}

TEST(ManySourcesTest, RepeatedStartKeepsItsLowestDelay) {
  Graph g(2);
  add(g, 0, 1, 1.0);
  const std::vector<Start> starts{
      {.node = 1, .delay_s = 3.0}, {.node = 1, .delay_s = 0.5}, {.node = 1, .delay_s = 2.0}};
  const ManySourcePaths p = shortest_paths_from_many(g, starts);
  EXPECT_EQ(p.delay_s, (std::vector<double>{1.5, 0.5}));
  EXPECT_EQ(p.start, (std::vector<std::size_t>{1, 1}));
  EXPECT_EQ(p.predecessor, (std::vector<std::size_t>{1, kNoNode}));
}

TEST(ManySourcesTest, NoStartsReachNothing) {
  Graph g(3);
  add(g, 0, 1, 1.0);
  const ManySourcePaths p = shortest_paths_from_many(g, {});
  EXPECT_EQ(p.delay_s, (std::vector<double>{kInf, kInf, kInf}));
  for (std::size_t v = 0; v < 3; ++v) {
    EXPECT_FALSE(p.reachable(v));
    EXPECT_EQ(p.start[v], kNoNode);
    EXPECT_EQ(p.predecessor[v], kNoNode);
    EXPECT_EQ(p.hops[v], 0U);
    EXPECT_TRUE(p.path_to(v).empty());
  }
}

// Edges go one way: from start 0, 1 -> 0 does not reach 1; 0 -> 2 reaches 2.
TEST(ManySourcesTest, DirectedEdgesGoOneWay) {
  Graph g(3);
  add_directed(g, 1, 0, 1.0);
  add_directed(g, 0, 2, 1.0);
  const std::vector<Start> starts{{.node = 0, .delay_s = 0.0}};
  const ManySourcePaths p = shortest_paths_from_many(g, starts);
  EXPECT_FALSE(p.reachable(1));
  EXPECT_EQ(p.delay_s[1], kInf);
  EXPECT_TRUE(p.reachable(2));
  EXPECT_EQ(p.delay_s[2], 1.0);
}

// One start at 0 s and no extra: exactly shortest_paths, bit for bit, also with delays that
// are not binary fractions.
TEST(ManySourcesTest, OneStartAtZeroIsShortestPaths) {
  Graph g(6);
  add(g, 0, 1, 0.1);
  add(g, 0, 2, 0.3);
  add(g, 1, 2, 0.2);
  add(g, 1, 3, 0.7);
  add(g, 2, 3, 0.4);
  add(g, 3, 4, 0.05);
  add_directed(g, 4, 5, 0.01);
  for (std::size_t source = 0; source < 6; ++source) {
    const std::vector<Start> starts{{.node = source, .delay_s = 0.0}};
    const ManySourcePaths many = shortest_paths_from_many(g, starts);
    const ShortestPaths one = shortest_paths(g, source);
    EXPECT_EQ(many.delay_s, one.delay_s) << "source " << source;
    EXPECT_EQ(many.hops, one.hops) << "source " << source;
    EXPECT_EQ(many.predecessor, one.predecessor) << "source " << source;
    for (std::size_t v = 0; v < 6; ++v) {
      EXPECT_EQ(many.reachable(v), one.reachable(v));
      EXPECT_EQ(many.path_to(v), one.path_to(v));
      EXPECT_EQ(many.start[v], one.reachable(v) ? source : kNoNode);
    }
  }
}

// The graph with every edge carrying delay + extra.
Graph with_extra(const Graph& g, double extra_s) {
  Graph out(g.node_count());
  for (std::size_t u = 0; u < g.node_count(); ++u) {
    for (const Edge& edge : g.edges_from(u)) {
      EXPECT_FALSE(out.add_edge(u, edge.to, edge.delay_s + extra_s).has_value());
    }
  }
  return out;
}

struct Case {
  Graph graph;
  std::vector<Start> starts;
  double extra_s = 0.0;
};

std::vector<Case> property_cases() {
  std::vector<Case> cases;
  {
    // A ring of 8 with a chord, three starts at different delays.
    Graph g(8);
    for (std::size_t i = 0; i < 8; ++i) {
      add(g, i, (i + 1) % 8, 0.5 + 0.125 * static_cast<double>(i));
    }
    add(g, 1, 5, 0.25);
    cases.push_back({.graph = g,
                     .starts = {{.node = 0, .delay_s = 1.0},
                                {.node = 3, .delay_s = 0.0},
                                {.node = 6, .delay_s = 2.5}},
                     .extra_s = 0.125});
  }
  {
    // A 3 x 3 grid with unequal delays, one directed short cut, and an isolated node 9.
    Graph g(10);
    for (std::size_t r = 0; r < 3; ++r) {
      for (std::size_t c = 0; c < 3; ++c) {
        const std::size_t v = 3 * r + c;
        if (c + 1 < 3) {
          add(g, v, v + 1, 0.25 * static_cast<double>(r + 1));
        }
        if (r + 1 < 3) {
          add(g, v, v + 3, 0.5 * static_cast<double>(c + 1));
        }
      }
    }
    add_directed(g, 8, 0, 0.0625);
    cases.push_back({.graph = g,
                     .starts = {{.node = 4, .delay_s = 0.75}, {.node = 8, .delay_s = 0.5}},
                     .extra_s = 0.0625});
    cases.push_back({.graph = g, .starts = {{.node = 2, .delay_s = 0.0}}, .extra_s = 1.0});
  }
  {
    // Two pieces, a start in each, zero-delay edges and a start inside a piece's middle.
    Graph g(7);
    add(g, 0, 1, 0.0);
    add(g, 1, 2, 0.75);
    add(g, 2, 3, 0.25);
    add(g, 0, 3, 2.0);
    add(g, 4, 5, 0.5);
    add_directed(g, 5, 6, 0.5);
    cases.push_back({.graph = g,
                     .starts = {{.node = 2, .delay_s = 0.25},
                                {.node = 0, .delay_s = 0.5},
                                {.node = 6, .delay_s = 0.0}},
                     .extra_s = 0.0});
    cases.push_back({.graph = g,
                     .starts = {{.node = 1, .delay_s = 0.0}, {.node = 5, .delay_s = 0.125}},
                     .extra_s = 0.5});
  }
  return cases;
}

// Spec 6.5: the total at every node is the lowest, over the starts, of the start's delay plus
// the single-source delay on the graph whose edges carry delay + extra. The delays are binary
// fractions, so the two ways of summing agree exactly.
TEST(ManySourcesTest, EqualsTheLowestOfTheSingleSourceResults) {
  const std::vector<Case> cases = property_cases();
  for (std::size_t k = 0; k < cases.size(); ++k) {
    const Case& c = cases[k];
    const std::size_t n = c.graph.node_count();
    const ManySourcePaths many = shortest_paths_from_many(c.graph, c.starts, c.extra_s);
    const Graph extra = with_extra(c.graph, c.extra_s);
    std::vector<double> lowest_s(n, kInf);
    for (const Start& s : c.starts) {
      const ShortestPaths one = shortest_paths(extra, s.node);
      for (std::size_t v = 0; v < n; ++v) {
        lowest_s[v] = std::min(lowest_s[v], s.delay_s + one.delay_s[v]);
      }
    }
    EXPECT_EQ(many.delay_s, lowest_s) << "case " << k;

    // And the chosen path is consistent: it starts at the node's start, has hops edges, and
    // each node's total is its predecessor's plus the cheapest edge between them.
    for (std::size_t v = 0; v < n; ++v) {
      const Path path = many.path_to(v);
      if (!many.reachable(v)) {
        EXPECT_TRUE(path.empty());
        continue;
      }
      ASSERT_FALSE(path.empty());
      EXPECT_EQ(path.front(), many.start[v]) << "case " << k << " node " << v;
      EXPECT_EQ(path.size(), many.hops[v] + 1) << "case " << k << " node " << v;
      const std::size_t u = many.predecessor[v];
      if (u != kNoNode) {
        double cheapest_s = kInf;
        for (const Edge& edge : c.graph.edges_from(u)) {
          if (edge.to == v) {
            cheapest_s = std::min(cheapest_s, edge.delay_s + c.extra_s);
          }
        }
        EXPECT_EQ(many.delay_s[v], many.delay_s[u] + cheapest_s) << "case " << k << " node " << v;
      }
    }
  }
}

}  // namespace
