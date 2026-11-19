#include "common/arguments.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>

#include "common/steps.hpp"

namespace satsim::app {

namespace {

constexpr std::size_t kMaxQuotedLength = 40;

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

}  // namespace

std::string quote(std::string_view text) {
  if (text.size() > kMaxQuotedLength) {
    return "\"" + std::string(text.substr(0, kMaxQuotedLength)) + "...\"";
  }
  return "\"" + std::string(text) + "\"";
}

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

std::optional<double> parse_finite_double(std::string_view text) noexcept {
  if (text.empty()) {
    return std::nullopt;
  }
  double value = 0.0;
  const auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), value);
  if (ec != std::errc{} || end != text.data() + text.size() || !std::isfinite(value)) {
    return std::nullopt;
  }
  return value;
}

OptionValues read_option_values(std::span<const std::string_view> args,
                                std::span<const std::string_view> names) {
  OptionValues result;
  result.values.assign(names.size(), std::nullopt);
  for (std::size_t i = 0; i < args.size(); ++i) {
    const std::string_view name = args[i];
    const auto known = std::find(names.begin(), names.end(), name);
    if (known == names.end()) {
      result.error = "unknown argument " + quote(name);
      return result;
    }
    auto& value = result.values.at(static_cast<std::size_t>(known - names.begin()));
    if (value) {
      result.error = std::string(name) + " given more than once";
      return result;
    }
    // A following option name is a forgotten value, not a value ("-" alone is one: stdout).
    if (i + 1 >= args.size() || args[i + 1].starts_with("--")) {
      result.error = std::string(name) + " needs a value";
      return result;
    }
    value = args[++i];
  }
  return result;
}

}  // namespace satsim::app
