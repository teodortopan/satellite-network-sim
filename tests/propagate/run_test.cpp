// The propagate run, called as a library: CSV values against the library functions computed
// directly, row order and count, filtering, the time grid, fatal errors, failed satellites,
// CSV quoting and the summary.

#include "propagate/run.hpp"

#include <gtest/gtest.h>

#include <Eigen/Core>
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <tuple>
#include <variant>
#include <vector>

#include "common/csv.hpp"
#include "common/steps.hpp"
#include "propagate/csv.hpp"
#include "satsim/core/angles.hpp"
#include "satsim/frames/geodetic.hpp"
#include "satsim/frames/teme_ecef.hpp"
#include "satsim/frames/teme_j2000.hpp"
#include "satsim/orbit/element_selection.hpp"
#include "satsim/orbit/omm_loader.hpp"
#include "satsim/orbit/propagator.hpp"
#include "satsim/time/earth_orientation.hpp"
#include "satsim/time/time_scales.hpp"
#include "test_support.hpp"

namespace {

using satsim::orbit::ElementSource;
using satsim::orbit::PropagationError;
using satsim::orbit::Propagator;
using satsim::orbit::SourcedElementSet;
using satsim::orbit::TemeState;
using satsim::propagate::kCsvColumns;
using satsim::propagate::Options;
using satsim::propagate::RunOutcome;
using satsim::propagate::testing::column;
using satsim::propagate::testing::kFixtureIds;
using satsim::propagate::testing::kFixtureSnapshot;
using satsim::propagate::testing::kStandardOnlyId;
using satsim::propagate::testing::make_snapshot;
using satsim::propagate::testing::number;
using satsim::propagate::testing::options;
using satsim::propagate::testing::parse_csv;
using satsim::propagate::testing::read_file;
using satsim::propagate::testing::ScratchDir;
using satsim::propagate::testing::utc;
using satsim::propagate::testing::write_file;
using satsim::time::UtcTime;

using Records = std::vector<std::vector<std::string>>;

// Runs and parses the CSV; the run must succeed.
Records run_ok(const Options& o, RunOutcome* outcome_out = nullptr) {
  std::ostringstream csv;
  RunOutcome outcome = satsim::propagate::run(o, csv);
  EXPECT_FALSE(outcome.error.has_value()) << outcome.error.value_or("");
  Records records = parse_csv(csv.str());
  if (outcome_out != nullptr) {
    *outcome_out = std::move(outcome);
  }
  return records;
}

// Runs a case that must fail before writing anything; returns the message.
std::string run_fails(const Options& o) {
  std::ostringstream csv;
  const RunOutcome outcome = satsim::propagate::run(o, csv);
  EXPECT_TRUE(csv.str().empty()) << "output written before the error";
  EXPECT_TRUE(outcome.error.has_value());
  return outcome.error.value_or("");
}

// The element sets the fixture's satellites use, chosen directly with the library.
std::vector<SourcedElementSet> fixture_selection() {
  const auto standard = satsim::orbit::load_omm_json(kFixtureSnapshot / "gp.json");
  const auto supplemental = satsim::orbit::load_omm_json(kFixtureSnapshot / "supgp.json");
  EXPECT_FALSE(standard.file_error || supplemental.file_error);
  return satsim::orbit::select_element_sets(standard.element_sets, supplemental.element_sets)
      .selected;
}

satsim::time::EarthOrientationTable fixture_earth_orientation() {
  auto loaded = satsim::time::load_earth_orientation(kFixtureSnapshot / "finals2000A.all");
  if (!loaded.table) {
    ADD_FAILURE() << loaded.error.value_or("");
    return {};
  }
  return std::move(*loaded.table);
}

// One element in OMM JSON, with the fields the loader requires.
std::string omm_record(std::string_view name_json, std::int64_t id, std::string_view epoch,
                       double mean_motion, double eccentricity, double inclination_deg,
                       double mean_anomaly_deg, double bstar) {
  std::ostringstream s;
  s.precision(17);
  s << R"({"OBJECT_NAME":)" << name_json << R"(,"OBJECT_ID":"2026-999A","EPOCH":")" << epoch
    << R"(","MEAN_MOTION":)" << mean_motion << R"(,"ECCENTRICITY":)" << eccentricity
    << R"(,"INCLINATION":)" << inclination_deg
    << R"(,"RA_OF_ASC_NODE":157.9986,"ARG_OF_PERICENTER":244.0492,"MEAN_ANOMALY":)"
    << mean_anomaly_deg << R"(,"EPHEMERIS_TYPE":0,"NORAD_CAT_ID":)" << id << R"(,"BSTAR":)" << bstar
    << "}";
  return s.str();
}

