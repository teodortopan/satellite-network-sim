#include "topology/arguments.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdlib>
#include <numbers>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "satsim/ground/distance.hpp"
#include "satsim/time/utc_time.hpp"
#include "satsim/topology/link_plan.hpp"
#include "topology/endpoints.hpp"

namespace {

using satsim::topology_app::EndpointModel;
using satsim::topology_app::parse_arguments;
using satsim::topology_app::ParsedArguments;
using satsim::topology_app::RoleFilter;

const std::vector<std::string_view> kRequired = {"--snapshot",   "snap",
                                                 "--ground",     "ground",
                                                 "--cities",     "cities.json",
                                                 "--start",      "2026-11-18T21:15:00",
                                                 "--duration-s", "43200",
                                                 "--step-s",     "15",
                                                 "--pattern",    "three",
                                                 "--model",      "realistic",
                                                 "--out",        "-"};

ParsedArguments parse(const std::vector<std::string_view>& args) { return parse_arguments(args); }

// The required arguments with `extra` appended.
ParsedArguments parse_with(const std::vector<std::string_view>& extra) {
  std::vector<std::string_view> args = kRequired;
  args.insert(args.end(), extra.begin(), extra.end());
  return parse(args);
}

// The required arguments with one option's value replaced.
ParsedArguments parse_replacing(std::string_view option, std::string_view value) {
  std::vector<std::string_view> args = kRequired;
  for (std::size_t i = 0; i + 1 < args.size(); ++i) {
    if (args[i] == option) {
      args[i + 1] = value;
    }
  }
  return parse(args);
}

// The options of arguments that must be valid; the test stops if they are not.
const satsim::topology_app::Options& options_of(const ParsedArguments& p) {
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

TEST(TopologyArgumentsTest, RequiredOnlyGivesTheDefaults) {
  const ParsedArguments p = parse(kRequired);
  if (!p.options) {
    FAIL() << p.error.value_or("");
  }
  const auto& o = *p.options;
  EXPECT_EQ(o.snapshot_dir, "snap");
  EXPECT_EQ(o.ground_dir, "ground");
  EXPECT_EQ(o.cities_file, "cities.json");
  EXPECT_EQ(o.start, satsim::time::UtcTime::from_civil({2026, 11, 18, 21, 15, 0.0}));
  EXPECT_EQ(o.duration_ns, 43'200'000'000'000);
  EXPECT_EQ(o.step_ns, 15'000'000'000);
  EXPECT_EQ(o.pattern, satsim::topology::LaserPattern::kThree);
  EXPECT_EQ(o.model, EndpointModel::kRealistic);
  EXPECT_EQ(o.out, "-");
  EXPECT_TRUE(o.shells.empty());
  EXPECT_EQ(o.roles, RoleFilter::kAll);
  EXPECT_FALSE(o.limits.range_cap_km.has_value());
  EXPECT_FALSE(o.limits.sun_exclusion_rad.has_value());
  EXPECT_EQ(o.limits.lock_on_s, 0.0);
  EXPECT_EQ(o.limits.atmosphere_km, 80.0);
  EXPECT_EQ(o.fibre_factor, satsim::ground::kDefaultFibreRouteInflation);
  EXPECT_FALSE(o.mask.high_latitude_allowance);
  EXPECT_EQ(o.serving_rule, satsim::topology::ServingRule::kHighestElevation);
  EXPECT_EQ(o.classification.shells.size(), 4U);
}

TEST(TopologyArgumentsTest, EveryOption) {
  const ParsedArguments p =
      parse_with({"--shells", "43deg,53deg", "--roles", "validation", "--range-cap-km", "2500.5",
                  "--sun-exclusion-deg", "180", "--lock-on-s", "30.25", "--fibre-factor", "1.5",
                  "--high-latitude-mask", "on", "--serving", "highest-elevation"});
  if (!p.options) {
    FAIL() << p.error.value_or("");
  }
  const auto& o = *p.options;
  EXPECT_EQ(o.shells, (std::vector<std::string>{"43deg", "53deg"}));
  EXPECT_EQ(o.roles, RoleFilter::kValidation);
  EXPECT_EQ(o.limits.range_cap_km, 2500.5);
  EXPECT_EQ(o.limits.sun_exclusion_rad, std::numbers::pi);  // Exactly pi: (0, pi] accepts it.
  EXPECT_EQ(o.limits.lock_on_s, 30.25);
  EXPECT_EQ(o.fibre_factor, 1.5);
  EXPECT_TRUE(o.mask.high_latitude_allowance);

  for (const auto& [name, pattern] :
       {std::pair{"three", satsim::topology::LaserPattern::kThree},
        std::pair{"plus-grid", satsim::topology::LaserPattern::kPlusGrid},
        std::pair{"six", satsim::topology::LaserPattern::kSix}}) {
    const ParsedArguments q = parse_replacing("--pattern", name);
    ASSERT_TRUE(q.options.has_value()) << name;
    EXPECT_EQ(options_of(q).pattern, pattern);
  }
  const ParsedArguments best = parse_replacing("--model", "best-case");
  ASSERT_TRUE(best.options.has_value());
  EXPECT_EQ(options_of(best).model, EndpointModel::kBestCase);
  for (const auto& [name, roles] :
       {std::pair{"study", RoleFilter::kStudy}, std::pair{"all", RoleFilter::kAll}}) {
    const ParsedArguments q = parse_with({"--roles", name});
    ASSERT_TRUE(q.options.has_value()) << name;
    EXPECT_EQ(options_of(q).roles, roles);
  }
  const ParsedArguments off = parse_with({"--high-latitude-mask", "off"});
  ASSERT_TRUE(off.options.has_value());
  EXPECT_FALSE(options_of(off).mask.high_latitude_allowance);
  const ParsedArguments sun = parse_with({"--sun-exclusion-deg", "20"});
  ASSERT_TRUE(sun.options.has_value());
  EXPECT_DOUBLE_EQ(options_of(sun).limits.sun_exclusion_rad.value_or(0.0),
                   20.0 * std::numbers::pi / 180.0);
  // The order of the arguments does not matter.
  std::vector<std::string_view> reversed;
  for (std::size_t i = kRequired.size(); i >= 2; i -= 2) {
    reversed.push_back(kRequired[i - 2]);
    reversed.push_back(kRequired[i - 1]);
  }
  EXPECT_TRUE(parse(reversed).options.has_value());
}

TEST(TopologyArgumentsTest, ShellsFileDefersTheShellNamesToTheFile) {
  EXPECT_FALSE(options_of(parse(kRequired)).shells_file.has_value());
  const ParsedArguments p = parse_with({"--shells-file", "ideal.json", "--shells", "ideal,x"});
  const auto& o = options_of(p);
  EXPECT_EQ(o.shells_file.value_or(""), "ideal.json");
  // Not checked against the built-in shells: the file is read when the run is prepared.
  EXPECT_EQ(o.shells, (std::vector<std::string>{"ideal", "x"}));
  EXPECT_EQ(error_of(parse_with({"--shells-file", ""})), "--shells-file is empty");
  EXPECT_EQ(error_of(parse_with({"--shells-file", "f.json", "--shells", "a,a"})),
            "--shells: \"a\" is named twice");
  EXPECT_EQ(error_of(parse_with({"--shells-file", "f.json", "--shells", "a,"})),
            "--shells: \"\" is not a shell");
}

TEST(TopologyArgumentsTest, HelpWinsOverAnyProblem) {
  EXPECT_TRUE(parse({"--bogus", "-h"}).help);
  EXPECT_TRUE(parse({"--help"}).help);
  EXPECT_TRUE(parse_with({"--help"}).help);
  EXPECT_NE(satsim::topology_app::usage().find("usage: topology"), std::string_view::npos);
}

TEST(TopologyArgumentsTest, StructuralRejections) {
  EXPECT_EQ(error_of(parse({})),
            "missing required option(s): --snapshot, --ground, --cities, --start, --duration-s, "
            "--step-s, --pattern, --model, --out");
  std::vector<std::string_view> no_out(kRequired.begin(), kRequired.end() - 2);
  EXPECT_EQ(error_of(parse(no_out)), "missing required option(s): --out");
  EXPECT_EQ(error_of(parse_with({"--bogus", "1"})), "unknown argument \"--bogus\"");
  EXPECT_EQ(error_of(parse_with({"--out", "x"})), "--out given more than once");
  EXPECT_EQ(error_of(parse_with({"--shells"})), "--shells needs a value");
  EXPECT_EQ(error_of(parse_with({"--roles", "--shells", "53deg"})), "--roles needs a value");
  for (const std::string_view path : {"--snapshot", "--ground", "--cities"}) {
    EXPECT_EQ(error_of(parse_replacing(path, "")), std::string(path) + " is empty");
  }
  EXPECT_EQ(error_of(parse_replacing("--out", "")), "--out is empty (use - for standard output)");
}

TEST(TopologyArgumentsTest, ValueRejections) {
  const auto rejected = [](const ParsedArguments& p, std::string_view starts_with) {
    const std::string error = error_of(p);
    EXPECT_EQ(error.substr(0, starts_with.size()), starts_with) << error;
  };
  rejected(parse_replacing("--start", "2026-11-18 21:15:00"),
           "--start \"2026-11-18 21:15:00\" is not a UTC");
  rejected(parse_replacing("--start", "2026-02-30T00:00:00"),
           "--start \"2026-02-30T00:00:00\" is not an existing");
  for (const std::string_view bad : {"-1", "1e3", "x", "1.0000000001", "1000000001"}) {
    rejected(parse_replacing("--duration-s", bad), "--duration-s");
  }
  for (const std::string_view bad : {"0", "0.0", "-15", "15s"}) {
    rejected(parse_replacing("--step-s", bad), "--step-s");
  }
  rejected(parse_replacing("--pattern", "grid"), "--pattern \"grid\" is not three");
  rejected(parse_replacing("--model", "best"), "--model \"best\" is not realistic");
  rejected(parse_with({"--shells", "54deg"}), "--shells: \"54deg\" is not a shell (shells: 53deg");
  rejected(parse_with({"--shells", "53deg,53deg"}), "--shells: \"53deg\" is named twice");
  rejected(parse_with({"--shells", ""}), "--shells: \"\" is not a shell");
  rejected(parse_with({"--shells", "53deg,"}), "--shells: \"\" is not a shell");
  rejected(parse_with({"--roles", "both"}), "--roles \"both\" is not study");
  for (const std::string_view bad : {"0", "-1", "inf", "nan", "x", "1e999"}) {
    rejected(parse_with({"--range-cap-km", bad}), "--range-cap-km");
  }
  for (const std::string_view bad : {"0", "-5", "180.0001", "nan", "x"}) {
    rejected(parse_with({"--sun-exclusion-deg", bad}), "--sun-exclusion-deg");
  }
  for (const std::string_view bad : {"-1", "1e1", "x"}) {
    rejected(parse_with({"--lock-on-s", bad}), "--lock-on-s");
  }
  for (const std::string_view bad : {"0.99", "0", "-2", "inf", "x"}) {
    rejected(parse_with({"--fibre-factor", bad}), "--fibre-factor");
  }
  rejected(parse_with({"--high-latitude-mask", "yes"}), "--high-latitude-mask \"yes\" is not on");
  rejected(parse_with({"--serving", "lowest"}), "--serving \"lowest\" is not highest-elevation");
  // The edges that are accepted.
  EXPECT_TRUE(parse_with({"--fibre-factor", "1"}).options.has_value());
  EXPECT_TRUE(parse_with({"--lock-on-s", "0"}).options.has_value());
  EXPECT_TRUE(parse_replacing("--duration-s", "0").options.has_value());
}

}  // namespace
