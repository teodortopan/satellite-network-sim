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

/// The ground-data directory's files (written by python/fetch_ground.py). assumed_gateways.json
/// is optional: gateways assumed to exist where the community list has none, in the format of
/// gateways.json.
inline constexpr std::string_view kGatewaysFileName = "gateways.json";
inline constexpr std::string_view kAssumedGatewaysFileName = "assumed_gateways.json";
inline constexpr std::string_view kPopCodesFileName = "pop_codes.json";
inline constexpr std::string_view kPopBlocksFileName = "starlink_pops.csv";
inline constexpr std::string_view kGeofeedFileName = "starlink_geofeed.csv";

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

/// "city", "region", "country", "none".
[[nodiscard]] std::string_view name_of(ground::HomePopLevel level) noexcept;

/// One direction of a selected city pair.
struct PairDirection {
  std::size_t from = 0;  ///< Index into PreparedRun::cities.
  std::size_t to = 0;
  ground::PairRole role = ground::PairRole::kStudy;
  double great_circle_km = 0.0;  ///< On the mean sphere (ground::great_circle_...).
  double lower_bound_s = 0.0;    ///< great_circle_km / c.
};

/// Every count of the ground data that the summary reports.
struct GroundCounts {
  std::size_t pairs_in_file = 0;
  std::size_t real_gateways = 0;  ///< Records of gateways.json, any status.
  std::size_t real_operational = 0;
  bool assumed_file = false;  ///< assumed_gateways.json exists.
  std::size_t assumed_gateways = 0;
  std::size_t assumed_operational = 0;
  std::size_t pops = 0;
  std::size_t pop_blocks = 0;
  std::size_t geofeed_blocks = 0;
  ground::PopAssignment assignment;
};

/// A run whose inputs are loaded and checked: write_rows can no longer fail on them.
struct PreparedRun {
  Options options;
  std::int64_t time_count = 0;  ///< Number of time steps, at least 1.
  int fraction_digits = 0;      ///< Fraction digits of the utc column.
  std::string first_time;
  std::string last_time;
  time::EarthOrientationTable earth_orientation;  ///< Covers every step.

  orbit::ElementFileLoad standard_file{};
  orbit::ElementFileLoad supplemental_file{};
  std::size_t superseded = 0;
  /// The classification of every satellite at the start, with options.classification.
  constellation::Classification classification{};
  /// Per shell of options.classification: whether the run uses it (options.shells).
  std::vector<bool> shell_used;
  /// `classification` with the planes of unused shells removed (their satellites then carry
  /// no traffic: they are given ExclusionReason::kNoPlane here, and only here). Everything
  /// that steps through time uses this one.
  constellation::Classification working{};
  /// One per satellite of `working`: set for the plane members.
  std::vector<std::optional<orbit::Propagator>> propagators;
  /// Positions failed at the start (the reference time of the plan).
  std::size_t start_propagation_failures = 0;
  topology::LinkPlan plan{};

  /// Every city of the city file, in file order, and each one's home PoP.
  std::vector<ground::City> cities;
  std::vector<ground::HomePop> home_pops;
  /// The operational gateways: gateways.json's in file order, then assumed_gateways.json's.
  std::vector<GatewaySite> gateways;
  GroundCounts ground{};
  /// Two per selected pair: from-to, then to-from.
  std::vector<PairDirection> directions;
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
/// limits, fibre factor, a shell name), the snapshot directory or one of its files is missing, a
/// step has no Earth-orientation values, a ground file is missing or has any bad record
/// (a skipped gateway or city would change the results silently), no pair has the selected
/// role, or an element file cannot be loaded as a whole. Then classifies the snapshot at the
/// start, propagates every plane member there and plans the laser links.
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
/// file named by --out is only created once the inputs have been checked.
[[nodiscard]] int run_main(std::span<const std::string_view> args, std::ostream& out,
                           std::ostream& err, const RunHooks& hooks = {});

}  // namespace satsim::topology_app