// A healthy Starlink record (44753 from the fixture's gp.json, under another catalogue number
// and name when asked).
std::string healthy_record(std::string_view name_json = R"("HEALTHY")", std::int64_t id = 44753) {
  return omm_record(name_json, id, "2026-11-17T12:17:04.161408", 16.0798325, 0.0001771, 53.0321,
                    294.9295, 0.00065973);
}

// Vallado's verification case 28872 (MINOTAUR R/B), moved to a 2026 epoch: SGP4 near-earth
// propagation does not depend on the calendar date, so it still decays between 50 and 55
// minutes after epoch (tests/orbit/propagator_test.cpp, DecayIsAnErrorValue).
std::string decaying_record() {
  return omm_record(R"("DECAYING")", 28872, "2026-11-17T18:00:00.000000", 16.46015938, 0.0303955,
                    96.4736, 110.6523, 0.00024476);
}

TEST(PropagateRunTest, CsvValuesEqualTheLibraryFunctionsComputedDirectly) {
  const Records records = run_ok(options("2026-11-17T18:00:00", 600, 300));
  ASSERT_EQ(records.size(), 1 + 3 * kFixtureIds.size());

  const std::vector<SourcedElementSet> selection = fixture_selection();
  const auto table = fixture_earth_orientation();
  for (std::size_t r = 1; r < records.size(); ++r) {
    const std::vector<std::string>& row = records[r];
    ASSERT_EQ(row.size(), kCsvColumns.size());
    const UtcTime t = utc(row[column("utc")]);
    const auto id = static_cast<std::int64_t>(number(row[column("norad_cat_id")]));
    const auto chosen = std::find_if(selection.begin(), selection.end(),
                                     [id](const auto& s) { return s.elements.norad_cat_id == id; });
    ASSERT_NE(chosen, selection.end()) << id;
    SCOPED_TRACE(row[column("utc")] + " " + std::to_string(id));

    EXPECT_EQ(row[column("object_name")], chosen->elements.object_name);
    EXPECT_EQ(row[column("element_source")],
              chosen->source == ElementSource::kSupplemental ? "supplemental" : "standard");

    const auto built = Propagator::create(chosen->elements);
    ASSERT_TRUE(std::holds_alternative<Propagator>(built));
    const auto& p = std::get<Propagator>(built);
    const auto state = p.propagate(t);
    ASSERT_TRUE(std::holds_alternative<TemeState>(state));
    const auto& teme = std::get<TemeState>(state);
    const auto eop = table.at(t);
    if (!eop) {
      FAIL() << "no Earth orientation";
    }
    const Eigen::Vector3d ecef = satsim::frames::teme_to_ecef(teme.position_km, t, *eop);
    const satsim::frames::Geodetic geodetic = satsim::frames::ecef_to_geodetic(ecef);
    const UtcTime tt = satsim::time::tt_from_utc(t);
    const Eigen::Vector3d j2000 = satsim::frames::teme_to_j2000(teme.position_km, tt);
    const Eigen::Vector3d j2000_velocity = satsim::frames::teme_to_j2000(teme.velocity_km_s, tt);

    const std::array<std::pair<std::string_view, double>, 19> expected = {{
        {"element_age_s", t.seconds_since(p.epoch())},
        {"teme_x_km", teme.position_km.x()},
        {"teme_y_km", teme.position_km.y()},
        {"teme_z_km", teme.position_km.z()},
        {"teme_vx_km_s", teme.velocity_km_s.x()},
        {"teme_vy_km_s", teme.velocity_km_s.y()},
        {"teme_vz_km_s", teme.velocity_km_s.z()},
        {"ecef_x_km", ecef.x()},
        {"ecef_y_km", ecef.y()},
        {"ecef_z_km", ecef.z()},
        {"geodetic_latitude_deg", satsim::rad_to_deg(geodetic.latitude_rad)},
        {"geodetic_longitude_deg", satsim::rad_to_deg(geodetic.longitude_rad)},
        {"geodetic_height_km", geodetic.height_km},
        {"j2000_x_km", j2000.x()},
        {"j2000_y_km", j2000.y()},
        {"j2000_z_km", j2000.z()},
        {"j2000_vx_km_s", j2000_velocity.x()},
        {"j2000_vy_km_s", j2000_velocity.y()},
        {"j2000_vz_km_s", j2000_velocity.z()},
    }};
    for (const auto& [name, value] : expected) {
      // Exact: the text must read back as the very same double.
      EXPECT_EQ(number(row[column(name)]), value) << name;
    }
  }
}

