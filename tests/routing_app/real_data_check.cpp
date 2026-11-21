// Spec 6.2 on the committed snapshot and ground data: the routing program's lowest-delay,
// re-planned every step with a serving slot of one step, equals the topology program at every
// step of 3 minutes at 1 s from the Phase 2 start, for the six study pairs in both directions
// and both endpoint models. Built only without sanitizers and not run under Valgrind, like the
// other snapshot checks. Prints each routing run's summary.

#include <gtest/gtest.h>

#include <iostream>
#include <sstream>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "agreement.hpp"
#include "inputs/inputs.hpp"
#include "routing/arguments.hpp"
#include "routing/run.hpp"
#include "satsim/routing/strategy.hpp"
#include "test_support.hpp"
#include "topology/arguments.hpp"
#include "topology/run.hpp"

namespace {

namespace routing = satsim::routing_app;
namespace topo = satsim::topology_app;
using satsim::propagate::testing::parse_csv;

constexpr std::int64_t kDurationNs = 180'000'000'000;
constexpr std::int64_t kStepNs = 1'000'000'000;

topo::Options topology_options(satsim::app::EndpointModel model) {
  topo::Options o{.snapshot_dir = "data/starlink/2026-11-18",
                  .ground_dir = "data/ground/2026-11-19",
                  .cities_file = "data/ground/cities.json",
                  .start = satsim::propagate::testing::utc("2026-11-18T21:15:00"),
                  .duration_ns = kDurationNs,
                  .step_ns = kStepNs,
                  .shells = {},
                  .out = "-"};
  o.roles = satsim::app::RoleFilter::kStudy;
  o.model = model;
  return o;
}

void check_model(satsim::app::EndpointModel model) {
  const topo::Options t = topology_options(model);
  topo::PreparedRunOrError t_prepared = topo::prepare_run(t);
  if (const auto* error = std::get_if<std::string>(&t_prepared)) {
    FAIL() << *error;
  }
  std::ostringstream t_csv;
  const topo::RunOutcome t_out = topo::write_rows(std::get<topo::PreparedRun>(t_prepared), t_csv);
  ASSERT_FALSE(t_out.error.has_value());

  routing::Options r{.snapshot_dir = t.snapshot_dir,
                     .ground_dir = t.ground_dir,
                     .cities_file = t.cities_file,
                     .start = t.start,
                     .shells = {},
                     .strategies = {},
                     .out = {}};
  satsim::app::assign_input_options(r, satsim::app::input_options_of(t));
  r.model = model;
  r.strategies = {satsim::routing::Strategy{}};  // lowest-delay
  r.replan_ns = kStepNs;
  r.slot_ns = kStepNs;
  r.out = "-";
  routing::PreparedRunOrError r_prepared = routing::prepare_run(r);
  if (const auto* error = std::get_if<std::string>(&r_prepared)) {
    FAIL() << *error;
  }
  const auto& run = std::get<routing::PreparedRun>(r_prepared);
  ASSERT_EQ(run.directions.size(), 12U);
  std::ostringstream r_csv;
  const routing::RunOutcome r_out = routing::write_routes(run, r_csv);
  ASSERT_FALSE(r_out.error.has_value()) << r_out.error.value_or("");
  std::ostringstream summary;
  routing::write_summary(run, r_out.statistics, summary);
  std::cout << summary.str();
  EXPECT_TRUE(r_out.statistics.passed());
  satsim::routing_app::testing::expect_equal_to_topology(parse_csv(t_csv.str()),
                                                         parse_csv(r_csv.str()), model);
}

TEST(RoutingRealDataCheck, RealisticEqualsTopology) {
  check_model(satsim::app::EndpointModel::kRealistic);
}
TEST(RoutingRealDataCheck, BestCaseEqualsTopology) {
  check_model(satsim::app::EndpointModel::kBestCase);
}

}  // namespace
