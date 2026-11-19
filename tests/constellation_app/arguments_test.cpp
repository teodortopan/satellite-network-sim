#include "constellation/arguments.hpp"

#include <gtest/gtest.h>

#include <string>
#include <string_view>
#include <vector>

#include "satsim/time/utc_time.hpp"

namespace {

using satsim::constellation_app::parse_arguments;
using satsim::constellation_app::ParsedArguments;

ParsedArguments parse(const std::vector<std::string_view>& args) { return parse_arguments(args); }

TEST(ConstellationArgumentsTest, ValidArguments) {
  const ParsedArguments p =
      parse({"--snapshot", "data/x", "--time", "2026-11-18T21:15:00.5", "--out", "-"});
  if (!p.options) {
    FAIL() << p.error.value_or("");
  }
  EXPECT_EQ(p.options->snapshot_dir, "data/x");
  EXPECT_EQ(p.options->out, "-");
  EXPECT_EQ(satsim::time::UtcTime::from_civil({2026, 11, 18, 21, 15, 0.5}), p.options->reference);
  EXPECT_FALSE(p.help);
}

TEST(ConstellationArgumentsTest, OrderDoesNotMatter) {
  const ParsedArguments p =
      parse({"--out", "a.csv", "--time", "2026-11-18T21:15:00", "--snapshot", "d"});
  if (!p.options) {
    FAIL() << p.error.value_or("");
  }
  EXPECT_EQ(p.options->out, "a.csv");
}

TEST(ConstellationArgumentsTest, HelpWinsOverAnyProblem) {
  EXPECT_TRUE(parse({"--bogus", "-h"}).help);
  EXPECT_TRUE(parse({"--help"}).help);
}

TEST(ConstellationArgumentsTest, Rejections) {
  const auto error = [](const std::vector<std::string_view>& args) {
    const ParsedArguments p = parse(args);
    EXPECT_FALSE(p.options.has_value());
    return p.error.value_or("");
  };
  EXPECT_EQ(error({}), "missing required option(s): --snapshot, --time, --out");
  EXPECT_EQ(error({"--snapshot", "d", "--time", "2026-11-18T21:15:00"}),
            "missing required option(s): --out");
  EXPECT_EQ(error({"--start", "x"}), "unknown argument \"--start\"");
  EXPECT_EQ(error({"--out", "-", "--out", "-"}), "--out given more than once");
  EXPECT_EQ(error({"--snapshot", "--time", "2026-11-18T21:15:00"}), "--snapshot needs a value");
  EXPECT_EQ(error({"--snapshot", "", "--time", "2026-11-18T21:15:00", "--out", "-"}),
            "--snapshot is empty");
  EXPECT_EQ(error({"--snapshot", "d", "--time", "2026-11-18T21:15:00", "--out", ""}),
            "--out is empty (use - for standard output)");
  EXPECT_EQ(error({"--snapshot", "d", "--time", "2026-11-18 21:15:00", "--out", "-"}),
            "--time \"2026-11-18 21:15:00\" is not a UTC time YYYY-MM-DDTHH:MM:SS[.fffffffff]");
  EXPECT_EQ(error({"--snapshot", "d", "--time", "2026-02-30T00:00:00", "--out", "-"}),
            "--time \"2026-02-30T00:00:00\" is not an existing date and time");
}

}  // namespace
