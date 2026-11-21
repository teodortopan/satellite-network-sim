#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "inputs/inputs.hpp"
#include "routing/settings.hpp"
#include "satsim/constellation/classification.hpp"
#include "satsim/ground/distance.hpp"
#include "satsim/routing/strategy.hpp"
#include "satsim/time/utc_time.hpp"
#include "satsim/topology/geometry.hpp"
#include "satsim/topology/link_plan.hpp"
#include "satsim/topology/link_state.hpp"

namespace satsim::routing_app {

using app::RoleFilter;

/// What one run of the routing program computes and where it writes it (spec 4.8). Built by
/// parse_arguments, which has checked every value; tests may also fill one in directly
/// (prepare_run checks again what it relies on). The members up to max_lasers are those of
/// app::InputOptions, with the same meaning.
struct Options {
  std::filesystem::path snapshot_dir;
  std::filesystem::path ground_dir;
  std::filesystem::path cities_file;
  time::UtcTime start;
  std::int64_t duration_ns = 0;
  std::int64_t step_ns = 0;
  topology::LaserPattern pattern = topology::LaserPattern::kThree;
  std::vector<std::string> shells;
  RoleFilter roles = RoleFilter::kAll;
  topology::LinkLimits limits{};
  double fibre_factor = ground::kDefaultFibreRouteInflation;
  topology::ElevationMaskSettings mask{};
  constellation::ClassificationConfig classification = constellation::default_config();
  std::optional<std::filesystem::path> shells_file = std::nullopt;
  std::optional<std::filesystem::path> links_file = std::nullopt;
  std::optional<std::size_t> max_lasers = std::nullopt;

  app::EndpointModel model = app::EndpointModel::kRealistic;
  /// --strategies, in the order given (each at most once).
  std::vector<routing::Strategy> strategies;
  /// --replan-s and --serving-slot-s: positive multiples of step_ns.
  std::int64_t replan_ns = 0;
  std::int64_t slot_ns = 15'000'000'000;
  Planner planner = Planner::kStart;
  ServingRule serving = ServingRule::kHighestElevation;
  bool backup = false;
  /// --hop-delay-ms, in seconds: added to every radio and laser edge.
  double hop_delay_s = 0.0;
  /// --directions: both directions of every pair (default), or only the file's from-to.
  bool both_directions = true;
  /// The routes CSV, or "-" for standard output.
  std::string out;
  /// --drops-out: the drops CSV (not written when unset).
  std::optional<std::filesystem::path> drops_out = std::nullopt;
  /// --links-out: where to write the laser links used.
  std::optional<std::filesystem::path> links_out = std::nullopt;
};

/// The outcome of parse_arguments: exactly one of options, help or error.
struct ParsedArguments {
  std::optional<Options> options;
  bool help = false;
  std::optional<std::string> error;
};

/// Parses the command line, without the program name. --help or -h anywhere wins. Rejected,
/// besides everything app::parse_input_options rejects: unknown or repeated options, a missing
/// value or required option, an empty --out, --drops-out or --links-out, a --drops-out of "-",
/// an unknown model, planner, serving rule, backup setting or direction set, a strategy list
/// that is empty, has an empty item, an unknown strategy or one named twice, a re-plan interval
/// or serving slot that is not a positive number of seconds (at most 9 decimals) or not a whole
/// multiple of the step, and a hop delay that is not a finite number of milliseconds >= 0.
[[nodiscard]] ParsedArguments parse_arguments(std::span<const std::string_view> args);

/// The --help text.
[[nodiscard]] std::string_view usage() noexcept;

}  // namespace satsim::routing_app
