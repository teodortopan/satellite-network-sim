#include "topology/arguments.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <numbers>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "common/arguments.hpp"
#include "common/steps.hpp"
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
                [--shells <name,...>] [--roles <study|validation|all>]
                [--range-cap-km <km>] [--sun-exclusion-deg <deg>] [--lock-on-s <s>]
                [--fibre-factor <x>] [--high-latitude-mask <on|off>]
                [--serving <highest-elevation>]

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
                              reference time of the shells, planes and laser plan
  --duration-s <s>            length of the run in seconds (decimal, >= 0); the end time is
                              included when it lands on a step
  --step-s <s>                spacing of the times in seconds (decimal, > 0)
  --pattern <name>            laser pattern: three, plus-grid or six
  --model <name>              realistic (one serving satellite, a gateway, fibre through
                              the sender's home PoP) or best-case (any satellites, space only)
  --out <path>                CSV file to write, or - for standard output
  --shells <name,...>         only these shells (default: all; names as 53deg, 43deg, ...)
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
  kRoles,
  kRangeCap,
  kSunExclusion,
  kLockOn,
  kFibreFactor,
  kHighLatitude,
  kServing,
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
                                                                     "--roles",
                                                                     "--range-cap-km",
                                                                     "--sun-exclusion-deg",
                                                                     "--lock-on-s",
                                                                     "--fibre-factor",
                                                                     "--high-latitude-mask",
                                                                     "--serving"};

std::optional<RoleFilter> parse_roles(std::string_view text) noexcept {
  for (const RoleFilter roles : {RoleFilter::kStudy, RoleFilter::kValidation, RoleFilter::kAll}) {
    if (text == name_of(roles)) {
      return roles;
    }
  }
  return std::nullopt;
}

// "53deg,43deg": names of `config`'s shells, each at most once, in the order given.
std::optional<std::vector<std::string>> parse_shells(
    std::string_view text, const constellation::ClassificationConfig& config, std::string& error) {
  std::vector<std::string> shells;
  std::size_t begin = 0;
  while (true) {
    const std::size_t comma = text.find(',', begin);
    const std::string_view item = text.substr(
        begin, comma == std::string_view::npos ? std::string_view::npos : comma - begin);
    const bool known = std::any_of(config.shells.begin(), config.shells.end(),
                                   [&](const auto& shell) { return shell.name == item; });
    if (!known) {
      std::string names;
      for (const auto& shell : config.shells) {
        names += (names.empty() ? "" : ", ") + shell.name;
      }
      error = "--shells: " + quote(item) + " is not a shell (shells: " + names + ")";
      return std::nullopt;
    }
    if (std::find(shells.begin(), shells.end(), item) != shells.end()) {
      error = "--shells: " + quote(item) + " is named twice";
      return std::nullopt;
    }
    shells.emplace_back(item);
    if (comma == std::string_view::npos) {
      break;
    }
    begin = comma + 1;
  }
  return shells;
}

}  // namespace

