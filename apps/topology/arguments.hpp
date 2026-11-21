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
#include "satsim/constellation/classification.hpp"
#include "satsim/ground/distance.hpp"
#include "satsim/time/utc_time.hpp"
#include "satsim/topology/geometry.hpp"
#include "satsim/topology/ground_links.hpp"
#include "satsim/topology/link_plan.hpp"
#include "satsim/topology/link_state.hpp"
#include "topology/endpoints.hpp"

namespace satsim::topology_app {

/// Which city pairs of the city file a run computes (shared with the routing program).
using app::name_of;
using app::RoleFilter;

/// What one run of the program computes and where it writes it. Built by parse_arguments,
/// which has checked every value; tests may also fill one in directly (prepare_run checks
/// again what it relies on).
struct Options {
  /// Directory holding gp.json, supgp.json and finals2000A.all.
  std::filesystem::path snapshot_dir;
  /// Directory holding gateways.json, pop_codes.json, starlink_pops.csv, starlink_geofeed.csv
  /// and optionally assumed_gateways.json.
  std::filesystem::path ground_dir;
  /// The cities and pairs (cities.json).
  std::filesystem::path cities_file;
  /// First time step, and the reference time of the classification and the link plan.
  time::UtcTime start;
  /// Length of the run and spacing of its steps, as in propagate (app::step_count).
  std::int64_t duration_ns = 0;
  std::int64_t step_ns = 0;
  topology::LaserPattern pattern = topology::LaserPattern::kThree;
  EndpointModel model = EndpointModel::kRealistic;
  /// Names of the shells of `classification` to use; empty means every shell. Satellites of
  /// the other shells carry no traffic.
  std::vector<std::string> shells;
  RoleFilter roles = RoleFilter::kAll;
  /// Range cap, Sun exclusion and lock-on time (spec 4.5); the atmosphere layer stays at its
  /// fixed 80 km.
  topology::LinkLimits limits{};
  /// Fibre delay over light in a vacuum along the great circle; finite and >= 1.
  double fibre_factor = ground::kDefaultFibreRouteInflation;
  /// The 5 deg allowance above 62 deg latitude (off by default), or one of the two overrides
  /// for published setups: one minimum elevation (--min-elevation-deg) or a longest
  /// dish-to-satellite distance with no elevation test (--max-ground-range-km).
  topology::ElevationMaskSettings mask{};
  topology::ServingRule serving_rule = topology::ServingRule::kHighestElevation;
  /// Path of the CSV file to write, or "-" for standard output.
  std::string out;
  /// Shells, planes and exclusions: the default, unless shells_file is set (prepare_run then
  /// replaces it with the file's). Tests may also set their own.
  constellation::ClassificationConfig classification = constellation::default_config();
  /// --shells-file: shell definitions to read (constellation::load_classification_config).
  std::optional<std::filesystem::path> shells_file = std::nullopt;
  /// --links-file: the laser links to use instead of planning them
  /// (topology::read_link_plan_file, against the classification of the used shells).
  std::optional<std::filesystem::path> links_file = std::nullopt;
  /// --max-lasers: the most links one satellite may have in links_file; nullopt means the
  /// pattern's laser count. Only with links_file.
  std::optional<std::size_t> max_lasers = std::nullopt;
  /// --links-out: where to write the laser links used (topology::write_link_plan_file).
  std::optional<std::filesystem::path> links_out = std::nullopt;
};

/// The outcome of parse_arguments: exactly one of options, help or error.
struct ParsedArguments {
  std::optional<Options> options;    ///< Set when the arguments are valid.
  bool help = false;                 ///< --help or -h was given: print usage() and exit 0.
  std::optional<std::string> error;  ///< Why the arguments were rejected, for the user.
};

/// Parses the command line, without the program name. Options take their value as the next
/// argument. --help or -h anywhere wins over any other problem. Rejected: unknown or repeated
/// options, a missing value, a missing required option, an empty path, a start time that is
/// not YYYY-MM-DDTHH:MM:SS[.fffffffff] or does not exist, a duration or step that is not a
/// plain decimal number of seconds (at most 9 decimals, step > 0, at most 1e9 s), an unknown
/// pattern, model, role set, serving rule or shell name (or a shell named twice; without
/// --shells-file, the names must be default shells, otherwise they are checked by prepare_run
/// against the file), an empty --shells-file, a range cap
/// that is not a finite number > 0 km, a Sun exclusion angle not in (0, 180] degrees, a
/// lock-on time that is not a plain decimal number of seconds, a fibre factor that is not a
/// finite number >= 1, a high-latitude setting other than on or off, a minimum elevation not
/// in [0, 90) degrees, a longest ground range that is not a finite number > 0 km, both of
/// those, either with the high-latitude mask on, an empty --links-file or --links-out, and a
/// --max-lasers that is not a whole number from 1 to 1000 or is given without --links-file.
[[nodiscard]] ParsedArguments parse_arguments(std::span<const std::string_view> args);

/// The --help text.
[[nodiscard]] std::string_view usage() noexcept;

}  // namespace satsim::topology_app
