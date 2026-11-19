#include "fixture.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <span>
#include <string>

#include "common/csv.hpp"
#include "satsim/core/angles.hpp"
#include "satsim/orbit/element_set.hpp"
#include "satsim/time/utc_time.hpp"
#include "support/synthetic.hpp"
#include "topology/arguments.hpp"

namespace satsim::topology_app::testing {

namespace {

std::string pad2(int value) { return (value < 10 ? "0" : "") + std::to_string(value); }

void field(std::string& out, std::string_view name, double value) {
  out += ",\"" + std::string(name) + "\":";
  app::append_double(out, value);
}

}  // namespace

constellation::testing::WalkerShell fixture_shell() {
  return {.planes = 10,
          .per_plane = 16,
          .inclination_deg = 53.16,
          .height_km = 462.5,
          .phasing = 1,
          .first_raan_deg = 10.0,
          .first_id = 900000,
          .object_id = "2025-001A",
          .epoch = {2026, 11, 18, 12, 0, 0.0}};
}

void write_snapshot(const std::filesystem::path& dir, std::span<const orbit::ElementSet> sets) {
  std::string json = "[\n";
  for (std::size_t i = 0; i < sets.size(); ++i) {
    const orbit::ElementSet& e = sets[i];
    // Whole seconds only: the fixture epochs have no fraction.
    const std::string epoch = std::to_string(e.epoch.year) + "-" + pad2(e.epoch.month) + "-" +
                              pad2(e.epoch.day) + "T" + pad2(e.epoch.hour) + ":" +
                              pad2(e.epoch.minute) + ":" + pad2(static_cast<int>(e.epoch.second)) +
                              ".000000";
    json += R"({"OBJECT_NAME":")" + e.object_name + R"(","OBJECT_ID":")" + e.object_id +
            R"(","EPOCH":")" + epoch + R"(","NORAD_CAT_ID":)" + std::to_string(e.norad_cat_id);
    field(json, "MEAN_MOTION", e.mean_motion_rev_per_day);
    field(json, "ECCENTRICITY", e.eccentricity);
    field(json, "INCLINATION", rad_to_deg(e.inclination_rad));
    field(json, "RA_OF_ASC_NODE", rad_to_deg(e.raan_rad));
    field(json, "ARG_OF_PERICENTER", rad_to_deg(e.arg_perigee_rad));
    field(json, "MEAN_ANOMALY", rad_to_deg(e.mean_anomaly_rad));
    field(json, "BSTAR", e.bstar_per_earth_radius);
    json += i + 1 < sets.size() ? "},\n" : "}\n";
  }
  json += "]\n";
  {
    std::ofstream gp(dir / "gp.json", std::ios::binary | std::ios::trunc);
    gp << json;
    std::ofstream sup(dir / "supgp.json", std::ios::binary | std::ios::trunc);
    sup << "[]\n";
    ASSERT_TRUE(gp.good() && sup.good()) << dir;
  }
  std::filesystem::copy_file("tests/propagate/data/snapshot/finals2000A.all",
                             dir / "finals2000A.all",
                             std::filesystem::copy_options::overwrite_existing);
}

void copy_ground(const std::filesystem::path& dir) {
  std::filesystem::copy(
      kGroundDir, dir,
      std::filesystem::copy_options::recursive | std::filesystem::copy_options::overwrite_existing);
}

Options fixture_options(const std::filesystem::path& snapshot_dir, std::int64_t duration_s,
                        std::int64_t step_s, topology::LaserPattern pattern, EndpointModel model) {
  constexpr std::int64_t kNs = 1'000'000'000;
  const auto start = time::UtcTime::from_civil(fixture_shell().epoch);
  if (!start) {
    ADD_FAILURE() << "bad fixture epoch";
    std::abort();
  }
  Options options{.snapshot_dir = snapshot_dir,
                  .ground_dir = kGroundDir,
                  .cities_file = kCities,
                  .start = *start,
                  .duration_ns = duration_s * kNs,
                  .step_ns = step_s * kNs,
                  .shells = {},
                  .out = "-"};
  options.pattern = pattern;
  options.model = model;
  return options;
}

}  // namespace satsim::topology_app::testing
