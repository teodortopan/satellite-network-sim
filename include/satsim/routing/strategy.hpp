#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <variant>

namespace satsim::routing {

// The rule that chooses one direction's route at a re-plan instant (spec 4.5). What each kind
// does is in planner.hpp.

enum class StrategyKind : std::uint8_t {
  kLowestDelay,      ///< "lowest-delay": always the shortest path.
  kKeepUntilBroken,  ///< "keep-until-broken": the held route while it is up.
  kLongestLived,     ///< "longest-lived": the held route while it is up, then access candidates.
  kThreshold,        ///< "threshold:<n>ms" or "threshold:<n>%": switch only for a large gain.
};

/// How a threshold is given.
enum class ThresholdKind : std::uint8_t {
  kAbsolute,  ///< A delay: threshold_s.
  kFraction,  ///< A fraction of the held route's delay: threshold_fraction.
};

struct Strategy {
  StrategyKind kind = StrategyKind::kLowestDelay;
  /// Meaningful for kThreshold only.
  ThresholdKind threshold_kind = ThresholdKind::kAbsolute;
  /// kThreshold with kAbsolute: the threshold in seconds (given in milliseconds on the
  /// command line). Positive and finite.
  double threshold_s = 0.0;
  /// kThreshold with kFraction: the threshold as a fraction of the held route's delay (5% is
  /// 0.05). Positive and finite.
  double threshold_fraction = 0.0;

  friend bool operator==(const Strategy&, const Strategy&) = default;
};

/// The command-line spelling: "lowest-delay", "keep-until-broken", "longest-lived",
/// "threshold:<n>ms" (n = threshold_s in milliseconds) or "threshold:<n>%" (n =
/// threshold_fraction in percent). n is printed with up to 15 significant digits, which hides
/// the rounding of the unit conversion, so name_of(parse_strategy(s)) == s for every s that
/// parse_strategy accepts and that is already in that shortest form (no leading zeros other
/// than "0.", no trailing zeros after the point, at most 15 significant digits, and a value
/// that %g prints without an exponent: from 0.0001 up to, not including, 1e15).
[[nodiscard]] std::string name_of(const Strategy& strategy);

/// A Strategy, or why the text is not one.
using StrategyOrError = std::variant<Strategy, std::string>;

/// Reads one of the spellings of name_of. n is a positive decimal: one or more digits,
/// optionally a point and one or more digits ("0.5", "2", "10"); no sign, exponent, spaces,
/// "inf" or "nan", and not zero. Anything else is an error, with a message naming the text.
[[nodiscard]] StrategyOrError parse_strategy(std::string_view text);

}  // namespace satsim::routing
