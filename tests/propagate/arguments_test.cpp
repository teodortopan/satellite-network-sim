#include "propagate/arguments.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <initializer_list>
#include <string>
#include <string_view>
#include <vector>

#include "test_support.hpp"

namespace {

using satsim::propagate::Options;
using satsim::propagate::ParsedArguments;
using satsim::propagate::testing::utc;

ParsedArguments parse(std::initializer_list<std::string_view> args) {
  const std::vector<std::string_view> v(args);
  return satsim::propagate::parse_arguments(v);
}

// A valid command line with one option's value replaced (or the option left out when the
// replacement is null).
std::vector<std::string_view> valid_with(std::string_view name, const char* value) {
  const std::vector<std::pair<std::string_view, std::string_view>> valid = {
      {"--snapshot", "snap"},  {"--start", "2026-11-18T21:00:00"},
      {"--duration-s", "600"}, {"--step-s", "60"},
      {"--out", "out.csv"},
  };
  std::vector<std::string_view> args;
  for (const auto& [option, default_value] : valid) {
    if (option == name && value == nullptr) {
      continue;
    }
    args.push_back(option);
    args.push_back(option == name ? std::string_view(value) : default_value);
  }
  return args;
}

std::string error_with(std::string_view name, const char* value) {
  const std::vector<std::string_view> args = valid_with(name, value);
  const ParsedArguments parsed = satsim::propagate::parse_arguments(args);
  EXPECT_FALSE(parsed.options.has_value())
      << name << " " << (value != nullptr ? value : "(absent)");
  EXPECT_FALSE(parsed.help);
  return parsed.error.value_or("");
}

TEST(PropagateArgumentsTest, ReadsEveryOption) {
  const ParsedArguments parsed =
      parse({"--snapshot", "data/starlink/2026-11-18", "--start", "2026-11-18T21:00:00.25",
             "--duration-s", "3600.5", "--step-s", "0.000000001", "--out", "-", "--ids",
             "799501782,44714,44714,1"});
  if (!parsed.options) {
    FAIL() << parsed.error.value_or("");
  }
  EXPECT_FALSE(parsed.error.has_value());
  EXPECT_FALSE(parsed.help);
  const Options& o = *parsed.options;
  EXPECT_EQ(o.snapshot_dir, "data/starlink/2026-11-18");
  EXPECT_EQ(o.start, utc("2026-11-18T21:00:00.25"));
  EXPECT_EQ(o.duration_ns, 3'600'500'000'000);
  EXPECT_EQ(o.step_ns, 1);
  EXPECT_EQ(o.out, "-");
  EXPECT_EQ(o.ids, (std::vector<std::int64_t>{1, 44714, 799501782}));
}

TEST(PropagateArgumentsTest, OrderDoesNotMatterAndIdsAreOptional) {
  const ParsedArguments parsed = parse({"--out", "x.csv", "--step-s", "60", "--duration-s", "0",
                                        "--start", "2026-11-18T21:00:00", "--snapshot", "s"});
  if (!parsed.options) {
    FAIL() << parsed.error.value_or("");
  }
  EXPECT_EQ(parsed.options->duration_ns, 0);
  EXPECT_EQ(parsed.options->step_ns, 60'000'000'000);
  EXPECT_TRUE(parsed.options->ids.empty());
}

TEST(PropagateArgumentsTest, HelpWinsOverAnyOtherProblem) {
  EXPECT_TRUE(parse({"--help"}).help);
  EXPECT_TRUE(parse({"-h"}).help);
  const ParsedArguments parsed = parse({"--bogus", "--step-s", "0", "--help"});
  EXPECT_TRUE(parsed.help);
  EXPECT_FALSE(parsed.options.has_value());
  EXPECT_FALSE(parsed.error.has_value());
  EXPECT_NE(satsim::propagate::usage().find("usage: propagate"), std::string_view::npos);
}

TEST(PropagateArgumentsTest, RejectsMalformedCommandLines) {
  EXPECT_EQ(parse({}).error,
            "missing required option(s): --snapshot, --start, --duration-s, "
            "--step-s, --out");
  EXPECT_EQ(error_with("--out", nullptr), "missing required option(s): --out");
  EXPECT_EQ(parse({"--snapshot", "s", "extra"}).error, "unknown argument \"extra\"");
  EXPECT_EQ(parse({"--snapshot", "a", "--snapshot", "b"}).error, "--snapshot given more than once");
  EXPECT_EQ(parse({"--snapshot"}).error, "--snapshot needs a value");
  EXPECT_EQ(parse({"--snapshot", "--start", "x"}).error, "--snapshot needs a value");
  EXPECT_EQ(error_with("--snapshot", ""), "--snapshot is empty");
  EXPECT_EQ(error_with("--out", ""), "--out is empty (use - for standard output)");
}

TEST(PropagateArgumentsTest, RejectsBadStartTimes) {
  for (const char* bad : {"2026-11-18", "2026-11-18 21:00:00", "2026-11-18T21:00:00Z", "now"}) {
    EXPECT_EQ(
        error_with("--start", bad),
        "--start \"" + std::string(bad) + "\" is not a UTC time YYYY-MM-DDTHH:MM:SS[.fffffffff]");
  }
  for (const char* bad : {"2026-02-29T00:00:00", "2026-11-18T24:00:00", "2026-11-18T23:59:60"}) {
    EXPECT_EQ(error_with("--start", bad),
              "--start \"" + std::string(bad) + "\" is not an existing date and time");
  }
}

TEST(PropagateArgumentsTest, DurationAndStepAreExactDecimalSeconds) {
  const auto step_ns = [](const char* text) {
    const ParsedArguments parsed = satsim::propagate::parse_arguments(valid_with("--step-s", text));
    EXPECT_TRUE(parsed.options.has_value()) << text << ": " << parsed.error.value_or("");
    return parsed.options ? parsed.options->step_ns : -1;
  };
  EXPECT_EQ(step_ns("0.1"), 100'000'000);
  EXPECT_EQ(step_ns("1.000000001"), 1'000'000'001);
  EXPECT_EQ(step_ns("007"), 7'000'000'000);
  EXPECT_EQ(step_ns("1000000000"), 1'000'000'000'000'000'000);

  for (const char* bad : {"0", "0.0", "-5", "+5", "1e3", "1.", ".5", "1.0000000001", "", "1,5",
                          "inf", "nan", " 1", "1000000000.000000001", "99999999999"}) {
    const std::string message = error_with("--step-s", bad);
    EXPECT_EQ(message.rfind("--step-s \"", 0), 0U) << bad << ": " << message;
  }
  for (const char* bad : {"-1", "1e3", "1000000001", "0.5.5"}) {
    const std::string message = error_with("--duration-s", bad);
    EXPECT_EQ(message.rfind("--duration-s \"", 0), 0U) << bad << ": " << message;
  }
}

TEST(PropagateArgumentsTest, RejectsBadIds) {
  for (const char* bad : {"", "1,,2", "1,", ",1", "0", "1000000000", "12a", "-3", "1 ,2"}) {
    std::vector<std::string_view> args = valid_with("", nullptr);
    args.emplace_back("--ids");
    args.emplace_back(bad);
    const ParsedArguments parsed = satsim::propagate::parse_arguments(args);
    EXPECT_FALSE(parsed.options.has_value()) << bad;
    EXPECT_EQ(parsed.error.value_or("").rfind("--ids: ", 0), 0U)
        << bad << ": " << parsed.error.value_or("");
  }
}

}  // namespace
