#include "satsim/network/graph.hpp"

#include <gtest/gtest.h>

#include <limits>
#include <optional>
#include <vector>

namespace {

using satsim::network::Edge;
using satsim::network::EdgeError;
using satsim::network::Graph;

TEST(GraphTest, EdgesAreStoredAtTheirTailInOrder) {
  Graph g(3);
  EXPECT_EQ(g.node_count(), 3U);
  EXPECT_FALSE(g.add_edge(0, 1, 2.0).has_value());
  EXPECT_FALSE(g.add_undirected_edge(1, 2, 0.5).has_value());
  EXPECT_FALSE(g.add_edge(0, 2, 0.0).has_value());  // Zero delay is allowed.
  EXPECT_EQ(g.edge_count(), 4U);
  const auto from0 = g.edges_from(0);
  EXPECT_EQ(std::vector<Edge>(from0.begin(), from0.end()),
            (std::vector<Edge>{{.to = 1, .delay_s = 2.0}, {.to = 2, .delay_s = 0.0}}));
  const auto from1 = g.edges_from(1);
  EXPECT_EQ(std::vector<Edge>(from1.begin(), from1.end()),
            (std::vector<Edge>{{.to = 2, .delay_s = 0.5}}));
  const auto from2 = g.edges_from(2);
  EXPECT_EQ(std::vector<Edge>(from2.begin(), from2.end()),
            (std::vector<Edge>{{.to = 1, .delay_s = 0.5}}));

  g.clear_edges();
  EXPECT_EQ(g.node_count(), 3U);
  EXPECT_EQ(g.edge_count(), 0U);
  EXPECT_TRUE(g.edges_from(0).empty());
}

TEST(GraphTest, BadEdgesAreRefusedAndLeaveTheGraphUnchanged) {
  Graph g(2);
  EXPECT_EQ(g.add_edge(0, 2, 1.0), std::optional(EdgeError::kNodeOutOfRange));
  EXPECT_EQ(g.add_edge(5, 0, 1.0), std::optional(EdgeError::kNodeOutOfRange));
  EXPECT_EQ(g.add_edge(0, 1, -1e-300), std::optional(EdgeError::kNegativeDelay));
  EXPECT_EQ(g.add_edge(0, 1, std::numeric_limits<double>::quiet_NaN()),
            std::optional(EdgeError::kNonFiniteDelay));
  EXPECT_EQ(g.add_edge(0, 1, std::numeric_limits<double>::infinity()),
            std::optional(EdgeError::kNonFiniteDelay));
  EXPECT_EQ(g.add_undirected_edge(0, 1, -2.0), std::optional(EdgeError::kNegativeDelay));
  EXPECT_EQ(g.add_undirected_edge(1, 2, 2.0), std::optional(EdgeError::kNodeOutOfRange));
  EXPECT_EQ(g.edge_count(), 0U);
  EXPECT_TRUE(g.edges_from(0).empty());
  EXPECT_TRUE(g.edges_from(1).empty());
  EXPECT_EQ(satsim::network::name_of(EdgeError::kNegativeDelay), "negative_delay");
  EXPECT_EQ(satsim::network::name_of(EdgeError::kNonFiniteDelay), "non_finite_delay");
  EXPECT_EQ(satsim::network::name_of(EdgeError::kNodeOutOfRange), "node_out_of_range");
}

}  // namespace
