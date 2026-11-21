#pragma once

#include <array>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <variant>

#include "inputs/inputs.hpp"

// Command-line options that the network programs (topology, routing) share, parsed the same
// way with the same messages.

namespace satsim::app {

/// The value given for an option name ("--snapshot"), or nullopt if it was not given.
using OptionLookup = std::function<std::optional<std::string_view>(std::string_view)>;

/// The required shared options, in usage order.
inline constexpr std::array<std::string_view, 7> kRequiredInputOptionNames = {
    "--snapshot", "--ground", "--cities", "--start", "--duration-s", "--step-s", "--pattern"};

/// The optional shared options.
inline constexpr std::array<std::string_view, 13> kOptionalInputOptionNames = {
    "--shells",
    "--shells-file",
    "--roles",
    "--range-cap-km",
    "--sun-exclusion-deg",
    "--lock-on-s",
    "--fibre-factor",
    "--high-latitude-mask",
    "--min-elevation-deg",
    "--max-ground-range-km",
    "--links-file",
    "--max-lasers",
    "--plan-time"};

/// The shared options, or why they are rejected. The required ones must all be present (the caller
/// checks). Rejected: an empty --snapshot, --ground or --cities, a start time that is not
/// YYYY-MM-DDTHH:MM:SS[.fffffffff] or does not exist, a duration or step that is not a plain
/// decimal number of seconds (at most 9 decimals, step > 0, at most 1e9 s), an unknown pattern,
/// role set or shell name (or a shell named twice; without --shells-file the names must be
/// default shells, otherwise prepare_inputs checks them against the file), an empty
/// --shells-file, a range cap that is not a finite number > 0 km, a Sun exclusion angle not in
/// (0, 180] degrees, a lock-on time that is not a plain decimal number of seconds, a fibre
/// factor that is not a finite number >= 1, a high-latitude setting other than on or off, a
/// minimum elevation not in [0, 90) degrees, a longest ground range that is not a finite
/// number > 0 km, both of those, either with the high-latitude mask on, an empty --links-file,
/// a --max-lasers that is not a whole number from 1 to 1000 or is given without --links-file,
/// and a --plan-time that is not YYYY-MM-DDTHH:MM:SS[.fffffffff] or does not exist (it may be
/// before or after the start: it is only the reference time of the shells, planes and laser
/// plan).
[[nodiscard]] std::variant<InputOptions, std::string> parse_input_options(
    const OptionLookup& value_of);

}  // namespace satsim::app
