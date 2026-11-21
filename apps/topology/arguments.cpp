#include "topology/arguments.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <numbers>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <variant>
#include <vector>

#include "common/arguments.hpp"
#include "inputs/input_arguments.hpp"
#include "inputs/inputs.hpp"
#include "satsim/constellation/classification.hpp"
#include "satsim/time/civil_time.hpp"
#include "satsim/time/utc_time.hpp"
#include "satsim/topology/ground_links.hpp"
#include "satsim/topology/link_plan.hpp"
#include "topology/endpoints.hpp"

namespace satsim::topology_app {

namespace {

using app::quote;

constexpr std::string_view kUsage =
    R"(usage: topology --snapshot <dir> --ground <dir> --cities <file> --start <UTC>
                --duration-s <s> --step-s <s> --pattern <three|plus-grid|six>
                --model <realistic|best-case> --out <csv|->
                [--shells <name,...>] [--shells-file <file>]
                [--roles <study|validation|all>]
                [--range-cap-km <km>] [--sun-exclusion-deg <deg>] [--lock-on-s <s>]
                [--fibre-factor <x>] [--high-latitude-mask <on|off>]
                [--serving <highest-elevation>]
                [--min-elevation-deg <deg> | --max-ground-range-km <km>]
                [--links-file <file> [--max-lasers <n>]] [--links-out <file>]
                [--plan-time <UTC>]

Builds the Starlink network at each time step of a saved snapshot (shells and planes,
laser links, ground links) and writes, for every city pair in both directions, the one-way
delay of the shortest path under the chosen endpoint model, with its parts. A summary goes
to standard output, or to standard error when the CSV does. Every step's network is checked
for physical possibility (spec 6.1).

