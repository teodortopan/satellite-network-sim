#include "routing/settings.hpp"

#include <optional>
#include <string_view>

namespace satsim::routing_app {

std::string_view name_of(Planner planner) noexcept {
  return planner == Planner::kStart ? "start" : "whole-interval";
}

std::optional<Planner> parse_planner(std::string_view text) noexcept {
  for (const Planner planner : {Planner::kStart, Planner::kWholeInterval}) {
    if (text == name_of(planner)) {
      return planner;
    }
  }
  return std::nullopt;
}

std::string_view name_of(ServingRule rule) noexcept {
  switch (rule) {
    case ServingRule::kHighestElevation:
      return "highest-elevation";
    case ServingRule::kLongestInView:
      return "longest-in-view";
    case ServingRule::kHighestWholeSlot:
      return "highest-whole-slot";
    case ServingRule::kWithRoute:
      return "with-route";
  }
  return "unknown";
}

std::optional<ServingRule> parse_serving_rule(std::string_view text) noexcept {
  for (const ServingRule rule : {ServingRule::kHighestElevation, ServingRule::kLongestInView,
                                 ServingRule::kHighestWholeSlot, ServingRule::kWithRoute}) {
    if (text == name_of(rule)) {
      return rule;
    }
  }
  return std::nullopt;
}

std::string_view name_of(DropCause cause) noexcept {
  switch (cause) {
    case DropCause::kNoService:
      return "no_service";
    case DropCause::kNoCoverage:
      return "no_coverage";
    case DropCause::kServingLeftView:
      return "serving_left_view";
    case DropCause::kGroundLinkLost:
      return "ground_link_lost";
    case DropCause::kLaserLinkDown:
      return "laser_link_down";
    case DropCause::kNoGateway:
      return "no_gateway";
    case DropCause::kNoPath:
      return "no_path";
  }
  return "unknown";
}

}  // namespace satsim::routing_app
