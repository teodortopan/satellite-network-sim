#pragma once

#include <cstddef>
#include <vector>

#include "satsim/network/graph.hpp"
#include "satsim/network/shortest_path.hpp"

namespace satsim::network {

/// Shortest delays from every node to one target, and the tree of chosen paths towards it
/// (Phase 4 spec 4.5: a node's table towards a destination).
struct PathsTo {
  std::size_t target = 0;
  /// Per node: the smallest delay from that node to the target, seconds; +infinity when the
  /// target cannot be reached from it. 0 at the target.
  std::vector<double> delay_s;
  /// Per node: edges on the chosen path to the target; 0 at the target and for nodes that
  /// cannot reach it.
  std::vector<std::size_t> hops;
  /// Per node: the next node on the chosen path; kNoNode at the target and for nodes that
  /// cannot reach it.
  std::vector<std::size_t> next;

  [[nodiscard]] bool reachable(std::size_t node) const noexcept {
    return node == target || next[node] != kNoNode;
  }

  /// The chosen path from `node` to the target, both included (just the target for the target
  /// itself); empty if the target cannot be reached from `node`. It follows `next`.
  [[nodiscard]] std::vector<std::size_t> path_from(std::size_t node) const;
};

/// Shortest paths from every node to `target`, along directed edges forwards (node -> next ->
/// ... -> target). The search runs from the target over the edges reversed, with the tie rule
/// of shortest_paths applied from the target's side:
///   1. the smallest delay (exact double comparison; each node's delay is formed as edge
///      delay + next's delay);
///   2. then the fewest hops;
///   3. then the lowest-numbered next node.
/// So the result is exactly shortest_paths from `target` on the graph with every edge
/// reversed, with `next` its predecessor.
///
/// Every node's choice comes from one tree: hops[next[v]] == hops[v] - 1, so following `next`
/// from any node that can reach the target arrives there after hops[v] steps, with no loop.
///
/// delay_s[v] is the smallest delay of any path from v to the target, as shortest_paths(graph,
/// v).delay_s[target] is; the two are summed in opposite orders (from the target here, from v
/// there), so they agree exactly when the sums are exact and may otherwise differ in the last
/// bit. For the same reason, path_delay_s(graph, path_from(v)) may differ from delay_s[v] in
/// the last bit.
///
/// Precondition: target < graph.node_count(). O((V + E) log V), plus building the reversed
/// graph.
[[nodiscard]] PathsTo shortest_paths_to(const Graph& graph, std::size_t target);

}  // namespace satsim::network