TEST(PropagateRunTest, HeaderThenOneRowPerSatelliteAndTimeOrderedByTimeThenId) {
  RunOutcome outcome;
  const Records records = run_ok(options("2026-11-17T18:00:00", 600, 300), &outcome);
  ASSERT_FALSE(records.empty());
  EXPECT_TRUE(
      std::equal(records[0].begin(), records[0].end(), kCsvColumns.begin(), kCsvColumns.end()));
  const std::array<std::string_view, 3> times = {"2026-11-17T18:00:00", "2026-11-17T18:05:00",
                                                 "2026-11-17T18:10:00"};
  ASSERT_EQ(records.size(), 1 + times.size() * kFixtureIds.size());
  std::size_t r = 1;
  for (const std::string_view time : times) {
    for (const std::int64_t id : kFixtureIds) {
      EXPECT_EQ(records[r][column("utc")], time) << r;
      EXPECT_EQ(records[r][column("norad_cat_id")], std::to_string(id)) << r;
      ++r;
    }
  }
  EXPECT_EQ(outcome.summary.rows_written, times.size() * kFixtureIds.size());
  EXPECT_EQ(outcome.summary.time_count, 3);
  EXPECT_EQ(outcome.summary.selected_supplemental, 5U);
  EXPECT_EQ(outcome.summary.selected_standard, 1U);
  EXPECT_EQ(outcome.summary.standard_file.records_loaded, 4U);
  EXPECT_EQ(outcome.summary.supplemental_file.records_loaded, 5U);
  EXPECT_TRUE(outcome.summary.build_failures.empty());
  EXPECT_TRUE(outcome.summary.propagation_failures.empty());
}

// 44753 is only in the standard file and falls back to it; the others use supplemental sets.
TEST(PropagateRunTest, StandardSetIsTheFallback) {
  const Records records = run_ok(options("2026-11-17T18:00:00", 0, 60));
  ASSERT_EQ(records.size(), 1 + kFixtureIds.size());
  for (std::size_t r = 1; r < records.size(); ++r) {
    const bool standard_only =
        records[r][column("norad_cat_id")] == std::to_string(kStandardOnlyId);
    EXPECT_EQ(records[r][column("element_source")], standard_only ? "standard" : "supplemental");
  }
}

TEST(PropagateRunTest, IdsRestrictTheSatellitesAndUnknownOnesAreReported) {
  Options o = options("2026-11-17T18:00:00", 60, 60);
  o.ids = {123, kStandardOnlyId, 799501782};
  RunOutcome outcome;
  const Records records = run_ok(o, &outcome);
  ASSERT_EQ(records.size(), 1 + 2 * 2);
  for (std::size_t r = 1; r < records.size(); ++r) {
    const std::string& id = records[r][column("norad_cat_id")];
    EXPECT_TRUE(id == "44753" || id == "799501782") << id;
  }
  EXPECT_EQ(outcome.summary.ids_not_found, std::vector<std::int64_t>{123});
  EXPECT_EQ(outcome.summary.selected_standard, 1U);
  EXPECT_EQ(outcome.summary.selected_supplemental, 1U);
}

std::vector<std::string> times_of(const Records& records) {
  std::vector<std::string> times;
  for (std::size_t r = 1; r < records.size(); ++r) {
    if (times.empty() || times.back() != records[r][column("utc")]) {
      times.push_back(records[r][column("utc")]);
    }
  }
  return times;
}

