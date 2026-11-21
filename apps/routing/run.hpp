#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <iosfwd>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "inputs/inputs.hpp"
#include "inputs/step_source.hpp"
#include "routing/arguments.hpp"
#include "routing/engine.hpp"
#include "routing/graph.hpp"
#include "satsim/topology/link_state.hpp"
#include "satsim/topology/physical_checks.hpp"

namespace satsim::routing_app {

/// The routes CSV columns, in order (spec 4.8). One row per time step, direction and strategy,
/// ordered by time, then direction (pairs in city-file order, from-to before to-from), then
/// strategy (--strategies order). A field is empty when it does not apply.
///  - utc; from, to; role; pattern; model; strategy (its --strategies spelling);
///  - status: ok or drop; drop_cause: one of spec 4.6 on a drop;
///  - one_way_ms: the held route's delay (ok); ideal_ms: the lowest delay at this step, with
///    the sender's dish free to use any satellite it sees (empty when there is none);
///  - laser_hops, route (the route's satellites as catalogue numbers, separated by spaces),
///    first_norad_cat_id and last_norad_cat_id (its access satellites), gateway and
///    gateway_source (realistic): on ok rows;
///  - serving_norad_cat_id: realistic, the dish's serving satellite in this slot;
///  - home_pop: realistic, the sender's PoP code;
///  - plan: at a re-plan instant what the plan did (kept, replaced, none), empty otherwise;
///  - backup_ms: with --backup on, at a re-plan instant with a route, the backup path's delay.
inline constexpr std::array<std::string_view, 21> kRoutesColumns = {"utc",
                                                                    "from",
                                                                    "to",
                                                                    "role",
                                                                    "pattern",
                                                                    "model",
                                                                    "strategy",
                                                                    "status",
                                                                    "drop_cause",
                                                                    "one_way_ms",
                                                                    "ideal_ms",
                                                                    "laser_hops",
                                                                    "route",
                                                                    "first_norad_cat_id",
                                                                    "last_norad_cat_id",
                                                                    "serving_norad_cat_id",
                                                                    "gateway",
                                                                    "gateway_source",
                                                                    "home_pop",
                                                                    "plan",
                                                                    "backup_ms"};

/// The drops CSV columns: one row per drop event, ordered by start, then direction, then
/// strategy. start_utc and length_s (steps × step); the direction and strategy as in the
/// routes CSV; the cause; what was responsible at the event's first step: a satellite, a laser
/// link (its two catalogue numbers and why it is down) or a site (its name, and city or
/// gateway).
inline constexpr std::array<std::string_view, 15> kDropsColumns = {"start_utc",
                                                                   "length_s",
                                                                   "from",
                                                                   "to",
                                                                   "role",
                                                                   "pattern",
                                                                   "model",
                                                                   "strategy",
                                                                   "cause",
                                                                   "satellite_norad_cat_id",
                                                                   "link_norad_cat_id_a",
                                                                   "link_norad_cat_id_b",
                                                                   "link_down_reason",
                                                                   "site",
                                                                   "site_kind"};

/// A run whose inputs are loaded and checked. With --directions forward, `directions` holds
/// only each pair's from-to.
struct PreparedRun : app::Inputs {
  Options options;
};

using PreparedRunOrError = std::variant<PreparedRun, std::string>;

/// What stepping through the run found.
struct RunStatistics {
  std::int64_t steps = 0;
  std::size_t rows_written = 0;
  std::size_t propagation_failures = 0;
  std::size_t link_steps_up = 0;
  std::array<std::size_t, topology::kAllLinkDownReasons.size()> link_steps_down{};
  std::size_t city_link_steps = 0;
  std::size_t gateway_link_steps = 0;
  topology::PhysicalCheckCounts physical;
  RouteChecks routes;
  /// [direction * strategies + strategy].
  std::vector<Measures> measures;
  /// Per direction: the ideal delay at every step that has one, ms.
  std::vector<std::vector<double>> ideal_ms;
  /// Sorted as the drops CSV.
  std::vector<DropEvent> drop_events;

  [[nodiscard]] bool passed() const noexcept {
    return physical.total() == 0 && routes.total() == 0;
  }
};

struct RunOutcome {
  std::optional<std::string> error;
  RunStatistics statistics;
};

/// Test seams; never set by the program.
struct RunHooks {
  app::AfterLinks after_links;
  BeforeChecks before_checks;
};

/// Loads and checks everything (app::prepare_inputs, with the look-ahead the options need).
/// Fails if an option is out of range (strategies empty, re-plan interval or slot not a
/// positive multiple of the step, hop delay negative or not finite, everything prepare_inputs
/// refuses).
[[nodiscard]] PreparedRunOrError prepare_run(const Options& options);

/// --directions forward: keeps each pair's from-to (the even directions) and sets
/// options.both_directions to false. prepare_run calls it; tests may call it on a run prepared
/// with both directions.
void keep_forward_directions(PreparedRun& run);

/// The engine's view of a prepared run: the directions, and in the realistic model the end
/// delays, gateway g to direction d's end node: ground::fibre_delay_s from g to the sender's
/// home PoP plus ground::fibre_delay_s from the PoP to the destination (great-circle distances
/// on the mean sphere, the run's fibre factor), summed in that order as the topology program
/// sums them.
[[nodiscard]] EngineNetwork network_of(const PreparedRun& run);

/// One row of the routes CSV without its line end: kRoutesColumns in order for `row`, at the
/// step written `time_text`, under the strategy spelled `strategy_name`. `layout` numbers the
/// row's route. (write_routes writes every row with it; the failures program too.)
void append_route_columns(std::string& line, const PreparedRun& run, const GraphLayout& layout,
                          std::string_view time_text, std::string_view strategy_name,
                          const RouteRow& row);

/// Steps through the run once for every strategy and writes the routes CSV.
[[nodiscard]] RunOutcome write_routes(const PreparedRun& run, std::ostream& csv,
                                      const RunHooks& hooks = {});

/// Writes the drops CSV. Returns false if the stream fails.
bool write_drops(const PreparedRun& run, const RunStatistics& stats, std::ostream& csv);

/// The human-readable summary without its last line ("  checks: PASSED" or "FAILED").
void write_summary_body(const PreparedRun& run, const RunStatistics& stats, std::ostream& out);

/// Writes the human-readable summary: write_summary_body, then the checks line.
void write_summary(const PreparedRun& run, const RunStatistics& stats, std::ostream& out);

/// The whole program: parses `args`, prepares, writes the routes CSV to --out (or `out` for
/// "-"), the drops CSV to --drops-out, the summary to `out` (to `err` when the CSV goes to
/// `out`), messages to `err`. Returns 0 on success, 1 if a check fails, 2 for invalid
/// arguments or inputs, or output that cannot be written.
[[nodiscard]] int run_main(std::span<const std::string_view> args, std::ostream& out,
                           std::ostream& err, const RunHooks& hooks = {});

}  // namespace satsim::routing_app
