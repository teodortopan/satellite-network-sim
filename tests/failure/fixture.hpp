#pragma once

// A small network for the failure tests, small enough for the sanitizer and Valgrind runs.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "satsim/failure/schedule.hpp"
#include "satsim/time/utc_time.hpp"
#include "satsim/topology/link_plan.hpp"
#include "topology/ideal.hpp"

namespace satsim::failure::testing {

/// The link file tests' two-shell classification:
///   shell 0: 4 planes of 4 at 550 km, catalogue numbers 100000 + 4 p + s; satellite 100003
///            (plane 0, s 3) is excluded (no_lasers), so plane 0 has 3 members;
///   shell 1: 2 planes of 3 at 400 km, catalogue numbers 200000 + 3 p + s.
/// Classification indices follow catalogue order: 100000 is 0, ..., 100015 is 15, 200000 is
/// 16, 200001 is 17.
const topology::testing::Ideal& small();

/// A laser plan on small() read from a link file, so its links are known by hand:
///   0: 100000-100001   1: 100001-100002   2: 100000-100004
///   3: 100004-100005   4: 100001-100005   5: 200000-200001
/// (satellite_a as listed first). By classification index: 0-1, 1-2, 0-4, 4-5, 1-5, 16-17.
const topology::LinkPlan& small_plan();

/// The run's gateways: 0 "Hawthorne", 1 "New York, NY", 2 `The "Q" site`, 3 and 4 both "Twin".
const std::vector<std::string>& gateway_names();

/// The run's start, 2026-11-18T21:15:00 UTC.
time::UtcTime run_start();

/// The run's step, 1 s.
inline constexpr std::int64_t kStepNs = 1'000'000'000;

/// A failure at `start_s` seconds from run_start() (start_utc to match).
Failure failure_at(ElementKind kind, std::size_t element, double start_s,
                   std::optional<double> length_s = std::nullopt);

}  // namespace satsim::failure::testing
