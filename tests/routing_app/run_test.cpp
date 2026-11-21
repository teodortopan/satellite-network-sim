// The routing program as a library, on the topology program's synthetic snapshot (an ideal
// Walker shell of 160 satellites) and its small ground fixture: agreement with the topology
// program (spec 6.2), the end-node delays, every strategy, planner and serving rule passing the
// checks, determinism, the drops CSV against the routes CSV, a failed physical check exiting 1,
// and the exit status of bad arguments and inputs.

#include "routing/run.hpp"

#include <gtest/gtest.h>

#include <Eigen/Core>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "agreement.hpp"
#include "common/steps.hpp"
#include "fixture.hpp"
#include "routing/arguments.hpp"
#include "routing/engine.hpp"
#include "routing/settings.hpp"
#include "satsim/ground/distance.hpp"
#include "satsim/routing/strategy.hpp"
#include "satsim/time/utc_time.hpp"
#include "satsim/topology/link_plan.hpp"
#include "satsim/topology/link_state.hpp"
#include "satsim/topology/positions.hpp"
#include "support/synthetic.hpp"
#include "test_support.hpp"
#include "topology/arguments.hpp"
#include "topology/run.hpp"

namespace {

namespace fs = std::filesystem;
namespace app = satsim::routing_app;
namespace topo = satsim::topology_app;
namespace fixture = satsim::topology_app::testing;
namespace topology = satsim::topology;
namespace ground = satsim::ground;
using satsim::propagate::testing::number;
using satsim::propagate::testing::parse_csv;
using satsim::propagate::testing::read_file;
using satsim::propagate::testing::ScratchDir;
using Rows = std::vector<std::vector<std::string>>;

constexpr std::int64_t kNs = 1'000'000'000;

using satsim::routing_app::testing::col;
using satsim::routing_app::testing::expect_equal_to_topology;
std::size_t rc(std::string_view name) { return col(app::kRoutesColumns, name); }

struct Snapshot {
  ScratchDir dir;
  Snapshot() {
    const auto sets = satsim::constellation::testing::walker_element_sets(fixture::fixture_shell());
    fixture::write_snapshot(dir.path(), sets);
  }
};

std::vector<satsim::routing::Strategy> strategies(std::initializer_list<std::string_view> names) {
  std::vector<satsim::routing::Strategy> result;
  for (const std::string_view name : names) {
    result.push_back(std::get<satsim::routing::Strategy>(satsim::routing::parse_strategy(name)));
  }
  return result;
}

// The topology fixture's options as the routing program's.
app::Options options(const fs::path& snapshot_dir, std::int64_t duration_s, std::int64_t step_s,
                     app::EndpointModel model) {
  const topo::Options t = fixture::fixture_options(snapshot_dir, duration_s, step_s,
                                                   topology::LaserPattern::kThree, model);
  app::Options o{.snapshot_dir = t.snapshot_dir,
                 .ground_dir = t.ground_dir,
                 .cities_file = t.cities_file,
                 .start = t.start,
                 .shells = {},
                 .strategies = {},
                 .out = {}};
  satsim::app::assign_input_options(o, satsim::app::input_options_of(t));
  o.model = model;
  o.strategies = strategies({"lowest-delay"});
  o.replan_ns = step_s * kNs;
  o.slot_ns = step_s * kNs;
  o.out = "-";
  return o;
}

app::PreparedRun prepare(const app::Options& o) {
  app::PreparedRunOrError prepared = app::prepare_run(o);
  if (const auto* error = std::get_if<std::string>(&prepared)) {
    ADD_FAILURE() << *error;
    std::abort();
  }
  return std::move(std::get<app::PreparedRun>(prepared));
}

struct Written {
  std::string csv;
  app::RunOutcome outcome;
};

Written write(const app::PreparedRun& run, const app::RunHooks& hooks = {}) {
  std::ostringstream csv;
  Written w{.csv = {}, .outcome = app::write_routes(run, csv, hooks)};
  w.csv = csv.str();
  EXPECT_FALSE(w.outcome.error.has_value()) << w.outcome.error.value_or("");
  return w;
}

// What the tests share, prepared at most once per process: GCC's sanitizers make each
// classification of the fixture shell take seconds, and ctest runs every TEST in a process of
// its own, so the program's pieces are checked in a few TESTs that share one prepared run. 60 s at
// 15 s (5 steps), realistic, lowest-delay, re-planned every step with a slot of one step. Tests
// copy the run and change only what writing reads (run.options' model, strategies, intervals,
// planner, serving rule, backup).
constexpr std::int64_t kDurationS = 60;
constexpr std::int64_t kStepS = 15;
constexpr std::size_t kSteps = 5;

const Snapshot& snapshot() {
  static const Snapshot s;
  return s;
}

const app::PreparedRun& routing_run() {
  static const app::PreparedRun run =
      prepare(options(snapshot().dir.path(), kDurationS, kStepS, app::EndpointModel::kRealistic));
  return run;
}

const topo::PreparedRun& topology_run() {
  static const topo::PreparedRun run = [] {
    topo::PreparedRunOrError t = topo::prepare_run(
        fixture::fixture_options(snapshot().dir.path(), kDurationS, kStepS,
                                 topology::LaserPattern::kThree, app::EndpointModel::kRealistic));
    if (!std::holds_alternative<topo::PreparedRun>(t)) {
      ADD_FAILURE() << "topology prepare failed";
      std::abort();
    }
    return std::move(std::get<topo::PreparedRun>(t));
  }();
  return run;
}

// Command-line arguments on the shared snapshot: 30 s at 30 s, realistic, lowest-delay.
std::vector<std::string> main_args(const fs::path& out) {
  return {"--snapshot",
          snapshot().dir.path().string(),
          "--ground",
          fixture::kGroundDir.string(),
          "--cities",
          fixture::kCities.string(),
          "--start",
          std::string(fixture::kStart),
          "--duration-s",
          "30",
          "--step-s",
          "30",
          "--pattern",
          "three",
          "--model",
          "realistic",
          "--strategies",
          "lowest-delay,keep-until-broken",
          "--replan-s",
          "30",
          "--serving-slot-s",
          "30",
          "--out",
          out.string()};
}

int run_main(const std::vector<std::string>& args, const app::RunHooks& hooks = {},
             std::string* err_text = nullptr) {
  const std::vector<std::string_view> views(args.begin(), args.end());
  std::ostringstream out;
  std::ostringstream err;
  const int status = app::run_main(views, out, err, hooks);
  if (err_text != nullptr) {
    *err_text = err.str() + out.str();
  }
  return status;
}

TEST(RoutingRunTest, LowestDelayEveryStepEqualsTheTopologyProgramInBothModels) {
  for (const auto model : {app::EndpointModel::kRealistic, app::EndpointModel::kBestCase}) {
    SCOPED_TRACE(std::string(satsim::app::name_of(model)));
    topo::PreparedRun t = topology_run();
    t.options.model = model;
    std::ostringstream t_csv;
    const topo::RunOutcome t_out = topo::write_rows(t, t_csv);
    ASSERT_FALSE(t_out.error.has_value());
    app::PreparedRun r = routing_run();
    r.options.model = model;
    const Written w = write(r);
    EXPECT_TRUE(w.outcome.statistics.passed());
    expect_equal_to_topology(parse_csv(t_csv.str()), parse_csv(w.csv), model);
  }
}

// The end node's edge from each gateway is the fibre from the gateway to the sender's home PoP
// plus the fibre from the PoP to the destination; a sender without a PoP has none.
void EndDelaysAreTheFibreThroughTheHomePop() {
  const app::PreparedRun& run = routing_run();
  const app::EngineNetwork net = app::network_of(run);
  const std::size_t gateways = run.gateways.size();
  ASSERT_EQ(net.end_delays_s.size(), run.directions.size() * gateways);
  std::size_t without = 0;
  for (std::size_t d = 0; d < run.directions.size(); ++d) {
    const auto& pop = run.home_pops[run.directions[d].from].pop;
    for (std::size_t g = 0; g < gateways; ++g) {
      const double got = net.end_delays_s[d * gateways + g];
      if (!pop) {
        EXPECT_TRUE(std::isnan(got));
        ++without;
        continue;
      }
      const double gateway_pop_km =
          ground::great_circle_distance_on_mean_sphere_km(run.gateways[g].site, pop->site);
      const double pop_destination_km = ground::great_circle_distance_on_mean_sphere_km(
          pop->site, run.cities[run.directions[d].to].site);
      EXPECT_EQ(got, ground::fibre_delay_s(gateway_pop_km, run.options.fibre_factor) +
                         ground::fibre_delay_s(pop_destination_km, run.options.fibre_factor));
    }
  }
  EXPECT_GT(without, 0U);  // Gamma has no service.
}

// The strategies, both planners, both models and the serving rules not covered by hand in the
// engine tests pass every check with the backup on.
void EverySettingPassesTheChecks() {
  app::PreparedRun run = routing_run();
  run.options.strategies = strategies({"lowest-delay", "longest-lived", "threshold:5%"});
  run.options.replan_ns = 30 * kNs;
  run.options.slot_ns = 15 * kNs;
  run.options.backup = true;
  struct Setting {
    app::EndpointModel model;
    app::Planner planner;
    app::ServingRule rule;
  };
  for (const Setting& s :
       {Setting{app::EndpointModel::kRealistic, app::Planner::kStart, app::ServingRule::kWithRoute},
        Setting{app::EndpointModel::kRealistic, app::Planner::kWholeInterval,
                app::ServingRule::kLongestInView},
        Setting{app::EndpointModel::kBestCase, app::Planner::kWholeInterval,
                app::ServingRule::kHighestElevation}}) {
    run.options.model = s.model;
    run.options.planner = s.planner;
    run.options.serving = s.rule;
    const Written w = write(run);
    const std::string what = std::string(satsim::app::name_of(s.model)) + " " +
                             std::string(app::name_of(s.planner)) + " " +
                             std::string(app::name_of(s.rule));
    EXPECT_TRUE(w.outcome.statistics.passed()) << what;
    EXPECT_EQ(w.outcome.statistics.rows_written, kSteps * 6U * 3U) << what;
    EXPECT_GT(w.outcome.statistics.measures[0].ok_steps, 0U) << what;
  }
}

// The drops CSV has one row per drop event, and the events' lengths add up to the routes CSV's
// dropped steps for each direction and strategy.
void DropEventsAddUpToTheDroppedSteps() {
  app::PreparedRun run = routing_run();
  run.options.strategies = strategies({"lowest-delay", "keep-until-broken"});
  run.options.replan_ns = 45 * kNs;
  run.options.slot_ns = 30 * kNs;
  const Written w = write(run);
  std::ostringstream drops;
  ASSERT_TRUE(app::write_drops(run, w.outcome.statistics, drops));
  const Rows events = parse_csv(drops.str());
  ASSERT_EQ(events.size(), w.outcome.statistics.drop_events.size() + 1);
  std::map<std::string, double> dropped_s;
  for (const auto& row : parse_csv(w.csv)) {
    if (row[rc("status")] == "drop") {
      dropped_s[row[rc("from")] + row[rc("to")] + row[rc("strategy")]] +=
          static_cast<double>(kStepS);
    }
  }
  std::map<std::string, double> event_s;
  const auto ec = [](std::string_view name) { return col(app::kDropsColumns, name); };
  for (std::size_t i = 1; i < events.size(); ++i) {
    event_s[events[i][ec("from")] + events[i][ec("to")] + events[i][ec("strategy")]] +=
        number(events[i][ec("length_s")]);
  }
  EXPECT_EQ(dropped_s, event_s);
  EXPECT_FALSE(event_s.empty());  // Gamma's no_service at least.
}

// A failed physical check (a satellite moved to the Earth's centre after its links were found
// up, through the test seam) fails the run.
void AFailedPhysicalCheckFailsTheRun() {
  app::PreparedRun run = routing_run();
  run.options.model = app::EndpointModel::kBestCase;
  const app::RunHooks hooks{.after_links =
                                [](topology::StepPositions& positions, topology::StepLinks&) {
                                  for (auto& p : positions.position_ecef_km) {
                                    if (p) {
                                      p = Eigen::Vector3d::Zero();
                                      break;
                                    }
                                  }
                                },
                            .before_checks = {}};
  const Written w = write(run, hooks);
  EXPECT_FALSE(w.outcome.statistics.passed());
  EXPECT_GT(w.outcome.statistics.physical.line_of_sight, 0U);
  EXPECT_EQ(w.outcome.statistics.routes.total(), 0U);
}

// The program: exit 0 writing the routes, drops and links files; 1 when a check fails (a route
// check broken through the engine's seam); 2 for bad arguments or inputs; 0 for --help.
TEST(RoutingRunTest, ExitStatusAndOutputFiles) {
  const fs::path dir = snapshot().dir.path();
  const fs::path routes = dir / "routes.csv";
  const fs::path drops = dir / "drops.csv";
  const fs::path links = dir / "links.csv";
  std::vector<std::string> good = main_args(routes);
  for (const std::string& extra :
       {std::string("--drops-out"), drops.string(), std::string("--links-out"), links.string()}) {
    good.push_back(extra);
  }
  std::string text;
  ASSERT_EQ(run_main(good, {}, &text), 0) << text;
  EXPECT_NE(text.find("checks: PASSED"), std::string::npos);
  EXPECT_EQ(parse_csv(read_file(routes)).size(), 1U + 2U * 6U * 2U);
  EXPECT_FALSE(parse_csv(read_file(drops)).empty());
  EXPECT_TRUE(fs::exists(links));

  const app::RunHooks broken{.after_links = {},
                             .before_checks = [](app::RouteRow& r) { r.cause.reset(); }};
  EXPECT_EQ(run_main(good, broken, &text), 1);
  EXPECT_NE(text.find("checks failed"), std::string::npos) << text;
  EXPECT_NE(text.find("checks: FAILED"), std::string::npos);

  EXPECT_EQ(run_main({"--help"}), 0);
  std::vector<std::string> no_out = good;
  no_out.resize(no_out.size() - 6);  // Without --out, --drops-out and --links-out.
  EXPECT_EQ(run_main(no_out), 2);
  const auto replacing = [&](std::string_view option, const std::string& value) {
    std::vector<std::string> args = good;
    for (std::size_t i = 0; i + 1 < args.size(); ++i) {
      if (args[i] == option) {
        args[i + 1] = value;
      }
    }
    return args;
  };
  EXPECT_EQ(run_main(replacing("--replan-s", "45")), 2);
  EXPECT_EQ(run_main(replacing("--serving-slot-s", "20")), 2);
  EXPECT_EQ(run_main(replacing("--snapshot", "/nonexistent/snapshot")), 2);
}

// --directions forward keeps only each pair's from-to; the summary names every direction.
void ForwardDirectionsOnlyAndTheSummary() {
  app::PreparedRun run = routing_run();
  run.options.model = app::EndpointModel::kBestCase;
  app::keep_forward_directions(run);
  ASSERT_EQ(run.directions.size(), 3U);
  EXPECT_FALSE(run.options.both_directions);
  EXPECT_EQ(run.cities[run.directions[0].from].site.name, "Alpha");
  EXPECT_EQ(run.cities[run.directions[0].to].site.name, "Beta");
  const Written w = write(run);
  EXPECT_EQ(write(run).csv, w.csv);  // The same inputs give the same bytes.
  std::ostringstream summary;
  app::write_summary(run, w.outcome.statistics, summary);
  const std::string text = summary.str();
  EXPECT_NE(text.find("Alpha -> Beta"), std::string::npos);
  EXPECT_EQ(text.find("Beta -> Alpha"), std::string::npos);
  EXPECT_NE(text.find("path changes per hour"), std::string::npos);
  EXPECT_NE(text.find("checks: PASSED"), std::string::npos);
}

// --plan-time (Phase 4 spec 4.10): a run started 30 s late whose plan time is the fixture's
// start classifies and plans its lasers on the network of the run that started then, so
// lowest-delay re-planned every step with a slot of one step gives that run's rows at the same
// times, in both models. Only the first plan's outcome may differ: the late run holds no route
// before it, so it is "replaced" where the earlier run may have "kept". The summary names the
// plan time only when one is given.
TEST(RoutingRunTest, APlanTimeGivesTheNetworkOfTheRunStartedThen) {
  const app::PreparedRun& early = routing_run();  // 60 s from the fixture's start.
  app::Options late_options = options(snapshot().dir.path(), kDurationS - 2 * kStepS, kStepS,
                                      app::EndpointModel::kRealistic);
  late_options.plan_time = late_options.start;
  late_options.start = satsim::app::time_of_step(late_options.start, kStepS * kNs, 2);
  const app::PreparedRun late = prepare(late_options);
  // The same laser plan (on this shell, one planned 30 s later differs in some links).
  EXPECT_TRUE(late.plan.links == early.plan.links) << "the laser plans differ";
  const std::size_t per_step = early.directions.size();
  for (const auto model : {app::EndpointModel::kRealistic, app::EndpointModel::kBestCase}) {
    SCOPED_TRACE(std::string(satsim::app::name_of(model)));
    app::PreparedRun a = early;
    app::PreparedRun b = late;
    a.options.model = model;
    b.options.model = model;
    const Written wa = write(a);
    const Written wb = write(b);
    EXPECT_TRUE(wb.outcome.statistics.passed());
    const Rows ra = parse_csv(wa.csv);
    const Rows rb = parse_csv(wb.csv);
    ASSERT_EQ(ra.size(), 1 + kSteps * per_step);
    ASSERT_EQ(rb.size(), 1 + (kSteps - 2) * per_step);
    for (std::size_t i = 1; i < rb.size(); ++i) {
      const std::vector<std::string>& got = rb[i];
      const std::vector<std::string>& want = ra[i + 2 * per_step];
      ASSERT_EQ(got.size(), want.size());
      for (std::size_t c = 0; c < got.size(); ++c) {
        if (c == rc("plan") && i <= per_step) {
          continue;
        }
        EXPECT_EQ(got[c], want[c]) << "row " << i << ", column " << app::kRoutesColumns[c];
      }
    }
    std::ostringstream with;
    app::write_summary(b, wb.outcome.statistics, with);
    EXPECT_NE(with.str().find(", plan time 2026-11-18T12:00:00 UTC"), std::string::npos);
    std::ostringstream without;
    app::write_summary(a, wa.outcome.statistics, without);
    EXPECT_EQ(without.str().find("plan time"), std::string::npos);
  }
}

// A plan time outside the Earth-orientation table (2026-10-01 to 2026-12-31) is refused,
// naming it, before anything slow is loaded.
TEST(RoutingRunTest, APlanTimeWithoutEarthOrientationIsRefused) {
  app::Options o =
      options(snapshot().dir.path(), kDurationS, kStepS, app::EndpointModel::kRealistic);
  o.plan_time = satsim::time::UtcTime::from_civil({2027, 6, 1, 0, 0, 0.0});
  ASSERT_TRUE(o.plan_time.has_value());
  const app::PreparedRunOrError prepared = app::prepare_run(o);
  const auto* error = std::get_if<std::string>(&prepared);
  ASSERT_NE(error, nullptr);
  EXPECT_NE(error->find("no Earth-orientation values for the plan time 2027-06-01T00:00:00"),
            std::string::npos)
      << *error;
}

// The program's pieces on one prepared run (one process).
TEST(RoutingRunTest, SettingsDropsEndDelaysForwardDirectionsAndAFailedCheck) {
  EndDelaysAreTheFibreThroughTheHomePop();
  EverySettingPassesTheChecks();
  DropEventsAddUpToTheDroppedSteps();
  AFailedPhysicalCheckFailsTheRun();
  ForwardDirectionsOnlyAndTheSummary();
}

}  // namespace