std::string_view name_of(RoleFilter roles) noexcept {
  switch (roles) {
    case RoleFilter::kStudy:
      return "study";
    case RoleFilter::kValidation:
      return "validation";
    case RoleFilter::kAll:
      return "all";
  }
  return "unknown";
}

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

  for (const Option path_option : {kSnapshot, kGround, kCities, kOut}) {
    if (value(path_option).empty()) {
      return failure(std::string(kOptionNames.at(path_option)) + " is empty" +
                     (path_option == kOut ? " (use - for standard output)" : ""));
    }
  }

  const std::optional<time::CivilTime> civil = time::parse_iso_civil_time(value(kStart));
  if (!civil) {
    return failure("--start " + quote(value(kStart)) +
                   " is not a UTC time YYYY-MM-DDTHH:MM:SS[.fffffffff]");
  }
  const std::optional<time::UtcTime> start = time::UtcTime::from_civil(*civil);
  if (!start) {
    return failure("--start " + quote(value(kStart)) + " is not an existing date and time");
  }
  Options options{.snapshot_dir = std::filesystem::path(value(kSnapshot)),
                  .ground_dir = std::filesystem::path(value(kGround)),
                  .cities_file = std::filesystem::path(value(kCities)),
                  .start = *start,
                  .shells = {},
                  .out = std::string(value(kOut))};

  const std::optional<std::int64_t> duration_ns = app::parse_seconds_as_ns(value(kDuration));
  if (!duration_ns) {
    return failure("--duration-s " + quote(value(kDuration)) +
                   " is not a number of seconds from 0 to 1000000000 with at most 9 decimals");
  }
  const std::optional<std::int64_t> step_ns = app::parse_seconds_as_ns(value(kStep));
  if (!step_ns || *step_ns == 0) {
    return failure("--step-s " + quote(value(kStep)) +
                   " is not a positive number of seconds up to 1000000000 with at most 9 "
                   "decimals");
  }
  options.duration_ns = *duration_ns;
  options.step_ns = *step_ns;

  const std::optional<topology::LaserPattern> pattern =
      topology::parse_laser_pattern(value(kPattern));
  if (!pattern) {
    return failure("--pattern " + quote(value(kPattern)) + " is not three, plus-grid or six");
  }
  options.pattern = *pattern;
  const std::optional<EndpointModel> model = parse_endpoint_model(value(kModel));
  if (!model) {
    return failure("--model " + quote(value(kModel)) + " is not realistic or best-case");
  }
  options.model = *model;

  if (values[kShells]) {
    std::string error;
    auto shells = parse_shells(*values[kShells], options.classification, error);
    if (!shells) {
      return failure(std::move(error));
    }
    options.shells = std::move(*shells);
  }
  if (values[kRoles]) {
    const std::optional<RoleFilter> roles = parse_roles(*values[kRoles]);
    if (!roles) {
      return failure("--roles " + quote(*values[kRoles]) + " is not study, validation or all");
    }
    options.roles = *roles;
  }
  if (values[kRangeCap]) {
    const std::optional<double> cap = app::parse_finite_double(*values[kRangeCap]);
    if (!cap || *cap <= 0.0) {
      return failure("--range-cap-km " + quote(*values[kRangeCap]) +
                     " is not a finite number of kilometres > 0");
    }
    options.limits.range_cap_km = cap;
  }
  if (values[kSunExclusion]) {
    const std::optional<double> angle = app::parse_finite_double(*values[kSunExclusion]);
    if (!angle || *angle <= 0.0 || *angle > 180.0) {
      return failure("--sun-exclusion-deg " + quote(*values[kSunExclusion]) +
                     " is not an angle in (0, 180] degrees");
    }
    // Divided first, so 180 deg is exactly pi.
    options.limits.sun_exclusion_rad = *angle / 180.0 * std::numbers::pi;
  }
  if (values[kLockOn]) {
    const std::optional<std::int64_t> lock_on_ns = app::parse_seconds_as_ns(*values[kLockOn]);
    if (!lock_on_ns) {
      return failure("--lock-on-s " + quote(*values[kLockOn]) +
                     " is not a number of seconds from 0 to 1000000000 with at most 9 "
                     "decimals");
    }
    options.limits.lock_on_s =
        static_cast<double>(*lock_on_ns) / static_cast<double>(app::kNanosecondsPerSecond);
  }
  if (values[kFibreFactor]) {
    const std::optional<double> factor = app::parse_finite_double(*values[kFibreFactor]);
    if (!factor || *factor < 1.0) {
      return failure("--fibre-factor " + quote(*values[kFibreFactor]) +
                     " is not a finite number >= 1 (fibre is never faster than light in a "
                     "vacuum along the great circle)");
    }
    options.fibre_factor = *factor;
  }
  if (values[kHighLatitude]) {
    const std::string_view setting = *values[kHighLatitude];
    if (setting != "on" && setting != "off") {
      return failure("--high-latitude-mask " + quote(setting) + " is not on or off");
    }
    options.mask.high_latitude_allowance = setting == "on";
  }
  if (values[kServing] && *values[kServing] != "highest-elevation") {
    return failure("--serving " + quote(*values[kServing]) + " is not highest-elevation");
  }

  ParsedArguments result;
  result.options = std::move(options);
  return result;
}

}  // namespace satsim::topology_app
