#include "satsim/network/path.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <optional>
#include <vector>

#include "satsim/network/graph.hpp"
#include "satsim/network/shortest_path.hpp"

namespace {

using satsim::network::Graph;
using satsim::network::path_delay_s;
using satsim::network::path_is_up;
using Path = std::vector<std::size_t>;

void add(Graph& g, std::size_t a, std::size_t b, double delay_s) {
  ASSERT_FALSE(g.add_undirected_edge(a, b, delay_s).has_value());
}

void add_directed(Graph& g, std::size_t a, std::size_t b, double delay_s) {
  ASSERT_FALSE(g.add_edge(a, b, delay_s).has_value());
}

// 0 -0.5- 1 -0.25- 2, and 2 -> 3 one way (0.125). Delays are binary fractions: sums exact.
Graph line() {
  Graph g(4);
  add(g, 0, 1, 0.5);
  add(g, 1, 2, 0.25);
  add_directed(g, 2, 3, 0.125);
  return g;
}

TEST(PathTest, AnUpPathHasTheSumOfItsEdges) {
  const Graph g = line();
  EXPECT_TRUE(path_is_up(g, Path{0, 1, 2, 3}));
  EXPECT_EQ(path_delay_s(g, Path{0, 1, 2, 3}), std::optional(0.875));
  EXPECT_EQ(path_delay_s(g, Path{2, 1, 0}), std::optional(0.75));  // Undirected: both ways.
}

TEST(PathTest, OneNodeIsUpWithNoDelayAndEmptyIsNotUp) {
  const Graph g = line();
  EXPECT_TRUE(path_is_up(g, Path{3}));
  EXPECT_EQ(path_delay_s(g, Path{3}), std::optional(0.0));
  EXPECT_FALSE(path_is_up(g, Path{}));
  EXPECT_EQ(path_delay_s(g, Path{}), std::nullopt);
}

TEST(PathTest, AMissingEdgeOrANodeOutOfRangeIsNotUp) {
  const Graph g = line();
  EXPECT_FALSE(path_is_up(g, Path{0, 2}));  // No edge 0 - 2.
  EXPECT_EQ(path_delay_s(g, Path{0, 2}), std::nullopt);
  EXPECT_FALSE(path_is_up(g, Path{0, 1, 2, 3, 0}));  // The last edge is missing.
  EXPECT_FALSE(path_is_up(g, Path{0, 4}));
  EXPECT_FALSE(path_is_up(g, Path{4}));
  EXPECT_FALSE(path_is_up(g, Path{1, 1}));  // No self-loop at 1.
}

TEST(PathTest, ADirectedEdgeUsedTheWrongWayIsNotUp) {
  const Graph g = line();
  EXPECT_TRUE(path_is_up(g, Path{2, 3}));
  EXPECT_FALSE(path_is_up(g, Path{3, 2}));
  EXPECT_EQ(path_delay_s(g, Path{3, 2, 1}), std::nullopt);
}

TEST(PathTest, ParallelEdgesCountWithTheSmallestDelay) {
  Graph g(3);
  add_directed(g, 0, 1, 2.0);
  add_directed(g, 0, 1, 0.5);
  add_directed(g, 0, 1, 1.0);
  add(g, 1, 2, 0.25);
  add(g, 1, 2, 0.125);
  add(g, 2, 2, 0.0);  // A self-loop makes [2, 2] up.
  EXPECT_EQ(path_delay_s(g, Path{0, 1, 2}), std::optional(0.625));
  EXPECT_EQ(path_delay_s(g, Path{2, 1}), std::optional(0.125));
  EXPECT_EQ(path_delay_s(g, Path{2, 2}), std::optional(0.0));
  EXPECT_FALSE(path_is_up(g, Path{1, 0}));
}

// The delay of the path shortest_paths returns equals its delay_s bit for bit, with delays
// that are not binary fractions (so the order of the sum matters).
TEST(PathTest, AgreesWithShortestPathsExactly) {
  Graph g(5);
  add(g, 0, 1, 0.1);
  add(g, 1, 2, 0.2);
  add(g, 2, 3, 0.3);
  add(g, 3, 4, 0.7);
  add(g, 0, 4, 2.0);
  add(g, 1, 2, 0.3);  // A parallel, longer edge.
  const auto p = satsim::network::shortest_paths(g, 0);
  for (std::size_t node = 0; node < 5; ++node) {
    EXPECT_EQ(path_delay_s(g, p.path_to(node)), std::optional(p.delay_s[node])) << node;
  }
}

}  // namespace
