#pragma once

#include <cstddef>
#include <optional>
#include <span>
#include <vector>

#include "failures/loop.hpp"
#include "satsim/failure/schedule.hpp"

// The measures of Phase 4 spec 4.7 per direction, strategy and lane, from the loop's intervals
// (continuous time) and its delays at the steps.

namespace satsim::failures_app {

/// One lane's measures. Every one is nullopt ("none") when the run has no failure instant.
struct LaneMeasures {
  /// Seconds in black_hole from t_f to the end of the run.
  std::optional<double> black_hole_s = std::nullopt;
  /// Seconds from t_f on in which the baseline is delivered and this lane is not.
  std::optional<double> lost_s = std::nullopt;
  /// From t_f to the first instant the lane is delivered again after it first stops being
  /// delivered at or after t_f: 0 if it never stops; nullopt if it never comes back.
  std::optional<double> restore_s = std::nullopt;
  /// From t_f to the end of the last interval at or after t_f in which the lane's state or
  /// route differs from the reference's at the same instants: 0 if never; nullopt if it still
  /// differs at the end of the run.
  std::optional<double> settle_s = std::nullopt;
  /// At the steps at or after t_f where this lane and the baseline are both delivered: this
  /// lane's delay minus the baseline's, ms.
  std::vector<double> extra_ms;
  /// The same at the first step at or after the settle instant t_f + settle_s (spec 4.10
  /// Details); nullopt when it never settles, there is no such step, or either lane is not
  /// delivered there.
  std::optional<double> extra_settled_ms = std::nullopt;
};

/// The run's failure instant (spec 4.7): the earliest start_s >= 0 of the schedule; nullopt
/// when every failure is old news (or there is none).
[[nodiscard]] std::optional<double> failure_instant(const failure::FailureSchedule& schedule);

/// The measures of every lane, [(e * D + d) * S + s] (e an engine of the loop; the baseline's
/// and the reference's are worked out too). `intervals` sorted as FailureLoop::intervals, each
/// lane's covering [0, end_s) without gaps; `step_delays` as FailureLoop::step_delays; engine 0
/// is the baseline and engine 1 the reference.
[[nodiscard]] std::vector<LaneMeasures> lane_measures(
    std::span<const Interval> intervals,
    const std::vector<std::vector<std::optional<double>>>& step_delays, std::size_t engines,
    std::size_t directions, std::size_t strategies, double step_s, double end_s,
    std::optional<double> t_f);

}  // namespace satsim::failures_app
