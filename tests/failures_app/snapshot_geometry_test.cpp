#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <numbers>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include "common/steps.hpp"
#include "failures/geometry.hpp"
#include "inputs/inputs.hpp"
#include "inputs/step_source.hpp"
#include "satsim/topology/link_plan.hpp"
#include "satsim/topology/link_state.hpp"
#include "support/synthetic.hpp"
#include "test_support.hpp"
#include "topology_app/fixture.hpp"

// SnapshotGeometry against app::StepSource on the topology program's synthetic snapshot and
// ground fixture: the same ground links and the same geometric link states at every step,
// whatever order the steps are asked in.

namespace {

namespace fixture = satsim::topology_app::testing;
using satsim::propagate::testing::ScratchDir;

void expect_same_as_step_source(satsim::app::InputOptions options) {
  ScratchDir dir;
  fixture::write_snapshot(
      dir.path(), satsim::constellation::testing::walker_element_sets(fixture::fixture_shell()));
  options.snapshot_dir = dir.path();
  satsim::app::InputsOrError prepared = satsim::app::prepare_inputs(options);
  if (const auto* error = std::get_if<std::string>(&prepared)) {
    FAIL() << *error;
  }
  const satsim::app::Inputs& inputs = std::get<satsim::app::Inputs>(prepared);
  ASSERT_EQ(inputs.time_count, 4);
  ASSERT_FALSE(inputs.plan.links.empty());
  ASSERT_FALSE(inputs.gateways.empty());

  // What StepSource gives, step by step in order.
  struct Expected {
    std::vector<std::vector<satsim::topology::VisibleSatellite>> visible;
    std::vector<satsim::topology::LinkState> states;
  };
  std::vector<Expected> expected;
  satsim::app::StepSource source(inputs, options, 1, 1);
  for (std::int64_t k = 0; k < inputs.time_count; ++k) {
    ASSERT_EQ(source.advance(), std::nullopt);
    expected.push_back({.visible = source.visible(), .states = source.links().states});
  }
  std::size_t up = 0;
  std::size_t city_links = 0;
  for (const Expected& e : expected) {
    for (const auto& s : e.states) {
      up += s.up() ? 1U : 0U;
    }
    for (std::size_t c = 0; c < inputs.cities.size(); ++c) {
      city_links += e.visible[c].size();
    }
  }
  ASSERT_GT(up, 0U);
  ASSERT_GT(city_links, 0U);

  // A cache of two steps, asked in an order that evicts.
  satsim::failures_app::SnapshotGeometry geometry(inputs, options, 2);
  for (const std::int64_t k : {3, 0, 2, 0, 1, 3}) {
    const auto& e = expected[static_cast<std::size_t>(k)];
    for (std::size_t i = 0; i < inputs.plan.links.size(); ++i) {
      const auto& l = inputs.plan.links[i];
      EXPECT_EQ(geometry.link_state(k, l.satellite_a, l.satellite_b), e.states[i])
          << "step " << k << " link " << i;
    }
    const satsim::failures_app::StepGeometry& g = geometry.at(k);
    ASSERT_EQ(g.city_visible.size(), inputs.cities.size());
    ASSERT_EQ(g.gateway_visible.size(), inputs.gateways.size());
    for (std::size_t c = 0; c < inputs.cities.size(); ++c) {
      EXPECT_EQ(g.city_visible[c], e.visible[c]) << "step " << k << " city " << c;
    }
    for (std::size_t gw = 0; gw < inputs.gateways.size(); ++gw) {
      EXPECT_EQ(g.gateway_visible[gw], e.visible[inputs.cities.size() + gw])
          << "step " << k << " gateway " << gw;
    }
    EXPECT_EQ(g.position_ecef_km.size(), inputs.working.satellites.size());
    EXPECT_EQ(g.utc, satsim::app::time_of_step(options.start, options.step_ns, k));
  }
  EXPECT_EQ(geometry.error(), std::nullopt);
}

satsim::app::InputOptions options_of(satsim::topology::LaserPattern pattern) {
  return satsim::app::input_options_of(fixture::fixture_options(
      "unused", 360, 120, pattern, satsim::app::EndpointModel::kRealistic));
}

TEST(SnapshotGeometryTest, SameAsStepSource) {
  expect_same_as_step_source(options_of(satsim::topology::LaserPattern::kThree));
}

// With the Sun exclusion on (a wide angle, so that some links are excluded at some step) and a
// range cap.
TEST(SnapshotGeometryTest, SameAsStepSourceWithSunExclusionAndRangeCap) {
  satsim::app::InputOptions o = options_of(satsim::topology::LaserPattern::kPlusGrid);
  o.limits.sun_exclusion_rad = 60.0 * std::numbers::pi / 180.0;
  o.limits.range_cap_km = 3000.0;
  expect_same_as_step_source(o);
}

}  // namespace
