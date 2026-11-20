#include "satsim/network/shortest_path.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <queue>
#include <span>
#include <tuple>
#include <vector>

#include "satsim/network/graph.hpp"

namespace satsim::network {

std::vector<std::size_t> ShortestPaths::path_to(std::size_t node) const {
  if (!reachable(node)) {
    return {};
  }
  std::vector<std::size_t> path;
  path.reserve(hops[node] + 1);
  for (std::size_t n = node; n != kNoNode; n = predecessor[n]) {
    path.push_back(n);
  }
  std::reverse(path.begin(), path.end());
  return path;
}

namespace {

// Dijkstra with the documented tie rule. `blocked` is empty (no node blocked) or has one entry
// per node, nonzero for a node that is never entered. Shared by both public functions.
ShortestPaths dijkstra(const Graph& graph, std::size_t source,
                       const std::vector<std::uint8_t>& blocked) {
  const std::size_t n = graph.node_count();
  const bool any_blocked = !blocked.empty();
  ShortestPaths result{
      .source = source,
      .delay_s = std::vector<double>(n, std::numeric_limits<double>::infinity()),
      .hops = std::vector<std::size_t>(n, 0),
      .predecessor = std::vector<std::size_t>(n, kNoNode),
  };
  std::vector<bool> settled(n, false);
  result.delay_s[source] = 0.0;

  // (delay, hops, node), smallest first: the order in which labels become final.
  using Entry = std::tuple<double, std::size_t, std::size_t>;
  std::priority_queue<Entry, std::vector<Entry>, std::greater<>> queue;
  queue.emplace(0.0, 0, source);
  while (!queue.empty()) {
    const auto [delay_s, hops, u] = queue.top();
    queue.pop();
    if (settled[u]) {
      continue;  // A stale entry: u was settled with a better label.
    }
    settled[u] = true;
    for (const Edge& edge : graph.edges_from(u)) {
      const std::size_t v = edge.to;
      if (settled[v] || (any_blocked && blocked[v] != 0)) {
        continue;
      }
      const double candidate_s = delay_s + edge.delay_s;
      const std::size_t candidate_hops = hops + 1;
      // The source is settled first and never relabelled, so v != source here.
      // (delay, hops, predecessor) compared lexicographically, written out: this is the inner
      // loop, and unoptimised sanitizer builds pay for every helper call.
      const double current_s = result.delay_s[v];
      const std::size_t current_hops = result.hops[v];
      const bool better = candidate_s < current_s ||
                          (candidate_s == current_s &&
                           (candidate_hops < current_hops ||
                            (candidate_hops == current_hops && u < result.predecessor[v])));
      if (better) {
        result.delay_s[v] = candidate_s;
        result.hops[v] = candidate_hops;
        result.predecessor[v] = u;
        queue.emplace(candidate_s, candidate_hops, v);
      }
    }
  }
  return result;
}

}  // namespace

ShortestPaths shortest_paths(const Graph& graph, std::size_t source) {
  return dijkstra(graph, source, {});
}

ShortestPaths shortest_paths_avoiding(const Graph& graph, std::size_t source,
                                      std::span<const std::size_t> blocked) {
  std::vector<std::uint8_t> mask(graph.node_count(), 0);
  for (const std::size_t node : blocked) {
    if (node < mask.size()) {
      mask[node] = 1;
    }
  }
  return dijkstra(graph, source, mask);
}

}  // namespace satsim::network
