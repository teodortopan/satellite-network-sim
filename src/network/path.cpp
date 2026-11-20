#include "satsim/network/path.hpp"

#include <cstddef>
#include <optional>
#include <span>

#include "satsim/network/graph.hpp"

namespace satsim::network {

namespace {

// The smallest delay of the edges from -> to, or nullopt if there is none. Both are below
// graph.node_count().
std::optional<double> smallest_edge_delay_s(const Graph& graph, std::size_t from,
                                            std::size_t to) noexcept {
  std::optional<double> best_s;
  for (const Edge& edge : graph.edges_from(from)) {
    if (edge.to == to && (!best_s.has_value() || edge.delay_s < *best_s)) {
      best_s = edge.delay_s;
    }
  }
  return best_s;
}

}  // namespace

std::optional<double> path_delay_s(const Graph& graph, std::span<const std::size_t> path) noexcept {
  if (path.empty()) {
    return std::nullopt;
  }
  const std::size_t n = graph.node_count();
  if (path.front() >= n) {
    return std::nullopt;
  }
  double total_s = 0.0;
  for (std::size_t i = 1; i < path.size(); ++i) {
    if (path[i] >= n) {
      return std::nullopt;
    }
    const std::optional<double> edge_s = smallest_edge_delay_s(graph, path[i - 1], path[i]);
    if (!edge_s.has_value()) {
      return std::nullopt;
    }
    total_s += *edge_s;
  }
  return total_s;
}

bool path_is_up(const Graph& graph, std::span<const std::size_t> path) noexcept {
  return path_delay_s(graph, path).has_value();
}

}  // namespace satsim::network
