#pragma once

#include <cstddef>
#include <vector>

#include "satsim/failure/schedule.hpp"
#include "satsim/topology/link_plan.hpp"

namespace satsim::failure {

// Who can detect a failure (docs/phase-4-failures.md 4.4), as far as the laser plan alone
// decides it. Only neighbours detect, and a neighbour sees a link go silent: it cannot tell a
// dead laser link from a dead satellite, so what it detects is a link.
//
// Not decided here, because it needs the step's geometry, and done later in the program:
//  - which of a failed satellite's planned links were up at the failure instant (a link
//    already down then, out of sight say, is detected when it would have come back up);
//  - the dishes with a ground edge to a failed satellite and the gateways that see it;
//  - the satellites a failed gateway sees (the planner learns of a gateway's failure by
//    itself after the detection time).
// When each detector detects (the failure instant plus the detection time) is the program's
// setting, not this module's.

/// A satellite that can detect a failure, and the planned link through which it does.
struct Detector {
  std::size_t satellite = 0;  ///< Index into Classification::satellites.
  std::size_t link = 0;       ///< Index into LinkPlan::links.

  friend bool operator==(const Detector&, const Detector&) = default;
};

/// The satellites that can detect `failure` from `plan` alone:
///  - a laser link: its two ends, satellite_a then satellite_b, each through that link;
///  - a satellite: the satellite at the other end of each of its planned links, through that
///    link, in increasing link index;
///  - a gateway: none (see above).
[[nodiscard]] std::vector<Detector> possible_detectors(const Failure& failure,
                                                       const topology::LinkPlan& plan);

/// possible_detectors without the satellites that are themselves down at `t_s` (a satellite
/// failure of theirs in force in `schedule`): a dead neighbour detects nothing.
[[nodiscard]] std::vector<Detector> working_detectors(const Failure& failure,
                                                      const FailureSchedule& schedule,
                                                      const topology::LinkPlan& plan, double t_s);

}  // namespace satsim::failure
