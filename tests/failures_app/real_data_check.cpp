// Spec 6.2 on the committed snapshot and ground data: with no failure file, the baseline,
// reference, planner and local-repair rows of the failures program equal the routing program's
// (status, drop cause, route, plan, delays to 1e-9 ms), and the flooding rows its lowest-delay
// rows, 120 s at 1 s from 2026-11-18T21:15:00, the study pairs, lowest-delay and
// keep-until-broken, both endpoint models and both planners. Then one run with a failure: the
// first satellite of New York to London's route at 20 s. Built only without sanitizers and not
// run under Valgrind, like the other snapshot checks.

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "failures/arguments.hpp"
#include "failures/run.hpp"
#include "routing/run.hpp"
#include "test_support.hpp"

namespace {

namespace fa = satsim::failures_app;
namespace ra = satsim::routing_app;
using satsim::propagate::testing::number;
using satsim::propagate::testing::parse_csv;
using satsim::propagate::testing::ScratchDir;
using Rows = std::vector<std::vector<std::string>>;

std::size_t col(std::string_view name) {
  for (std::size_t i = 0; i < ra::kRoutesColumns.size(); ++i) {
    if (ra::kRoutesColumns[i] == name) {
      return i;
    }
  }
  ADD_FAILURE() << name;
  return 0;
}

std::vector<std::string> args(std::string_view model, std::string_view planner) {
  return {"--snapshot",   "data/starlink/2026-11-18",
          "--ground",     "data/ground/2026-11-19",
          "--cities",     "data/ground/cities.json",
          "--start",      "2026-11-18T21:15:00",
          "--duration-s", "120",
          "--step-s",     "1",
          "--pattern",    "three",
          "--model",      std::string(model),
          "--strategies", "lowest-delay,keep-until-broken",
          "--replan-s",   "15",
          "--planner",    std::string(planner),
          "--roles",      "study",
          "--out",        "-",
          "--reactions",  "planner,local-repair,flooding"};
}

fa::FailureRun prepare(const std::vector<std::string>& a) {
  const std::vector<std::string_view> views(a.begin(), a.end());
  const fa::ParsedArguments parsed = fa::parse_arguments(views);
  if (!parsed.options) {
    ADD_FAILURE() << parsed.error.value_or("");
    std::abort();
  }
  fa::FailureRunOrError r = fa::prepare_failure_run(*parsed.options);
  if (const auto* error = std::get_if<std::string>(&r)) {
    ADD_FAILURE() << *error;
    std::abort();
  }
  return std::move(std::get<fa::FailureRun>(r));
}

struct Outcome {
  Rows rows;
  fa::FailureStatistics stats;
};

Outcome run(const fa::FailureRun& r) {
  std::ostringstream csv;
  fa::FailureOutcome outcome = fa::write_failure_routes(r, csv, nullptr);
  EXPECT_FALSE(outcome.error.has_value()) << outcome.error.value_or("");
  std::ostringstream summary;
  fa::write_failure_summary(r, outcome.statistics, summary);
  std::cout << summary.str();
  return {parse_csv(csv.str()), std::move(outcome.statistics)};
}

void check(std::string_view model, std::string_view planner) {
  SCOPED_TRACE(std::string(model) + " " + std::string(planner));
  const fa::FailureRun r = prepare(args(model, planner));
  const Outcome f = run(r);
  EXPECT_TRUE(f.stats.passed());
  ra::PreparedRunOrError rp = ra::prepare_run(r.options.routing);
  ASSERT_TRUE(std::holds_alternative<ra::PreparedRun>(rp));
  std::ostringstream routing_csv;
  ASSERT_FALSE(ra::write_routes(std::get<ra::PreparedRun>(rp), routing_csv).error.has_value());
  const Rows g = parse_csv(routing_csv.str());
  // Four lanes per strategy, and flooding under lowest-delay.
  ASSERT_EQ(f.rows.size() - 1, 4 * (g.size() - 1) + (g.size() - 1) / 2);
  // utc, from, to, strategy -> the routing program's row.
  std::map<std::vector<std::string>, std::vector<std::string>> routing_rows;
  for (std::size_t i = 1; i < g.size(); ++i) {
    routing_rows[{g[i][col("utc")], g[i][col("from")], g[i][col("to")], g[i][col("strategy")]}] =
        g[i];
  }
  std::size_t ok = 0;
  std::size_t flooding = 0;
  for (std::size_t i = 1; i < f.rows.size(); ++i) {
    const auto& a = f.rows[i];
    const auto found =
        routing_rows.find({a[col("utc")], a[col("from")], a[col("to")], a[col("strategy")]});
    ASSERT_NE(found, routing_rows.end()) << i;
    const auto& b = found->second;
    if (a[ra::kRoutesColumns.size()] == "flooding") {
      ++flooding;
      ASSERT_EQ(a[col("strategy")], "lowest-delay") << i;
    }
    for (const std::string_view name :
         {"utc", "from", "to", "strategy", "status", "drop_cause", "route", "plan", "laser_hops",
          "serving_norad_cat_id", "gateway"}) {
      ASSERT_EQ(a[col(name)], b[col(name)])
          << i << " " << name << " " << a[ra::kRoutesColumns.size()];
    }
    for (const std::string_view name : {"one_way_ms", "ideal_ms", "backup_ms"}) {
      if (b[col(name)].empty()) {
        EXPECT_EQ(a[col(name)], "") << i << " " << name;
      } else {
        EXPECT_NEAR(number(a[col(name)]), number(b[col(name)]), 1e-9) << i << " " << name;
      }
    }
    ok += b[col("status")] == "ok" ? 1U : 0U;
  }
  EXPECT_GT(ok, 0U);
  EXPECT_EQ(flooding, (g.size() - 1) / 2);
}

TEST(FailuresRealDataCheck, NoFailureEqualsRoutingBestCaseStart) { check("best-case", "start"); }
TEST(FailuresRealDataCheck, NoFailureEqualsRoutingBestCaseWholeInterval) {
  check("best-case", "whole-interval");
}
TEST(FailuresRealDataCheck, NoFailureEqualsRoutingRealisticStart) { check("realistic", "start"); }
TEST(FailuresRealDataCheck, NoFailureEqualsRoutingRealisticWholeInterval) {
  check("realistic", "whole-interval");
}

// The first satellite of New York to London's lowest-delay route at 20 s (from the baseline
// rows) fails then: the checks pass, and the planner and flooding lanes are a black hole and
// then delivered; the flooding lane settles on the reference's route.
TEST(FailuresRealDataCheck, OneFailure) {
  std::vector<std::string> a = args("best-case", "whole-interval");
  const Outcome clean = run(prepare(a));
  std::string first;
  for (std::size_t i = 1; i < clean.rows.size(); ++i) {
    const auto& row = clean.rows[i];
    if (row[col("utc")] == "2026-11-18T21:15:20" && row[col("from")] == "New York" &&
        row[col("to")] == "London" && row[col("strategy")] == "lowest-delay" &&
        row[ra::kRoutesColumns.size()] == "baseline") {
      first = row[col("first_norad_cat_id")];
    }
  }
  ASSERT_FALSE(first.empty());
  ScratchDir dir;
  const std::filesystem::path file = dir.path() / "failures.csv";
  {
    std::ofstream out(file);
    out << "kind,norad_cat_id_a,norad_cat_id_b,gateway,start_utc,length_s\n"
        << "satellite," << first << ",,,2026-11-18T21:15:20,\n";
  }
  a.insert(a.end(), {"--failures", file.string()});
  {
    const std::vector<std::string_view> views(a.begin(), a.end());
    std::ostringstream out;
    std::ostringstream err;
    EXPECT_EQ(fa::run_main(views, out, err), 0) << err.str();
  }
  const fa::FailureRun r = prepare(a);
  const Outcome failed = run(r);
  EXPECT_TRUE(failed.stats.passed());
  for (const fa::LaneKind kind : {fa::LaneKind::kPlanner, fa::LaneKind::kFlooding}) {
    SCOPED_TRACE(std::string(fa::name_of(kind)));
    bool black_hole = false;
    bool back = false;
    std::optional<std::size_t> direction;
    for (const fa::Interval& iv : failed.stats.intervals) {
      const auto& pd = r.run.directions.at(iv.direction);
      if (r.run.cities[pd.from].site.name != "New York" ||
          r.run.cities[pd.to].site.name != "London" || iv.strategy != 0 ||
          failed.stats.kinds.at(iv.engine) != kind) {
        continue;
      }
      direction = iv.direction;
      if (iv.status.state == fa::DirectionState::kBlackHole && iv.start_s == 20.0) {
        black_hole = true;
      } else if (black_hole && iv.status.state == fa::DirectionState::kDelivered) {
        back = true;
      }
    }
    EXPECT_TRUE(black_hole);
    EXPECT_TRUE(back);
    ASSERT_TRUE(direction.has_value());
    const std::size_t d_count = r.run.directions.size();
    const std::size_t s_count = r.run.options.strategies.size();
    for (std::size_t e = 0; e < failed.stats.kinds.size(); ++e) {
      if (failed.stats.kinds[e] == kind) {
        const fa::LaneMeasures& m =
            failed.stats.measures.at((e * d_count + direction.value_or(0)) * s_count);
        EXPECT_TRUE(m.settle_s.has_value());
        EXPECT_TRUE(m.restore_s.has_value());
      }
    }
  }
  EXPECT_EQ(failed.stats.failures.settled_differs, 0U);
  EXPECT_EQ(failed.stats.failures.settled_tables_differ, 0U);
}

}  // namespace
