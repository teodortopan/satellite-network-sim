#include "satsim/routing/change.hpp"

#include <algorithm>
#include <string_view>

namespace satsim::routing {

std::string_view name_of(ChangeKind kind) noexcept {
  switch (kind) {
    case ChangeKind::kNone:
      return "none";
    case ChangeKind::kBadChange:
      return "bad_change";
    case ChangeKind::kDelayIncreaseChange:
      return "delay_increase_change";
    case ChangeKind::kBreak:
      return "break";
    case ChangeKind::kRecover:
      return "recover";
    case ChangeKind::kStillDown:
      return "still_down";
  }
  return "unknown";
}

Change classify_change(const DirectionStep& previous, const DirectionStep& current) noexcept {
  if (!previous.up) {
    return {.kind = current.up ? ChangeKind::kRecover : ChangeKind::kStillDown,
            .size_s = std::nullopt};
  }
  if (!current.up) {
    return {.kind = ChangeKind::kBreak, .size_s = std::nullopt};
  }
  if (std::ranges::equal(previous.route, current.route)) {
    return {.kind = ChangeKind::kNone, .size_s = std::nullopt};
  }
  const double size_s = current.delay_s - previous.delay_s;
  return {.kind = current.delay_s < previous.delay_s ? ChangeKind::kBadChange
                                                     : ChangeKind::kDelayIncreaseChange,
          .size_s = size_s};
}

}  // namespace satsim::routing
