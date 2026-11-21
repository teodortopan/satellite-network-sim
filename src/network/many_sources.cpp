#include "satsim/network/many_sources.hpp"

#include <algorithm>
#include <cstddef>
#include <functional>
#include <limits>
#include <queue>
#include <span>
#include <tuple>
#include <vector>

#include "satsim/network/graph.hpp"
#include "satsim/network/shortest_path.hpp"

namespace satsim::network {

bool ManySourcePaths::reachable(std::size_t node) const noexcept { return start[node] != kNoNode; }

std::vector<std::size_t> ManySourcePaths::path_to(std::size_t node) const {
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

ManySourcePaths shortest_paths_from_many(const Graph& graph, std::span<const Start> starts,
                                         double extra_per_edge_s) {
  const std::size_t n = graph.node_count();
  ManySourcePaths result{
      .delay_s = std::vector<double>(n, std::numeric_limits<double>::infinity()),
      .hops = std::vector<std::size_t>(n, 0),
      .predecessor = std::vector<std::size_t>(n, kNoNode),
      .start = std::vector<std::size_t>(n, kNoNode),
  };
  for (const Start& s : starts) {
    // A start's label is (delay, 0 hops, no predecessor); of a repeated start the lowest delay.
    if (result.start[s.node] == kNoNode || s.delay_s < result.delay_s[s.node]) {
      result.delay_s[s.node] = s.delay_s;
      result.start[s.node] = s.node;
    }
  }

  // (total, hops, node), smallest first: the order in which labels become final. A node's
  // predecessor has no more total and strictly fewer hops, so it is settled first and every
  // candidate for a node is known before the node is settled, which makes rule 3 well defined.
  using Entry = std::tuple<double, std::size_t, std::size_t>;
  std::priority_queue<Entry, std::vector<Entry>, std::greater<>> queue;
  for (std::size_t v = 0; v < n; ++v) {
    if (result.start[v] == v) {
      queue.emplace(result.delay_s[v], 0, v);
    }
  }
  std::vector<bool> settled(n, false);
  while (!queue.empty()) {
    const auto [total_s, hops, u] = queue.top();
    queue.pop();
    if (settled[u]) {
      continue;  // A stale entry: u was settled with a better label.
    }
    settled[u] = true;
    for (const Edge& edge : graph.edges_from(u)) {
      const std::size_t v = edge.to;
      if (settled[v]) {
        continue;
      }
      const double candidate_s = total_s + (edge.delay_s + extra_per_edge_s);
      const std::size_t candidate_hops = hops + 1;
      // (total, hops, predecessor) compared lexicographically; a start's predecessor is
      // kNoNode, the largest, but its 0 hops already beat any reached label of equal total.
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
        result.start[v] = result.start[u];
        queue.emplace(candidate_s, candidate_hops, v);
      }
    }
  }
  return result;
}

}  // namespace satsim::network
