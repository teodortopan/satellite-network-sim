#include "propagate/arguments.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "satsim/time/civil_time.hpp"
#include "satsim/time/utc_time.hpp"

namespace satsim::propagate {

namespace {

constexpr std::int64_t kNanosecondsPerSecond = 1'000'000'000;
constexpr std::int64_t kMaxCatalogNumber = 999'999'999;  // OMM allows nine digits.
constexpr std::size_t kMaxQuotedLength = 40;             // Keeps messages short on junk input.

constexpr std::string_view kUsage =
    R"(usage: propagate --snapshot <dir> --start <UTC> --duration-s <s> --step-s <s> --out <csv|->
                 [--ids <n,n,...>]

Propagates every satellite of a saved snapshot with SGP4 and writes its position at each
time as CSV: TEME position and velocity, ECEF (ITRF) position, WGS-84 geodetic latitude,
longitude and height, and J2000 (FK5) position and velocity. A summary goes to standard
output, or to standard error when the CSV does.

  --snapshot <dir>     directory holding gp.json, supgp.json and finals2000A.all
  --start <UTC>        first time, YYYY-MM-DDTHH:MM:SS[.fffffffff], UTC
  --duration-s <s>     length of the run in seconds (decimal, >= 0); the end time is
                       included when it lands on a step
  --step-s <s>         spacing of the times in seconds (decimal, > 0)
  --out <path>         CSV file to write, or - for standard output
  --ids <n,n,...>      only these catalogue numbers (default: every satellite)
  --help, -h           print this text

Each satellite uses its supplemental element set if there is one, else its standard one.
Every time must lie inside the Earth-orientation file; otherwise nothing is written.
Exit status: 0 on success (satellites that fail to propagate are counted, not fatal),
1 if the run fails, 2 if the arguments are invalid.
)";

std::string quote(std::string_view text) {
  if (text.size() > kMaxQuotedLength) {
    return "\"" + std::string(text.substr(0, kMaxQuotedLength)) + "...\"";
  }
  return "\"" + std::string(text) + "\"";
}

bool is_digit(char c) noexcept { return c >= '0' && c <= '9'; }

bool all_digits(std::string_view text) noexcept {
  return !text.empty() && std::all_of(text.begin(), text.end(), is_digit);
}

// Digits only, at most 18 of them, so the value fits in int64_t (below 1e18).
std::optional<std::int64_t> parse_small_unsigned(std::string_view digits) noexcept {
  constexpr std::size_t kMaxDigits = 18;
  if (!all_digits(digits) || digits.size() > kMaxDigits) {
    return std::nullopt;
  }
  std::int64_t value = 0;
  for (const char c : digits) {
    value = value * 10 + (c - '0');
  }
  return value;
}

// A plain decimal number of seconds, "12" or "12.345", with at most 9 fraction digits, read
// exactly as integer nanoseconds. Nullopt for anything else or above kMaxDurationNs.
std::optional<std::int64_t> parse_seconds_as_ns(std::string_view text) noexcept {
  constexpr std::size_t kMaxFractionDigits = 9;
  const std::size_t dot = text.find('.');
  const std::string_view whole = text.substr(0, dot);
  std::string_view fraction;
  if (dot != std::string_view::npos) {
    fraction = text.substr(dot + 1);
    if (fraction.empty() || fraction.size() > kMaxFractionDigits) {
      return std::nullopt;
    }
  }
  // Whole part limited to 10 digits (kMaxDurationNs is 1e9 s), so the product cannot overflow.
  constexpr std::size_t kMaxWholeDigits = 10;
  if (whole.size() > kMaxWholeDigits) {
    return std::nullopt;
  }
  const std::optional<std::int64_t> whole_s = parse_small_unsigned(whole);
  if (!whole_s) {
    return std::nullopt;
  }
  std::int64_t fraction_ns = 0;
  if (!fraction.empty()) {
    const std::optional<std::int64_t> digits = parse_small_unsigned(fraction);
    if (!digits) {
      return std::nullopt;
    }
    fraction_ns = *digits;
    for (std::size_t i = fraction.size(); i < kMaxFractionDigits; ++i) {
      fraction_ns *= 10;
    }
  }
  const std::int64_t total_ns = *whole_s * kNanosecondsPerSecond + fraction_ns;
  if (total_ns > kMaxDurationNs) {
    return std::nullopt;
  }
  return total_ns;
}

// "44714,100001": catalogue numbers, returned ascending without repeats.
std::optional<std::vector<std::int64_t>> parse_ids(std::string_view text, std::string& error) {
  std::vector<std::int64_t> ids;
  std::size_t begin = 0;
  while (true) {
    const std::size_t comma = text.find(',', begin);
    const std::string_view item = text.substr(
        begin, comma == std::string_view::npos ? std::string_view::npos : comma - begin);
    const std::optional<std::int64_t> id = parse_small_unsigned(item);
    if (!id || *id < 1 || *id > kMaxCatalogNumber) {
      error = "--ids: " + quote(item) + " is not a catalogue number 1 to 999999999";
      return std::nullopt;
    }
    ids.push_back(*id);
    if (comma == std::string_view::npos) {
      break;
    }
    begin = comma + 1;
  }
  std::sort(ids.begin(), ids.end());
  ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
  return ids;
}

ParsedArguments failure(std::string message) {
  ParsedArguments result;
  result.error = std::move(message);
  return result;
}

// The options that take a value, in usage order.
enum Option : std::uint8_t { kSnapshot, kStart, kDuration, kStep, kOut, kIds, kOptionCount };
constexpr std::array<std::string_view, kOptionCount> kOptionNames = {
    "--snapshot", "--start", "--duration-s", "--step-s", "--out", "--ids"};

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
  for (std::size_t option = 0; option < kIds; ++option) {
    if (!values.at(option)) {
      missing += (missing.empty() ? "" : ", ") + std::string(kOptionNames.at(option));
    }
  }
  if (!missing.empty()) {
    return failure("missing required option(s): " + missing);
  }
  // Every required value is present from here on.
  const std::string_view snapshot = values[kSnapshot].value_or("");
  const std::string_view start_text = values[kStart].value_or("");
  const std::string_view duration_text = values[kDuration].value_or("");
  const std::string_view step_text = values[kStep].value_or("");
  const std::string_view out = values[kOut].value_or("");

