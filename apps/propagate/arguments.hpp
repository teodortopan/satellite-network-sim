#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "common/steps.hpp"
#include "satsim/time/utc_time.hpp"

namespace satsim::propagate {

/// Longest accepted --duration-s (app::kMaxDurationNs, about 31.7 years).
using app::kMaxDurationNs;

/// What one run of the program computes and where it writes it. Built by parse_arguments,
/// which has checked every value; tests may also fill one in directly.
struct Options {
  /// Directory holding gp.json, supgp.json and finals2000A.all.
  std::filesystem::path snapshot_dir;
  /// First output time.
  time::UtcTime start;
  /// Length of the run, in [0, kMaxDurationNs]. Output times are start + i * step for
  /// i = 0, 1, ... up to the last one not after start + duration, so the end is included
  /// exactly when it lands on a step.
  std::int64_t duration_ns = 0;
  /// Spacing of the output times, at least 1 ns. Times are integer nanoseconds, so a step
  /// such as 0.1 s is exact, unlike in a binary double.
  std::int64_t step_ns = 0;
  /// Path of the CSV file to write, or "-" for standard output.
  std::string out;
  /// Catalogue numbers to output, ascending and without repeats; empty means every satellite.
  std::vector<std::int64_t> ids;
};

/// The outcome of parse_arguments: exactly one of options, help or error.
struct ParsedArguments {
  std::optional<Options> options;    ///< Set when the arguments are valid.
  bool help = false;                 ///< --help or -h was given: print usage() and exit 0.
  std::optional<std::string> error;  ///< Why the arguments were rejected, for the user.
};

/// Parses the command line, without the program name. Options take their value as the next
/// argument (`--step-s 60`). --help or -h anywhere wins over any other problem. Rejected:
/// unknown or repeated options, a missing value, a missing required option, a start time
/// that is not YYYY-MM-DDTHH:MM:SS[.fffffffff] or does not exist, a duration or step that
/// is not a plain decimal number of seconds with at most 9 fraction digits (no sign, no
/// exponent), a zero step, a duration above kMaxDurationNs, and --ids that is not a
/// comma-separated list of catalogue numbers 1 to 999999999.
[[nodiscard]] ParsedArguments parse_arguments(std::span<const std::string_view> args);

/// The --help text.
[[nodiscard]] std::string_view usage() noexcept;

}  // namespace satsim::propagate
