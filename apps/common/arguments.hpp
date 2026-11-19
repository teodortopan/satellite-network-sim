#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// Command-line helpers shared by the programs.

namespace satsim::app {

/// `text` in double quotes for an error message, cut to 40 characters (then "...") so that
/// junk input keeps messages short.
[[nodiscard]] std::string quote(std::string_view text);

/// A plain decimal number of seconds, "12" or "12.345", with at most 9 fraction digits, read
/// exactly as integer nanoseconds. nullopt for anything else (signs, exponents, spaces) or
/// above kMaxDurationNs (steps.hpp).
[[nodiscard]] std::optional<std::int64_t> parse_seconds_as_ns(std::string_view text) noexcept;

/// A finite decimal number such as "12", "-0.5" or "1e3" (std::from_chars, general format,
/// locale-independent): the whole text must be the number. nullopt for anything else,
/// including "inf", "nan", a leading '+' and surrounding spaces.
[[nodiscard]] std::optional<double> parse_finite_double(std::string_view text) noexcept;

/// The values of "--name value" options, one slot per name in `names` (same order), empty
/// where the option was not given; or why the arguments are not of that form.
struct OptionValues {
  std::vector<std::optional<std::string_view>> values;
  std::optional<std::string> error;
};

/// Reads `args` as a sequence of "--name value" pairs. Rejected: a name not in `names`
/// ("unknown argument"), a name given twice, and a name with no value after it or followed
/// by another "--" argument (a forgotten value; "-" alone is a value). The values point into
/// `args`.
[[nodiscard]] OptionValues read_option_values(std::span<const std::string_view> args,
                                              std::span<const std::string_view> names);

}  // namespace satsim::app
