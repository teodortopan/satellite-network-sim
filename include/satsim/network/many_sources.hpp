#pragma once

#include <cstddef>
#include <span>
#include <vector>

#include "satsim/network/graph.hpp"
#include "satsim/network/shortest_path.hpp"

namespace satsim::network {

// Shortest delays from several sources at once (Phase 4 spec 4.5, 6.3): the instant each node
// learns a flooded fact, when the flood starts at every detecting node, and the instant a
// report reaches the nearest gateway.

/// One place a search starts: a node, and the delay already spent when the search starts
/// there (a detection instant, say), seconds.
struct Start {
  std::size_t node = 0;
  double delay_s = 0.0;

  friend bool operator==(const Start&, const Start&) = default;
};

/// The result of shortest_paths_from_many.
struct ManySourcePaths {
  /// Per node: the smallest total, seconds: a start's delay plus the cost of every edge from
  /// that start to the node. +infinity when no start reaches it.
  std::vector<double> delay_s;
  /// Per node: edges on the chosen path from its start; 0 at a node that stays a start and for
  /// unreachable nodes.
  std::vector<std::size_t> hops;
  /// Per node: the node before it on the chosen path; kNoNode at a node that stays a start
  /// (it keeps its own start delay) and for unreachable nodes.
  std::vector<std::size_t> predecessor;
  /// Per node: the start node its chosen path begins at (the node itself at a node that stays
  /// a start); kNoNode for unreachable nodes.
  std::vector<std::size_t> start;

  [[nodiscard]] bool reachable(std::size_t node) const noexcept;

  /// The chosen path from its start to `node`, both included (just the node at a node that
  /// stays a start); empty if `node` is unreachable.
  [[nodiscard]] std::vector<std::size_t> path_to(std::size_t node) const;
};

/// Dijkstra's algorithm from every start at once. Every edge costs edge.delay_s +
/// extra_per_edge_s (the per-hop time of the spec), formed in that order, once per edge; a
/// node's total is formed as its predecessor's total + that cost. A node's label starts as
/// (start delay, 0 hops, no predecessor) at a start and as unreached elsewhere.
///
/// Among labels the choice is fixed, so the result never depends on hash order, heap order or
/// the order of edges or starts:
///   1. the smallest total (exact double comparison);
///   2. then the fewest hops (a start node counts 0 hops, so between staying a start and being
///      reached at the same total, it stays a start: a start whose start delay is <= the
///      reached total stays a start);
///   3. then the lowest-numbered predecessor (so if two starts give a node the same total and
///      hops, the lower predecessor decides).
/// This is the tie rule of shortest_paths, with every start as a source. A start whose own
/// delay is larger than a path from another start is simply reached from the other: its
/// predecessor, hops and start are those of that path.
///
/// With one start of delay 0 and extra_per_edge_s 0 the result is shortest_paths from that
/// node, bit for bit. In general a node's total equals the minimum over starts of
/// (start delay + the single-source delay on a graph whose edges carry delay + extra) when the
/// sums are exact; otherwise it can differ in the last bit, because the start delay is added
/// first here.
///
/// Preconditions: every start's node is below graph.node_count(); every start delay is finite
/// and >= 0; extra_per_edge_s is finite and >= 0. A node given twice as a start keeps its
/// lowest delay. No starts: every node is unreachable.
///
/// O((V + E) log V) with a binary heap.
[[nodiscard]] ManySourcePaths shortest_paths_from_many(const Graph& graph,
                                                       std::span<const Start> starts,
                                                       double extra_per_edge_s = 0.0);

}  // namespace satsim::network
