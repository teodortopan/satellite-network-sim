#pragma once

#include <optional>
#include <string>

// The times of Phase 4 spec 4.9 that are settings, and whether freed lasers re-point (4.8).

namespace satsim::failures_app {

/// How long the actors of a failure take (spec 4.9), seconds, and whether lasers re-point.
struct ReactionTimes {
  /// From a failure (or from the instant a link that was out of sight would have come back
  /// up) to a neighbour knowing it (--detection-s).
  double detection_s = 0.1;
  /// Added for every hop a report or a flooded fact makes (--hop-processing-s).
  double hop_processing_s = 0.001;
  /// From a node learning a fact to its tables using it (--compute-s).
  double compute_s = 0.1;
  /// From a report reaching a gateway to the planner knowing (--planner-processing-s).
  double planner_processing_s = 0.0;
  /// From a laser link being matched or returning to it carrying traffic (--acquisition-s).
  double acquisition_s = 30.0;
  /// Whether freed lasers look for new partners (--repoint).
  bool repoint = false;
};

/// nullopt if usable, else what is wrong: every time finite and >= 0.
[[nodiscard]] std::optional<std::string> check_reaction_times(const ReactionTimes& times);

}  // namespace satsim::failures_app
