#pragma once

#include <cstddef>
#include <optional>
#include <vector>

#include "failures/geometry.hpp"
#include "failures/timeline.hpp"

// The reports of every fact (Phase 4 spec 4.5, 4.10 facts CSV): how many of its detectors can
// report to a gateway, and the hops of the report that arrives first. Worked out as
// build_timeline works out planner_known_s, on the same control graphs.

namespace satsim::failures_app {

struct FactReports {
  /// Detectors whose report reaches a working gateway; a gateway that detects counts as one
  /// report with 0 hops.
  std::size_t reports = 0;
  /// The hops of the report that arrives first (the lowest detection instant + report time;
  /// at the same instant the fewer hops, then the earlier detector); nullopt without a report.
  std::optional<std::size_t> first_hops = std::nullopt;
  /// The hops of every report, summed: the messages the reports take (one per hop).
  std::size_t hops_total = 0;
};

/// Per fact of `timeline`, in order. An old fact has no reports. A satellite or dish detector
/// reports when its detection instant's step is a step of the run (as build_timeline) and
/// network::shortest_paths_from_many from every working gateway (start delay 0, per-hop time
/// settings.times.hop_processing_s) on the control graph of the truth at that instant reaches
/// it; its hops are that path's.
[[nodiscard]] std::vector<FactReports> fact_reports(const Timeline& timeline,
                                                    const TimelineSettings& settings,
                                                    Geometry& geometry);

}  // namespace satsim::failures_app
