#include "constellation/arguments.hpp"

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

#include "common/arguments.hpp"
#include "satsim/time/civil_time.hpp"
#include "satsim/time/utc_time.hpp"

namespace satsim::constellation_app {

namespace {

constexpr std::string_view kUsage =
    R"(usage: constellation --snapshot <dir> --time <UTC> --out <csv|->

Classifies every satellite of a saved snapshot into a shell, an orbital plane and a position
in the plane, or gives the reason it has none. Every satellite is propagated with SGP4 to the
one reference time, and its inclination, right ascension of the ascending node (RAAN),
argument of latitude and height are averaged over one orbit around that time. One CSV row per
satellite; a summary (satellites and planes per shell, exclusions by reason) goes to standard
output, or to standard error when the CSV does.

  --snapshot <dir>     directory holding gp.json and supgp.json
  --time <UTC>         reference time, YYYY-MM-DDTHH:MM:SS[.fffffffff], UTC
  --out <path>         CSV file to write, or - for standard output
  --help, -h           print this text

Each satellite uses its supplemental element set if there is one, else its standard one.
Exit status: 0 on success (excluded satellites are counted, not fatal), 1 if the run fails,
2 if the arguments are invalid.
)";

using app::quote;

ParsedArguments failure(std::string message) {
  ParsedArguments result;
  result.error = std::move(message);
  return result;
}

enum Option : std::uint8_t { kSnapshot, kTime, kOut, kOptionCount };
constexpr std::array<std::string_view, kOptionCount> kOptionNames = {"--snapshot", "--time",
                                                                     "--out"};

}  // namespace

std::string_view usage() noexcept { return kUsage; }

ParsedArguments parse_arguments(std::span<const std::string_view> args) {
  if (std::any_of(args.begin(), args.end(),
                  [](std::string_view a) { return a == "--help" || a == "-h"; })) {
    ParsedArguments result;
    result.help = true;
    return result;
  }

  std::array<std::optional<std::string_view>, kOptionCount> values;
  for (std::size_t i = 0; i < args.size(); ++i) {
    const std::string_view name = args[i];
    const auto* const known = std::find(kOptionNames.begin(), kOptionNames.end(), name);
    if (known == kOptionNames.end()) {
      return failure("unknown argument " + quote(name));
    }
    auto& value = values.at(static_cast<std::size_t>(known - kOptionNames.begin()));
    if (value) {
      return failure(std::string(name) + " given more than once");
    }
    // A following option name is a forgotten value, not a value ("-" alone is one: stdout).
    if (i + 1 >= args.size() || args[i + 1].starts_with("--")) {
      return failure(std::string(name) + " needs a value");
    }
    value = args[++i];
  }

  std::string missing;
  for (std::size_t option = 0; option < kOptionCount; ++option) {
    if (!values.at(option)) {
      missing += (missing.empty() ? "" : ", ") + std::string(kOptionNames.at(option));
    }
  }
  if (!missing.empty()) {
    return failure("missing required option(s): " + missing);
  }
  const std::string_view snapshot = values[kSnapshot].value_or("");
  const std::string_view time_text = values[kTime].value_or("");
  const std::string_view out = values[kOut].value_or("");
  if (snapshot.empty()) {
    return failure("--snapshot is empty");
  }
  if (out.empty()) {
    return failure("--out is empty (use - for standard output)");
  }
  const std::optional<time::CivilTime> civil = time::parse_iso_civil_time(time_text);
  if (!civil) {
    return failure("--time " + quote(time_text) +
                   " is not a UTC time YYYY-MM-DDTHH:MM:SS[.fffffffff]");
  }
  const std::optional<time::UtcTime> reference = time::UtcTime::from_civil(*civil);
  if (!reference) {
    return failure("--time " + quote(time_text) + " is not an existing date and time");
  }

  ParsedArguments result;
  result.options = Options{
      .snapshot_dir = std::filesystem::path(snapshot),
      .reference = *reference,
      .out = std::string(out),
  };
  return result;
}

}  // namespace satsim::constellation_app
