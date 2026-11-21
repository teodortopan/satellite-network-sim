#include "satsim/network/pieces.hpp"

#include <cstddef>
#include <cstdint>
#include <numeric>
#include <span>
#include <utility>
#include <vector>

namespace satsim::network {

namespace {

// The root of `node`'s set, halving the path on the way.
std::size_t find_root(std::vector<std::size_t>& parent, std::size_t node) {
  while (parent[node] != node) {
    parent[node] = parent[parent[node]];
    node = parent[node];
  }
  return node;
}

}  // namespace

Pieces connected_pieces(std::size_t node_count, std::span<const NodePair> undirected_edges,
                        std::span<const std::uint8_t> present) {
  const auto is_present = [&](std::size_t node) {
    return node < node_count && (present.empty() || (node < present.size() && present[node] != 0));
  };

  std::vector<std::size_t> parent(node_count);
  std::iota(parent.begin(), parent.end(), std::size_t{0});
  for (const auto& [a, b] : undirected_edges) {
    if (!is_present(a) || !is_present(b)) {
      continue;
    }
    const std::size_t root_a = find_root(parent, a);
    const std::size_t root_b = find_root(parent, b);
    // The lower root becomes the root, so every set's root is its lowest node.
    if (root_a < root_b) {
      parent[root_b] = root_a;
    } else if (root_b < root_a) {
      parent[root_a] = root_b;
    }
  }

  Pieces result{.piece = std::vector<std::size_t>(node_count, kNoPiece), .size = {}};
  for (std::size_t node = 0; node < node_count; ++node) {
    if (!is_present(node)) {
      continue;
    }
    const std::size_t root = find_root(parent, node);
    if (root == node) {
      // The lowest node of a new piece: pieces are numbered in this order.
      result.piece[node] = result.size.size();
      result.size.push_back(0);
    } else {
      result.piece[node] = result.piece[root];  // root < node, so it is already numbered.
    }
    ++result.size[result.piece[node]];
  }
  return result;
}

}  // namespace satsim::network
