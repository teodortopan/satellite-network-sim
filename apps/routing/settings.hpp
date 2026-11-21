#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string_view>

// The routing program's named settings and outcomes (Phase 3 spec 4.3 to 4.6).

namespace satsim::routing_app {

/// How the planning graph is made at a re-plan instant (spec 4.3).
enum class Planner : std::uint8_t {
  kStart,          ///< "start": the graph of the re-plan step.
  kWholeInterval,  ///< "whole-interval": only the edges that exist at every step of the interval.
};

/// "start", "whole-interval".
[[nodiscard]] std::string_view name_of(Planner planner) noexcept;
[[nodiscard]] std::optional<Planner> parse_planner(std::string_view text) noexcept;

/// How a dish picks its serving satellite at a slot start (spec 4.4, realistic model).
enum class ServingRule : std::uint8_t {
  kHighestElevation,  ///< "highest-elevation"
  kLongestInView,     ///< "longest-in-view"
  kHighestWholeSlot,  ///< "highest-whole-slot"
  kWithRoute,         ///< "with-route"
};

/// "highest-elevation", "longest-in-view", "highest-whole-slot", "with-route".
[[nodiscard]] std::string_view name_of(ServingRule rule) noexcept;
[[nodiscard]] std::optional<ServingRule> parse_serving_rule(std::string_view text) noexcept;

/// Why a step has no usable route (spec 4.6), in the order they are tested; then the three
/// failure causes of Phase 4 spec 4.6, which are tested before all of them (Engine::evaluate)
/// and occur only when a step's ground marks something failed.
enum class DropCause : std::uint8_t {
  kNoService,
  kNoCoverage,
  kServingLeftView,
  kGroundLinkLost,
  kLaserLinkDown,
  kNoGateway,
  kNoPath,
  kFailedSatellite,
  kFailedLink,
  kFailedGateway,
};

inline constexpr std::array<DropCause, 10> kAllDropCauses = {
    DropCause::kNoService,      DropCause::kNoCoverage,      DropCause::kServingLeftView,
    DropCause::kGroundLinkLost, DropCause::kLaserLinkDown,   DropCause::kNoGateway,
    DropCause::kNoPath,         DropCause::kFailedSatellite, DropCause::kFailedLink,
    DropCause::kFailedGateway};

/// "no_service", "no_coverage", "serving_left_view", "ground_link_lost", "laser_link_down",
/// "no_gateway", "no_path", "failed_satellite", "failed_link", "failed_gateway".
[[nodiscard]] std::string_view name_of(DropCause cause) noexcept;

}  // namespace satsim::routing_app
