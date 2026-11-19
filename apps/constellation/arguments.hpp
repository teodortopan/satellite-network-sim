#pragma once

#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>

#include "satsim/time/utc_time.hpp"

namespace satsim::constellation_app {

/// What one run of the program classifies and where it writes it. Built by parse_arguments,
/// which has checked every value; tests may also fill one in directly.
struct Options {
  /// Directory holding gp.json and supgp.json.
  std::filesystem::path snapshot_dir;
  /// The common reference time every satellite is propagated to.
  time::UtcTime reference;
  /// Path of the CSV file to write, or "-" for standard output.
  std::string out;
};

/// The outcome of parse_arguments: exactly one of options, help or error.
struct ParsedArguments {
  std::optional<Options> options;    ///< Set when the arguments are valid.
  bool help = false;                 ///< --help or -h was given: print usage() and exit 0.
  std::optional<std::string> error;  ///< Why the arguments were rejected, for the user.
};

/// Parses the command line, without the program name. Options take their value as the next
/// argument. --help or -h anywhere wins over any other problem. Rejected: unknown or repeated
/// options, a missing value, a missing option, an empty --snapshot or --out, and a --time that
/// is not YYYY-MM-DDTHH:MM:SS[.fffffffff] or does not exist.
[[nodiscard]] ParsedArguments parse_arguments(std::span<const std::string_view> args);

/// The --help text.
[[nodiscard]] std::string_view usage() noexcept;

}  // namespace satsim::constellation_app
