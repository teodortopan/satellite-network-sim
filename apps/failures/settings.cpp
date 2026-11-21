#include "failures/settings.hpp"

#include <array>
#include <cmath>
#include <optional>
#include <string>
#include <utility>

namespace satsim::failures_app {

std::optional<std::string> check_reaction_times(const ReactionTimes& times) {
  const std::array<std::pair<const char*, double>, 5> values = {{
      {"detection_s", times.detection_s},
      {"hop_processing_s", times.hop_processing_s},
      {"compute_s", times.compute_s},
      {"planner_processing_s", times.planner_processing_s},
      {"acquisition_s", times.acquisition_s},
  }};
  for (const auto& [name, value] : values) {
    if (!std::isfinite(value) || value < 0.0) {
      return std::string(name) + " must be finite and at least 0";
    }
  }
  return std::nullopt;
}

}  // namespace satsim::failures_app
