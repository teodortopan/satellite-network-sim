#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string_view>

#include "satsim/network/paths_to.hpp"
#include "satsim/routing/planner.hpp"

namespace satsim::routing {

// Following tables hop by hop (Phase 4 spec 4.5, flooding): each node sends traffic to the
// next hop its own table gives, and different nodes may hold different views, so the walk
// can end at a node with no way on or pass a node twice.

/// How a walk ended.
enum class WalkOutcome : std::uint8_t {
  kDelivered,  ///< It reached the target.
  kDeadEnd,    ///< It reached a node, not the target, that has no next hop.
  kLoop,       ///< It came back to a node it had already passed.
};

/// "delivered", "dead_end", "loop".
[[nodiscard]] std::string_view name_of(WalkOutcome outcome) noexcept;

struct Walk {
  WalkOutcome outcome = WalkOutcome::kDeadEnd;
  /// The nodes passed, from the source:
  /// - delivered: source to target, both included;
  /// - dead end: source to the node with no next hop, both included;
  /// - loop: source to the node whose next hop was already passed, then that repeated node
  ///   once more, so the loop is visible (e.g. [0, 2, 3, 2]).
  Route path;
};

/// A node's next hop towards the walk's target, or network::kNoNode for none.
using NextHop = std::function<std::size_t(std::size_t)>;

/// Follows next hops from `source` towards `target`.
///
/// - At the target the walk is delivered; a source equal to the target is delivered with the
///   one-node path [source], and next_hop is never called.
/// - At any other node it asks next_hop(node), once per node passed. kNoNode: a dead end,
///   the path ending at that node. A node already on the path (the node itself included):
///   a loop, the path ending with that node appended once. Otherwise it moves on.
/// - next_hop is called for each node the walk passes, in order, so each node may answer from
///   its own view. The walk does not check that a hop is an edge of any graph: that is the
///   caller's business.
///
/// The walk ends after at most as many hops as there are distinct nodes it can visit, since
/// every hop that does not end it adds a new node.
[[nodiscard]] Walk walk(std::size_t source, std::size_t target, const NextHop& next_hop);

/// The tree a node's view gives it towards the walk's target.
using ViewOf = std::function<const network::PathsTo&(std::size_t)>;

/// The common next hop: node v sends to view_of(v).next[v], the next node on its own view's
/// shortest-path tree (network::shortest_paths_to), or kNoNode where that view cannot reach
/// the target. Preconditions, when the result is called with v: view_of(v) returns a tree
/// whose target is the walk's target, that lives at least until the call returns, and v is
/// below its node count. `view_of` is copied into the result.
[[nodiscard]] NextHop next_hop_from_views(ViewOf view_of);

}  // namespace satsim::routing
