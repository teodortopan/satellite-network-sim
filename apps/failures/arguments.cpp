#include "failures/arguments.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "common/arguments.hpp"
#include "failures/settings.hpp"
#include "inputs/input_arguments.hpp"
#include "routing/arguments.hpp"
#include "routing/settings.hpp"

namespace satsim::failures_app {

namespace {

using app::quote;

constexpr std::string_view kUsage =
    R"(usage: failures --snapshot <dir> --ground <dir> --cities <file> --start <UTC>
                --duration-s <s> --step-s <s> --pattern <three|plus-grid|six>
                --model <realistic|best-case> --strategies <name,...> --replan-s <s>
                --out <csv|-> --reactions <planner,local-repair,flooding>
                [--failures <file>] [--replan-on-report <on|off>] [--repoint <on|off>]
                [--detection-s <s>] [--hop-processing-s <s>] [--compute-s <s>]
                [--planner-processing-s <s>] [--acquisition-s <s>]
                [--events-out <csv>] [--facts-out <csv>] [--floods-out <csv>]
                [--measures-out <csv>] [--network-out <csv>]
                [--planner <start|whole-interval>]
                [--serving <highest-elevation|longest-in-view|highest-whole-slot>]
                [--serving-slot-s <s>] [--backup <on|off>] [--hop-delay-ms <ms>]
                [--directions <both|forward>] [--drops-out <csv>]
                [--shells <name,...>] [--shells-file <file>]
                [--roles <study|validation|all>]
                [--range-cap-km <km>] [--sun-exclusion-deg <deg>]
                [--fibre-factor <x>] [--high-latitude-mask <on|off>]
                [--min-elevation-deg <deg> | --max-ground-range-km <km>]
                [--links-file <file> [--max-lasers <n>]] [--links-out <file>]
                [--plan-time <UTC>]

Follows the route of every city pair through time as the routing program does
(docs/phase-3-routing.md 4.8), while satellites, laser links and gateways fail at the instants
of a failure file (docs/phase-4-failures.md 4.10). One pass over time evaluates, per direction
and strategy, the baseline (no failures), a reference planner that knows the true network at
every plan, and every listed reaction mode, in continuous time between the 1 s steps. A summary
goes to standard output, or to standard error when the routes CSV does. Every step is checked
(spec 6.1).

Options of the routing program, with the same meaning (see routing --help):
  --snapshot, --ground, --cities, --start, --duration-s, --step-s, --pattern, --model,
  --strategies, --replan-s, --out, --planner, --serving, --serving-slot-s, --backup,
  --hop-delay-ms, --directions, --drops-out (the baseline's drop events), --shells,
  --shells-file, --roles, --range-cap-km, --sun-exclusion-deg, --fibre-factor,
  --high-latitude-mask, --min-elevation-deg, --max-ground-range-km, --links-file,
  --max-lasers, --links-out, --plan-time
  Refused here: --lock-on-s other than 0 (--acquisition-s is this program's lock-on) and
  --serving with-route (the reaction modes share one dish per city).

Options of this program:
  --reactions <list>          comma-separated reaction modes: planner (the planner only),
                              local-repair (the node before a dead element switches to a
                              backup), flooding (not available yet)
  --failures <file>           the failure file (kind,norad_cat_id_a,norad_cat_id_b,gateway,
                              start_utc,length_s); without it nothing fails
  --replan-on-report <on|off> the planner re-plans every direction the instant it learns a
                              fact (default off)
  --repoint <on|off>          lasers whose link failed look for new partners (default off)
  --detection-s <s>           from a failure to a neighbour knowing it (default 0.1)
  --hop-processing-s <s>      added for every hop of a report (default 0.001)
  --compute-s <s>             from a node learning a fact to its tables using it (flooding;
                              default 0.1)
  --planner-processing-s <s>  from a report reaching a gateway to the planner knowing
                              (default 0)
  --acquisition-s <s>         from a laser link being matched or returning to it carrying
                              traffic (default 30)
  --events-out <path>         events CSV: one row per interval of a direction's state
  --facts-out <path>          facts CSV: one row per fact, with its detectors and reports
  --floods-out <path>         floods CSV (flooding; not written yet)
  --measures-out <path>       measures CSV: one row per direction, strategy and reaction mode
  --network-out <path>        network CSV: per step and shell, the pieces the working
                              satellites' laser links join them into
  --help, -h                  print this text

Exit status: 0 on success, 1 if any check fails, 2 if the arguments or inputs are invalid
or the output cannot be written.
)";

ParsedArguments failure(std::string message) {
  ParsedArguments result;
  result.error = std::move(message);
  return result;
}

constexpr std::array<std::string_view, 1> kRequiredOwn = {"--reactions"};
constexpr std::array<std::string_view, 13> kOptionalOwn = {"--failures",
                                                           "--replan-on-report",
                                                           "--repoint",
                                                           "--detection-s",
                                                           "--hop-processing-s",
                                                           "--compute-s",
                                                           "--planner-processing-s",
                                                           "--acquisition-s",
                                                           "--events-out",
                                                           "--facts-out",
                                                           "--floods-out",
                                                           "--measures-out",
                                                           "--network-out"};

// The routing program's names (its required ones first), then ours.
std::vector<std::string_view> option_names() {
  std::vector<std::string_view> names = routing_app::routing_option_names();
  names.insert(
      names.begin() + static_cast<std::ptrdiff_t>(routing_app::kRoutingRequiredOptionCount),
      kRequiredOwn.begin(), kRequiredOwn.end());
  names.insert(names.end(), kOptionalOwn.begin(), kOptionalOwn.end());
  return names;
}

std::optional<std::vector<Reaction>> parse_reactions(std::string_view text, std::string& error) {
  std::vector<Reaction> reactions;
  std::size_t begin = 0;
  while (true) {
    const std::size_t comma = text.find(',', begin);
    const std::string_view item = text.substr(
        begin, comma == std::string_view::npos ? std::string_view::npos : comma - begin);
    const std::optional<Reaction> reaction = parse_reaction(item);
    if (!reaction) {
      error = "--reactions: " + quote(item) + " is not planner, local-repair or flooding";
      return std::nullopt;
    }
    if (std::find(reactions.begin(), reactions.end(), *reaction) != reactions.end()) {
      error = "--reactions: " + quote(item) + " is named twice";
      return std::nullopt;
    }
    reactions.push_back(*reaction);
    if (comma == std::string_view::npos) {
      break;
    }
    begin = comma + 1;
  }
  return reactions;
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
  const std::size_t required = routing_app::kRoutingRequiredOptionCount + kRequiredOwn.size();
  for (std::size_t i = 0; i < required; ++i) {
    if (!read.values.at(i)) {
      missing += (missing.empty() ? "" : ", ") + std::string(names.at(i));
    }
  }
  if (!missing.empty()) {
    return failure("missing required option(s): " + missing);
  }
  std::variant<routing_app::Options, std::string> routing =
      routing_app::parse_routing_options(value_of);
  if (auto* error = std::get_if<std::string>(&routing)) {
    return failure(std::move(*error));
  }
  Options options{.routing = std::move(std::get<routing_app::Options>(routing)), .reactions = {}};
  if (options.routing.limits.lock_on_s != 0.0) {
    return failure("--lock-on-s must be 0 here: --acquisition-s is the failures program's lock-on");
  }
  if (options.routing.serving == routing_app::ServingRule::kWithRoute) {
    return failure(
        "--serving with-route is refused here: it gives every strategy a dish of its own, and "
        "the reaction modes share one dish per city");
  }
  std::string error;
  auto reactions = parse_reactions(value_of("--reactions").value_or(""), error);
  if (!reactions) {
    return failure(std::move(error));
  }
  options.reactions = std::move(*reactions);
  if (const auto path = value_of("--failures")) {
    if (path->empty()) {
      return failure("--failures is empty");
    }
    options.failures = std::filesystem::path(*path);
  }
  for (const auto& [name, target] :
       {std::pair{std::string_view("--replan-on-report"), &options.replan_on_report},
        std::pair{std::string_view("--repoint"), &options.times.repoint}}) {
    if (const auto text = value_of(name)) {
      if (*text != "on" && *text != "off") {
        return failure(std::string(name) + " " + quote(*text) + " is not on or off");
      }
      *target = *text == "on";
    }
  }
  for (const auto& [name, target] :
       {std::pair{std::string_view("--detection-s"), &options.times.detection_s},
        std::pair{std::string_view("--hop-processing-s"), &options.times.hop_processing_s},
        std::pair{std::string_view("--compute-s"), &options.times.compute_s},
        std::pair{std::string_view("--planner-processing-s"), &options.times.planner_processing_s},
        std::pair{std::string_view("--acquisition-s"), &options.times.acquisition_s}}) {
    if (const auto text = value_of(name)) {
      const std::optional<double> seconds = app::parse_finite_double(*text);
      if (!seconds || *seconds < 0.0) {
        return failure(std::string(name) + " " + quote(*text) +
                       " is not a finite number of seconds >= 0");
      }
      *target = *seconds;
    }
  }
  for (const auto& [name, target] :
       {std::pair{std::string_view("--events-out"), &options.events_out},
        std::pair{std::string_view("--facts-out"), &options.facts_out},
        std::pair{std::string_view("--floods-out"), &options.floods_out},
        std::pair{std::string_view("--measures-out"), &options.measures_out},
        std::pair{std::string_view("--network-out"), &options.network_out}}) {
    if (const auto path = value_of(name)) {
      if (path->empty() || *path == "-") {
        return failure(std::string(name) + " must name a file");
      }
      *target = std::filesystem::path(*path);
    }
  }
  ParsedArguments result;
  result.options = std::move(options);
  return result;
}

}  // namespace satsim::failures_app
