#include "satsim/routing/forwarding.hpp"

#include <cstddef>
#include <string_view>
#include <unordered_set>
#include <utility>

#include "satsim/network/paths_to.hpp"
#include "satsim/network/shortest_path.hpp"
#include "satsim/routing/planner.hpp"

namespace satsim::routing {

std::string_view name_of(WalkOutcome outcome) noexcept {
  switch (outcome) {
    case WalkOutcome::kDelivered:
      return "delivered";
    case WalkOutcome::kDeadEnd:
      return "dead_end";
    case WalkOutcome::kLoop:
      return "loop";
  }
  return "unknown";
}

Walk walk(std::size_t source, std::size_t target, const NextHop& next_hop) {
  Walk result{.outcome = WalkOutcome::kDelivered, .path = {source}};
  // Only membership is asked of the set, so its hash order never shows in the result.
  std::unordered_set<std::size_t> passed{source};
  std::size_t node = source;
  while (node != target) {
    const std::size_t next = next_hop(node);
    if (next == network::kNoNode) {
      result.outcome = WalkOutcome::kDeadEnd;
      return result;
    }
    result.path.push_back(next);
    if (!passed.insert(next).second) {
      result.outcome = WalkOutcome::kLoop;
      return result;
    }
    node = next;
  }
  return result;
}

NextHop next_hop_from_views(ViewOf view_of) {
  return [view_of = std::move(view_of)](std::size_t node) { return view_of(node).next[node]; };
}

}  // namespace satsim::routing
