#include "fixture.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <optional>
#include <sstream>
#include <string>
#include <variant>
#include <vector>

#include "satsim/failure/schedule.hpp"
#include "satsim/time/civil_time.hpp"
#include "satsim/time/utc_time.hpp"
#include "satsim/topology/link_file.hpp"
#include "satsim/topology/link_plan.hpp"
#include "support/synthetic.hpp"
#include "topology/ideal.hpp"

namespace satsim::failure::testing {

namespace {

constexpr time::CivilTime kEpoch{2026, 11, 18, 21, 15, 0.0};

}  // namespace

const topology::testing::Ideal& small() {
  static const topology::testing::Ideal kIdeal = [] {
    using topology::testing::WalkerShell;
    const std::array<WalkerShell, 2> shells = {
        WalkerShell{.planes = 4, .per_plane = 4, .height_km = 550.0, .epoch = kEpoch},
        WalkerShell{.planes = 2,
                    .per_plane = 3,
                    .inclination_deg = 70.0,
                    .height_km = 400.0,
                    .first_id = 200000,
                    .epoch = kEpoch}};
    topology::testing::IdealOptions options;
    options.exclude = [](std::size_t shell, std::size_t p, std::size_t s) {
      return shell == 0 && p == 0 && s == 3;
    };
    return topology::testing::ideal_shells(shells, options);
  }();
  return kIdeal;
}

const topology::LinkPlan& small_plan() {
  static const topology::LinkPlan kPlan = [] {
    std::istringstream in(
        "norad_cat_id_a,norad_cat_id_b\n100000,100001\n100001,100002\n100000,100004\n"
        "100004,100005\n100001,100005\n200000,200001\n");
    const topology::LinkPlanOrError plan = topology::read_link_plan(in, small().classification, {});
    if (const auto* error = std::get_if<std::string>(&plan)) {
      ADD_FAILURE() << *error;
      return topology::LinkPlan{};
    }
    return std::get<topology::LinkPlan>(plan);
  }();
  return kPlan;
}

const std::vector<std::string>& gateway_names() {
  static const std::vector<std::string> kNames = {"Hawthorne", "New York, NY", "The \"Q\" site",
                                                  "Twin", "Twin"};
  return kNames;
}

time::UtcTime run_start() { return constellation::testing::utc(kEpoch); }

Failure failure_at(ElementKind kind, std::size_t element, double start_s,
                   std::optional<double> length_s) {
  return Failure{.kind = kind,
                 .element = element,
                 .start_utc = run_start().plus_seconds(start_s),
                 .start_s = start_s,
                 .length_s = length_s};
}

}  // namespace satsim::failure::testing
