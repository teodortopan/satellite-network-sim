#include "routing/arguments.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "common/arguments.hpp"
#include "inputs/input_arguments.hpp"
#include "inputs/inputs.hpp"
#include "routing/settings.hpp"
#include "satsim/routing/strategy.hpp"

namespace satsim::routing_app {

namespace {

using app::quote;

constexpr double kMsPerS = 1000.0;

constexpr std::string_view kUsage =
    R"(usage: routing --snapshot <dir> --ground <dir> --cities <file> --start <UTC>
               --duration-s <s> --step-s <s> --pattern <three|plus-grid|six>
               --model <realistic|best-case> --strategies <name,...> --replan-s <s>
               --out <csv|->
               [--planner <start|whole-interval>]
               [--serving <highest-elevation|longest-in-view|highest-whole-slot|with-route>]
               [--serving-slot-s <s>] [--backup <on|off>] [--hop-delay-ms <ms>]
               [--directions <both|forward>] [--drops-out <csv>]
               [--shells <name,...>] [--shells-file <file>]
               [--roles <study|validation|all>]
               [--range-cap-km <km>] [--sun-exclusion-deg <deg>] [--lock-on-s <s>]
               [--fibre-factor <x>] [--high-latitude-mask <on|off>]
               [--min-elevation-deg <deg> | --max-ground-range-km <km>]
               [--links-file <file> [--max-lasers <n>]] [--links-out <file>]
               [--plan-time <UTC>]

Follows the route of every city pair through time (docs/phase-3-routing.md 4.8): at each
re-plan instant every listed strategy keeps or replaces its route, between plans the route is
held, and every step reports whether it still works, its delay against the ideal, and why it
dropped. One pass over time evaluates every strategy. A summary goes to standard output, or to
standard error when the routes CSV does. Every step is checked (spec 6.1).

