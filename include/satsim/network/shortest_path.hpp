#pragma once

#include <cstddef>
#include <limits>
#include <optional>
#include <vector>

#include "satsim/network/graph.hpp"

namespace satsim::network {

/// Marks "no node" in ShortestPaths::predecessor.
inline constexpr std::size_t kNoNode = std::numeric_limits<std::size_t>::max();

/// Shortest delays from one source to every node, and the chosen path to each.
struct ShortestPaths {
  std::size_t source = 0;
  /// Per node: the smallest delay from the source, seconds; +infinity when unreachable. 0 at
  /// the source.
  std::vector<double> delay_s;
  /// Per node: edges on the chosen path; 0 at the source and for unreachable nodes.
  std::vector<std::size_t> hops;
  /// Per node: the node before it on the chosen path; kNoNode at the source and for
  /// unreachable nodes.
  std::vector<std::size_t> predecessor;

  [[nodiscard]] bool reachable(std::size_t node) const noexcept {
    return node == source || predecessor[node] != kNoNode;
  }

  /// The chosen path from the source to `node`, both included (just the source for the
  /// source itself); empty if `node` is unreachable.
  [[nodiscard]] std::vector<std::size_t> path_to(std::size_t node) const;
};

/// Dijkstra's algorithm from `source` (precondition: source < graph.node_count()).
///
/// Among paths of equal delay the choice is fixed, so the result never depends on hash order,
/// heap order or the order edges were added:
///   1. the smallest delay (exact double comparison of the sums along the path, each sum
///      formed as predecessor's delay + edge delay);
///   2. then the fewest hops;
///   3. then the lowest-numbered predecessor; applied at every node, so the whole path is
///      the one whose nodes, read backwards from the end, are lowest at the first difference.
/// Rule 2 is what makes rule 3 well defined: a node's predecessor has strictly fewer hops and
/// no more delay, so it is settled first, and all candidates for a node are known before the
/// node is settled. Zero-delay edges are allowed. Edges are validated when added
/// (Graph::add_edge), so every delay here is finite and non-negative.
///
/// O((V + E) log V) with a binary heap.
[[nodiscard]] ShortestPaths shortest_paths(const Graph& graph, std::size_t source);

}  // namespace satsim::network
