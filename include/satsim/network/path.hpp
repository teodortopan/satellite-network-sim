#pragma once

#include <cstddef>
#include <optional>
#include <span>

#include "satsim/network/graph.hpp"

namespace satsim::network {

// A path is a list of node indices, first to last (spec 4.3, 4.5): traffic goes from each node
// to the next along a directed edge. One node is a path of no edges.

/// True iff `path` has at least one node, every node is below graph.node_count(), and the graph
/// has a directed edge from each node to the next. One node: true (no edge is needed). Empty:
/// false. Parallel edges are fine; an edge stored only the other way is not enough.
[[nodiscard]] bool path_is_up(const Graph& graph, std::span<const std::size_t> path) noexcept;

/// The delay of `path` in seconds: the sum of its edge delays, with the smallest delay where
/// parallel edges join the same two nodes; nullopt if the path is not up (path_is_up). One
/// node: 0.
///
/// The sum is formed in path order, starting from 0: ((0 + d01) + d12) + ..., the same order
/// in which shortest_paths forms ShortestPaths::delay_s along its chosen path, so the delay of
/// a path that shortest_paths returned equals its delay_s bit for bit.
[[nodiscard]] std::optional<double> path_delay_s(const Graph& graph,
                                                 std::span<const std::size_t> path) noexcept;

}  // namespace satsim::network