  --snapshot <dir>            directory holding gp.json, supgp.json and finals2000A.all
  --ground <dir>              directory holding gateways.json, pop_codes.json,
                              starlink_pops.csv, starlink_geofeed.csv and optionally
                              assumed_gateways.json (same format as gateways.json)
  --cities <file>             the cities and city pairs (cities.json)
  --start <UTC>               first time, YYYY-MM-DDTHH:MM:SS[.fffffffff], UTC; also the
                              reference time of the shells, planes and laser plan unless
                              --plan-time is given
  --duration-s <s>            length of the run in seconds (decimal, >= 0)
  --step-s <s>                spacing of the times in seconds (decimal, > 0)
  --pattern <name>            laser pattern: three, plus-grid or six
  --model <name>              realistic (one serving satellite, a gateway, fibre through
                              the sender's home PoP) or best-case (any satellites, space only)
  --strategies <list>         comma-separated: lowest-delay, keep-until-broken,
                              longest-lived, threshold:<n>ms, threshold:<n>%
  --replan-s <s>              re-plan interval, a whole multiple of the step
  --out <path>                routes CSV to write, or - for standard output
  --planner <name>            start (default: the graph of the re-plan step) or
                              whole-interval (only edges that last until the next plan)
  --serving <rule>            realistic: how a dish picks its serving satellite at a slot
                              start: highest-elevation (default), longest-in-view,
                              highest-whole-slot or with-route
  --serving-slot-s <s>        how long a dish keeps its serving satellite, a whole multiple
                              of the step (default 15)
  --backup <on|off>           at each re-plan, also find the lowest-delay route sharing no
                              satellite with the held one (default off)
  --hop-delay-ms <ms>         added to every radio and laser link (default 0)
  --directions <set>          both directions of every pair (default) or forward (the
                              file's from-to only)
  --drops-out <path>          drops CSV to write, one row per drop event
  --shells <name,...>         only these shells (default: all)
  --shells-file <file>        shell definitions and excluded groups to classify with, as JSON
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
  --min-elevation-deg <deg>   one elevation mask for every site and shell, in [0, 90),
                              replacing the FCC masks; for published setups only
  --max-ground-range-km <km>  a site may use any satellite at most this far away, with no
                              elevation test (Hypatia's rule); for published setups only
  --links-file <file>         read the laser links from this file instead of planning them
  --max-lasers <n>            the most links one satellite may have in --links-file
                              (default: the pattern's laser count)
  --links-out <file>          write the laser links used to this file, in the same format
  --plan-time <UTC>           reference time of the shells, planes and laser plan, same
                              format as --start, before or after it (default: --start)
  --help, -h                  print this text

Exit status: 0 on success, 1 if any check fails, 2 if the arguments or inputs are invalid
or the output cannot be written.
)";

ParsedArguments failure(std::string message) {
  ParsedArguments result;
  result.error = std::move(message);
  return result;
}

// The routing program's own options, required ones first.
constexpr std::array<std::string_view, 4> kRequiredOwn = {"--model", "--strategies", "--replan-s",
                                                          "--out"};
constexpr std::array<std::string_view, 8> kOptionalOwn = {
    "--planner",      "--serving",    "--serving-slot-s", "--backup",
    "--hop-delay-ms", "--directions", "--drops-out",      "--links-out"};

// Every option name: the shared required ones, ours, the shared optional ones, ours.
std::vector<std::string_view> option_names() {
  std::vector<std::string_view> names;
  for (const auto& group : {std::span<const std::string_view>(app::kRequiredInputOptionNames),
                            std::span<const std::string_view>(kRequiredOwn),
                            std::span<const std::string_view>(app::kOptionalInputOptionNames),
                            std::span<const std::string_view>(kOptionalOwn)}) {
    for (const std::string_view name : group) {
      names.push_back(name);
    }
  }
  return names;
}

// "lowest-delay,threshold:2ms": each a strategy, none twice.
std::optional<std::vector<routing::Strategy>> parse_strategies(std::string_view text,
                                                               std::string& error) {
  std::vector<routing::Strategy> strategies;
  std::size_t begin = 0;
  while (true) {
    const std::size_t comma = text.find(',', begin);
    const std::string_view item = text.substr(
        begin, comma == std::string_view::npos ? std::string_view::npos : comma - begin);
    routing::StrategyOrError parsed = routing::parse_strategy(item);
    if (const auto* message = std::get_if<std::string>(&parsed)) {
      error = "--strategies: " + *message;
      return std::nullopt;
    }
    const routing::Strategy strategy = std::get<routing::Strategy>(parsed);
    if (std::find(strategies.begin(), strategies.end(), strategy) != strategies.end()) {
      error = "--strategies: " + quote(item) + " is named twice";
      return std::nullopt;
    }
    strategies.push_back(strategy);
    if (comma == std::string_view::npos) {
      break;
    }
    begin = comma + 1;
  }
  return strategies;
}

// A positive whole multiple of step_ns, from a number of seconds.
std::optional<std::int64_t> parse_multiple(std::string_view name, std::string_view text,
                                           std::int64_t step_ns, std::string& error) {
  const std::optional<std::int64_t> ns = app::parse_seconds_as_ns(text);
  if (!ns || *ns == 0) {
    error = std::string(name) + " " + quote(text) +
            " is not a positive number of seconds up to 1000000000 with at most 9 decimals";
    return std::nullopt;
  }
  if (*ns % step_ns != 0) {
    error = std::string(name) + " " + quote(text) + " is not a whole multiple of the step";
    return std::nullopt;
  }
  return ns;
}

}  // namespace

std::string_view usage() noexcept { return kUsage; }

ParsedArguments parse_arguments(std::span<const std::string_view> args) {
  if (std::any_of(args.begin(), args.end(),
                  [](std::string_view a) { return a == "--help" || a == "-h"; })) {
    ParsedArguments result;
    result.help = true;
    return result;
  }
  const std::vector<std::string_view> names = option_names();
  const app::OptionValues read = app::read_option_values(args, names);
  if (read.error) {
    return failure(*read.error);
  }
  const app::OptionLookup value_of = [&](std::string_view name) -> std::optional<std::string_view> {
    const auto it = std::find(names.begin(), names.end(), name);
    return it == names.end() ? std::nullopt
                             : read.values.at(static_cast<std::size_t>(it - names.begin()));
  };
  std::string missing;
  const std::size_t required = app::kRequiredInputOptionNames.size() + kRequiredOwn.size();
  for (std::size_t i = 0; i < required; ++i) {
    if (!read.values.at(i)) {
      missing += (missing.empty() ? "" : ", ") + std::string(names.at(i));
    }
  }
  if (!missing.empty()) {
    return failure("missing required option(s): " + missing);
  }
  const auto value = [&](std::string_view name) { return value_of(name).value_or(""); };

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
                  .strategies = {},
                  .out = {}};
  app::assign_input_options(options, inputs);

  if (value("--out").empty()) {
    return failure("--out is empty (use - for standard output)");
  }
  options.out = std::string(value("--out"));
  const std::optional<app::EndpointModel> model = app::parse_endpoint_model(value("--model"));
  if (!model) {
    return failure("--model " + quote(value("--model")) + " is not realistic or best-case");
  }
  options.model = *model;
  std::string error;
  auto strategies = parse_strategies(value("--strategies"), error);
  if (!strategies) {
    return failure(std::move(error));
  }
  options.strategies = std::move(*strategies);
  const auto replan_ns = parse_multiple("--replan-s", value("--replan-s"), options.step_ns, error);
  if (!replan_ns) {
    return failure(std::move(error));
  }
  options.replan_ns = *replan_ns;
  if (const auto text = value_of("--serving-slot-s")) {
    const auto slot_ns = parse_multiple("--serving-slot-s", *text, options.step_ns, error);
    if (!slot_ns) {
      return failure(std::move(error));
    }
    options.slot_ns = *slot_ns;
  } else if (options.slot_ns % options.step_ns != 0) {
    return failure(
        "the default serving slot of 15 s is not a whole multiple of the step; give "
        "--serving-slot-s");
  }
  if (const auto text = value_of("--planner")) {
    const std::optional<Planner> planner = parse_planner(*text);
    if (!planner) {
      return failure("--planner " + quote(*text) + " is not start or whole-interval");
    }
    options.planner = *planner;
  }
  if (const auto text = value_of("--serving")) {
    const std::optional<ServingRule> rule = parse_serving_rule(*text);
    if (!rule) {
      return failure("--serving " + quote(*text) +
                     " is not highest-elevation, longest-in-view, highest-whole-slot or "
                     "with-route");
    }
    options.serving = *rule;
  }
  if (const auto text = value_of("--backup")) {
    if (*text != "on" && *text != "off") {
      return failure("--backup " + quote(*text) + " is not on or off");
    }
    options.backup = *text == "on";
  }
  if (const auto text = value_of("--hop-delay-ms")) {
    const std::optional<double> ms = app::parse_finite_double(*text);
    if (!ms || *ms < 0.0) {
      return failure("--hop-delay-ms " + quote(*text) +
                     " is not a finite number of milliseconds >= 0");
    }
    options.hop_delay_s = *ms / kMsPerS;
  }
  if (const auto text = value_of("--directions")) {
    if (*text != "both" && *text != "forward") {
      return failure("--directions " + quote(*text) + " is not both or forward");
    }
    options.both_directions = *text == "both";
  }
  if (const auto path = value_of("--drops-out")) {
    if (path->empty() || *path == "-") {
      return failure("--drops-out must name a file");
    }
    options.drops_out = std::filesystem::path(*path);
  }
  if (const auto path = value_of("--links-out")) {
    if (path->empty()) {
      return failure("--links-out is empty");
    }
    options.links_out = std::filesystem::path(*path);
  }
  ParsedArguments result;
  result.options = std::move(options);
  return result;
}

}  // namespace satsim::routing_app