#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <iosfwd>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "inputs/inputs.hpp"
#include "satsim/constellation/classification.hpp"
#include "satsim/ground/home_pop.hpp"
#include "satsim/ground/sites.hpp"
#include "satsim/orbit/propagator.hpp"
#include "satsim/orbit/snapshot.hpp"
#include "satsim/time/earth_orientation.hpp"
#include "satsim/topology/link_plan.hpp"
#include "satsim/topology/link_state.hpp"
#include "satsim/topology/physical_checks.hpp"
#include "satsim/topology/positions.hpp"
#include "topology/arguments.hpp"
#include "topology/endpoints.hpp"

namespace satsim::topology_app {

/// The ground-data directory's files (shared with the routing program).
using app::kAssumedGatewaysFileName;
using app::kGatewaysFileName;
using app::kGeofeedFileName;
using app::kPopBlocksFileName;
using app::kPopCodesFileName;

/// The CSV columns, in order. One row per time step, city pair and direction, ordered by time,
/// then pair (city-file order), then direction (the file's from-to first). A field is empty
/// when it does not apply or there is no path; it never holds a made-up number.
///  - utc: the time (YYYY-MM-DDTHH:MM:SS[.f...], UTC); from, to: city names; role: study or
///    validation; pattern; model ("realistic" or "best-case");
///  - status: ok, no_service, no_coverage, no_gateway or no_path (PathStatus);
///  - one_way_ms: the path's delay; round_trip_ms: best case twice one_way_ms, realistic this
///    direction plus the reverse one at the same step when both are ok;
///  - laser_hops; path_km = uplink_km + laser_km + downlink_km + fibre_km (fibre_km is
///    great-circle length; its delay is fibre_km * fibre factor / c);
///  - great_circle_km, lower_bound_ms (great circle / c) and stretch (one_way / lower bound):
///    the first two on every row;
///  - realistic only: serving_norad_cat_id (also when the status is no_gateway),
///    gateway_norad_cat_id (the satellite down to the gateway), gateway (its name),
///    gateway_source (real or assumed), and on every realistic row home_pop (the sender's
///    PoP code, empty without one) and home_pop_level (city, region, country or none);
///  - best case only: first_norad_cat_id and last_norad_cat_id.
inline constexpr std::array<std::string_view, 26> kCsvColumns = {
    "utc",
    "from",
    "to",
    "role",
    "pattern",
    "model",
    "status",
    "one_way_ms",
    "round_trip_ms",
    "laser_hops",
    "path_km",
    "uplink_km",
    "laser_km",
    "downlink_km",
    "fibre_km",
    "great_circle_km",
    "lower_bound_ms",
    "stretch",
    "serving_norad_cat_id",
    "gateway_norad_cat_id",
    "gateway",
    "gateway_source",
    "home_pop",
    "home_pop_level",
    "first_norad_cat_id",
    "last_norad_cat_id",
};

/// "city", "region", "country", "none"; a selected pair's direction and the ground counts
/// (shared with the routing program).
using app::GroundCounts;
using app::name_of;
using app::PairDirection;

/// A run whose inputs are loaded and checked: write_rows can no longer fail on them. The
/// loaded inputs are app::Inputs's (inputs.hpp); `options` are the run's, with
/// options.classification replaced by the shells file's when there is one.
struct PreparedRun : app::Inputs {
  Options options;
};

using PreparedRunOrError = std::variant<PreparedRun, std::string>;

/// Statistics of one direction over the run.
struct DirectionStatistics {
  /// Steps by status, indexed by PathStatus.
  std::array<std::size_t, kAllPathStatuses.size()> steps_by_status{};
  std::vector<double> one_way_ms;  ///< Of the ok steps.
  std::vector<double> stretch;     ///< Of the ok steps.
  /// Realistic: steps through each gateway ("name (source)"), and through each laser-hop
  /// count; best case: the hop counts only.
  std::map<std::string, std::size_t> gateway_steps;
  std::map<std::size_t, std::size_t> laser_hop_steps;
  std::size_t lower_bound_failures = 0;
};

/// What stepping through the run found.
struct RunStatistics {
  std::int64_t steps = 0;
  std::size_t rows_written = 0;
  /// Satellite-steps whose propagation failed (those satellites have no links that step).
  std::size_t propagation_failures = 0;
  /// Planned laser links up and down, summed over the steps; down indexed by LinkDownReason.
  std::size_t link_steps_up = 0;
  std::array<std::size_t, topology::kAllLinkDownReasons.size()> link_steps_down{};
  /// Ground links (cities and gateways), summed over the steps.
  std::size_t city_link_steps = 0;
  std::size_t gateway_link_steps = 0;
  /// Physical-check violations summed over the steps (spec 6.1). Must all be zero.
  topology::PhysicalCheckCounts checks;
  /// Ok paths whose delay is below great circle / c. Must be zero.
  std::size_t lower_bound_failures = 0;
  std::vector<DirectionStatistics> directions;  ///< One per PreparedRun::directions.

