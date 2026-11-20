#include "satsim/routing/strategy.hpp"

#include <array>
#include <cassert>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>

namespace satsim::routing {

namespace {

constexpr double kMillisecondsPerSecond = 1000.0;
constexpr double kPercent = 100.0;
constexpr int kSignificantDigits = 15;

constexpr std::string_view kThresholdPrefix = "threshold:";

// `value` with up to 15 significant digits, as %g prints it ("0.5", "2", "1e+20").
std::string format_number(double value) {
  std::array<char, 40> buffer{};
  const auto [end, ec] = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value,
                                       std::chars_format::general, kSignificantDigits);
  assert(ec == std::errc{});
  return {buffer.data(), end};
}

bool is_digit(char c) noexcept { return c >= '0' && c <= '9'; }

// A positive finite decimal: digits, optionally "." and digits; nullopt otherwise.
std::optional<double> parse_positive_decimal(std::string_view text) noexcept {
  std::size_t i = 0;
  while (i < text.size() && is_digit(text[i])) {
    ++i;
  }
  if (i == 0) {
    return std::nullopt;
  }
  if (i < text.size()) {
    if (text[i] != '.') {
      return std::nullopt;
    }
    const std::size_t fraction_start = ++i;
    while (i < text.size() && is_digit(text[i])) {
      ++i;
    }
    if (i == fraction_start || i != text.size()) {
      return std::nullopt;
    }
  }
  double value = 0.0;
  const auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), value);
  if (ec != std::errc{} || end != text.data() + text.size() || !std::isfinite(value) ||
      value <= 0.0) {
    return std::nullopt;
  }
  return value;
}

std::string unknown_strategy(std::string_view text) {
  return "unknown strategy '" + std::string(text) +
         "' (expected lowest-delay, keep-until-broken, longest-lived, threshold:<n>ms or "
         "threshold:<n>%, n a positive decimal)";
}

}  // namespace

std::string name_of(const Strategy& strategy) {
  switch (strategy.kind) {
    case StrategyKind::kLowestDelay:
      return "lowest-delay";
    case StrategyKind::kKeepUntilBroken:
      return "keep-until-broken";
    case StrategyKind::kLongestLived:
      return "longest-lived";
    case StrategyKind::kThreshold:
      if (strategy.threshold_kind == ThresholdKind::kAbsolute) {
        return std::string(kThresholdPrefix) +
               format_number(strategy.threshold_s * kMillisecondsPerSecond) + "ms";
      }
      return std::string(kThresholdPrefix) + format_number(strategy.threshold_fraction * kPercent) +
             "%";
  }
  return "unknown";
}

StrategyOrError parse_strategy(std::string_view text) {
  if (text == "lowest-delay") {
    return Strategy{.kind = StrategyKind::kLowestDelay};
  }
  if (text == "keep-until-broken") {
    return Strategy{.kind = StrategyKind::kKeepUntilBroken};
  }
  if (text == "longest-lived") {
    return Strategy{.kind = StrategyKind::kLongestLived};
  }
  if (!text.starts_with(kThresholdPrefix)) {
    return unknown_strategy(text);
  }
  std::string_view rest = text.substr(kThresholdPrefix.size());
  if (rest.ends_with("ms")) {
    rest.remove_suffix(2);
    if (const auto ms = parse_positive_decimal(rest)) {
      return Strategy{.kind = StrategyKind::kThreshold,
                      .threshold_kind = ThresholdKind::kAbsolute,
                      .threshold_s = *ms / kMillisecondsPerSecond};
    }
    return unknown_strategy(text);
  }
  if (rest.ends_with('%')) {
    rest.remove_suffix(1);
    if (const auto percent = parse_positive_decimal(rest)) {
      return Strategy{.kind = StrategyKind::kThreshold,
                      .threshold_kind = ThresholdKind::kFraction,
                      .threshold_fraction = *percent / kPercent};
    }
    return unknown_strategy(text);
  }
  return unknown_strategy(text);
}

}  // namespace satsim::routing
