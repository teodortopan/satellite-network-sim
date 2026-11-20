#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

namespace satsim::routing {

// What happened to one direction between two consecutive steps (spec 2 and 4.7).

/// One direction at one step.
struct DirectionStep {
  /// The held route; empty when there is none.
  std::span<const std::size_t> route;
  /// Whether the route is up in this step's graph. Precondition: false when route is empty.
  bool up = false;
  /// The route's delay in this step's graph, seconds. Read only when up.
  double delay_s = 0.0;
};

enum class ChangeKind : std::uint8_t {
  kNone,                 ///< Both steps up, the same route.
  kBadChange,            ///< Both up, routes differ, the delay strictly lower.
  kDelayIncreaseChange,  ///< Both up, routes differ, the delay equal or higher.
  kBreak,                ///< The previous step up, this one not.
  kRecover,              ///< The previous step not up, this one up.
  kStillDown,            ///< Neither step up.
};

/// "none", "bad_change", "delay_increase_change", "break", "recover", "still_down".
[[nodiscard]] std::string_view name_of(ChangeKind kind) noexcept;

struct Change {
  ChangeKind kind = ChangeKind::kNone;
  /// For kBadChange and kDelayIncreaseChange: current.delay_s - previous.delay_s, the new
  /// route's delay at its first step minus the old route's at its last (negative for a bad
  /// change). nullopt for every other kind.
  std::optional<double> size_s;
};

/// Classifies the step `current` against the step before it, `previous`. "Up" is the `up`
/// flag only: a step with a route that is not up counts as down. Routes are compared as node
/// lists.
[[nodiscard]] Change classify_change(const DirectionStep& previous,
                                     const DirectionStep& current) noexcept;

}  // namespace satsim::routing
