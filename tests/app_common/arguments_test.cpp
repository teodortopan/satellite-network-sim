#include "common/arguments.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <optional>
#include <string_view>
#include <vector>

#include "common/steps.hpp"

namespace {

using satsim::app::parse_finite_double;
using satsim::app::parse_seconds_as_ns;
using satsim::app::read_option_values;

TEST(AppArgumentsTest, SecondsAreReadExactlyAsNanoseconds) {
  EXPECT_EQ(parse_seconds_as_ns("15"), 15'000'000'000);
  EXPECT_EQ(parse_seconds_as_ns("0.1"), 100'000'000);
  EXPECT_EQ(parse_seconds_as_ns("0.000000001"), 1);
  EXPECT_EQ(parse_seconds_as_ns("1000000000"), satsim::app::kMaxDurationNs);
  for (const std::string_view bad :
       {"", "-1", "+1", "1e3", "1.", ".5", "1.0000000001", "1000000000.1", " 1", "inf"}) {
    EXPECT_FALSE(parse_seconds_as_ns(bad).has_value()) << bad;
  }
}

TEST(AppArgumentsTest, FiniteDoublesOnly) {
  EXPECT_EQ(parse_finite_double("1.98"), 1.98);
  EXPECT_EQ(parse_finite_double("-0.5"), -0.5);
  EXPECT_EQ(parse_finite_double("1e3"), 1000.0);
  for (const std::string_view bad : {"", "inf", "nan", "-inf", "+1", " 1", "1 ", "1x", "1e999"}) {
    EXPECT_FALSE(parse_finite_double(bad).has_value()) << bad;
  }
}

TEST(AppArgumentsTest, OptionValues) {
  constexpr std::array<std::string_view, 2> kNames = {"--a", "--b"};
  {
    const std::vector<std::string_view> args = {"--b", "x", "--a", "-"};
    const auto read = read_option_values(args, kNames);
    ASSERT_FALSE(read.error.has_value());
    EXPECT_EQ(read.values[0], std::optional<std::string_view>("-"));
    EXPECT_EQ(read.values[1], std::optional<std::string_view>("x"));
  }
  {
    const std::vector<std::string_view> args = {"--a", "1"};
    const auto read = read_option_values(args, kNames);
    ASSERT_FALSE(read.error.has_value());
    EXPECT_FALSE(read.values[1].has_value());
  }
  const auto error_of = [&](std::vector<std::string_view> args) {
    return read_option_values(args, kNames).error.value_or("");
  };
  EXPECT_EQ(error_of({"--c", "1"}), "unknown argument \"--c\"");
  EXPECT_EQ(error_of({"--a", "1", "--a", "2"}), "--a given more than once");
  EXPECT_EQ(error_of({"--a"}), "--a needs a value");
  EXPECT_EQ(error_of({"--a", "--b", "1"}), "--a needs a value");
}

}  // namespace