  [[nodiscard]] bool passed() const noexcept {
    return checks.total() == 0 && lower_bound_failures == 0;
  }
};

/// The outcome of writing a run.
struct RunOutcome {
  std::optional<std::string> error;  ///< The run could not be completed.
  RunStatistics statistics;
};

/// Test seams. Never set by the program; they let a test break a step on purpose to show
/// that the checks catch it, without weakening the checks.
struct RunHooks {
  /// Called at every step after the laser link states are computed and before the ground
  /// links, the physical checks and the paths.
  std::function<void(topology::StepPositions&, topology::StepLinks&)> after_links;
  /// Called on every path result before the lower-bound check.
  std::function<void(PathResult&)> before_lower_bound;
};

/// Loads and checks everything a run needs, writing nothing. With options.shells_file, the
/// shells and excluded groups are read from that file first (PreparedRun::options then holds
/// them). Fails, with a message naming the file, time or value at fault, if: the shells file
/// cannot be read or is not a usable configuration, an option is out of range (step, duration,
/// limits, ground mask, fibre factor, a shell name), the snapshot directory or one of its files
/// is missing, a step has no Earth-orientation values, a ground file is missing or has any bad
/// record (a skipped gateway or city would change the results silently), no pair has the
/// selected role, an element file cannot be loaded as a whole, or options.links_file is refused
/// (topology::read_link_plan_file). Then classifies the snapshot at the start, propagates every
/// plane member there and plans the laser links, or reads them from options.links_file
/// against `working` with the pattern and options.max_lasers.
[[nodiscard]] PreparedRunOrError prepare_run(const Options& requested);

/// Steps through the run: positions, laser link states, ground links, the physical checks
/// (topology::run_physical_checks, every step) and the endpoint model's paths; writes the
/// CSV header and the rows. Returns an error only if the stream fails or a step's graph
/// cannot be built; failed checks are counted (RunStatistics::passed).
[[nodiscard]] RunOutcome write_rows(const PreparedRun& run, std::ostream& csv,
                                    const RunHooks& hooks = {});

/// Writes the human-readable summary of a run.
void write_summary(const PreparedRun& run, const RunStatistics& stats, std::ostream& out);

/// The whole program, minus opening the standard streams: parses `args` (without the program
/// name), prepares, writes the CSV to the --out file or to `out` for "-", and the summary to
/// `out` (to `err` when the CSV goes to `out`). Messages go to `err`. Returns 0 on success, 1
/// if the run fails or a physical check (or a lower bound) fails, 2 for invalid arguments. A
/// file named by --out is only created once the inputs have been checked; the --links-out file
/// is written (from PreparedRun::plan, catalogue numbers from `working`) just before it, and a
/// failure to write it exits 1.
[[nodiscard]] int run_main(std::span<const std::string_view> args, std::ostream& out,
                           std::ostream& err, const RunHooks& hooks = {});

}  // namespace satsim::topology_app
