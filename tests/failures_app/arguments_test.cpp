// The failures program's command line: every option of its own, the routing program's taken
// as that program takes them, and every refusal.

#include "failures/arguments.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdlib>
#include <string>
#include <string_view>
#include <vector>

#include "failures/settings.hpp"
#include "routing/settings.hpp"

namespace {

using satsim::failures_app::parse_arguments;
using satsim::failures_app::ParsedArguments;
using satsim::failures_app::Reaction;

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
                                                 "--out",        "-",
                                                 "--reactions",  "planner"};

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

satsim::failures_app::Options options_of(const ParsedArguments& p) {
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

TEST(FailuresArgumentsTest, RequiredOnlyGivesTheDefaults) {
  const auto o = options_of(parse_arguments(kRequired));
  EXPECT_EQ(o.reactions, std::vector<Reaction>{Reaction::kPlanner});
  EXPECT_FALSE(o.failures.has_value());
  EXPECT_FALSE(o.replan_on_report);
  EXPECT_EQ(o.times.detection_s, 0.1);
  EXPECT_EQ(o.times.hop_processing_s, 0.001);
  EXPECT_EQ(o.times.compute_s, 0.1);
  EXPECT_EQ(o.times.planner_processing_s, 0.0);
  EXPECT_EQ(o.times.acquisition_s, 30.0);
  EXPECT_FALSE(o.times.repoint);
  EXPECT_FALSE(o.events_out || o.facts_out || o.floods_out || o.measures_out || o.network_out);
  // The routing program's options, as it parses them.
  EXPECT_EQ(o.routing.out, "-");
  EXPECT_EQ(o.routing.replan_ns, 15'000'000'000);
  EXPECT_EQ(o.routing.strategies.size(), 1U);
}

TEST(FailuresArgumentsTest, EveryOptionOfItsOwn) {
  const auto o = options_of(parse_with({"--failures",
                                        "f.csv",
                                        "--replan-on-report",
                                        "on",
                                        "--repoint",
                                        "on",
                                        "--detection-s",
                                        "0.01",
                                        "--hop-processing-s",
                                        "0.002",
                                        "--compute-s",
                                        "0.5",
                                        "--planner-processing-s",
                                        "1.5",
                                        "--acquisition-s",
                                        "12",
                                        "--events-out",
                                        "e.csv",
                                        "--facts-out",
                                        "x.csv",
                                        "--floods-out",
                                        "fl.csv",
                                        "--measures-out",
                                        "m.csv",
                                        "--network-out",
                                        "n.csv",
                                        "--planner",
                                        "whole-interval",
                                        "--lock-on-s",
                                        "0",
                                        "--drops-out",
                                        "d.csv"}));
  EXPECT_EQ(o.failures.value_or("").string(), "f.csv");
  EXPECT_TRUE(o.replan_on_report);
  EXPECT_TRUE(o.times.repoint);
  EXPECT_EQ(o.times.detection_s, 0.01);
  EXPECT_EQ(o.times.hop_processing_s, 0.002);
  EXPECT_EQ(o.times.compute_s, 0.5);
  EXPECT_EQ(o.times.planner_processing_s, 1.5);
  EXPECT_EQ(o.times.acquisition_s, 12.0);
  EXPECT_EQ(o.events_out.value_or("").string(), "e.csv");
  EXPECT_EQ(o.facts_out.value_or("").string(), "x.csv");
  EXPECT_EQ(o.floods_out.value_or("").string(), "fl.csv");
  EXPECT_EQ(o.measures_out.value_or("").string(), "m.csv");
  EXPECT_EQ(o.network_out.value_or("").string(), "n.csv");
  EXPECT_EQ(o.routing.planner, satsim::routing_app::Planner::kWholeInterval);
  EXPECT_EQ(o.routing.drops_out.value_or("").string(), "d.csv");
}

TEST(FailuresArgumentsTest, ReactionLists) {
  EXPECT_EQ(
      options_of(parse_replacing("--reactions", "local-repair,planner,flooding")).reactions,
      (std::vector<Reaction>{Reaction::kLocalRepair, Reaction::kPlanner, Reaction::kFlooding}));
  EXPECT_EQ(error_of(parse_replacing("--reactions", "planner,planner")),
            "--reactions: \"planner\" is named twice");
  EXPECT_EQ(error_of(parse_replacing("--reactions", "planner,")),
            "--reactions: \"\" is not planner, local-repair or flooding");
  EXPECT_EQ(error_of(parse_replacing("--reactions", "")),
            "--reactions: \"\" is not planner, local-repair or flooding");
  EXPECT_EQ(error_of(parse_replacing("--reactions", "oracle")),
            "--reactions: \"oracle\" is not planner, local-repair or flooding");
}

TEST(FailuresArgumentsTest, Refusals) {
  std::vector<std::string_view> without(kRequired.begin(), kRequired.end() - 2);
  EXPECT_EQ(error_of(parse_arguments(without)), "missing required option(s): --reactions");
  EXPECT_EQ(error_of(parse_with({"--lock-on-s", "1"})),
            "--lock-on-s must be 0 here: --acquisition-s is the failures program's lock-on");
  EXPECT_NE(error_of(parse_with({"--serving", "with-route"}))
                .find("--serving with-route is "
                      "refused here"),
            std::string::npos);
  EXPECT_EQ(error_of(parse_with({"--failures", ""})), "--failures is empty");
  EXPECT_EQ(error_of(parse_with({"--replan-on-report", "yes"})),
            "--replan-on-report \"yes\" is not on or off");
  EXPECT_EQ(error_of(parse_with({"--repoint", "1"})), "--repoint \"1\" is not on or off");
  for (const std::string_view name : {"--detection-s", "--hop-processing-s", "--compute-s",
                                      "--planner-processing-s", "--acquisition-s"}) {
    for (const std::string_view bad : {"-1", "inf", "x", ""}) {
      EXPECT_EQ(error_of(parse_with({name, bad})), std::string(name) + " \"" + std::string(bad) +
                                                       "\" is not a finite number of seconds >= 0");
    }
  }
  for (const std::string_view name :
       {"--events-out", "--facts-out", "--floods-out", "--measures-out", "--network-out"}) {
    EXPECT_EQ(error_of(parse_with({name, "-"})), std::string(name) + " must name a file");
    EXPECT_EQ(error_of(parse_with({name, ""})), std::string(name) + " must name a file");
  }
  // The routing program's refusals come through unchanged.
  EXPECT_EQ(error_of(parse_replacing("--model", "x")),
            "--model \"x\" is not realistic or best-case");
  EXPECT_EQ(error_of(parse_with({"--unknown", "1"})), "unknown argument \"--unknown\"");
}

TEST(FailuresArgumentsTest, HelpWins) {
  EXPECT_TRUE(parse_arguments(std::vector<std::string_view>{"--bad", "-h"}).help);
  EXPECT_NE(satsim::failures_app::usage().find("--reactions"), std::string_view::npos);
}

}  // namespace