TEST(PropagateRunTest, EndTimeIsIncludedOnlyWhenItLandsOnAStep) {
  Options o = options("2026-11-17T18:00:00", 600, 300);
  o.ids = {kStandardOnlyId};
  EXPECT_EQ(times_of(run_ok(o)),
            (std::vector<std::string>{"2026-11-17T18:00:00", "2026-11-17T18:05:00",
                                      "2026-11-17T18:10:00"}));
  o.duration_ns = 599'999'999'999;
  EXPECT_EQ(times_of(run_ok(o)),
            (std::vector<std::string>{"2026-11-17T18:00:00", "2026-11-17T18:05:00"}));
  o.duration_ns = 0;
  EXPECT_EQ(times_of(run_ok(o)), (std::vector<std::string>{"2026-11-17T18:00:00"}));
}

// 0.3 s / 0.1 s is 2.9999999999999996 in doubles; in integer nanoseconds the end is a step,
// and every time is written exactly with one decimal.
TEST(PropagateRunTest, DecimalStepsAreExact) {
  Options o = options("2026-11-17T18:00:00", 0, 1);
  o.ids = {kStandardOnlyId};
  o.duration_ns = 300'000'000;
  o.step_ns = 100'000'000;
  EXPECT_EQ(times_of(run_ok(o)),
            (std::vector<std::string>{"2026-11-17T18:00:00.0", "2026-11-17T18:00:00.1",
                                      "2026-11-17T18:00:00.2", "2026-11-17T18:00:00.3"}));
}

