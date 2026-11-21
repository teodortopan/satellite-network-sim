#pragma once

// Spec 6.2 for the routing tests: the routing program's rows against the topology program's.

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#include "inputs/inputs.hpp"
#include "routing/run.hpp"
#include "test_support.hpp"
#include "topology/run.hpp"

namespace satsim::routing_app::testing {

using Rows = std::vector<std::vector<std::string>>;

/// Index of a column; a failure if there is none.
template <std::size_t N>
std::size_t col(const std::array<std::string_view, N>& columns, std::string_view name) {
  const auto* const it = std::find(columns.begin(), columns.end(), name);
  EXPECT_NE(it, columns.end()) << name;
  return static_cast<std::size_t>(it - columns.begin());
}

// Spec 6.2: lowest-delay, re-planned every step with a serving slot of one step, equals the
// topology program at every step: status, delay to 1e-9 ms, laser hops, first and last
// satellite, serving satellite and gateway. `t` and `r` are the two CSVs, header first; both
// are ordered by time then direction.
inline void expect_equal_to_topology(const Rows& t, const Rows& r,
                                     satsim::app::EndpointModel model) {
  ASSERT_EQ(t.size(), r.size());
  ASSERT_GT(t.size(), 1U);
  std::size_t ok = 0;
  for (std::size_t i = 1; i < t.size(); ++i) {
    const auto a = [&](std::string_view name) {
      return t[i][col(topology_app::kCsvColumns, name)];
    };
    const auto b = [&](std::string_view name) { return r[i][col(kRoutesColumns, name)]; };
    ASSERT_EQ(a("utc"), b("utc")) << i;
    ASSERT_EQ(a("from"), b("from")) << i;
    ASSERT_EQ(a("to"), b("to")) << i;
    const bool topology_ok = a("status") == "ok";
    EXPECT_EQ(topology_ok, b("status") == "ok") << i << " " << a("utc") << " " << a("from");
    if (!topology_ok) {
      EXPECT_EQ(a("status"), b("drop_cause")) << i;
      continue;
    }
    ++ok;
    EXPECT_NEAR(propagate::testing::number(a("one_way_ms")),
                propagate::testing::number(b("one_way_ms")), 1e-9)
        << i;
    EXPECT_EQ(a("laser_hops"), b("laser_hops")) << i;
    if (model == satsim::app::EndpointModel::kBestCase) {
      EXPECT_EQ(a("first_norad_cat_id"), b("first_norad_cat_id")) << i;
      EXPECT_EQ(a("last_norad_cat_id"), b("last_norad_cat_id")) << i;
    } else {
      EXPECT_EQ(a("serving_norad_cat_id"), b("serving_norad_cat_id")) << i;
      EXPECT_EQ(a("serving_norad_cat_id"), b("first_norad_cat_id")) << i;
      EXPECT_EQ(a("gateway_norad_cat_id"), b("last_norad_cat_id")) << i;
      EXPECT_EQ(a("gateway"), b("gateway")) << i;
      EXPECT_EQ(a("home_pop"), b("home_pop")) << i;
    }
  }
  EXPECT_GT(ok, 0U);
}

}  // namespace satsim::routing_app::testing
