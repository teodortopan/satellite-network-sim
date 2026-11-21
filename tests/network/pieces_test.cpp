#include "satsim/network/pieces.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace {

using satsim::network::connected_pieces;
using satsim::network::kNoPiece;
using satsim::network::NodePair;
using satsim::network::Pieces;
using Ids = std::vector<std::size_t>;

// 0 - 1 - 2   3   4 - 5: pieces {0, 1, 2}, {3} and {4, 5}, numbered by their lowest node.
TEST(PiecesTest, HandMadeGraphWithAnIsolatedNode) {
  const std::vector<NodePair> edges{{0, 1}, {1, 2}, {4, 5}};
  const Pieces p = connected_pieces(6, edges);
  EXPECT_EQ(p.piece, (Ids{0, 0, 0, 1, 2, 2}));
  EXPECT_EQ(p.size, (Ids{3, 1, 2}));
  EXPECT_EQ(p.count(), 3U);
}

// The numbering follows the lowest node, not the order of the edges or of their ends:
// {0, 5}, {1}, {2}, {3, 4}.
TEST(PiecesTest, NumberedByLowestNodeWhateverTheEdgeOrder) {
  for (const bool reversed : {false, true}) {
    std::vector<NodePair> edges{{4, 3}, {5, 0}};
    if (reversed) {
      std::reverse(edges.begin(), edges.end());
    }
    const Pieces p = connected_pieces(6, edges);
    EXPECT_EQ(p.piece, (Ids{0, 1, 2, 3, 3, 0})) << "reversed " << reversed;
    EXPECT_EQ(p.size, (Ids{2, 1, 1, 2})) << "reversed " << reversed;
  }
}

// Two pieces joined late through their highest nodes: 2 - 3 and 0 - 1 first, then 1 - 3, and
// a long chain 9 - 8 - ... - 4 that joins 0 at its end: everything is one piece.
TEST(PiecesTest, JoinsChainsInAnyOrder) {
  std::vector<NodePair> edges{{2, 3}, {0, 1}, {1, 3}};
  for (std::size_t v = 9; v > 4; --v) {
    edges.emplace_back(v, v - 1);
  }
  edges.emplace_back(4, 0);
  const Pieces p = connected_pieces(10, edges);
  EXPECT_EQ(p.piece, Ids(10, 0));
  EXPECT_EQ(p.size, (Ids{10}));
}

// Node 1 is not present: the chain 0 - 1 - 2 falls in two, and node 1 is in no piece.
TEST(PiecesTest, AbsentNodesAndTheirEdgesAreLeftOut) {
  const std::vector<NodePair> edges{{0, 1}, {1, 2}, {4, 5}, {3, 1}};
  const std::vector<std::uint8_t> present{1, 0, 1, 1, 1, 1};
  const Pieces p = connected_pieces(6, edges, present);
  EXPECT_EQ(p.piece, (Ids{0, kNoPiece, 1, 2, 3, 3}));
  EXPECT_EQ(p.size, (Ids{1, 1, 1, 2}));
  EXPECT_EQ(p.count(), 4U);
}

// A `present` shorter than node_count leaves the nodes beyond it out.
TEST(PiecesTest, NodesBeyondPresentAreAbsent) {
  const std::vector<NodePair> edges{{0, 1}, {1, 2}};
  const std::vector<std::uint8_t> present{1, 1};
  const Pieces p = connected_pieces(3, edges, present);
  EXPECT_EQ(p.piece, (Ids{0, 0, kNoPiece}));
  EXPECT_EQ(p.size, (Ids{2}));
}

// Out-of-range ends, self-loops and repeated edges change nothing.
TEST(PiecesTest, OutOfRangeSelfLoopsAndRepeatsAreIgnored) {
  const std::vector<NodePair> edges{{0, 9}, {7, 2}, {1, 1}, {2, 3}, {3, 2}, {2, 3}};
  const Pieces p = connected_pieces(4, edges);
  EXPECT_EQ(p.piece, (Ids{0, 1, 2, 2}));
  EXPECT_EQ(p.size, (Ids{1, 1, 2}));
}

TEST(PiecesTest, EmptyAndNothingPresent) {
  const Pieces none = connected_pieces(0, {});
  EXPECT_TRUE(none.piece.empty());
  EXPECT_EQ(none.count(), 0U);

  const std::vector<NodePair> edges{{0, 1}};
  const std::vector<std::uint8_t> present{0, 0};
  const Pieces absent = connected_pieces(2, edges, present);
  EXPECT_EQ(absent.piece, (Ids{kNoPiece, kNoPiece}));
  EXPECT_EQ(absent.count(), 0U);

  const Pieces isolated = connected_pieces(3, {});
  EXPECT_EQ(isolated.piece, (Ids{0, 1, 2}));
  EXPECT_EQ(isolated.size, (Ids{1, 1, 1}));
}

}  // namespace
