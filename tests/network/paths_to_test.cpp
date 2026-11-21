#include "satsim/network/paths_to.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <limits>
#include <vector>

#include "satsim/network/graph.hpp"
#include "satsim/network/path.hpp"
#include "satsim/network/shortest_path.hpp"

namespace {

using satsim::network::Graph;
using satsim::network::kNoNode;
using satsim::network::path_is_up;
using satsim::network::PathsTo;
using satsim::network::shortest_paths;
using satsim::network::shortest_paths_to;
using Path = std::vector<std::size_t>;

constexpr double kInf = std::numeric_limits<double>::infinity();

void add(Graph& g, std::size_t a, std::size_t b, double delay_s) {
  ASSERT_FALSE(g.add_undirected_edge(a, b, delay_s).has_value());
}

void add_directed(Graph& g, std::size_t a, std::size_t b, double delay_s) {
  ASSERT_FALSE(g.add_edge(a, b, delay_s).has_value());
}

// Every delay here is a binary fraction, so every sum is exact and every tie is real.

// The hand-worked graph of shortest_path_test (undirected), towards 4:
//   0 -1- 1 -2- 3        to 4: 3:1 (3-4), 2:2 (2-4), 1:3 (1-3-4, not 1-2-4: 4),
//   |      \    |              0:4 (0-1-3-4, not 0-2-4: 6). 5 is unconnected.
//   4      2    1
//   (0-2)   \   |
//            2 -2- 4
Graph hand_worked() {
  Graph g(6);
  add(g, 0, 1, 1.0);
  add(g, 0, 2, 4.0);
  add(g, 1, 2, 2.0);
  add(g, 1, 3, 2.0);
  add(g, 3, 4, 1.0);
  add(g, 2, 4, 2.0);
  return g;
}

TEST(PathsToTest, HandWorkedGraph) {
  const PathsTo p = shortest_paths_to(hand_worked(), 4);
  EXPECT_EQ(p.target, 4U);
  EXPECT_EQ(p.delay_s, (std::vector<double>{4.0, 3.0, 2.0, 1.0, 0.0, kInf}));
  EXPECT_EQ(p.hops, (std::vector<std::size_t>{3, 2, 1, 1, 0, 0}));
  EXPECT_EQ(p.next, (std::vector<std::size_t>{1, 3, 4, 4, kNoNode, kNoNode}));
  EXPECT_EQ(p.path_from(0), (Path{0, 1, 3, 4}));
  EXPECT_EQ(p.path_from(4), (Path{4}));
  EXPECT_TRUE(p.reachable(4));
  EXPECT_FALSE(p.reachable(5));
  EXPECT_TRUE(p.path_from(5).empty());
}

// Directed edges are followed forwards only:
//   0 -> 1 (1), 1 -> 2 (1), 0 -> 2 (5), 2 -> 0 (1), 2 -> 3 (1), 4 -> 3 (1).
// Towards 2: 0 goes 0 -> 1 -> 2 (2, not 5), 1 goes straight, 3 has no edge out and 4 only
// reaches 3, so neither reaches 2.
TEST(PathsToTest, DirectedEdgesAreFollowedForwards) {
  Graph g(5);
  add_directed(g, 0, 1, 1.0);
  add_directed(g, 1, 2, 1.0);
  add_directed(g, 0, 2, 5.0);
  add_directed(g, 2, 0, 1.0);
  add_directed(g, 2, 3, 1.0);
  add_directed(g, 4, 3, 1.0);
  const PathsTo to2 = shortest_paths_to(g, 2);
  EXPECT_EQ(to2.delay_s, (std::vector<double>{2.0, 1.0, 0.0, kInf, kInf}));
  EXPECT_EQ(to2.next, (std::vector<std::size_t>{1, 2, kNoNode, kNoNode, kNoNode}));
  EXPECT_EQ(to2.hops, (std::vector<std::size_t>{2, 1, 0, 0, 0}));
  // Towards 3: 2 -> 3 (1); 1 -> 2 -> 3 (2); 0 -> 1 -> 2 -> 3 (3); 4 -> 3 (1).
  const PathsTo to3 = shortest_paths_to(g, 3);
  EXPECT_EQ(to3.delay_s, (std::vector<double>{3.0, 2.0, 1.0, 0.0, 1.0}));
  EXPECT_EQ(to3.next, (std::vector<std::size_t>{1, 2, 3, kNoNode, 3}));
  // Towards 4: nothing has an edge into 4.
  const PathsTo to4 = shortest_paths_to(g, 4);
  for (std::size_t v = 0; v < 4; ++v) {
    EXPECT_FALSE(to4.reachable(v));
    EXPECT_EQ(to4.delay_s[v], kInf);
  }
}

// Ties, from the target's side.
TEST(PathsToTest, EqualDelaysGoToFewestHopsThenLowestNext) {
  // Diamond 0 -> {2, 1} -> 3, both 0.5 + 0.25: next 1, the lower, whatever the order of
  // insertion.
  for (const bool reversed : {false, true}) {
    Graph g(4);
    if (reversed) {
      add_directed(g, 2, 3, 0.25);
      add_directed(g, 1, 3, 0.25);
      add_directed(g, 0, 2, 0.5);
      add_directed(g, 0, 1, 0.5);
    } else {
      add_directed(g, 0, 1, 0.5);
      add_directed(g, 0, 2, 0.5);
      add_directed(g, 1, 3, 0.25);
      add_directed(g, 2, 3, 0.25);
    }
    const PathsTo p = shortest_paths_to(g, 3);
    EXPECT_EQ(p.delay_s[0], 0.75);
    EXPECT_EQ(p.next[0], 1U) << "reversed " << reversed;
    EXPECT_EQ(p.path_from(0), (Path{0, 1, 3})) << "reversed " << reversed;
  }
  {
    // Towards 4, from 0: 0 -> 1 -> 2 -> 4 (0.25 × 3) or 0 -> 3 -> 4 (0.5 + 0.25), both 0.75:
    // fewest hops, so next 3 although 1 is lower.
    Graph g(5);
    add(g, 0, 1, 0.25);
    add(g, 1, 2, 0.25);
    add(g, 2, 4, 0.25);
    add(g, 0, 3, 0.5);
    add(g, 3, 4, 0.25);
    const PathsTo p = shortest_paths_to(g, 4);
    EXPECT_EQ(p.delay_s[0], 0.75);
    EXPECT_EQ(p.hops[0], 2U);
    EXPECT_EQ(p.next[0], 3U);
    EXPECT_EQ(p.path_from(0), (Path{0, 3, 4}));
  }
  {
    // Zero-delay edges: 0 -0- 1 -0- 2 and 0 -0- 2, towards 2: 0 goes straight.
    Graph g(3);
    add(g, 0, 1, 0.0);
    add(g, 1, 2, 0.0);
    add(g, 0, 2, 0.0);
    const PathsTo p = shortest_paths_to(g, 2);
    EXPECT_EQ(p.delay_s, (std::vector<double>{0.0, 0.0, 0.0}));
    EXPECT_EQ(p.path_from(0), (Path{0, 2}));
  }
}

TEST(PathsToTest, SingleNode) {
  const PathsTo p = shortest_paths_to(Graph(1), 0);
  EXPECT_EQ(p.delay_s, (std::vector<double>{0.0}));
  EXPECT_EQ(p.next, (std::vector<std::size_t>{kNoNode}));
  EXPECT_EQ(p.path_from(0), (Path{0}));
}

std::vector<Graph> test_graphs() {
  std::vector<Graph> graphs;
  graphs.push_back(hand_worked());
  {
    // A ring of 7 with mixed directed and undirected edges and a chord.
    Graph g(7);
    for (std::size_t i = 0; i < 7; ++i) {
      const double delay_s = 0.25 + 0.125 * static_cast<double>(i % 3);
      if (i % 2 == 0) {
        add(g, i, (i + 1) % 7, delay_s);
      } else {
        add_directed(g, i, (i + 1) % 7, delay_s);
      }
    }
    add_directed(g, 5, 2, 0.0625);
    graphs.push_back(g);
  }
  {
    // A 3 x 3 grid with equal delays (many ties) and an isolated node 9.
    Graph g(10);
    for (std::size_t r = 0; r < 3; ++r) {
      for (std::size_t c = 0; c < 3; ++c) {
        const std::size_t v = 3 * r + c;
        if (c + 1 < 3) {
          add(g, v, v + 1, 0.5);
        }
        if (r + 1 < 3) {
          add(g, v, v + 3, 0.5);
        }
      }
    }
    graphs.push_back(g);
  }
  return graphs;
}

// Every node's delay is the single-source result from that node; following next from any
// node reaches the target in hops steps over edges of the graph, with no loop.
TEST(PathsToTest, AgreesWithShortestPathsFromEachNodeAndHasNoLoop) {
  const std::vector<Graph> graphs = test_graphs();
  for (std::size_t k = 0; k < graphs.size(); ++k) {
    const Graph& g = graphs[k];
    const std::size_t n = g.node_count();
    for (std::size_t target = 0; target < n; ++target) {
      const PathsTo to = shortest_paths_to(g, target);
      for (std::size_t v = 0; v < n; ++v) {
        const auto from_v = shortest_paths(g, v);
        EXPECT_EQ(to.delay_s[v], from_v.delay_s[target])
            << "graph " << k << " node " << v << " target " << target;
        EXPECT_EQ(to.reachable(v), from_v.reachable(target));
        EXPECT_EQ(to.hops[v], from_v.reachable(target) ? from_v.hops[target] : 0U);
        if (!to.reachable(v)) {
          EXPECT_EQ(to.next[v], kNoNode);
          continue;
        }
        // Following next: hops steps, each to a node one hop nearer, ending at the target.
        std::size_t node = v;
        for (std::size_t step = 0; step < to.hops[v]; ++step) {
          ASSERT_NE(to.next[node], kNoNode);
          EXPECT_EQ(to.hops[to.next[node]] + 1, to.hops[node]);
          node = to.next[node];
        }
        EXPECT_EQ(node, target);
        const Path path = to.path_from(v);
        EXPECT_EQ(path.size(), to.hops[v] + 1);
        EXPECT_EQ(path.front(), v);
        EXPECT_EQ(path.back(), target);
        EXPECT_TRUE(path_is_up(g, path));
      }
    }
  }
}

}  // namespace
