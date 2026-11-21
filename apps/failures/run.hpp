#pragma once

#include <array>
#include <cstddef>
#include <iosfwd>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "failures/arguments.hpp"
#include "failures/loop.hpp"
#include "failures/measures.hpp"
#include "failures/reports.hpp"
#include "failures/timeline.hpp"
#include "inputs/step_source.hpp"
#include "routing/engine.hpp"
#include "routing/run.hpp"
#include "satsim/failure/schedule.hpp"
#include "satsim/topology/link_state.hpp"
#include "satsim/topology/physical_checks.hpp"

// The failures program (Phase 4 spec 4.10): the routing program's run, with a failure schedule
// worked out before the run (timeline.hpp) and every lane stepped through it (loop.hpp).

namespace satsim::failures_app {

/// The routes CSV: the routing program's columns (routing_app::kRoutesColumns, with the lane's
/// status at the step's instant: status ok exactly when delivered), then these. One row per
/// step, direction, strategy and lane, ordered by time, direction, strategy, then lane
/// (baseline, reference, then the reaction modes as listed).
inline constexpr std::array<std::string_view, 3> kRoutesExtraColumns = {"reaction", "state",
                                                                        "failed_element"};

/// The events CSV (spec 4.10 Details): one row per interval, ordered by direction, strategy,
/// lane, then start. drop_cause on every state but delivered; failed_element on a black hole;
/// one_way_ms when delivered; route (its satellites' catalogue numbers, separated by spaces) and
/// gateway (the route's) whenever a route is held; serving_norad_cat_id realistic.
inline constexpr std::array<std::string_view, 16> kEventsColumns = {
    "start_s",    "end_s",          "from",       "to",       "role",
    "pattern",    "model",          "strategy",   "reaction", "state",
    "drop_cause", "failed_element", "one_way_ms", "route",    "serving_norad_cat_id",
    "gateway"};

/// The facts CSV (spec 4.10 Details); the three flood columns are empty (no flooding yet).
inline constexpr std::array<std::string_view, 12> kFactsColumns = {
    "fact",        "subject",     "state",           "t_s",
    "old",         "detectors",   "planner_known_s", "reports",
    "report_hops", "flood_nodes", "flood_last_s",    "messages"};

/// The measures CSV (spec 4.10 Details): one row per direction, strategy and reaction mode.
/// loop_s is none (no flooding yet); reports and messages are the run's: every fact's reports
/// and the hops of all of them.
inline constexpr std::array<std::string_view, 19> kMeasuresColumns = {"from",
                                                                      "to",
                                                                      "role",
                                                                      "pattern",
                                                                      "model",
                                                                      "strategy",
                                                                      "reaction",
                                                                      "failure_s",
                                                                      "black_hole_s",
                                                                      "loop_s",
                                                                      "lost_s",
                                                                      "restore_s",
                                                                      "settle_s",
                                                                      "extra_min_ms",
                                                                      "extra_median_ms",
                                                                      "extra_max_ms",
                                                                      "extra_settled_ms",
                                                                      "reports",
                                                                      "messages"};

/// The network CSV (spec 4.10 Details): per step, scope `all` then each used shell by name:
/// the plane members of the scope that are not down, the pieces their laser links that carry
/// traffic in the truth join them into (network::connected_pieces), the largest piece and its
/// share of the working satellites (none without one).
inline constexpr std::array<std::string_view, 6> kNetworkColumns = {
    "utc", "scope", "working_satellites", "pieces", "largest_piece", "largest_piece_share"};

/// A run whose inputs, schedule and timeline are ready.
struct FailureRun {
  /// The routing program's prepared run, with the original laser plan.
  routing_app::PreparedRun run;
  Options options;
  failure::FailureSchedule schedule;
  TimelineSettings timeline_settings;
  Timeline timeline;
  std::vector<FactReports> reports;
};

using FailureRunOrError = std::variant<FailureRun, std::string>;

/// What stepping through the run found.
struct FailureStatistics {
  /// The routing program's statistics of the baseline lane (its rows_written counts every row).
  routing_app::RunStatistics routing;
  /// The Phase 3 route checks of every other lane, summed.
  routing_app::RouteChecks other_routes;
  FailureChecks failures;
  topology::ForceCheckCounts force;
  /// Link-steps per force (topology::kAllLinkForces order), over every run link.
  std::array<std::size_t, topology::kAllLinkForces.size()> link_steps_by_force{};
  std::vector<LaneKind> kinds;
  std::vector<Interval> intervals;
  /// [(e * D + d) * S + s].
  std::vector<LaneMeasures> measures;
  std::optional<double> t_f;
  std::size_t repairs = 0;

  [[nodiscard]] bool passed() const noexcept {
    return routing.passed() && other_routes.total() == 0 && failures.total() == 0 &&
           force.total() == 0;
  }
};

struct FailureOutcome {
  std::optional<std::string> error;
  FailureStatistics statistics;
};

/// Test seams; never set by the program.
struct FailureHooks {
  LoopHooks loop;
  app::AfterLinks after_links;
};

/// Loads and checks everything: routing_app::prepare_run on options.routing, the reaction times,
/// the failure file (failure::read_failure_schedule_file with the run's gateway names, start
/// and step; none: an empty schedule), the timeline on the snapshot's geometry
/// (SnapshotGeometry, build_timeline) and every fact's reports.
[[nodiscard]] FailureRunOrError prepare_failure_run(const Options& options);

/// Steps through the run once for every lane; writes the routes CSV and, if `network` is set,
/// the network CSV.
[[nodiscard]] FailureOutcome write_failure_routes(const FailureRun& run, std::ostream& routes,
                                                  std::ostream* network,
                                                  const FailureHooks& hooks = {});

/// The events, facts and measures CSVs. Each returns false if the stream fails.
bool write_events(const FailureRun& run, const FailureStatistics& stats, std::ostream& out);
bool write_facts(const FailureRun& run, std::ostream& out);
bool write_measures(const FailureRun& run, const FailureStatistics& stats, std::ostream& out);

/// The summary: the routing program's for the baseline lane, then the failures section, then
/// "  checks: PASSED" or "FAILED".
void write_failure_summary(const FailureRun& run, const FailureStatistics& stats,
                           std::ostream& out);

/// An element as the CSVs write it: satellite:<catalogue number>, link:<a>-<b> (lower first),
/// gateway:<name> (not CSV-quoted).
[[nodiscard]] std::string element_text(const FailureRun& run, const Subject& subject);

/// The one place that refuses what is not available yet: a reaction list holding flooding.
[[nodiscard]] std::optional<std::string> not_available_yet(const Options& options);

/// The whole program, as routing_app::run_main: the routes CSV to --out (or `out` for "-"),
/// the other CSVs where asked, the summary to `out` (to `err` when the CSV goes to `out`).
/// Returns 0, 1 if a check fails, 2 for invalid arguments or inputs, an unavailable reaction
/// mode, or output that cannot be written.
[[nodiscard]] int run_main(std::span<const std::string_view> args, std::ostream& out,
                           std::ostream& err, const FailureHooks& hooks = {});

}  // namespace satsim::failures_app
