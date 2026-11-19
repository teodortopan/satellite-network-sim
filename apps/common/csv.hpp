#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include "satsim/time/utc_time.hpp"

// Text helpers shared by the programs (propagate, constellation, topology): CSV fields,
// numbers that read back exactly, ISO times, fixed-point summary numbers.

namespace satsim::app {

/// Appends the shortest decimal text that reads back as exactly `value` (std::to_chars, so
/// independent of the C locale), e.g. "6928.137" or "1e-07".
void append_double(std::string& out, double value);

/// Appends an integer in decimal.
void append_integer(std::string& out, std::int64_t value);

/// Appends `text` as one CSV field (RFC 4180): unchanged unless it holds a comma, a double
/// quote, a carriage return or a line feed, in which case it is enclosed in double quotes
/// and every double quote inside is doubled.
void append_csv_field(std::string& out, std::string_view text);

/// The number of fraction digits (0 to 9) that writes every time start + i * step exactly:
/// the fewest digits for which both start's fraction of a second and step_ns are whole
/// multiples of the last digit's unit.
[[nodiscard]] int fraction_digits_for(const time::UtcTime& start, std::int64_t step_ns) noexcept;

/// `utc` as YYYY-MM-DDTHH:MM:SS with `fraction_digits` (0 to 9) digits after a '.' (none for
/// 0). Digits beyond `fraction_digits` are dropped, so callers pick enough of them
/// (fraction_digits_for). The form parse_iso_civil_time reads.
[[nodiscard]] std::string format_utc_iso(const time::UtcTime& utc, int fraction_digits);

/// `value` with `decimals` digits after the point, locale-independent ("?" if it does not
/// fit, which no finite summary number does). For human-readable summaries only.
[[nodiscard]] std::string fixed(double value, int decimals);

/// A whole number of nanoseconds as seconds: "15", "0.25", "1.000000001" (no trailing
/// zeros). For summaries.
[[nodiscard]] std::string seconds_text(std::int64_t ns);

}  // namespace satsim::app
