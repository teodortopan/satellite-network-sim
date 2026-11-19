// The topology program on the committed snapshot and ground data, for a short window (5
// minutes at 15 s from the Phase 2 start) with every pattern and both endpoint models. Built
// only without sanitizers and not run under Valgrind, like the other snapshot checks (parsing
// the snapshot under ASan takes minutes). Prints each run's summary.

#include <gtest/gtest.h>

#include <cstddef>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "satsim/core/physical_constants.hpp"
#include "satsim/ground/sites.hpp"
#include "satsim/time/utc_time.hpp"
#include "satsim/topology/link_plan.hpp"
#include "test_support.hpp"
#include "topology/arguments.hpp"
#include "topology/endpoints.hpp"
#include "topology/run.hpp"

namespace {

namespace app = satsim::topology_app;
namespace topology = satsim::topology;
using satsim::propagate::testing::number;
using satsim::propagate::testing::parse_csv;

std::size_t col(std::string_view name) {
  for (std::size_t i = 0; i < app::kCsvColumns.size(); ++i) {
    if (app::kCsvColumns[i] == name) {
      return i;
    }
  }
  ADD_FAILURE() << "no column " << name;
  return 0;
}

void check_pattern(topology::LaserPattern pattern) {
  app::Options o{.snapshot_dir = "data/starlink/2026-11-18",
                 .ground_dir = "data/ground/2026-11-19",
                 .cities_file = "data/ground/cities.json",
                 .start = satsim::propagate::testing::utc("2026-11-18T21:15:00"),
                 .duration_ns = 300'000'000'000,
                 .step_ns = 15'000'000'000,
                 .shells = {},
                 .out = "-"};
  o.pattern = pattern;
  app::PreparedRunOrError prepared = app::prepare_run(o);
  if (const auto* error = std::get_if<std::string>(&prepared)) {
    FAIL() << *error;
  }
  app::PreparedRun run = std::move(std::get<app::PreparedRun>(prepared));
  EXPECT_TRUE(run.ground.assumed_file);
  EXPECT_GT(run.ground.assumed_operational, 0U);

  for (const auto model : {app::EndpointModel::kRealistic, app::EndpointModel::kBestCase}) {
    run.options.model = model;
    std::ostringstream csv;
    const app::RunOutcome outcome = app::write_rows(run, csv);
    ASSERT_FALSE(outcome.error.has_value()) << outcome.error.value_or("");
    std::ostringstream summary;
    app::write_summary(run, outcome.statistics, summary);
    std::cout << summary.str();
    EXPECT_EQ(outcome.statistics.checks.total(), 0U);
    EXPECT_EQ(outcome.statistics.lower_bound_failures, 0U);
    EXPECT_EQ(outcome.statistics.steps, 21);

    const auto rows = parse_csv(csv.str());
    ASSERT_EQ(rows.size(), 1 + 21 * run.directions.size());
    const std::string first_time = rows.at(1)[col("utc")];
    std::size_t ok = 0;
    for (std::size_t i = 1; i < rows.size(); ++i) {
      const auto& row = rows[i];
      if (model == app::EndpointModel::kBestCase && row[col("role")] == "study" &&
          row[col("utc")] == first_time) {
        EXPECT_EQ(row[col("status")], "ok") << row[col("from")] << " -> " << row[col("to")];
      }
      if (row[col("status")] != "ok") {
        continue;
      }
      ++ok;
      const double up = number(row[col("uplink_km")]);
      const double laser = number(row[col("laser_km")]);
      const double down = number(row[col("downlink_km")]);
      const double fibre = number(row[col("fibre_km")]);
      EXPECT_DOUBLE_EQ(number(row[col("path_km")]), up + laser + down + fibre);
      EXPECT_NEAR(
          number(row[col("one_way_ms")]),
          (up + laser + down + fibre * o.fibre_factor) / satsim::kSpeedOfLightVacuumKmPerS * 1000.0,
          1e-9);
      EXPECT_GE(number(row[col("stretch")]), 1.0);
      EXPECT_GE(number(row[col("one_way_ms")]), number(row[col("lower_bound_ms")]));
    }
    EXPECT_GT(ok, rows.size() / 2);
  }
}

TEST(TopologyRealDataCheck, Three) { check_pattern(topology::LaserPattern::kThree); }
TEST(TopologyRealDataCheck, PlusGrid) { check_pattern(topology::LaserPattern::kPlusGrid); }
TEST(TopologyRealDataCheck, Six) { check_pattern(topology::LaserPattern::kSix); }

}  // namespace
