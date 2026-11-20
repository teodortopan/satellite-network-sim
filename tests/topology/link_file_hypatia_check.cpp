// The link-file round trip (Phase 3 spec 4.9) on Hypatia's full first shell, every pattern.
// Built only without sanitizers and not run under Valgrind, like the snapshot checks: planning
// 1,584 satellites three times takes about 20 minutes under GCC's ASan. link_file_test.cpp
// runs the same round trip on a compact shell inside the sanitizer builds.

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <sstream>
#include <string>
#include <utility>
#include <variant>

#include "ideal.hpp"
#include "satsim/time/civil_time.hpp"
#include "satsim/topology/link_file.hpp"
#include "satsim/topology/link_plan.hpp"
#include "satsim/topology/link_state.hpp"
#include "satsim/topology/physical_checks.hpp"
#include "satsim/topology/positions.hpp"
#include "support/synthetic.hpp"

namespace {

using satsim::topology::LaserPattern;
using satsim::topology::LinkPlan;
using satsim::topology::testing::WalkerShell;

constexpr satsim::time::CivilTime kEpoch{2026, 11, 18, 21, 15, 0.0};

// Hypatia's first Starlink shell (Phase 2 spec 6.2): 72 planes of 22 at 550 km and 53 deg.
// Link counts by hand: every satellite has every laser linked on it (link_plan_test), so 1584
// satellites give 1584 x 3 / 2 = 2376 links for three, 1584 x 4 / 2 = 3168 for plus-grid and
// 1584 x 6 / 2 = 4752 for six; the file has two comment lines and the header before them.
TEST(LinkFileHypatiaCheck, RoundTripGivesTheSamePlanForEveryPattern) {
  const std::array<WalkerShell, 1> shells = {WalkerShell{.planes = 72,
                                                         .per_plane = 22,
                                                         .inclination_deg = 53.0,
                                                         .height_km = 550.0,
                                                         .phasing = 17,
                                                         .epoch = kEpoch}};
  const auto ideal = satsim::topology::testing::ideal_shells(shells);
  const auto positions = satsim::topology::testing::step_positions(
      satsim::constellation::testing::utc(kEpoch), ideal.positions_ecef_km);
  const std::array<std::pair<LaserPattern, std::size_t>, 3> cases = {
      std::pair{LaserPattern::kThree, 2376U}, std::pair{LaserPattern::kPlusGrid, 3168U},
      std::pair{LaserPattern::kSix, 4752U}};
  for (const auto& [pattern, links] : cases) {
    const LinkPlan planned = satsim::topology::plan_laser_links(
        ideal.classification, ideal.positions_ecef_km, {.pattern = pattern});
    ASSERT_EQ(planned.links.size(), links);
    std::ostringstream out;
    ASSERT_TRUE(satsim::topology::write_link_plan(planned, ideal.classification, out));
    const std::string text = out.str();
    EXPECT_EQ(static_cast<std::size_t>(std::count(text.begin(), text.end(), '\n')), links + 3);

    std::istringstream in(text);
    const satsim::topology::LinkPlanOrError read =
        satsim::topology::read_link_plan(in, ideal.classification, {.pattern = pattern});
    ASSERT_TRUE(std::holds_alternative<LinkPlan>(read));
    const auto& read_back = std::get<LinkPlan>(read);
    EXPECT_EQ(read_back, planned) << satsim::topology::name_of(pattern);

    satsim::topology::LinkStateEvaluator a(planned, {});
    satsim::topology::LinkStateEvaluator b(read_back, {});
    const auto states = b.step(positions);
    EXPECT_EQ(a.step(positions).states, states.states);
    EXPECT_EQ(satsim::topology::run_physical_checks(ideal.classification, read_back, states,
                                                    positions, {}, {}, {})
                  .total(),
              0U);
  }
}

}  // namespace