TEST(PropagateRunTest, TimeOfStepIsExactOverLongRuns) {
  const UtcTime start = utc("2026-11-17T18:00:00.123456789");
  // 1e5 steps of 0.1 s plus a large index: no drift, unlike repeated addition.
  EXPECT_EQ(satsim::app::time_of_step(start, 100'000'000, 100'000), start.plus_seconds(10'000.0));
  EXPECT_EQ(satsim::app::time_of_step(start, 1, 999'999'999'999'999'999),
            utc("2058-07-26T19:46:40.123456788"));
}

TEST(PropagateRunTest, TimesOutsideTheEarthOrientationTableFailBeforeAnyOutput) {
  // The table's last row is 2026-12-31 0h; that instant itself is inside.
  EXPECT_EQ(times_of(run_ok(options("2026-12-31T00:00:00", 0, 60))),
            std::vector<std::string>{"2026-12-31T00:00:00"});
  // The end one step past it.
  std::string message = run_fails(options("2026-12-30T23:59:00", 120, 60));
  EXPECT_NE(message.find("no Earth-orientation values for 2026-12-31T00:01:00"), std::string::npos)
      << message;
  EXPECT_NE(message.find("2026-10-01T00:00:00 to 2026-12-31T00:00:00"), std::string::npos)
      << message;
  // Before the first row.
  message = run_fails(options("2026-09-30T23:59:59", 10, 1));
  EXPECT_NE(message.find("2026-09-30T23:59:59"), std::string::npos) << message;
}

TEST(PropagateRunTest, OptionsBuiltByHandAreCheckedToo) {
  EXPECT_EQ(run_fails(options("2026-11-17T18:00:00", 60, 0)),
            "step must be positive and duration in [0, 1e9] s");
  EXPECT_EQ(run_fails(options("2026-11-17T18:00:00", -60, 60)),
            "step must be positive and duration in [0, 1e9] s");
}

TEST(PropagateRunTest, MissingOrBadSnapshotFailsWithAMessage) {
  const ScratchDir dir;
  std::string message = run_fails(options("2026-11-17T18:00:00", 0, 60, dir.path() / "nope"));
  EXPECT_NE(message.find("does not exist"), std::string::npos) << message;

  write_file(dir.path() / "file", "x");
  message = run_fails(options("2026-11-17T18:00:00", 0, 60, dir.path() / "file"));
  EXPECT_NE(message.find("is not a directory"), std::string::npos) << message;

  make_snapshot(dir.path(), "[]", "[]");
  std::filesystem::remove(dir.path() / "supgp.json");
  message = run_fails(options("2026-11-17T18:00:00", 0, 60, dir.path()));
  EXPECT_NE(message.find("has no supgp.json"), std::string::npos) << message;

  make_snapshot(dir.path(), "[{\"NORAD_CAT_ID\": ", "[]");
  message = run_fails(options("2026-11-17T18:00:00", 0, 60, dir.path()));
  EXPECT_NE(message.find("gp.json: not valid JSON"), std::string::npos) << message;

  make_snapshot(dir.path(), "[]", "{}");
  message = run_fails(options("2026-11-17T18:00:00", 0, 60, dir.path()));
  EXPECT_NE(message.find("supgp.json"), std::string::npos) << message;

  make_snapshot(dir.path(), "[]", "[]");
  write_file(dir.path() / "finals2000A.all", "garbage\n");
  message = run_fails(options("2026-11-17T18:00:00", 0, 60, dir.path()));
  EXPECT_NE(message.find("finals2000A.all"), std::string::npos) << message;
}

TEST(PropagateRunTest, RecordErrorsAreCountedAndTheRunGoesOn) {
  const ScratchDir dir;
  make_snapshot(dir.path(), "[" + healthy_record() + R"(, {"NORAD_CAT_ID": 5}])", "[]");
  RunOutcome outcome;
  const Records records = run_ok(options("2026-11-17T18:00:00", 0, 60, dir.path()), &outcome);
  EXPECT_EQ(records.size(), 2U);
  EXPECT_EQ(outcome.summary.standard_file.records_loaded, 1U);
  ASSERT_EQ(outcome.summary.standard_file.record_errors.size(), 1U);
  EXPECT_EQ(outcome.summary.standard_file.record_errors[0].index, 1U);
  EXPECT_TRUE(outcome.summary.supplemental_file.record_errors.empty());
}

TEST(PropagateRunTest, SatelliteThatDecaysHasNoRowsAfterwardsAndIsCounted) {
  const ScratchDir dir;
  make_snapshot(dir.path(), "[" + healthy_record() + "," + decaying_record() + "]", "[]");
  // Epoch 18:00; times 18:30 to 19:00 every 5 minutes, so the decay falls inside.
  const Options o = options("2026-11-17T18:30:00", 1800, 300, dir.path());
  RunOutcome outcome;
  const Records records = run_ok(o, &outcome);

  // Expected directly from the propagator.
  const auto loaded = satsim::orbit::load_omm_json(dir.path() / "gp.json");
  ASSERT_EQ(loaded.element_sets.size(), 2U);
  ASSERT_EQ(loaded.element_sets[1].norad_cat_id, 28872);
  const auto built = Propagator::create(loaded.element_sets[1]);
  ASSERT_TRUE(std::holds_alternative<Propagator>(built));
  std::vector<std::string> expected_times;
  std::size_t expected_failures = 0;
  for (std::int64_t i = 0; i < 7; ++i) {
    const UtcTime t = satsim::app::time_of_step(o.start, o.step_ns, i);
    if (std::holds_alternative<TemeState>(std::get<Propagator>(built).propagate(t))) {
      expected_times.push_back(satsim::app::format_utc_iso(t, 0));
    } else {
      ++expected_failures;
    }
  }
  // The case must really exercise both sides.
  ASSERT_GT(expected_failures, 0U);
  ASSERT_FALSE(expected_times.empty());

  std::vector<std::string> decaying_times;
  std::size_t healthy_rows = 0;
  for (std::size_t r = 1; r < records.size(); ++r) {
    if (records[r][column("norad_cat_id")] == "28872") {
      decaying_times.push_back(records[r][column("utc")]);
    } else {
      ++healthy_rows;
    }
  }
  EXPECT_EQ(decaying_times, expected_times);
  EXPECT_EQ(healthy_rows, 7U);
  EXPECT_EQ(outcome.summary.rows_written, 7 + expected_times.size());
  EXPECT_EQ(
      outcome.summary.propagation_failures,
      (std::map<PropagationError, std::size_t>{{PropagationError::kDecayed, expected_failures}}));
  EXPECT_EQ(outcome.summary.satellites_with_propagation_failures, 1U);
}

TEST(PropagateRunTest, SatelliteWhosePropagatorCannotBeBuiltIsCountedAndAbsent) {
  // Perigee 0.1 * 6780 km from the Earth's centre: SGP4 rejects it at epoch.
  const std::string impossible =
      omm_record(R"("IMPOSSIBLE")", 99, "2026-11-17T18:00:00", 15.0, 0.9, 53.0, 0.0, 0.0001);
  const ScratchDir dir;
  make_snapshot(dir.path(), "[" + impossible + "," + healthy_record() + "]", "[]");
  const auto loaded = satsim::orbit::load_omm_json(dir.path() / "gp.json");
  ASSERT_EQ(loaded.element_sets.size(), 2U);
  const auto built = Propagator::create(loaded.element_sets[0]);
  ASSERT_TRUE(std::holds_alternative<PropagationError>(built));

  RunOutcome outcome;
  const Records records = run_ok(options("2026-11-17T18:00:00", 60, 60, dir.path()), &outcome);
  ASSERT_EQ(records.size(), 1 + 2U);
  EXPECT_EQ(records[1][column("norad_cat_id")], "44753");
  EXPECT_EQ(records[2][column("norad_cat_id")], "44753");
  EXPECT_EQ(outcome.summary.build_failures,
            (std::map<PropagationError, std::size_t>{{std::get<PropagationError>(built), 1}}));
  EXPECT_EQ(outcome.summary.selected_standard, 2U);
  EXPECT_EQ(outcome.summary.standard_age_at_start_s.size(), 1U);
}

TEST(PropagateRunTest, NamesWithCommasAndQuotesAreQuoted) {
  const ScratchDir dir;
  make_snapshot(dir.path(), "[" + healthy_record(R"("STAR,LINK \"X\"")") + "]", "[]");
  std::ostringstream csv;
  const RunOutcome outcome =
      satsim::propagate::run(options("2026-11-17T18:00:00", 0, 60, dir.path()), csv);
  ASSERT_FALSE(outcome.error.has_value());
  EXPECT_NE(csv.str().find(R"(,44753,"STAR,LINK ""X""",standard,)"), std::string::npos)
      << csv.str();
  const Records records = parse_csv(csv.str());
  ASSERT_EQ(records.size(), 2U);
  ASSERT_EQ(records[1].size(), kCsvColumns.size());
  EXPECT_EQ(records[1][column("object_name")], "STAR,LINK \"X\"");
}

TEST(PropagateRunTest, SummaryReportsTheCounts) {
  const ScratchDir dir;
  make_snapshot(dir.path(),
                "[" + healthy_record() + "," + decaying_record() + R"(, {"NORAD_CAT_ID": 5}])",
                "[" + healthy_record(R"("SUP")", 799501782) + "]");
  Options o = options("2026-11-17T18:30:00", 1800, 300, dir.path());
  o.ids = {28872, 44753, 799501782, 7};
  RunOutcome outcome;
  (void)run_ok(o, &outcome);
  std::ostringstream text;
  satsim::propagate::write_summary(outcome.summary, text);
  const std::string summary = text.str();
  for (const std::string_view expected : {
           "times: 7, from 2026-11-17T18:30:00 to 2026-11-17T19:00:00 UTC every 300 s",
           "table covers 2026-10-01 to 2026-12-31",
           "gp.json: 2 records loaded, 1 record errors",
           "    record 2 (5): EPOCH missing; ",
           "supgp.json: 1 records loaded, 0 record errors",
           "satellites selected: 3 (supplemental 1, standard 2)",
           "requested ids not in the snapshot: 1 (7)",
           "propagators that failed to build: 0",
           "    SGP4 error 6: satellite has decayed: 2\n",
           "element age at start, supplemental: 1 satellites",
           "element age at start, standard: 2 satellites, min 0.50 h, median 3.36 h, max 6.22 h",
           "geodetic height at start: 3 satellites",
           "km height band at start:",
       }) {
    EXPECT_NE(summary.find(expected), std::string::npos) << expected << "\n" << summary;
  }
  const std::string rows = "rows written: " + std::to_string(outcome.summary.rows_written);
  EXPECT_NE(summary.find(rows), std::string::npos) << summary;
}

}  // namespace
