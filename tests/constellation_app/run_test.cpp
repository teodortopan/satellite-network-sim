// The constellation program's library: classify_snapshot, the CSV, the summary and run_main,
// on the propagate program's fixture snapshot (six satellites: two first-generation at 53
// degrees, two at 70 degrees, two raising at 30.5 degrees).

#include "constellation/run.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "satsim/constellation/classification.hpp"
#include "satsim/core/angles.hpp"
#include "test_support.hpp"

namespace {

using satsim::constellation::ClassificationConfig;
using satsim::constellation::ExclusionReason;
using satsim::constellation::PlaneAssignment;
using satsim::constellation_app::ClassifiedRun;
using satsim::constellation_app::classify_snapshot;
using satsim::constellation_app::kCsvColumns;
using satsim::constellation_app::Options;
using satsim::propagate::testing::kFixtureIds;
using satsim::propagate::testing::kFixtureSnapshot;
using satsim::propagate::testing::number;
using satsim::propagate::testing::parse_csv;
using satsim::propagate::testing::read_file;
using satsim::propagate::testing::ScratchDir;
using satsim::propagate::testing::utc;
using satsim::propagate::testing::write_file;

constexpr std::string_view kTime = "2026-11-18T18:00:00";

Options options(std::filesystem::path snapshot = kFixtureSnapshot) {
  return {.snapshot_dir = std::move(snapshot), .reference = utc(kTime), .out = "-"};
}

// The default configuration with planes of one allowed in the 70 degree shell, so the fixture's
// 70 degree satellites get a plane.
ClassificationConfig single_satellite_planes() {
  ClassificationConfig config = satsim::constellation::default_config();
  for (auto& shell : config.shells) {
    shell.min_plane_size = 1;
    shell.min_height_km = 400.0;
    shell.max_height_km = 600.0;
  }
  config.excluded_groups.clear();
  return config;
}

ClassifiedRun classified(const ClassificationConfig& config) {
  auto result = classify_snapshot(options(), config);
  if (const auto* error = std::get_if<std::string>(&result)) {
    ADD_FAILURE() << *error;
    std::abort();  // The fixture must classify; nothing else can be checked.
  }
  return std::get<ClassifiedRun>(std::move(result));
}

std::size_t column(std::string_view name) {
  for (std::size_t i = 0; i < kCsvColumns.size(); ++i) {
    if (kCsvColumns[i] == name) {
      return i;
    }
  }
  ADD_FAILURE() << "no column " << name;
  return 0;
}

TEST(ConstellationRunTest, OneRowPerSatelliteWithExactlyOnePlaceOrReason) {
  const ClassifiedRun run = classified(satsim::constellation::default_config());
  std::ostringstream csv;
  ASSERT_TRUE(write_csv(run, csv));
  const auto records = parse_csv(csv.str());
  ASSERT_EQ(records.size(), 1 + kFixtureIds.size());
  EXPECT_EQ(records[0], std::vector<std::string>(kCsvColumns.begin(), kCsvColumns.end()));

  std::set<std::string> reason_names;
  for (const ExclusionReason r : satsim::constellation::kAllExclusionReasons) {
    reason_names.emplace(satsim::constellation::name_of(r));
  }
  for (std::size_t i = 1; i < records.size(); ++i) {
    const auto& row = records[i];
    ASSERT_EQ(row.size(), kCsvColumns.size());
    EXPECT_EQ(row[column("norad_cat_id")], std::to_string(kFixtureIds[i - 1]));
    const bool placed = !row[column("plane_index")].empty();
    EXPECT_EQ(placed, !row[column("index_in_plane")].empty());
    EXPECT_NE(placed, !row[column("exclusion_reason")].empty());
    if (!placed) {
      EXPECT_EQ(reason_names.count(row[column("exclusion_reason")]), 1U)
          << row[column("exclusion_reason")];
    }
    EXPECT_EQ(row[column("propagation_error")], "");
    EXPECT_NE(row[column("inclination_deg")], "");
  }
  // The two first-generation satellites (launch 2019-074) have no lasers.
  EXPECT_EQ(records[1][column("exclusion_reason")], "no_lasers");
  EXPECT_EQ(records[2][column("exclusion_reason")], "no_lasers");
  EXPECT_EQ(records[2][column("element_source")], "standard");
  EXPECT_EQ(records[1][column("element_source")], "supplemental");
  // The two at 30.5 degrees match no shell.
  EXPECT_EQ(records[5][column("exclusion_reason")], "no_shell");
  EXPECT_EQ(records[6][column("exclusion_reason")], "no_shell");
}

TEST(ConstellationRunTest, CsvValuesAreTheClassification) {
  const ClassifiedRun run = classified(single_satellite_planes());
  std::ostringstream csv;
  ASSERT_TRUE(write_csv(run, csv));
  const auto records = parse_csv(csv.str());
  ASSERT_EQ(records.size(), 1 + run.classification.satellites.size());
  std::size_t placed = 0;
  for (std::size_t i = 0; i < run.classification.satellites.size(); ++i) {
    const auto& sat = run.classification.satellites[i];
    const auto& row = records[i + 1];
    EXPECT_EQ(row[column("object_name")], sat.object_name);
    EXPECT_EQ(row[column("object_id")], sat.object_id);
    if (const auto* place = std::get_if<PlaneAssignment>(&sat.placement)) {
      ++placed;
      if (!sat.shell_index) {
        FAIL() << "no shell for a satellite in a plane";
      }
      EXPECT_EQ(row[column("shell")], run.config.shells[*sat.shell_index].name);
      EXPECT_EQ(row[column("plane_index")], std::to_string(place->plane_index));
      EXPECT_EQ(row[column("index_in_plane")], std::to_string(place->index_in_plane));
    }
    if (!sat.geometry) {
      FAIL() << "no geometry";
    }
    const auto& g = *sat.geometry;
    // Shortest round-trip text: the CSV reads back exactly.
    EXPECT_EQ(number(row[column("inclination_deg")]), satsim::rad_to_deg(g.inclination_rad));
    EXPECT_EQ(number(row[column("raan_teme_deg")]), satsim::rad_to_deg(g.raan_teme_rad));
    EXPECT_EQ(number(row[column("argument_of_latitude_deg")]),
              satsim::rad_to_deg(g.argument_of_latitude_rad));
    EXPECT_EQ(number(row[column("mean_height_km")]), g.mean_height_km);
    EXPECT_EQ(number(row[column("mean_radius_km")]), g.mean_radius_km);
    if (!g.raan_drift_rad_per_day) {
      FAIL() << "no drift";
    }
    EXPECT_EQ(number(row[column("raan_drift_deg_per_day")]),
              satsim::rad_to_deg(*g.raan_drift_rad_per_day));
    EXPECT_LT(number(row[column("raan_drift_deg_per_day")]), 0.0);  // Prograde: regression.
    const auto epoch = satsim::time::UtcTime::from_civil(run.element_sets[i].elements.epoch);
    if (!epoch) {
      FAIL() << "bad epoch";
    }
    EXPECT_NEAR(number(row[column("element_age_s")]), utc(kTime).seconds_since(*epoch), 1e-6);
  }
  // 70 degrees: 100001 and 55278, alone in their planes. 30.5: 799501782 and 799501787.
  EXPECT_GE(placed, 2U);
}

TEST(ConstellationRunTest, SummaryCountsShellsPlanesAndReasons) {
  const ClassifiedRun run = classified(satsim::constellation::default_config());
  std::ostringstream out;
  write_summary(run, out);
  const std::string summary = out.str();
  for (const std::string expected :
       {"constellation summary\n", "  reference time: 2026-11-18T18:00:00 UTC\n",
        "  gp.json: 4 records loaded, 0 record errors\n",
        "  supgp.json: 5 records loaded, 0 record errors\n", "  satellites: 6\n",
        "  shell 53deg (inclination 53.16 +- 0.30 deg, height 467.5 to 471.0 km",
        "    no_lasers: 2\n", "    no_shell: 2\n", "    propagation_failed: 0\n"}) {
    EXPECT_NE(summary.find(expected), std::string::npos) << expected << "\n" << summary;
  }
}

TEST(ConstellationRunTest, MissingElementFileIsAnError) {
  const ScratchDir dir;
  write_file(dir.path() / "gp.json", "[]");
  const auto result =
      classify_snapshot(options(dir.path()), satsim::constellation::default_config());
  ASSERT_TRUE(std::holds_alternative<std::string>(result));
  EXPECT_EQ(std::get<std::string>(result),
            "snapshot directory " + dir.path().string() + " has no supgp.json");
}

TEST(ConstellationRunTest, BadConfigurationIsAnError) {
  ClassificationConfig config = satsim::constellation::default_config();
  config.shells[0].plane_gap_threshold_deg = -1.0;
  const auto result = classify_snapshot(options(), config);
  ASSERT_TRUE(std::holds_alternative<std::string>(result));
  EXPECT_EQ(std::get<std::string>(result).rfind("configuration: ", 0), 0U);
}

struct Result {
  int status = -1;
  std::string out;
  std::string err;
};

Result run_main(const std::vector<std::string>& args) {
  const std::vector<std::string_view> views(args.begin(), args.end());
  std::ostringstream out;
  std::ostringstream err;
  Result result;
  result.status = satsim::constellation_app::run_main(views, out, err);
  result.out = out.str();
  result.err = err.str();
  return result;
}

TEST(ConstellationMainTest, WritesTheCsvFileAndTheSummaryToOut) {
  const ScratchDir dir;
  const std::string csv_path = (dir.path() / "constellation.csv").string();
  const Result r = run_main(
      {"--snapshot", kFixtureSnapshot.string(), "--time", std::string(kTime), "--out", csv_path});
  EXPECT_EQ(r.status, 0) << r.err;
  EXPECT_EQ(r.err, "");
  EXPECT_NE(r.out.find("satellites: 6\n"), std::string::npos) << r.out;
  EXPECT_EQ(parse_csv(read_file(csv_path)).size(), 7U);
}

TEST(ConstellationMainTest, CsvToStandardOutputMovesTheSummaryToErr) {
  const Result r = run_main(
      {"--snapshot", kFixtureSnapshot.string(), "--time", std::string(kTime), "--out", "-"});
  EXPECT_EQ(r.status, 0) << r.err;
  EXPECT_EQ(parse_csv(r.out).size(), 7U);
  EXPECT_NE(r.err.find("satellites: 6\n"), std::string::npos) << r.err;
}

TEST(ConstellationMainTest, HelpAndBadArguments) {
  const Result help = run_main({"--help"});
  EXPECT_EQ(help.status, 0);
  EXPECT_EQ(help.out.rfind("usage: constellation", 0), 0U);
  const Result bad = run_main({"--snapshot"});
  EXPECT_EQ(bad.status, 2);
  EXPECT_EQ(bad.err,
            "constellation: --snapshot needs a value\nRun 'constellation --help' for usage.\n");
}

TEST(ConstellationMainTest, FailedRunCreatesNoOutputFile) {
  const ScratchDir dir;
  const std::string csv_path = (dir.path() / "constellation.csv").string();
  const Result r = run_main({"--snapshot", (dir.path() / "missing").string(), "--time",
                             std::string(kTime), "--out", csv_path});
  EXPECT_EQ(r.status, 1);
  EXPECT_EQ(r.err, "constellation: snapshot directory " + (dir.path() / "missing").string() +
                       " does not exist\n");
  EXPECT_FALSE(std::filesystem::exists(csv_path));
}

TEST(ConstellationMainTest, ShellsFileReplacesTheBuiltInShells) {
  const ScratchDir dir;
  // One shell at 70 degrees that takes the fixture's two 70-degree satellites, each a plane of
  // its own or together; every other satellite is then in no shell.
  write_file(dir.path() / "shells.json",
             R"({"shells": [{"name": "polarish", "inclination_deg": 70.0,
                 "inclination_tolerance_deg": 0.5, "min_height_km": 400.0,
                 "max_height_km": 700.0, "plane_gap_threshold_deg": 180.0,
                 "min_plane_size": 1}]})");
  const Result r = run_main({"--snapshot", kFixtureSnapshot.string(), "--time", std::string(kTime),
                             "--out", "-", "--shells-file", (dir.path() / "shells.json").string()});
  EXPECT_EQ(r.status, 0) << r.err;
  EXPECT_NE(r.err.find("polarish"), std::string::npos) << r.err;
  const auto rows = parse_csv(r.out);
  std::size_t in_shell = 0;
  for (std::size_t i = 1; i < rows.size(); ++i) {
    in_shell += rows[i][column("shell")] == "polarish" ? 1U : 0U;
  }
  EXPECT_EQ(in_shell, 2U);

  const Result missing =
      run_main({"--snapshot", kFixtureSnapshot.string(), "--time", std::string(kTime), "--out", "-",
                "--shells-file", (dir.path() / "missing.json").string()});
  EXPECT_EQ(missing.status, 1);
  EXPECT_EQ(missing.err, "constellation: --shells-file cannot open " +
                             (dir.path() / "missing.json").string() + "\n");
  EXPECT_EQ(missing.out, "");
}

TEST(ConstellationMainTest, UnwritableOutputIsAnError) {
  const ScratchDir dir;
  const std::string csv_path = (dir.path() / "no" / "such" / "dir.csv").string();
  const Result r = run_main(
      {"--snapshot", kFixtureSnapshot.string(), "--time", std::string(kTime), "--out", csv_path});
  EXPECT_EQ(r.status, 1);
  EXPECT_EQ(r.err, "constellation: cannot open " + csv_path + " for writing\n");
}

}  // namespace
