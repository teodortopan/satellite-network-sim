#include "satsim/constellation/classification.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <random>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "satsim/constellation/geometry.hpp"
#include "satsim/core/angles.hpp"
#include "satsim/orbit/element_set.hpp"
#include "satsim/orbit/propagator.hpp"
#include "satsim/time/civil_time.hpp"
#include "satsim/time/utc_time.hpp"
#include "synthetic.hpp"

namespace {

using satsim::deg_to_rad;
using satsim::rad_to_deg;
using satsim::constellation::check_config;
using satsim::constellation::Classification;
using satsim::constellation::ClassificationConfig;
using satsim::constellation::ClassificationOrError;
using satsim::constellation::ClassifiedSatellite;
using satsim::constellation::classify_constellation;
using satsim::constellation::default_config;
using satsim::constellation::ExcludedGroup;
using satsim::constellation::ExclusionReason;
using satsim::constellation::Launch;
using satsim::constellation::name_of;
using satsim::constellation::PlaneAssignment;
using satsim::constellation::ShellDefinition;
using satsim::constellation::wrap_pi;
using satsim::constellation::testing::at_epoch;
using satsim::constellation::testing::utc;
using satsim::constellation::testing::walker_element_sets;
using satsim::constellation::testing::walker_id;
using satsim::constellation::testing::WalkerShell;
using satsim::orbit::ElementSet;
using satsim::orbit::PropagationError;
using satsim::time::CivilTime;
using satsim::time::UtcTime;

const CivilTime kEpoch{2026, 11, 18, 12, 0, 0.0};

// Shell "A": 53 degrees around 550 km. The measured mean geodetic height of a Walker satellite
// at "550 km" is a few km lower (WalkerShell::height_km), well inside the band.
ShellDefinition shell_a(double gap_deg = 2.0, std::size_t min_plane_size = 3) {
  return {.name = "A",
          .inclination_deg = 53.0,
          .inclination_tolerance_deg = 0.5,
          .min_height_km = 500.0,
          .max_height_km = 580.0,
          .plane_gap_threshold_deg = gap_deg,
          .min_plane_size = min_plane_size};
}

ClassificationConfig config_with(std::vector<ShellDefinition> shells) {
  ClassificationConfig config;
  config.shells = std::move(shells);
  config.last_launch_without_lasers = Launch{.year = 2021, .number = 44};
  return config;
}

Classification classify(const std::vector<ElementSet>& sets, const UtcTime& reference,
                        const ClassificationConfig& config) {
  ClassificationOrError result = classify_constellation(sets, reference, config);
  if (const auto* error = std::get_if<std::string>(&result)) {
    ADD_FAILURE() << *error;
    return {};
  }
  return std::get<Classification>(std::move(result));
}

const ClassifiedSatellite* find(const Classification& c, std::int64_t id) {
  const auto it = std::find_if(c.satellites.begin(), c.satellites.end(),
                               [&](const ClassifiedSatellite& s) { return s.norad_cat_id == id; });
  return it == c.satellites.end() ? nullptr : &*it;
}

std::optional<ExclusionReason> reason(const Classification& c, std::int64_t id) {
  const ClassifiedSatellite* s = find(c, id);
  if (s == nullptr) {
    ADD_FAILURE() << "no satellite " << id;
    return std::nullopt;
  }
  if (const auto* r = std::get_if<ExclusionReason>(&s->placement)) {
    return *r;
  }
  return std::nullopt;
}

// Checks that `c` holds exactly the Walker shell's planes in shell `shell_index`, with the
// right members, plane order (ascending RAAN: plane p+1 follows plane p, cyclically) and order
// in each plane (satellite s+1 follows s, cyclically), so ahead/behind and neighbouring planes
// come out right.
void expect_walker_planes(const Classification& c, std::size_t shell_index,
                          const WalkerShell& shell) {
  ASSERT_LT(shell_index, c.planes.size());
  const auto& planes = c.planes[shell_index];
  ASSERT_EQ(planes.size(), shell.planes);
  std::map<std::int64_t, std::pair<std::size_t, std::size_t>> walker_position;
  for (std::size_t p = 0; p < shell.planes; ++p) {
    for (std::size_t s = 0; s < shell.per_plane; ++s) {
      walker_position[walker_id(shell, p, s)] = {p, s};
    }
  }
  std::size_t first_walker_plane = 0;  // The Walker plane that comes out as plane 0.
  for (std::size_t plane_index = 0; plane_index < planes.size(); ++plane_index) {
    const auto& members = planes[plane_index].members;
    ASSERT_EQ(members.size(), shell.per_plane) << "plane " << plane_index;
    const auto [p0, s0] = walker_position.at(c.satellites[members[0]].norad_cat_id);
    if (plane_index == 0) {
      first_walker_plane = p0;
    }
    EXPECT_EQ(p0, (first_walker_plane + plane_index) % shell.planes) << "plane " << plane_index;
    for (std::size_t k = 0; k < members.size(); ++k) {
      const ClassifiedSatellite& sat = c.satellites[members[k]];
      const auto [p, s] = walker_position.at(sat.norad_cat_id);
      EXPECT_EQ(p, p0) << sat.norad_cat_id;
      EXPECT_EQ(s, (s0 + k) % shell.per_plane) << sat.norad_cat_id;
      EXPECT_EQ(sat.shell_index, shell_index);
      const auto* place = std::get_if<PlaneAssignment>(&sat.placement);
      ASSERT_NE(place, nullptr) << sat.norad_cat_id;
      EXPECT_EQ(place->plane_index, plane_index);
      EXPECT_EQ(place->index_in_plane, k);
    }
    if (plane_index > 0) {
      EXPECT_GT(planes[plane_index].mean_raan_teme_rad, planes[plane_index - 1].mean_raan_teme_rad);
    }
  }
}

TEST(ClassificationTest, Walker72PlanesOf22) {
  const WalkerShell shell{.planes = 72,
                          .per_plane = 22,
                          .inclination_deg = 53.0,
                          .height_km = 550.0,
                          .phasing = 17,
                          .epoch = kEpoch};
  const auto sets = walker_element_sets(shell);
  const Classification c =
      classify(sets, utc(kEpoch).plus_seconds(3 * 3600.0), config_with({shell_a(2.0, 10)}));
  ASSERT_EQ(c.satellites.size(), 72U * 22U);
  expect_walker_planes(c, 0, shell);
}

TEST(ClassificationTest, Walker5PlanesOf5) {
  const WalkerShell shell{.planes = 5,
                          .per_plane = 5,
                          .inclination_deg = 53.0,
                          .height_km = 550.0,
                          .phasing = 2,
                          .first_raan_deg = 40.0,
                          .epoch = kEpoch};
  const Classification c =
      classify(walker_element_sets(shell), utc(kEpoch), config_with({shell_a()}));
  expect_walker_planes(c, 0, shell);
}

// Satellites of one plane on both sides of RAAN 0 (spread +-0.2 degrees around 0.1) form one
// plane, the first one.
TEST(ClassificationTest, PlaneStraddlingRaanZeroIsOnePlane) {
  const WalkerShell shell{.planes = 4,
                          .per_plane = 6,
                          .inclination_deg = 53.0,
                          .height_km = 550.0,
                          .phasing = 0,
                          .first_raan_deg = 0.1,
                          .epoch = kEpoch};
  std::vector<ElementSet> sets = walker_element_sets(shell);
  for (std::size_t i = 0; i < sets.size(); ++i) {
    sets[i].raan_rad = satsim::wrap_two_pi(sets[i].raan_rad + deg_to_rad(i % 2 == 0 ? -0.2 : 0.2));
  }
  const Classification c = classify(sets, utc(kEpoch), config_with({shell_a(1.0, 3)}));
  ASSERT_EQ(c.planes.at(0).size(), 4U);
  std::vector<double> raans;
  for (const std::size_t m : c.planes[0][0].members) {
    const auto& geometry = c.satellites[m].geometry;
    if (!geometry) {
      FAIL() << "no geometry";
    }
    raans.push_back(rad_to_deg(geometry->raan_teme_rad));
  }
  EXPECT_EQ(raans.size(), 6U);
  EXPECT_TRUE(std::any_of(raans.begin(), raans.end(), [](double r) { return r > 359.0; }));
  EXPECT_TRUE(std::any_of(raans.begin(), raans.end(), [](double r) { return r < 1.0; }));
  for (std::size_t s = 0; s < 6; ++s) {
    const ClassifiedSatellite* sat = find(c, walker_id(shell, 0, s));
    ASSERT_NE(sat, nullptr);
    EXPECT_EQ(std::get<PlaneAssignment>(sat->placement).plane_index, 0U);
  }
}

TEST(ClassificationTest, TwoShellsAtDifferentHeights) {
  const WalkerShell low{.planes = 6,
                        .per_plane = 5,
                        .inclination_deg = 53.0,
                        .height_km = 550.0,
                        .phasing = 1,
                        .first_id = 1000,
                        .epoch = kEpoch};
  const WalkerShell high{.planes = 4,
                         .per_plane = 5,
                         .inclination_deg = 53.0,
                         .height_km = 700.0,
                         .phasing = 1,
                         .first_raan_deg = 3.0,
                         .first_id = 2000,
                         .epoch = kEpoch};
  std::vector<ElementSet> sets = walker_element_sets(low);
  const auto more = walker_element_sets(high);
  sets.insert(sets.end(), more.begin(), more.end());
  ShellDefinition b = shell_a();
  b.name = "B";
  b.min_height_km = 650.0;
  b.max_height_km = 730.0;
  const Classification c = classify(sets, utc(kEpoch), config_with({shell_a(), b}));
  expect_walker_planes(c, 0, low);
  expect_walker_planes(c, 1, high);
}

// Element sets of one ideal shell at epochs up to 1.5 days apart. RAAN regresses about
// 4.5 deg/day here, so the raw element values of one plane spread over more than 5 degrees
// while planes are 15 degrees apart: only positions at one common time give clean planes.
TEST(ClassificationTest, ElementSetsAtDifferentEpochsAreComparedAtOneTime) {
  const WalkerShell shell{.planes = 24,
                          .per_plane = 10,
                          .inclination_deg = 53.0,
                          .height_km = 550.0,
                          .phasing = 5,
                          .epoch = kEpoch};
  std::vector<ElementSet> sets = walker_element_sets(shell);
  const std::vector<double> offsets_h = {-36.0, -19.0, 0.0, 11.0, 31.0};
  for (std::size_t i = 0; i < sets.size(); ++i) {
    const UtcTime moved = utc(kEpoch).plus_seconds(offsets_h[i % offsets_h.size()] * 3600.0);
    sets[i] = at_epoch(sets[i], moved.to_civil());
  }
  // The test bites: plane 0's raw RAANs are spread far wider than any sensible gap threshold.
  double min_raw = 1e9;
  double max_raw = -1e9;
  for (std::size_t s = 0; s < shell.per_plane; ++s) {
    const double raw = rad_to_deg(wrap_pi(sets[s].raan_rad));
    min_raw = std::min(min_raw, raw);
    max_raw = std::max(max_raw, raw);
  }
  EXPECT_GT(max_raw - min_raw, 5.0);

  const Classification c =
      classify(sets, utc(kEpoch).plus_seconds(6 * 3600.0), config_with({shell_a(2.0, 10)}));
  expect_walker_planes(c, 0, shell);
}

// One of each reason, on a background shell of 4 planes of 4.
TEST(ClassificationTest, EachExclusionReason) {
  const WalkerShell shell{.planes = 4,
                          .per_plane = 4,
                          .inclination_deg = 53.0,
                          .height_km = 550.0,
                          .phasing = 1,
                          .first_id = 1000,
                          .epoch = kEpoch};
  std::vector<ElementSet> sets = walker_element_sets(shell);
  sets[0].object_id = "2021-044C";  // The cutoff launch itself: no lasers.
  sets[4].object_id = "2021-045A";  // The next launch: in a plane.
  sets[8].object_id = "2019-029B";  // Earlier: no lasers.
  sets[12].object_id = "";          // Unknown launch.

  const auto single = [&](std::int64_t id, double inclination_deg, double height_km,
                          double raan_deg) {
    WalkerShell one{.inclination_deg = inclination_deg,
                    .height_km = height_km,
                    .first_raan_deg = raan_deg,
                    .first_id = id,
                    .epoch = kEpoch};
    sets.push_back(walker_element_sets(one).front());
  };
  single(1, 53.0, 380.0, 0.0);   // Low group.
  single(2, 70.0, 950.0, 0.0);   // Old height.
  single(3, 53.0, 840.0, 0.0);   // Other height.
  single(4, 53.0, 700.0, 0.0);   // Inclination of shell A, but no shell height.
  single(5, 30.0, 550.0, 0.0);   // No shell's inclination.
  single(6, 53.0, 550.0, 45.0);  // In shell A, alone at RAAN 45: no plane.

  ElementSet invalid = sets.back();
  invalid.norad_cat_id = 7;
  invalid.eccentricity = 1.5;  // Rejected before SGP4 runs.
  sets.push_back(invalid);
  ElementSet decaying;  // Vallado's 28872: decays 55 minutes after its epoch.
  decaying.norad_cat_id = 8;
  decaying.object_id = "2005-048B";
  decaying.epoch = CivilTime{2026, 11, 18, 10, 0, 0.0};
  decaying.mean_motion_rev_per_day = 16.46015938;
  decaying.eccentricity = 0.0303955;
  decaying.inclination_rad = deg_to_rad(96.4736);
  decaying.raan_rad = deg_to_rad(157.9986);
  decaying.arg_perigee_rad = deg_to_rad(244.0492);
  decaying.mean_anomaly_rad = deg_to_rad(110.6523);
  decaying.bstar_per_earth_radius = 0.00024476;
  sets.push_back(decaying);

  ClassificationConfig config = config_with({shell_a()});
  config.excluded_groups = {
      ExcludedGroup{.name = "low",
                    .reason = ExclusionReason::kLowGroup,
                    .min_inclination_deg = 0.0,
                    .max_inclination_deg = 180.0,
                    .min_height_km = 300.0,
                    .max_height_km = 420.0},
      ExcludedGroup{.name = "old",
                    .reason = ExclusionReason::kOldHeight,
                    .min_inclination_deg = 69.5,
                    .max_inclination_deg = 70.5,
                    .min_height_km = 900.0,
                    .max_height_km = 1000.0},
      ExcludedGroup{.name = "other",
                    .reason = ExclusionReason::kOtherHeight,
                    .min_inclination_deg = 52.5,
                    .max_inclination_deg = 53.5,
                    .min_height_km = 800.0,
                    .max_height_km = 880.0},
  };
  const Classification c = classify(sets, utc(kEpoch), config);
  ASSERT_EQ(c.satellites.size(), sets.size());

  EXPECT_EQ(reason(c, walker_id(shell, 0, 0)), ExclusionReason::kNoLasers);
  EXPECT_FALSE(reason(c, walker_id(shell, 1, 0)).has_value());
  EXPECT_EQ(reason(c, walker_id(shell, 2, 0)), ExclusionReason::kNoLasers);
  EXPECT_EQ(reason(c, walker_id(shell, 3, 0)), ExclusionReason::kUnknownLaunch);
  EXPECT_EQ(reason(c, 1), ExclusionReason::kLowGroup);
  EXPECT_EQ(reason(c, 2), ExclusionReason::kOldHeight);
  EXPECT_EQ(reason(c, 3), ExclusionReason::kOtherHeight);
  EXPECT_EQ(reason(c, 4), ExclusionReason::kNotAtShellHeight);
  EXPECT_EQ(reason(c, 5), ExclusionReason::kNoShell);
  EXPECT_EQ(reason(c, 6), ExclusionReason::kNoPlane);
  EXPECT_EQ(reason(c, 7), ExclusionReason::kPropagationFailed);
  EXPECT_EQ(reason(c, 8), ExclusionReason::kPropagationFailed);

  // Satellites excluded by launch still have their geometry and shell.
  EXPECT_EQ(find(c, walker_id(shell, 0, 0))->shell_index, 0U);
  EXPECT_TRUE(find(c, walker_id(shell, 0, 0))->geometry.has_value());
  EXPECT_EQ(find(c, 6)->shell_index, 0U);
  EXPECT_FALSE(find(c, 4)->shell_index.has_value());
  EXPECT_EQ(find(c, 7)->propagation_error, PropagationError::kInvalidElements);
  EXPECT_EQ(find(c, 8)->propagation_error, PropagationError::kDecayed);
  EXPECT_FALSE(find(c, 8)->geometry.has_value());

  // The planes are formed without the excluded satellites: Walker planes 0, 2 and 3 lost one.
  ASSERT_EQ(c.planes.at(0).size(), 4U);
  std::vector<std::size_t> sizes;
  for (const auto& plane : c.planes[0]) {
    sizes.push_back(plane.members.size());
  }
  std::sort(sizes.begin(), sizes.end());
  EXPECT_EQ(sizes, (std::vector<std::size_t>{3, 3, 3, 4}));
  std::size_t placed = 0;
  for (const ClassifiedSatellite& s : c.satellites) {
    const bool in_plane = std::holds_alternative<PlaneAssignment>(s.placement);
    placed += in_plane ? 1 : 0;
    EXPECT_EQ(
        s.propagation_error.has_value(),
        !in_plane && std::get<ExclusionReason>(s.placement) == ExclusionReason::kPropagationFailed);
    EXPECT_EQ(s.geometry.has_value(), !s.propagation_error.has_value());
  }
  EXPECT_EQ(placed, 13U);  // 16 minus 2 without lasers and 1 unknown launch.
}

TEST(ClassificationTest, PlaneBelowTheMinimumSizeIsNoPlane) {
  const WalkerShell shell{.planes = 3,
                          .per_plane = 4,
                          .inclination_deg = 53.0,
                          .height_km = 550.0,
                          .phasing = 1,
                          .epoch = kEpoch};
  std::vector<ElementSet> sets = walker_element_sets(shell);
  sets.erase(sets.begin() + 4);  // Plane 1 keeps 3 satellites.
  const Classification c = classify(sets, utc(kEpoch), config_with({shell_a(2.0, 4)}));
  ASSERT_EQ(c.planes.at(0).size(), 2U);
  for (std::size_t s = 1; s < 4; ++s) {
    EXPECT_EQ(reason(c, walker_id(shell, 1, s)), ExclusionReason::kNoPlane);
  }
  // Planes are numbered without gaps: plane 2 of the Walker shell is plane 1.
  EXPECT_EQ(std::get<PlaneAssignment>(find(c, walker_id(shell, 2, 0))->placement).plane_index, 1U);
}

TEST(ClassificationTest, ShuffledInputGivesTheSameResult) {
  const WalkerShell shell{.planes = 6,
                          .per_plane = 5,
                          .inclination_deg = 53.0,
                          .height_km = 550.0,
                          .phasing = 1,
                          .epoch = kEpoch};
  std::vector<ElementSet> sets = walker_element_sets(shell);
  // Two satellites with identical elements apart from the catalogue number: a tie only the
  // catalogue number breaks.
  ElementSet twin = sets[3];
  twin.norad_cat_id = 99;
  sets.push_back(twin);
  const ClassificationConfig config = config_with({shell_a()});
  const Classification first = classify(sets, utc(kEpoch), config);

  std::mt19937 random(12345);  // NOLINT(cert-msc32-c,cert-msc51-cpp): fixed seed on purpose.
  for (int round = 0; round < 3; ++round) {
    std::shuffle(sets.begin(), sets.end(), random);
    const Classification again = classify(sets, utc(kEpoch), config);
    ASSERT_EQ(again.satellites.size(), first.satellites.size());
    for (std::size_t i = 0; i < first.satellites.size(); ++i) {
      const ClassifiedSatellite& a = first.satellites[i];
      const ClassifiedSatellite& b = again.satellites[i];
      EXPECT_EQ(a.norad_cat_id, b.norad_cat_id);
      EXPECT_EQ(a.shell_index, b.shell_index);
      EXPECT_EQ(a.placement, b.placement) << a.norad_cat_id;
      if (!a.geometry || !b.geometry) {
        FAIL() << "no geometry for " << a.norad_cat_id;
      }
      EXPECT_EQ(a.geometry->raan_teme_rad, b.geometry->raan_teme_rad);
      EXPECT_EQ(a.geometry->argument_of_latitude_rad, b.geometry->argument_of_latitude_rad);
    }
    ASSERT_EQ(again.planes, first.planes);
  }
  // The twins are neighbours in their plane, the lower catalogue number first.
  const auto twin_place = std::get<PlaneAssignment>(find(first, 99)->placement);
  const auto original_place =
      std::get<PlaneAssignment>(find(first, walker_id(shell, 0, 3))->placement);
  EXPECT_EQ(twin_place.plane_index, original_place.plane_index);
  EXPECT_EQ(twin_place.index_in_plane + 1, original_place.index_in_plane);
}

// A shell's height band has two limits. A plane 20 km above the upper one and a plane 20 km
// below the lower one are at the shell's inclination but not in the shell.
TEST(ClassificationTest, SatellitesAboveOrBelowTheHeightBandAreNotInTheShell) {
  const WalkerShell inside{.planes = 1, .per_plane = 4, .height_km = 550.0, .first_id = 1000};
  const WalkerShell above{.planes = 1, .per_plane = 4, .height_km = 600.0, .first_id = 2000};
  const WalkerShell below{.planes = 1, .per_plane = 4, .height_km = 480.0, .first_id = 3000};
  std::vector<ElementSet> sets = walker_element_sets(inside);
  for (const WalkerShell& shell : {above, below}) {
    const std::vector<ElementSet> more = walker_element_sets(shell);
    sets.insert(sets.end(), more.begin(), more.end());
  }
  const Classification c = classify(sets, utc(kEpoch), config_with({shell_a()}));
  ASSERT_EQ(c.planes.size(), 1U);
  ASSERT_EQ(c.planes[0].size(), 1U);
  EXPECT_EQ(c.planes[0][0].members.size(), 4U);
  for (std::size_t s = 0; s < 4; ++s) {
    EXPECT_EQ(reason(c, walker_id(inside, 0, s)), std::nullopt);
    EXPECT_EQ(reason(c, walker_id(above, 0, s)), ExclusionReason::kNotAtShellHeight);
    EXPECT_EQ(reason(c, walker_id(below, 0, s)), ExclusionReason::kNotAtShellHeight);
  }
}

TEST(ClassificationTest, BadConfigurationIsAnError) {
  ClassificationConfig config = config_with({shell_a()});
  config.shells[0].max_height_km = config.shells[0].min_height_km;
  const ClassificationOrError result = classify_constellation({}, utc(kEpoch), config);
  ASSERT_TRUE(std::holds_alternative<std::string>(result));
  EXPECT_NE(std::get<std::string>(result).find("\"A\""), std::string::npos);

  config = config_with({shell_a(0.0)});
  EXPECT_TRUE(check_config(config).has_value());
  config = config_with({shell_a(1.0, 0)});
  EXPECT_TRUE(check_config(config).has_value());
  config = config_with({shell_a()});
  config.sampling.samples_per_orbit = 3;
  EXPECT_TRUE(check_config(config).has_value());
  config = config_with({shell_a()});
  config.excluded_groups = {ExcludedGroup{.name = "x",
                                          .reason = ExclusionReason::kNoPlane,
                                          .min_height_km = 1.0,
                                          .max_height_km = 2.0}};
  EXPECT_TRUE(check_config(config).has_value());
}

TEST(ClassificationTest, DefaultConfigurationIsUsable) {
  const ClassificationConfig config = default_config();
  EXPECT_EQ(check_config(config), std::nullopt);
  EXPECT_EQ(config.shells.size(), 4U);
  EXPECT_EQ(config.last_launch_without_lasers, (Launch{.year = 2021, .number = 44}));
}

TEST(ClassificationTest, ReasonNamesAreDistinct) {
  std::vector<std::string> names;
  names.reserve(satsim::constellation::kAllExclusionReasons.size());
  for (const ExclusionReason r : satsim::constellation::kAllExclusionReasons) {
    names.emplace_back(name_of(r));
  }
  std::sort(names.begin(), names.end());
  EXPECT_EQ(std::unique(names.begin(), names.end()), names.end());
  EXPECT_EQ(name_of(ExclusionReason::kNoLasers), "no_lasers");
}

}  // namespace
