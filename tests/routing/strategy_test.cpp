#include "satsim/routing/strategy.hpp"

#include <gtest/gtest.h>

#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace {

using satsim::routing::name_of;
using satsim::routing::parse_strategy;
using satsim::routing::Strategy;
using satsim::routing::StrategyKind;
using satsim::routing::ThresholdKind;

Strategy parsed(std::string_view text) {
  const auto result = parse_strategy(text);
  EXPECT_TRUE(std::holds_alternative<Strategy>(result)) << text;
  return std::holds_alternative<Strategy>(result) ? std::get<Strategy>(result) : Strategy{};
}

TEST(StrategyTest, ParsesThePlainKinds) {
  EXPECT_EQ(parsed("lowest-delay").kind, StrategyKind::kLowestDelay);
  EXPECT_EQ(parsed("keep-until-broken").kind, StrategyKind::kKeepUntilBroken);
  EXPECT_EQ(parsed("longest-lived").kind, StrategyKind::kLongestLived);
}

TEST(StrategyTest, ParsesThresholdsInMillisecondsAndPercent) {
  const Strategy ms = parsed("threshold:2ms");
  EXPECT_EQ(ms.kind, StrategyKind::kThreshold);
  EXPECT_EQ(ms.threshold_kind, ThresholdKind::kAbsolute);
  EXPECT_DOUBLE_EQ(ms.threshold_s, 0.002);
  EXPECT_DOUBLE_EQ(parsed("threshold:0.5ms").threshold_s, 0.0005);

  const Strategy percent = parsed("threshold:5%");
  EXPECT_EQ(percent.kind, StrategyKind::kThreshold);
  EXPECT_EQ(percent.threshold_kind, ThresholdKind::kFraction);
  EXPECT_DOUBLE_EQ(percent.threshold_fraction, 0.05);
  EXPECT_DOUBLE_EQ(parsed("threshold:12.5%").threshold_fraction, 0.125);
}

TEST(StrategyTest, NameParseNameRoundTrips) {
  // The spec's runs (4.5), plus a few more.
  for (const std::string_view name :
       {"lowest-delay", "keep-until-broken", "longest-lived", "threshold:0.5ms", "threshold:1ms",
        "threshold:2ms", "threshold:5ms", "threshold:1%", "threshold:2%", "threshold:5%",
        "threshold:10%", "threshold:0.1ms", "threshold:0.3%", "threshold:250ms", "threshold:150%",
        "threshold:0.001ms"}) {
    EXPECT_EQ(name_of(parsed(name)), name);
  }
}

TEST(StrategyTest, NamesBuiltInCode) {
  EXPECT_EQ(name_of(Strategy{}), "lowest-delay");
  EXPECT_EQ(name_of(Strategy{.kind = StrategyKind::kThreshold,
                             .threshold_kind = ThresholdKind::kAbsolute,
                             .threshold_s = 0.0015}),
            "threshold:1.5ms");
  EXPECT_EQ(name_of(Strategy{.kind = StrategyKind::kThreshold,
                             .threshold_kind = ThresholdKind::kFraction,
                             .threshold_fraction = 0.02}),
            "threshold:2%");
}

TEST(StrategyTest, RefusesEverythingElse) {
  const std::vector<std::string_view> bad_spellings{"",
                                                    "lowest",
                                                    "Lowest-delay",
                                                    "lowest-delay ",
                                                    " lowest-delay",
                                                    "threshold",
                                                    "threshold:",
                                                    "threshold:ms",
                                                    "threshold:%",
                                                    "threshold:5",
                                                    "threshold:5s",
                                                    "threshold:5 ms",
                                                    "threshold: 5ms",
                                                    "threshold:0ms",
                                                    "threshold:0.0%",
                                                    "threshold:-1ms",
                                                    "threshold:+1ms",
                                                    "threshold:1e3ms",
                                                    "threshold:infms",
                                                    "threshold:nan%",
                                                    "threshold:.5ms",
                                                    "threshold:5.ms",
                                                    "threshold:1.2.3ms",
                                                    "threshold:5ms%",
                                                    "threshold:5%ms",
                                                    "threshold:0x1ms",
                                                    "threshold=5ms",
                                                    "threshold:5MS"};
  for (const std::string_view bad : bad_spellings) {
    const auto result = parse_strategy(bad);
    ASSERT_TRUE(std::holds_alternative<std::string>(result)) << "'" << bad << "'";
    EXPECT_NE(std::get<std::string>(result).find(std::string(bad)), std::string::npos);
  }
}

}  // namespace
