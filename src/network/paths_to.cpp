#include "satsim/network/paths_to.hpp"

#include <cstddef>
#include <utility>
#include <vector>

#include "satsim/network/graph.hpp"
#include "satsim/network/shortest_path.hpp"

namespace satsim::network {

std::vector<std::size_t> PathsTo::path_from(std::size_t node) const {
  if (!reachable(node)) {
    return {};
  }
  std::vector<std::size_t> path;
  path.reserve(hops[node] + 1);
  for (std::size_t n = node; n != kNoNode; n = next[n]) {
    path.push_back(n);
  }
  return path;
}

PathsTo shortest_paths_to(const Graph& graph, std::size_t target) {
  const std::size_t n = graph.node_count();
  Graph reversed(n);
  for (std::size_t from = 0; from < n; ++from) {
    for (const Edge& edge : graph.edges_from(from)) {
      // Both ends are in range and the delay is valid: the edge was accepted by `graph`.
      static_cast<void>(reversed.add_edge(edge.to, from, edge.delay_s));
    }
  }
  ShortestPaths from_target = shortest_paths(reversed, target);
  return PathsTo{
      .target = target,
      .delay_s = std::move(from_target.delay_s),
      .hops = std::move(from_target.hops),
      .next = std::move(from_target.predecessor),
  };
}

}  // namespace satsim::network
