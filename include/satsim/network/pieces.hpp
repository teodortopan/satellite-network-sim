#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <utility>
#include <vector>

namespace satsim::network {

// The pieces a graph falls into (Phase 4 spec 4.7, 6.3: reachability and the largest piece).
// Counted by union-find, not by a shortest-path search, so that the count is independent of
// Dijkstra.

/// Marks "in no piece" in Pieces::piece: a node that is not present.
inline constexpr std::size_t kNoPiece = std::numeric_limits<std::size_t>::max();

/// One undirected edge, by its two ends.
using NodePair = std::pair<std::size_t, std::size_t>;

struct Pieces {
  /// Per node: its piece number; kNoPiece for a node that is not present.
  std::vector<std::size_t> piece;
  /// Per piece: how many nodes it has (at least 1).
  std::vector<std::size_t> size;

  [[nodiscard]] std::size_t count() const noexcept { return size.size(); }
};

/// The connected pieces of the undirected graph on nodes 0 .. node_count - 1 with the given
/// edges, counting only present nodes.
///
/// - `present` is empty (every node is present) or has node_count entries, nonzero for a node
///   that is present. A node at or beyond present.size() when it is not empty counts as not
///   present.
/// - A node that is not present is in no piece, and every edge touching it is ignored. An
///   edge with an end not below node_count is ignored. Self-loops and repeated edges change
///   nothing.
/// - A present node with no edge is a piece of its own.
/// - Pieces are numbered 0, 1, ... in the order of their lowest node, so the result depends
///   only on which nodes are joined, not on the order of the edges.
[[nodiscard]] Pieces connected_pieces(std::size_t node_count,
                                      std::span<const NodePair> undirected_edges,
                                      std::span<const std::uint8_t> present = {});

}  // namespace satsim::network
