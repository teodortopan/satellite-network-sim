#include "satsim/network/graph.hpp"

#include <cmath>
#include <cstddef>
#include <optional>
#include <string_view>
#include <vector>

namespace satsim::network {

std::string_view name_of(EdgeError error) noexcept {
  switch (error) {
    case EdgeError::kNodeOutOfRange:
      return "node_out_of_range";
    case EdgeError::kNegativeDelay:
      return "negative_delay";
    case EdgeError::kNonFiniteDelay:
      return "non_finite_delay";
  }
  return "unknown";
}

Graph::Graph(std::size_t node_count) : adjacency_(node_count) {}

namespace {

std::optional<EdgeError> check_edge(std::size_t node_count, std::size_t a, std::size_t b,
                                    double delay_s) noexcept {
  if (a >= node_count || b >= node_count) {
    return EdgeError::kNodeOutOfRange;
  }
  if (!std::isfinite(delay_s)) {
    return EdgeError::kNonFiniteDelay;
  }
  if (delay_s < 0.0) {
    return EdgeError::kNegativeDelay;
  }
  return std::nullopt;
}

}  // namespace

std::optional<EdgeError> Graph::add_edge(std::size_t from, std::size_t to, double delay_s) {
  if (const auto error = check_edge(node_count(), from, to, delay_s)) {
    return error;
  }
  adjacency_[from].push_back({.to = to, .delay_s = delay_s});
  ++edge_count_;
  return std::nullopt;
}

std::optional<EdgeError> Graph::add_undirected_edge(std::size_t a, std::size_t b, double delay_s) {
  if (const auto error = check_edge(node_count(), a, b, delay_s)) {
    return error;
  }
  adjacency_[a].push_back({.to = b, .delay_s = delay_s});
  adjacency_[b].push_back({.to = a, .delay_s = delay_s});
  edge_count_ += 2;
  return std::nullopt;
}

void Graph::clear_edges() noexcept {
  for (std::vector<Edge>& edges : adjacency_) {
    edges.clear();
  }
  edge_count_ = 0;
}

}  // namespace satsim::network