  --snapshot <dir>            directory holding gp.json, supgp.json and finals2000A.all
  --ground <dir>              directory holding gateways.json, pop_codes.json,
                              starlink_pops.csv, starlink_geofeed.csv and optionally
                              assumed_gateways.json (same format as gateways.json)
  --cities <file>             the cities and city pairs (cities.json)
  --start <UTC>               first time, YYYY-MM-DDTHH:MM:SS[.fffffffff], UTC; also the
                              reference time of the shells, planes and laser plan unless
                              --plan-time is given
  --duration-s <s>            length of the run in seconds (decimal, >= 0); the end time is
                              included when it lands on a step
  --step-s <s>                spacing of the times in seconds (decimal, > 0)
  --pattern <name>            laser pattern: three, plus-grid or six
  --model <name>              realistic (one serving satellite, a gateway, fibre through
                              the sender's home PoP) or best-case (any satellites, space only)
  --out <path>                CSV file to write, or - for standard output
  --shells <name,...>         only these shells (default: all; names as 53deg, 43deg, ...)
  --shells-file <file>        shell definitions and excluded groups to classify with, as
                              JSON (constellation/config_file.hpp); default: the built-in
                              shells of the 2026-11-18 snapshot
  --roles <set>               city pairs to compute: study, validation or all (default all)
  --range-cap-km <km>         longest laser link (default: none, line of sight only)
  --sun-exclusion-deg <deg>   a laser link is down while the Sun is within this angle of
                              the beam, seen from either end (default: off)
  --lock-on-s <s>             a laser link that comes back up carries nothing for this long
                              (default 0)
  --fibre-factor <x>          fibre delay over light in a vacuum along the great circle,
                              >= 1 (default 1.98)
  --high-latitude-mask <on|off>
                              5 deg elevation mask above 62 deg latitude (default off)
  --serving <rule>            how a city's serving satellite is chosen: highest-elevation
                              (default, the only rule)
  --min-elevation-deg <deg>   one elevation mask for every site and shell, in [0, 90),
                              replacing the FCC masks; for published setups only
  --max-ground-range-km <km>  a site may use any satellite at most this far away, with no
                              elevation test (Hypatia's rule), replacing the masks; for
                              published setups only. Not with --min-elevation-deg
  --links-file <file>         read the laser links from this file instead of planning them
                              (lines of two catalogue numbers, optionally the two laser
                              sides; docs/phase-3-routing.md 4.9)
  --max-lasers <n>            the most links one satellite may have in --links-file
                              (default: the pattern's laser count)
  --links-out <file>          write the laser links used to this file, in the same format
  --plan-time <UTC>           reference time of the shells, planes and laser plan, same
                              format as --start, before or after it (default: --start)
  --help, -h                  print this text

Each satellite uses its supplemental element set if there is one, else its standard one.
Every time must lie inside the Earth-orientation file and every ground file must load
without errors; otherwise nothing is written.
Exit status: 0 on success, 1 if the run fails or any physical check fails (including a
delay below its lower bound), 2 if the arguments are invalid.
)";

ParsedArguments failure(std::string message) {
  ParsedArguments result;
  result.error = std::move(message);
  return result;
}

// The options that take a value, required ones first, in usage order.
enum Option : std::uint8_t {
  kSnapshot,
  kGround,
  kCities,
  kStart,
  kDuration,
  kStep,
  kPattern,
  kModel,
  kOut,
  kRequiredCount,
  kShells = kRequiredCount,
  kShellsFile,
  kRoles,
  kRangeCap,
  kSunExclusion,
  kLockOn,
  kFibreFactor,
  kHighLatitude,
  kServing,
  kMinElevation,
  kMaxGroundRange,
  kLinksFile,
  kMaxLasers,
  kLinksOut,
  kPlanTime,
  kOptionCount
};
constexpr std::array<std::string_view, kOptionCount> kOptionNames = {"--snapshot",
                                                                     "--ground",
                                                                     "--cities",
                                                                     "--start",
                                                                     "--duration-s",
                                                                     "--step-s",
                                                                     "--pattern",
                                                                     "--model",
                                                                     "--out",
                                                                     "--shells",
                                                                     "--shells-file",
                                                                     "--roles",
                                                                     "--range-cap-km",
                                                                     "--sun-exclusion-deg",
                                                                     "--lock-on-s",
                                                                     "--fibre-factor",
                                                                     "--high-latitude-mask",
                                                                     "--serving",
                                                                     "--min-elevation-deg",
                                                                     "--max-ground-range-km",
                                                                     "--links-file",
                                                                     "--max-lasers",
                                                                     "--links-out",
                                                                     "--plan-time"};

}  // namespace

std::string_view usage() noexcept { return kUsage; }

ParsedArguments parse_arguments(std::span<const std::string_view> args) {
  if (std::any_of(args.begin(), args.end(),
                  [](std::string_view a) { return a == "--help" || a == "-h"; })) {
    ParsedArguments result;
    result.help = true;
    return result;
  }
  const app::OptionValues read = app::read_option_values(args, kOptionNames);
  if (read.error) {
    return failure(*read.error);
  }
  const auto& values = read.values;

  std::string missing;
  for (std::size_t option = 0; option < kRequiredCount; ++option) {
    if (!values.at(option)) {
      missing += (missing.empty() ? "" : ", ") + std::string(kOptionNames.at(option));
    }
  }
  if (!missing.empty()) {
    return failure("missing required option(s): " + missing);
  }
  const auto value = [&](Option option) { return values.at(option).value_or(""); };
  const app::OptionLookup value_of = [&](std::string_view name) -> std::optional<std::string_view> {
    const auto* const it = std::find(kOptionNames.begin(), kOptionNames.end(), name);
    return it == kOptionNames.end()
               ? std::nullopt
               : values.at(static_cast<std::size_t>(it - kOptionNames.begin()));
  };

  for (const Option path_option : {kSnapshot, kGround, kCities, kOut}) {
    if (value(path_option).empty()) {
      return failure(std::string(kOptionNames.at(path_option)) + " is empty" +
                     (path_option == kOut ? " (use - for standard output)" : ""));
    }
  }
  std::variant<app::InputOptions, std::string> shared = app::parse_input_options(value_of);
  if (auto* error = std::get_if<std::string>(&shared)) {
    return failure(std::move(*error));
  }
  const app::InputOptions& inputs = std::get<app::InputOptions>(shared);
  Options options{.snapshot_dir = inputs.snapshot_dir,
                  .ground_dir = inputs.ground_dir,
                  .cities_file = inputs.cities_file,
                  .start = inputs.start,
                  .shells = {},
                  .out = std::string(value(kOut))};
  app::assign_input_options(options, inputs);

  const std::optional<EndpointModel> model = parse_endpoint_model(value(kModel));
  if (!model) {
    return failure("--model " + quote(value(kModel)) + " is not realistic or best-case");
  }
  options.model = *model;
  if (const std::optional<std::string_view> serving = values[kServing];
      serving && *serving != "highest-elevation") {
    return failure("--serving " + quote(*serving) + " is not highest-elevation");
  }
  if (const std::optional<std::string_view> links_out = values[kLinksOut]) {
    if (links_out->empty()) {
      return failure("--links-out is empty");
    }
    options.links_out = std::filesystem::path(*links_out);
  }

  ParsedArguments result;
  result.options = std::move(options);
  return result;
}

}  // namespace satsim::topology_app
