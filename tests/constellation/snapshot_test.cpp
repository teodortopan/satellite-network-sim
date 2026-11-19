// Classifies the full committed Starlink snapshot with the default configuration. Built only
// without sanitizers and not run under Valgrind (see tests/orbit/CMakeLists.txt for why).
// Asserts coarse facts that should survive small changes of the proposed thresholds.

#include "satsim/orbit/snapshot.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include "satsim/constellation/classification.hpp"
#include "satsim/constellation/geometry.hpp"
#include "satsim/orbit/element_set.hpp"
#include "satsim/time/civil_time.hpp"
#include "satsim/time/utc_time.hpp"

namespace {

using satsim::constellation::Classification;
using satsim::constellation::ClassificationConfig;
using satsim::constellation::ClassifiedSatellite;
using satsim::constellation::ExclusionReason;
using satsim::constellation::Launch;
using satsim::constellation::launch_of;
using satsim::constellation::PlaneAssignment;

const std::filesystem::path kSnapshotDir = "data/starlink/2026-11-18";

TEST(ConstellationSnapshotTest, FourShellsWithRoughlyTheExpectedSize) {
  auto loaded = satsim::orbit::load_snapshot_elements(kSnapshotDir);
  ASSERT_TRUE(std::holds_alternative<satsim::orbit::SnapshotElements>(loaded));
  const auto& selected = std::get<satsim::orbit::SnapshotElements>(loaded).selection.selected;
  std::vector<satsim::orbit::ElementSet> sets;
  sets.reserve(selected.size());
  for (const auto& s : selected) {
    sets.push_back(s.elements);
  }
  const auto reference = satsim::time::UtcTime::from_civil({2026, 11, 18, 21, 15, 0.0});
  ASSERT_TRUE(reference.has_value());
  const ClassificationConfig config = satsim::constellation::default_config();
  auto result = satsim::constellation::classify_constellation(sets, *reference, config);
  ASSERT_TRUE(std::holds_alternative<Classification>(result));
  const auto& c = std::get<Classification>(result);
  ASSERT_EQ(c.satellites.size(), sets.size());
  EXPECT_GT(sets.size(), 11000U);

  // Satellites in planes per shell, and planes per shell: [min, max] from the study, with room.
  struct Expected {
    std::string name;
    std::size_t min_in_planes;
    std::size_t max_in_planes;
    std::size_t min_planes;
    std::size_t max_planes;
  };
  const std::vector<Expected> expected = {
      {"53deg", 2800, 3600, 100, 140},
      {"43deg", 2900, 3400, 85, 100},
      {"97deg", 1300, 1550, 55, 70},
      {"70deg", 500, 700, 20, 28},
  };
  ASSERT_EQ(config.shells.size(), expected.size());
  std::vector<std::size_t> in_planes(config.shells.size(), 0);
  std::map<ExclusionReason, std::size_t> reasons;
  std::size_t first_generation = 0;
  for (const ClassifiedSatellite& s : c.satellites) {
    // Exactly one of a plane or a reason (the variant), a geometry exactly when propagation
    // worked, and a shell whenever there is a plane.
    EXPECT_EQ(s.geometry.has_value(), !s.propagation_error.has_value()) << s.norad_cat_id;
    if (const auto* place = std::get_if<PlaneAssignment>(&s.placement)) {
      ASSERT_TRUE(s.shell_index.has_value()) << s.norad_cat_id;
      ++in_planes[*s.shell_index];
      const auto& plane = c.planes[*s.shell_index].at(place->plane_index);
      EXPECT_EQ(c.satellites[plane.members.at(place->index_in_plane)].norad_cat_id, s.norad_cat_id);
    } else {
      ++reasons[std::get<ExclusionReason>(s.placement)];
    }
    const std::optional<Launch> launch = launch_of(s.object_id);
    if (launch && *launch <= Launch{.year = 2021, .number = 44}) {
      ++first_generation;
    }
  }
  std::size_t total_in_planes = 0;
  for (std::size_t i = 0; i < expected.size(); ++i) {
    EXPECT_EQ(config.shells[i].name, expected[i].name);
    EXPECT_GE(in_planes[i], expected[i].min_in_planes) << expected[i].name;
    EXPECT_LE(in_planes[i], expected[i].max_in_planes) << expected[i].name;
    EXPECT_GE(c.planes[i].size(), expected[i].min_planes) << expected[i].name;
    EXPECT_LE(c.planes[i].size(), expected[i].max_planes) << expected[i].name;
    std::size_t members = 0;
    for (const auto& plane : c.planes[i]) {
      EXPECT_GE(plane.members.size(), config.shells[i].min_plane_size);
      members += plane.members.size();
    }
    EXPECT_EQ(members, in_planes[i]);
    total_in_planes += in_planes[i];
  }
  std::size_t excluded = 0;
  for (const auto& [reason, count] : reasons) {
    excluded += count;
  }
  EXPECT_EQ(total_in_planes + excluded, c.satellites.size());

  // Every first-generation satellite is excluded as such (553 in this snapshot), nothing fails
  // to propagate at the snapshot time, and the labelled groups are populated.
  EXPECT_EQ(first_generation, 553U);
  EXPECT_EQ(reasons[ExclusionReason::kNoLasers], first_generation);
  EXPECT_EQ(reasons[ExclusionReason::kUnknownLaunch], 0U);
  EXPECT_EQ(reasons[ExclusionReason::kPropagationFailed], 0U);
  EXPECT_GT(reasons[ExclusionReason::kLowGroup], 600U);
  EXPECT_GT(reasons[ExclusionReason::kOldHeight], 100U);
  EXPECT_GT(reasons[ExclusionReason::kOtherHeight], 300U);
}

}  // namespace