  if (snapshot.empty()) {
    return failure("--snapshot is empty");
  }
  if (out.empty()) {
    return failure("--out is empty (use - for standard output)");
  }

  const std::optional<time::CivilTime> civil = time::parse_iso_civil_time(start_text);
  if (!civil) {
    return failure("--start " + quote(start_text) +
                   " is not a UTC time YYYY-MM-DDTHH:MM:SS[.fffffffff]");
  }
  const std::optional<time::UtcTime> start = time::UtcTime::from_civil(*civil);
  if (!start) {
    return failure("--start " + quote(start_text) + " is not an existing date and time");
  }

  const std::optional<std::int64_t> duration_ns = parse_seconds_as_ns(duration_text);
  if (!duration_ns) {
    return failure("--duration-s " + quote(duration_text) +
                   " is not a number of seconds from 0 to 1000000000 with at most 9 decimals");
  }
  const std::optional<std::int64_t> step_ns = parse_seconds_as_ns(step_text);
  if (!step_ns || *step_ns == 0) {
    return failure("--step-s " + quote(step_text) +
                   " is not a positive number of seconds up to 1000000000 with at most 9 "
                   "decimals");
  }

  std::vector<std::int64_t> ids;
  if (const std::optional<std::string_view> ids_text = values[kIds]) {
    std::string error;
    auto parsed = parse_ids(*ids_text, error);
    if (!parsed) {
      return failure(std::move(error));
    }
    ids = std::move(*parsed);
  }

  ParsedArguments result;
  result.options = Options{
      .snapshot_dir = std::filesystem::path(snapshot),
      .start = *start,
      .duration_ns = *duration_ns,
      .step_ns = *step_ns,
      .out = std::string(out),
      .ids = std::move(ids),
  };
  return result;
}

}  // namespace satsim::propagate
