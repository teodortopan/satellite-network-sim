#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace satsim::network {

// A small weighted graph for shortest-path delays (spec 4.1, 4.6). Nodes are the indices
// 0 .. node_count() - 1; what a node stands for is the caller's business. Every edge has a
// delay in seconds, finite and not negative.

/// One directed edge, as stored at its tail.
struct Edge {
  std::size_t to = 0;
  double delay_s = 0.0;

  friend bool operator==(const Edge&, const Edge&) = default;
};

/// Why an edge was refused.
enum class EdgeError : std::uint8_t {
  kNodeOutOfRange,  ///< An end is not below node_count().
  kNegativeDelay,   ///< delay_s < 0.
  kNonFiniteDelay,  ///< delay_s is NaN or infinite.
};

/// "node_out_of_range", "negative_delay", "non_finite_delay".
[[nodiscard]] std::string_view name_of(EdgeError error) noexcept;

class Graph {
 public:
  explicit Graph(std::size_t node_count = 0);

  [[nodiscard]] std::size_t node_count() const noexcept { return adjacency_.size(); }

  /// Directed edges stored (an undirected edge counts twice).
  [[nodiscard]] std::size_t edge_count() const noexcept { return edge_count_; }

  /// Adds the directed edge from -> to. A refused edge leaves the graph unchanged. Self-loops
  /// and parallel edges are allowed (they never shorten a path).
  [[nodiscard]] std::optional<EdgeError> add_edge(std::size_t from, std::size_t to, double delay_s);

  /// Adds a -> b and b -> a with the same delay, or neither.
  [[nodiscard]] std::optional<EdgeError> add_undirected_edge(std::size_t a, std::size_t b,
                                                             double delay_s);

  /// The edges leaving `node`, in the order they were added. Precondition: node <
  /// node_count().
  [[nodiscard]] std::span<const Edge> edges_from(std::size_t node) const noexcept {
    return adjacency_[node];
  }

  /// Removes every edge and keeps the nodes (and the memory, so a graph rebuilt at every time
  /// step does not reallocate).
  void clear_edges() noexcept;

 private:
  std::vector<std::vector<Edge>> adjacency_;
  std::size_t edge_count_ = 0;
};

}  // namespace satsim::network
