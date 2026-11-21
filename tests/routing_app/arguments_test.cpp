// The routing program's command line: every option, its defaults, and every rejection.

#include "routing/arguments.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdlib>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "routing/settings.hpp"
#include "satsim/routing/strategy.hpp"
#include "satsim/time/utc_time.hpp"
#include "satsim/topology/link_plan.hpp"

namespace {

using satsim::routing_app::parse_arguments;
using satsim::routing_app::ParsedArguments;
using satsim::routing_app::Planner;
using satsim::routing_app::ServingRule;

const std::vector<std::string_view> kRequired = {"--snapshot",   "snap",
                                                 "--ground",     "ground",
                                                 "--cities",     "cities.json",
                                                 "--start",      "2026-11-18T21:15:00",
                                                 "--duration-s", "600",
                                                 "--step-s",     "1",
                                                 "--pattern",    "three",
                                                 "--model",      "realistic",
                                                 "--strategies", "lowest-delay",
                                                 "--replan-s",   "15",
                                                 "--out",        "-"};

ParsedArguments parse_with(const std::vector<std::string_view>& extra) {
  std::vector<std::string_view> args = kRequired;
  args.insert(args.end(), extra.begin(), extra.end());
  return parse_arguments(args);
}

ParsedArguments parse_replacing(std::string_view option, std::string_view value) {
  std::vector<std::string_view> args = kRequired;
  for (std::size_t i = 0; i + 1 < args.size(); ++i) {
    if (args[i] == option) {
      args[i + 1] = value;
    }
  }
  return parse_arguments(args);
}

satsim::routing_app::Options options_of(const ParsedArguments& p) {
  if (!p.options) {
    ADD_FAILURE() << p.error.value_or("");
    std::abort();
  }
  return *p.options;
}

std::string error_of(const ParsedArguments& p) {
  EXPECT_FALSE(p.options.has_value());
  EXPECT_FALSE(p.help);
  return p.error.value_or("");
}

TEST(RoutingArgumentsTest, RequiredOnlyGivesTheDefaults) {
  const auto o = options_of(parse_arguments(kRequired));
  EXPECT_EQ(o.snapshot_dir, "snap");
  EXPECT_EQ(o.ground_dir, "ground");
  EXPECT_EQ(o.cities_file, "cities.json");
  EXPECT_EQ(o.start, satsim::time::UtcTime::from_civil({2026, 11, 18, 21, 15, 0.0}));
  EXPECT_EQ(o.duration_ns, 600'000'000'000);
  EXPECT_EQ(o.step_ns, 1'000'000'000);
  EXPECT_EQ(o.pattern, satsim::topology::LaserPattern::kThree);
  EXPECT_EQ(o.model, satsim::app::EndpointModel::kRealistic);
  ASSERT_EQ(o.strategies.size(), 1U);
  EXPECT_EQ(satsim::routing::name_of(o.strategies[0]), "lowest-delay");
  EXPECT_EQ(o.replan_ns, 15'000'000'000);
  EXPECT_EQ(o.slot_ns, 15'000'000'000);
  EXPECT_EQ(o.planner, Planner::kStart);
  EXPECT_EQ(o.serving, ServingRule::kHighestElevation);
  EXPECT_FALSE(o.backup);
  EXPECT_EQ(o.hop_delay_s, 0.0);
  EXPECT_TRUE(o.both_directions);
  EXPECT_EQ(o.out, "-");
  EXPECT_FALSE(o.drops_out.has_value());
  EXPECT_FALSE(o.links_out.has_value());
  EXPECT_FALSE(o.links_file.has_value());
  EXPECT_FALSE(o.mask.min_elevation_rad.has_value());
  EXPECT_FALSE(o.mask.max_ground_range_km.has_value());
}

TEST(RoutingArgumentsTest, EveryOption) {
  const auto o = options_of(parse_with({"--planner",
                                        "whole-interval",
                                        "--serving",
                                        "with-route",
                                        "--serving-slot-s",
                                        "30",
                                        "--backup",
                                        "on",
                                        "--hop-delay-ms",
                                        "1.012",
                                        "--directions",
                                        "forward",
                                        "--drops-out",
                                        "drops.csv",
                                        "--links-out",
                                        "links.csv",
                                        "--links-file",
                                        "in.csv",
                                        "--max-lasers",
                                        "4",
                                        "--min-elevation-deg",
                                        "40",
                                        "--shells",
                                        "53deg",
                                        "--roles",
                                        "study",
                                        "--range-cap-km",
                                        "5000",
                                        "--sun-exclusion-deg",
                                        "10",
                                        "--lock-on-s",
                                        "2",
                                        "--fibre-factor",
                                        "1.5"}));
  EXPECT_EQ(o.planner, Planner::kWholeInterval);
  EXPECT_EQ(o.serving, ServingRule::kWithRoute);
  EXPECT_EQ(o.slot_ns, 30'000'000'000);
  EXPECT_TRUE(o.backup);
  EXPECT_EQ(o.hop_delay_s, 1.012 / 1000.0);
  EXPECT_FALSE(o.both_directions);
  EXPECT_EQ(o.drops_out, "drops.csv");
  EXPECT_EQ(o.links_out, "links.csv");
  EXPECT_EQ(o.links_file, "in.csv");
  EXPECT_EQ(o.max_lasers, 4U);
  EXPECT_DOUBLE_EQ(o.mask.min_elevation_rad.value_or(0.0), 40.0 / 180.0 * 3.141592653589793);
  EXPECT_EQ(o.shells, (std::vector<std::string>{"53deg"}));
  EXPECT_EQ(o.roles, satsim::app::RoleFilter::kStudy);
  EXPECT_EQ(o.limits.range_cap_km, 5000.0);
  EXPECT_EQ(o.limits.lock_on_s, 2.0);
  EXPECT_EQ(o.fibre_factor, 1.5);

  EXPECT_EQ(options_of(parse_with({"--max-ground-range-km", "1089.686"})).mask.max_ground_range_km,
            1089.686);
  for (const auto& [name, rule] : {std::pair{"highest-elevation", ServingRule::kHighestElevation},
                                   std::pair{"longest-in-view", ServingRule::kLongestInView},
                                   std::pair{"highest-whole-slot", ServingRule::kHighestWholeSlot},
                                   std::pair{"with-route", ServingRule::kWithRoute}}) {
    EXPECT_EQ(options_of(parse_with({"--serving", name})).serving, rule) << name;
  }
  EXPECT_EQ(options_of(parse_with({"--backup", "off"})).backup, false);
  EXPECT_EQ(options_of(parse_with({"--directions", "both"})).both_directions, true);
  EXPECT_EQ(options_of(parse_replacing("--model", "best-case")).model,
            satsim::app::EndpointModel::kBestCase);
  const auto all = options_of(parse_replacing(
      "--strategies",
      "lowest-delay,keep-until-broken,longest-lived,threshold:0.5ms,threshold:10%"));
  ASSERT_EQ(all.strategies.size(), 5U);
  EXPECT_EQ(satsim::routing::name_of(all.strategies[3]), "threshold:0.5ms");
  EXPECT_EQ(satsim::routing::name_of(all.strategies[4]), "threshold:10%");
  // A re-plan interval and slot that are whole multiples of a fractional step.
  std::vector<std::string_view> args = kRequired;
  for (std::size_t i = 0; i + 1 < args.size(); ++i) {
    if (args[i] == "--step-s" || args[i] == "--replan-s") {
      args[i + 1] = "0.1";
    }
  }
  const auto fine = options_of(parse_arguments(args));
  EXPECT_EQ(fine.replan_ns, 100'000'000);
  EXPECT_EQ(fine.slot_ns, 15'000'000'000);
}

TEST(RoutingArgumentsTest, HelpWins) {
  EXPECT_TRUE(parse_arguments(std::vector<std::string_view>{"--model", "x", "-h"}).help);
  EXPECT_TRUE(parse_with({"--help"}).help);
}

TEST(RoutingArgumentsTest, Rejections) {
  EXPECT_NE(error_of(parse_arguments(std::vector<std::string_view>{})).find("missing required"),
            std::string::npos);
  EXPECT_NE(error_of(parse_with({"--bogus", "1"})).find("unknown"), std::string::npos);
  EXPECT_NE(error_of(parse_with({"--planner", "start", "--planner", "start"})), "");
  EXPECT_NE(error_of(parse_with({"--planner"})), "");
  EXPECT_NE(error_of(parse_replacing("--out", "")).find("--out is empty"), std::string::npos);
  EXPECT_NE(error_of(parse_replacing("--model", "ideal")).find("--model"), std::string::npos);
  for (const std::string_view list :
       {"", "lowest-delay,", "fastest", "threshold:0ms", "lowest-delay,lowest-delay",
        "threshold:2ms,threshold:2.0ms"}) {
    EXPECT_NE(error_of(parse_replacing("--strategies", list)).find("--strategies"),
              std::string::npos)
        << list;
  }
  for (const std::string_view replan : {"0", "-15", "abc", "1.5", "15.0000000001"}) {
    EXPECT_NE(error_of(parse_replacing("--replan-s", replan)).find("--replan-s"), std::string::npos)
        << replan;
  }
  for (const std::string_view slot : {"0", "2.5", "x"}) {
    EXPECT_NE(error_of(parse_with({"--serving-slot-s", slot})).find("--serving-slot-s"),
              std::string::npos)
        << slot;
  }
  // The default slot of 15 s is not a multiple of a 4 s step.
  std::vector<std::string_view> args = kRequired;
  for (std::size_t i = 0; i + 1 < args.size(); ++i) {
    if (args[i] == "--step-s") {
      args[i + 1] = "4";
    } else if (args[i] == "--replan-s") {
      args[i + 1] = "16";
    }
  }
  EXPECT_NE(error_of(parse_arguments(args)).find("serving slot"), std::string::npos);
  EXPECT_NE(error_of(parse_with({"--planner", "later"})).find("--planner"), std::string::npos);
  EXPECT_NE(error_of(parse_with({"--serving", "nearest"})).find("--serving"), std::string::npos);
  EXPECT_NE(error_of(parse_with({"--backup", "yes"})).find("--backup"), std::string::npos);
  for (const std::string_view hop : {"-1", "inf", "nan", "x"}) {
    EXPECT_NE(error_of(parse_with({"--hop-delay-ms", hop})).find("--hop-delay-ms"),
              std::string::npos)
        << hop;
  }
  EXPECT_NE(error_of(parse_with({"--directions", "reverse"})).find("--directions"),
            std::string::npos);
  EXPECT_NE(error_of(parse_with({"--drops-out", "-"})).find("--drops-out"), std::string::npos);
  EXPECT_NE(error_of(parse_with({"--drops-out", ""})), "");
  EXPECT_NE(error_of(parse_with({"--links-out", ""})).find("--links-out"), std::string::npos);
  // The shared options are refused as in the topology program.
  EXPECT_NE(error_of(parse_replacing("--step-s", "0")).find("--step-s"), std::string::npos);
  EXPECT_NE(error_of(parse_replacing("--pattern", "ring")).find("--pattern"), std::string::npos);
  EXPECT_NE(error_of(parse_with({"--max-lasers", "4"})).find("--links-file"), std::string::npos);
  EXPECT_NE(error_of(parse_with({"--min-elevation-deg", "40", "--max-ground-range-km", "1000"}))
                .find("cannot be combined"),
            std::string::npos);
}

// --plan-time is the shared option (its rejections are the topology program's tests).
TEST(RoutingArgumentsTest, PlanTime) {
  EXPECT_FALSE(options_of(parse_arguments(kRequired)).plan_time.has_value());
  EXPECT_EQ(options_of(parse_with({"--plan-time", "2026-11-18T21:14:00"})).plan_time,
            satsim::time::UtcTime::from_civil({2026, 11, 18, 21, 14, 0.0}));
  EXPECT_EQ(error_of(parse_with({"--plan-time", "x"})),
            "--plan-time \"x\" is not a UTC time YYYY-MM-DDTHH:MM:SS[.fffffffff]");
  EXPECT_NE(satsim::routing_app::usage().find("--plan-time <UTC>"), std::string_view::npos);
}

}  // namespace
