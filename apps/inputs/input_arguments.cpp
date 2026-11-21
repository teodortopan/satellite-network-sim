#include "inputs/input_arguments.hpp"

#include <algorithm>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <numbers>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <variant>
#include <vector>

#include "common/arguments.hpp"
#include "common/steps.hpp"
#include "inputs/inputs.hpp"
#include "satsim/constellation/classification.hpp"
#include "satsim/time/civil_time.hpp"
#include "satsim/time/utc_time.hpp"
#include "satsim/topology/link_plan.hpp"

namespace satsim::app {

namespace {

// The largest --max-lasers accepted: far above any terminal count, small enough to be a typo
// guard.
constexpr std::size_t kMaxLasersLimit = 1000;

std::optional<RoleFilter> parse_roles(std::string_view text) noexcept {
  for (const RoleFilter roles : {RoleFilter::kStudy, RoleFilter::kValidation, RoleFilter::kAll}) {
    if (text == name_of(roles)) {
      return roles;
    }
  }
  return std::nullopt;
}

// "53deg,43deg": shell names, each at most once, in the order given. With `config`, every
// name must be one of its shells; without (the shells come from a file read later), the names
// are checked when the run is prepared.
std::optional<std::vector<std::string>> parse_shells(
    std::string_view text, const constellation::ClassificationConfig* config, std::string& error) {
  std::vector<std::string> shells;
  std::size_t begin = 0;
  while (true) {
    const std::size_t comma = text.find(',', begin);
    const std::string_view item = text.substr(
        begin, comma == std::string_view::npos ? std::string_view::npos : comma - begin);
    if (config == nullptr && item.empty()) {
      error = "--shells: " + quote(item) + " is not a shell";
      return std::nullopt;
    }
    const bool known =
        config == nullptr || std::any_of(config->shells.begin(), config->shells.end(),
                                         [&](const auto& shell) { return shell.name == item; });
    if (!known) {
      std::string names;
      for (const auto& shell : config->shells) {
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

std::variant<InputOptions, std::string> parse_input_options(const OptionLookup& value_of) {
  const auto value = [&](std::string_view name) { return value_of(name).value_or(""); };
  for (const std::string_view path_option : {"--snapshot", "--ground", "--cities"}) {
    if (value(path_option).empty()) {
      return std::string(path_option) + " is empty";
    }
  }
  const std::optional<time::CivilTime> civil = time::parse_iso_civil_time(value("--start"));
  if (!civil) {
    return "--start " + quote(value("--start")) +
           " is not a UTC time YYYY-MM-DDTHH:MM:SS[.fffffffff]";
  }
  const std::optional<time::UtcTime> start = time::UtcTime::from_civil(*civil);
  if (!start) {
    return "--start " + quote(value("--start")) + " is not an existing date and time";
  }
  InputOptions options{.snapshot_dir = std::filesystem::path(value("--snapshot")),
                       .ground_dir = std::filesystem::path(value("--ground")),
                       .cities_file = std::filesystem::path(value("--cities")),
                       .start = *start,
                       .shells = {}};

  const std::optional<std::int64_t> duration_ns = parse_seconds_as_ns(value("--duration-s"));
  if (!duration_ns) {
    return "--duration-s " + quote(value("--duration-s")) +
           " is not a number of seconds from 0 to 1000000000 with at most 9 decimals";
  }
  const std::optional<std::int64_t> step_ns = parse_seconds_as_ns(value("--step-s"));
  if (!step_ns || *step_ns == 0) {
    return "--step-s " + quote(value("--step-s")) +
           " is not a positive number of seconds up to 1000000000 with at most 9 decimals";
  }
  options.duration_ns = *duration_ns;
  options.step_ns = *step_ns;

  const std::optional<topology::LaserPattern> pattern =
      topology::parse_laser_pattern(value("--pattern"));
  if (!pattern) {
    return "--pattern " + quote(value("--pattern")) + " is not three, plus-grid or six";
  }
  options.pattern = *pattern;

  if (const auto shells_file = value_of("--shells-file")) {
    if (shells_file->empty()) {
      return std::string("--shells-file is empty");
    }
    options.shells_file = std::filesystem::path(*shells_file);
  }
  if (const auto text = value_of("--shells")) {
    std::string error;
    auto shells =
        parse_shells(*text, options.shells_file ? nullptr : &options.classification, error);
    if (!shells) {
      return error;
    }
    options.shells = std::move(*shells);
  }
  if (const auto text = value_of("--roles")) {
    const std::optional<RoleFilter> roles = parse_roles(*text);
    if (!roles) {
      return "--roles " + quote(*text) + " is not study, validation or all";
    }
    options.roles = *roles;
  }
  if (const auto text = value_of("--range-cap-km")) {
    const std::optional<double> cap = parse_finite_double(*text);
    if (!cap || *cap <= 0.0) {
      return "--range-cap-km " + quote(*text) + " is not a finite number of kilometres > 0";
    }
    options.limits.range_cap_km = cap;
  }
  if (const auto text = value_of("--sun-exclusion-deg")) {
    const std::optional<double> angle = parse_finite_double(*text);
    if (!angle || *angle <= 0.0 || *angle > 180.0) {
      return "--sun-exclusion-deg " + quote(*text) + " is not an angle in (0, 180] degrees";
    }
    // Divided first, so 180 deg is exactly pi.
    options.limits.sun_exclusion_rad = *angle / 180.0 * std::numbers::pi;
  }
  if (const auto text = value_of("--lock-on-s")) {
    const std::optional<std::int64_t> lock_on_ns = parse_seconds_as_ns(*text);
    if (!lock_on_ns) {
      return "--lock-on-s " + quote(*text) +
             " is not a number of seconds from 0 to 1000000000 with at most 9 decimals";
    }
    options.limits.lock_on_s =
        static_cast<double>(*lock_on_ns) / static_cast<double>(kNanosecondsPerSecond);
  }
  if (const auto text = value_of("--fibre-factor")) {
    const std::optional<double> factor = parse_finite_double(*text);
    if (!factor || *factor < 1.0) {
      return "--fibre-factor " + quote(*text) +
             " is not a finite number >= 1 (fibre is never faster than light in a vacuum along "
             "the great circle)";
    }
    options.fibre_factor = *factor;
  }
  if (const auto setting = value_of("--high-latitude-mask")) {
    if (*setting != "on" && *setting != "off") {
      return "--high-latitude-mask " + quote(*setting) + " is not on or off";
    }
    options.mask.high_latitude_allowance = *setting == "on";
  }
  const auto min_elevation = value_of("--min-elevation-deg");
  const auto max_ground_range = value_of("--max-ground-range-km");
  if (min_elevation) {
    const std::optional<double> angle = parse_finite_double(*min_elevation);
    if (!angle || *angle < 0.0 || *angle >= 90.0) {
      return "--min-elevation-deg " + quote(*min_elevation) + " is not an angle in [0, 90) degrees";
    }
    options.mask.min_elevation_rad = *angle / 180.0 * std::numbers::pi;
  }
  if (max_ground_range) {
    const std::optional<double> range = parse_finite_double(*max_ground_range);
    if (!range || *range <= 0.0) {
      return "--max-ground-range-km " + quote(*max_ground_range) +
             " is not a finite number of kilometres > 0";
    }
    options.mask.max_ground_range_km = range;
  }
  if (min_elevation && max_ground_range) {
    return std::string("--min-elevation-deg and --max-ground-range-km cannot be combined");
  }
  if ((min_elevation || max_ground_range) && options.mask.high_latitude_allowance) {
    return std::string(min_elevation ? "--min-elevation-deg" : "--max-ground-range-km") +
           " replaces the masks and cannot be combined with --high-latitude-mask on";
  }
  if (const auto text = value_of("--plan-time")) {
    const std::optional<time::CivilTime> plan_civil = time::parse_iso_civil_time(*text);
    if (!plan_civil) {
      return "--plan-time " + quote(*text) + " is not a UTC time YYYY-MM-DDTHH:MM:SS[.fffffffff]";
    }
    const std::optional<time::UtcTime> plan_time = time::UtcTime::from_civil(*plan_civil);
    if (!plan_time) {
      return "--plan-time " + quote(*text) + " is not an existing date and time";
    }
    options.plan_time = *plan_time;
  }
  if (const auto path = value_of("--links-file")) {
    if (path->empty()) {
      return std::string("--links-file is empty");
    }
    options.links_file = std::filesystem::path(*path);
  }
  if (const auto text = value_of("--max-lasers")) {
    std::size_t count = 0;
    const auto [end, ec] = std::from_chars(text->data(), text->data() + text->size(), count);
    if (text->empty() || text->front() < '0' || text->front() > '9' || ec != std::errc() ||
        end != text->data() + text->size() || count < 1 || count > kMaxLasersLimit) {
      return "--max-lasers " + quote(*text) + " is not a whole number from 1 to 1000";
    }
    if (!options.links_file) {
      return std::string("--max-lasers applies only to --links-file");
    }
    options.max_lasers = count;
  }
  return options;
}

}  // namespace satsim::app
