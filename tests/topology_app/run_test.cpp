// The topology program as a library, on a synthetic snapshot (an ideal Walker shell in the
// default 53deg shell) and the small ground fixture: every CSV value against an independent
// computation through the library modules, determinism, row order, statuses, assumed
// gateways, fatal input errors, and exit status 1 when a check fails (through the test
// seams, without weakening any check).

#include "topology/run.hpp"

#include <gtest/gtest.h>

#include <Eigen/Core>
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <limits>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "common/csv.hpp"
#include "common/steps.hpp"
#include "fixture.hpp"
#include "satsim/constellation/classification.hpp"
#include "satsim/core/physical_constants.hpp"
#include "satsim/ground/distance.hpp"
#include "satsim/ground/home_pop.hpp"
#include "satsim/ground/loaders.hpp"
#include "satsim/network/graph.hpp"
#include "satsim/network/shortest_path.hpp"
#include "satsim/orbit/snapshot.hpp"
#include "satsim/time/earth_orientation.hpp"
#include "satsim/topology/ground_links.hpp"
#include "satsim/topology/link_plan.hpp"
#include "satsim/topology/link_state.hpp"
#include "satsim/topology/positions.hpp"
#include "support/synthetic.hpp"
#include "test_support.hpp"
#include "topology/arguments.hpp"
#include "topology/endpoints.hpp"

namespace {

namespace fs = std::filesystem;
namespace app = satsim::topology_app;
namespace fixture = satsim::topology_app::testing;
namespace topology = satsim::topology;
namespace ground = satsim::ground;
using satsim::propagate::testing::number;
using satsim::propagate::testing::parse_csv;
using satsim::propagate::testing::read_file;
using satsim::propagate::testing::ScratchDir;
using Rows = std::vector<std::vector<std::string>>;

constexpr double kC = satsim::kSpeedOfLightVacuumKmPerS;

std::size_t col(std::string_view name) {
  const auto* const it = std::find(app::kCsvColumns.begin(), app::kCsvColumns.end(), name);
  EXPECT_NE(it, app::kCsvColumns.end()) << name;
  return static_cast<std::size_t>(it - app::kCsvColumns.begin());
}

// A scratch directory holding the synthetic snapshot.
struct Snapshot {
  ScratchDir dir;
  Snapshot() {
    const auto sets = satsim::constellation::testing::walker_element_sets(fixture::fixture_shell());
    fixture::write_snapshot(dir.path(), sets);
  }
};

// A new directory inside the snapshot's scratch directory (two ScratchDirs in one test would
// share a name).
fs::path subdirectory(const Snapshot& snapshot, std::string_view name) {
  const fs::path dir = snapshot.dir.path() / name;
  fs::create_directories(dir);
  return dir;
}

app::PreparedRun prepare(const app::Options& options) {
  app::PreparedRunOrError prepared = app::prepare_run(options);
  if (const auto* error = std::get_if<std::string>(&prepared)) {
    ADD_FAILURE() << *error;
    std::abort();
  }
  return std::move(std::get<app::PreparedRun>(prepared));
}

struct Written {
  std::string csv;
  app::RunOutcome outcome;
};

Written write(const app::PreparedRun& run, const app::RunHooks& hooks = {}) {
  std::ostringstream csv;
  Written w{.csv = {}, .outcome = app::write_rows(run, csv, hooks)};
  w.csv = csv.str();
  EXPECT_FALSE(w.outcome.error.has_value()) << w.outcome.error.value_or("");
  return w;
}

// Everything the expected values are computed from, built here through the library modules
// directly (not through the program's library).
struct Reference {
  satsim::constellation::Classification classification;
  std::vector<std::optional<satsim::orbit::Propagator>> propagators;
  topology::LinkPlan plan;
  satsim::time::EarthOrientationTable eop;
  std::vector<ground::City> cities;
  std::vector<ground::CityPair> pairs;
  std::vector<ground::Gateway> gateways;  // Operational.
  std::vector<std::string> gateway_sources;
  std::vector<ground::HomePop> homes;
};

Reference reference(const app::Options& o, const fs::path& ground_dir) {
  auto loaded = satsim::orbit::load_snapshot_elements(o.snapshot_dir);
  auto& elements = std::get<satsim::orbit::SnapshotElements>(loaded);
  std::vector<satsim::orbit::ElementSet> sets;
  sets.reserve(elements.selection.selected.size());
  for (const auto& s : elements.selection.selected) {
    sets.push_back(s.elements);
  }
  auto classified = satsim::constellation::classify_constellation(
      sets, o.start, satsim::constellation::default_config());
  auto eop = satsim::time::load_earth_orientation(o.snapshot_dir / "finals2000A.all");
  Reference r{
      .classification = std::move(std::get<satsim::constellation::Classification>(classified)),
      .propagators = {},
      .plan = {},
      .eop = std::move(*eop.table),
      .cities = {},
      .pairs = {},
      .gateways = {},
      .gateway_sources = {},
      .homes = {}};
  r.propagators = topology::plane_member_propagators(r.classification, sets);
  const auto at_start = topology::positions_at(r.propagators, o.start, *r.eop.at(o.start));
  r.plan = topology::plan_laser_links(r.classification, at_start.position_ecef_km,
                                      {.pattern = o.pattern});
  auto cities = ground::load_cities(o.cities_file);
  r.cities = cities.cities;
  r.pairs = cities.pairs;
  for (const auto& [file, source] :
       {std::pair{"gateways.json", "real"}, std::pair{"assumed_gateways.json", "assumed"}}) {
    if (!fs::exists(ground_dir / file)) {
      continue;
    }
    for (const auto& g :
         ground::select_gateways(ground::load_gateways(ground_dir / file).records)) {
      r.gateways.push_back(g);
      r.gateway_sources.emplace_back(source);
    }
  }
  const ground::HomePopFinder finder(
      ground::load_starlink_geofeed(ground_dir / "starlink_geofeed.csv").records,
      ground::load_starlink_pops(ground_dir / "starlink_pops.csv").records,
      ground::load_pop_codes(ground_dir / "pop_codes.json").records);
  for (const auto& c : r.cities) {
    r.homes.push_back(finder.find(c));
  }
  return r;
}

std::size_t city_index(const Reference& r, const std::string& name) {
  for (std::size_t i = 0; i < r.cities.size(); ++i) {
    if (r.cities[i].site.name == name) {
      return i;
    }
  }
  ADD_FAILURE() << "no city " << name;
  return 0;
}

struct ExpectedRow {
  std::string status;
  double one_way_s = 0.0;
  std::int64_t first = 0;  // Best case first satellite, or realistic serving satellite.
  std::int64_t last = 0;   // Best case last satellite, or realistic gateway satellite.
  std::string gateway;
};

// Checks every row of `csv` against the reference, step by step.
void check_against_reference(const std::string& csv, const app::Options& o,
                             const fs::path& ground_dir, std::size_t& ok_rows) {
  const Reference r = reference(o, ground_dir);
  const Rows rows = parse_csv(csv);
  ASSERT_FALSE(rows.empty());
  ASSERT_EQ(rows[0], std::vector<std::string>(app::kCsvColumns.begin(), app::kCsvColumns.end()));
  const std::int64_t steps = satsim::app::step_count(o.duration_ns, o.step_ns);
  ASSERT_EQ(rows.size(), 1 + static_cast<std::size_t>(steps) * 2 * r.pairs.size());

  std::vector<ground::GroundSite> sites;
  sites.reserve(r.cities.size() + r.gateways.size());
  for (const auto& c : r.cities) {
    sites.push_back(c.site);
  }
  for (const auto& g : r.gateways) {
    sites.push_back(g.site);
  }
  const topology::GroundVisibility visibility(sites, r.classification);
  topology::LinkStateEvaluator links_at(r.plan, {});
  const bool best_case = o.model == app::EndpointModel::kBestCase;
  const auto id = [&](std::size_t s) { return r.classification.satellites[s].norad_cat_id; };

  std::size_t row = 1;
  for (std::int64_t i = 0; i < steps; ++i) {
    const auto utc = satsim::app::time_of_step(o.start, o.step_ns, i);
    const auto positions = topology::positions_at(r.propagators, utc, *r.eop.at(utc));
    const auto links = links_at.step(positions);
    const auto visible = visibility.at(positions);
    // The laser graph alone.
    satsim::network::Graph lasers(r.classification.satellites.size());
    for (std::size_t k = 0; k < r.plan.links.size(); ++k) {
      if (links.states[k].up()) {
        ASSERT_FALSE(lasers
                         .add_undirected_edge(r.plan.links[k].satellite_a,
                                              r.plan.links[k].satellite_b,
                                              *links.states[k].length_km / kC)
                         .has_value());
      }
    }
    for (const auto& pair : r.pairs) {
      std::vector<ExpectedRow> both;
      for (const auto& [from_name, to_name] :
           {std::pair{pair.from, pair.to}, std::pair{pair.to, pair.from}}) {
        const std::size_t a = city_index(r, from_name);
        const std::size_t b = city_index(r, to_name);
        ExpectedRow e;
        if (best_case) {
          // The minimum over every (first, last) pair of visible satellites.
          e.status = visible[a].empty() || visible[b].empty() ? "no_coverage" : "no_path";
          double best = std::numeric_limits<double>::infinity();
          for (const auto& first : visible[a]) {
            const auto paths = satsim::network::shortest_paths(lasers, first.satellite);
            for (const auto& last : visible[b]) {
              const double d = first.slant_range_km / kC + paths.delay_s[last.satellite] +
                               last.slant_range_km / kC;
              if (d < best) {
                best = d;
                e = {.status = "ok",
                     .one_way_s = d,
                     .first = id(first.satellite),
                     .last = id(last.satellite),
                     .gateway = ""};
              }
            }
          }
        } else if (!r.homes[a].pop) {
          e.status = "no_service";
        } else if (const auto serving = topology::pick_serving_satellite(visible[a]); !serving) {
          e.status = "no_coverage";
        } else {
          e.status = "no_gateway";
          e.first = id(serving->satellite);
          const auto paths = satsim::network::shortest_paths(lasers, serving->satellite);
          const auto& pop = r.homes[a].pop->site;
          double best = std::numeric_limits<double>::infinity();
          for (std::size_t g = 0; g < r.gateways.size(); ++g) {
            for (const auto& v : visible[r.cities.size() + g]) {
              const double d =
                  paths.delay_s[v.satellite] + v.slant_range_km / kC +
                  ground::fibre_delay_s(
                      ground::great_circle_distance_on_mean_sphere_km(r.gateways[g].site, pop),
                      o.fibre_factor);
              if (d < best) {
                best = d;
                e.status = "ok";
                e.last = id(v.satellite);
                e.gateway = r.gateways[g].site.name;
              }
            }
          }
          if (e.status == "ok") {
            e.one_way_s = serving->slant_range_km / kC + best +
                          ground::fibre_delay_s(ground::great_circle_distance_on_mean_sphere_km(
                                                    pop, r.cities[b].site),
                                                o.fibre_factor);
          }
        }
        both.push_back(e);
      }
      for (std::size_t d = 0; d < 2; ++d, ++row) {
        const auto& got = rows.at(row);
        const ExpectedRow& e = both[d];
        SCOPED_TRACE("row " + std::to_string(row));
        ASSERT_EQ(got.size(), app::kCsvColumns.size());
        EXPECT_EQ(got[col("utc")], satsim::app::format_utc_iso(utc, 0));
        EXPECT_EQ(got[col("from")], d == 0 ? pair.from : pair.to);
        EXPECT_EQ(got[col("to")], d == 0 ? pair.to : pair.from);
        EXPECT_EQ(got[col("pattern")], topology::name_of(o.pattern));
        EXPECT_EQ(got[col("model")], app::name_of(o.model));
        EXPECT_EQ(got[col("status")], e.status);
        const std::size_t from = city_index(r, got[col("from")]);
        const std::size_t to = city_index(r, got[col("to")]);
        const double gc =
            ground::great_circle_distance_on_mean_sphere_km(r.cities[from].site, r.cities[to].site);
        EXPECT_EQ(number(got[col("great_circle_km")]), gc);
        EXPECT_EQ(number(got[col("lower_bound_ms")]), gc / kC * 1000.0);
        if (!best_case) {
          EXPECT_EQ(got[col("home_pop")], r.homes[from].pop ? r.homes[from].pop->code : "");
          EXPECT_EQ(got[col("home_pop_level")], app::name_of(r.homes[from].level));
        } else {
          EXPECT_EQ(got[col("home_pop")], "");
          EXPECT_EQ(got[col("home_pop_level")], "");
        }
        if (e.status == "no_gateway") {
          EXPECT_EQ(number(got[col("serving_norad_cat_id")]), static_cast<double>(e.first));
        }
        if (e.status != "ok") {
          for (const auto* empty :
               {"one_way_ms", "round_trip_ms", "laser_hops", "path_km", "uplink_km", "laser_km",
                "downlink_km", "fibre_km", "stretch", "gateway_norad_cat_id", "gateway",
                "gateway_source", "first_norad_cat_id", "last_norad_cat_id"}) {
            EXPECT_EQ(got[col(empty)], "") << empty;
          }
          continue;
        }
        ++ok_rows;
        const double one_way_ms = number(got[col("one_way_ms")]);
        EXPECT_NEAR(one_way_ms, e.one_way_s * 1000.0, 1e-9);
        const double up = number(got[col("uplink_km")]);
        const double laser = number(got[col("laser_km")]);
        const double down = number(got[col("downlink_km")]);
        const double fibre = number(got[col("fibre_km")]);
        EXPECT_DOUBLE_EQ(number(got[col("path_km")]), up + laser + down + fibre);
        EXPECT_NEAR(one_way_ms, ((up + laser + down) + fibre * o.fibre_factor) / kC * 1000.0, 1e-9);
        EXPECT_DOUBLE_EQ(number(got[col("stretch")]), one_way_ms / (gc / kC * 1000.0));
        EXPECT_GE(number(got[col("stretch")]), 1.0);
        if (best_case) {
          EXPECT_EQ(fibre, 0.0);
          EXPECT_EQ(number(got[col("first_norad_cat_id")]), static_cast<double>(e.first));
          EXPECT_EQ(number(got[col("last_norad_cat_id")]), static_cast<double>(e.last));
          EXPECT_DOUBLE_EQ(number(got[col("round_trip_ms")]), 2.0 * one_way_ms);
          EXPECT_EQ(got[col("gateway")], "");
        } else {
          EXPECT_EQ(number(got[col("serving_norad_cat_id")]), static_cast<double>(e.first));
          EXPECT_EQ(number(got[col("gateway_norad_cat_id")]), static_cast<double>(e.last));
          EXPECT_EQ(got[col("gateway")], e.gateway);
          EXPECT_EQ(got[col("first_norad_cat_id")], "");
          const ExpectedRow& other = both[1 - d];
          if (other.status == "ok") {
            EXPECT_NEAR(number(got[col("round_trip_ms")]), (e.one_way_s + other.one_way_s) * 1000.0,
                        1e-9);
          } else {
            EXPECT_EQ(got[col("round_trip_ms")], "");
          }
        }
      }
    }
  }
}

class TopologyRunTest : public ::testing::TestWithParam<app::EndpointModel> {};

TEST_P(TopologyRunTest, RowsMatchTheLibraryAndAreDeterministic) {
  const Snapshot snapshot;
  const app::Options o = fixture::fixture_options(snapshot.dir.path(), 360, 120,
                                                  topology::LaserPattern::kThree, GetParam());
  const app::PreparedRun run = prepare(o);
  const Written first = write(run);
  // Writing the same prepared run again, and preparing it again, give the same bytes.
  EXPECT_EQ(write(run).csv, first.csv);
  EXPECT_EQ(write(prepare(o)).csv, first.csv);
  EXPECT_TRUE(first.outcome.statistics.passed());
  EXPECT_EQ(first.outcome.statistics.steps, 4);
  EXPECT_EQ(first.outcome.statistics.rows_written, 24U);

  std::size_t ok_rows = 0;
  check_against_reference(first.csv, o, fixture::kGroundDir, ok_rows);
  EXPECT_GT(ok_rows, 10U);  // The fixture is placed so that most rows have a path.

  // The summary's per-direction counts agree with the rows.
  const auto& stats = first.outcome.statistics;
  ASSERT_EQ(stats.directions.size(), 6U);
  std::size_t ok_counted = 0;
  for (const auto& d : stats.directions) {
    ok_counted += d.steps_by_status[0];
    EXPECT_EQ(d.one_way_ms.size(), d.steps_by_status[0]);
  }
  EXPECT_EQ(ok_counted, ok_rows);
  std::ostringstream summary;
  app::write_summary(run, stats, summary);
  EXPECT_NE(summary.str().find("checks: PASSED"), std::string::npos) << summary.str();
  EXPECT_NE(summary.str().find("shell 53deg: 160 satellites in 10 planes"), std::string::npos)
      << summary.str();
}

INSTANTIATE_TEST_SUITE_P(BothModels, TopologyRunTest,
                         ::testing::Values(app::EndpointModel::kRealistic,
                                           app::EndpointModel::kBestCase),
                         [](const ::testing::TestParamInfo<app::EndpointModel>& param_info) {
                           return param_info.param == app::EndpointModel::kRealistic ? "Realistic"
                                                                                     : "BestCase";
                         });

TEST(TopologyRunTest, GammaHasNoServiceAndItsFieldsAreEmpty) {
  const Snapshot snapshot;
  const app::Options o = fixture::fixture_options(
      snapshot.dir.path(), 0, 60, topology::LaserPattern::kThree, app::EndpointModel::kRealistic);
  const Rows rows = parse_csv(write(prepare(o)).csv);
  bool found = false;
  for (const auto& row : rows) {
    if (row[col("from")] == "Gamma") {
      found = true;
      EXPECT_EQ(row[col("status")], "no_service");
      EXPECT_EQ(row[col("home_pop")], "");
      EXPECT_EQ(row[col("home_pop_level")], "none");
      EXPECT_EQ(row[col("serving_norad_cat_id")], "");
      EXPECT_EQ(row[col("one_way_ms")], "");
    }
  }
  EXPECT_TRUE(found);
}

TEST(TopologyRunTest, EveryPatternPassesTheChecks) {
  const Snapshot snapshot;
  for (const auto pattern : topology::kAllLaserPatterns) {
    // Prepared once per pattern: the model only matters when writing.
    app::PreparedRun run = prepare(fixture::fixture_options(snapshot.dir.path(), 120, 120, pattern,
                                                            app::EndpointModel::kRealistic));
    EXPECT_EQ(run.plan.pattern, pattern);
    for (const auto model : {app::EndpointModel::kRealistic, app::EndpointModel::kBestCase}) {
      run.options.model = model;
      const Written w = write(run);
      EXPECT_TRUE(w.outcome.statistics.passed()) << topology::name_of(pattern);
      EXPECT_GT(w.outcome.statistics.link_steps_up, 0U);
    }
  }
}

TEST(TopologyRunTest, AssumedGatewaysAreLoadedAndReported) {
  const Snapshot snapshot;
  const fs::path ground_dir = subdirectory(snapshot, "ground");
  fixture::copy_ground(ground_dir);
  fs::copy_file(fixture::kAssumedGateways, ground_dir / "assumed_gateways.json");
  app::Options o = fixture::fixture_options(
      snapshot.dir.path(), 600, 60, topology::LaserPattern::kThree, app::EndpointModel::kRealistic);
  o.ground_dir = ground_dir;
  const app::PreparedRun run = prepare(o);
  ASSERT_EQ(run.gateways.size(), 3U);
  EXPECT_EQ(run.gateways[0].source, app::GatewaySource::kReal);
  EXPECT_EQ(run.gateways[1].source, app::GatewaySource::kReal);
  EXPECT_EQ(run.gateways[2].source, app::GatewaySource::kAssumed);
  EXPECT_EQ(run.gateways[2].site.name, "Assumed Delta");
  EXPECT_TRUE(run.ground.assumed_file);
  EXPECT_EQ(run.ground.assumed_gateways, 1U);
  EXPECT_EQ(run.ground.assumed_operational, 1U);
  EXPECT_EQ(run.ground.real_gateways, 3U);
  EXPECT_EQ(run.ground.real_operational, 2U);

  const Written w = write(run);
  std::size_t ok_rows = 0;
  check_against_reference(w.csv, o, ground_dir, ok_rows);
  // Delta's PoP is 15 km from the assumed gateway, which sees what Delta's dish sees: every
  // ok row from Delta goes down there, and is reported as assumed.
  std::size_t from_delta = 0;
  for (const auto& row : parse_csv(w.csv)) {
    if (row[col("from")] == "Delta" && row[col("status")] == "ok") {
      ++from_delta;
      EXPECT_EQ(row[col("gateway")], "Assumed Delta");
      EXPECT_EQ(row[col("gateway_source")], "assumed");
    }
    if (row[col("from")] == "Alpha" && row[col("status")] == "ok") {
      EXPECT_EQ(row[col("gateway_source")], "real");
    }
  }
  EXPECT_GT(from_delta, 0U);
  std::ostringstream summary;
  app::write_summary(run, w.outcome.statistics, summary);
  EXPECT_NE(summary.str().find("assumed 1 (1 operational)"), std::string::npos) << summary.str();
  EXPECT_NE(summary.str().find("Assumed Delta (assumed)"), std::string::npos) << summary.str();

  // Without the file, the summary says so and the gateways are the real ones only.
  o.ground_dir = fixture::kGroundDir;
  const app::PreparedRun without = prepare(o);
  EXPECT_EQ(without.gateways.size(), 2U);
  EXPECT_FALSE(without.ground.assumed_file);
}

TEST(TopologyRunTest, ShellsAndRolesFilter) {
  const Snapshot snapshot;
  app::Options o = fixture::fixture_options(
      snapshot.dir.path(), 0, 60, topology::LaserPattern::kThree, app::EndpointModel::kBestCase);
  o.shells = {"43deg"};  // The fixture's satellites are all in 53deg.
  o.roles = app::RoleFilter::kValidation;
  const app::PreparedRun run = prepare(o);
  EXPECT_EQ(run.directions.size(), 2U);
  EXPECT_EQ(run.shell_used, (std::vector<bool>{false, true, false, false}));
  EXPECT_TRUE(run.plan.links.empty());
  const Written w = write(run);
  const Rows rows = parse_csv(w.csv);
  ASSERT_EQ(rows.size(), 3U);
  for (std::size_t i = 1; i < rows.size(); ++i) {
    EXPECT_EQ(rows[i][col("status")], "no_coverage");
    EXPECT_EQ(rows[i][col("role")], "validation");
  }
  std::ostringstream summary;
  app::write_summary(run, w.outcome.statistics, summary);
  EXPECT_NE(summary.str().find("(not used in this run)"), std::string::npos);
  EXPECT_NE(summary.str().find("satellites carrying traffic: 0"), std::string::npos);

  o.shells.clear();
  o.roles = app::RoleFilter::kStudy;
  EXPECT_EQ(prepare(o).directions.size(), 4U);
}

// --shells-file replaces the built-in shells: the fixture's satellites go to the file's one
// shell under its own name, --shells names are checked against the file, and a file that
// cannot be used fails the run before anything else.
TEST(TopologyRunTest, ShellsFileReplacesTheBuiltInShells) {
  const Snapshot snapshot;
  const fs::path dir = subdirectory(snapshot, "shells");
  {
    std::ofstream file(dir / "shells.json");
    file << R"({"shells": [{"name": "fixture", "inclination_deg": 53.16,
        "inclination_tolerance_deg": 0.3, "min_height_km": 460.0, "max_height_km": 480.0,
        "plane_gap_threshold_deg": 5.0, "min_plane_size": 10}]})";
  }
  app::Options o = fixture::fixture_options(
      snapshot.dir.path(), 0, 60, topology::LaserPattern::kPlusGrid, app::EndpointModel::kBestCase);
  o.shells_file = dir / "shells.json";
  const app::PreparedRun run = prepare(o);
  ASSERT_EQ(run.options.classification.shells.size(), 1U);
  EXPECT_EQ(run.options.classification.shells[0].name, "fixture");
  EXPECT_TRUE(run.options.classification.excluded_groups.empty());
  // The same planes as with the built-in 53deg shell (fixture_shell: 10 planes of 16).
  ASSERT_EQ(run.classification.planes.size(), 1U);
  EXPECT_EQ(run.classification.planes[0].size(), 10U);
  for (const auto& plane : run.classification.planes[0]) {
    EXPECT_EQ(plane.members.size(), 16U);
  }
  const Written w = write(run);
  EXPECT_TRUE(w.outcome.statistics.passed());
  std::ostringstream summary;
  app::write_summary(run, w.outcome.statistics, summary);
  EXPECT_NE(summary.str().find("shells file " + (dir / "shells.json").string()), std::string::npos)
      << summary.str();
  EXPECT_NE(summary.str().find("shell fixture: 160 satellites in 10 planes"), std::string::npos)
      << summary.str();

  const auto error_of = [](const app::Options& options) {
    app::PreparedRunOrError prepared = app::prepare_run(options);
    EXPECT_TRUE(std::holds_alternative<std::string>(prepared));
    return std::holds_alternative<std::string>(prepared) ? std::get<std::string>(prepared)
                                                         : std::string();
  };
  app::Options named = o;
  named.shells = {"53deg"};  // A built-in name, not in the file.
  EXPECT_EQ(error_of(named), "no shell named 53deg");
  named.shells = {"fixture"};
  EXPECT_EQ(prepare(named).shell_used, std::vector<bool>{true});

  app::Options missing = o;
  missing.shells_file = dir / "missing.json";
  EXPECT_EQ(error_of(missing), "--shells-file cannot open " + (dir / "missing.json").string());
  {
    std::ofstream file(dir / "bad.json");
    file << R"({"shells": [{"name": "fixture"}]})";
  }
  app::Options bad = o;
  bad.shells_file = dir / "bad.json";
  const std::string bad_error = error_of(bad);
  EXPECT_NE(bad_error.find("bad.json: shells[0]: missing"), std::string::npos) << bad_error;
}

TEST(TopologyRunTest, InputErrorsFailBeforeAnything) {
  const Snapshot snapshot;
  const app::Options good = fixture::fixture_options(
      snapshot.dir.path(), 60, 60, topology::LaserPattern::kThree, app::EndpointModel::kRealistic);
  const auto error_of = [](const app::Options& o) {
    app::PreparedRunOrError prepared = app::prepare_run(o);
    EXPECT_TRUE(std::holds_alternative<std::string>(prepared));
    return std::holds_alternative<std::string>(prepared) ? std::get<std::string>(prepared)
                                                         : std::string();
  };
  {
    app::Options o = good;
    o.snapshot_dir = snapshot.dir.path() / "missing";
    EXPECT_NE(error_of(o).find("missing"), std::string::npos);
  }
  {
    app::Options o = good;
    o.cities_file = fixture::kData / "no_such_cities.json";
    EXPECT_NE(error_of(o).find("no_such_cities.json: no such file"), std::string::npos);
  }
  {
    // A start outside the Earth-orientation file (it ends 2026-12-31): the last step is
    // checked first.
    app::Options o = good;
    o.start = satsim::constellation::testing::utc({2027, 3, 1, 0, 0, 0.0});
    EXPECT_NE(error_of(o).find("no Earth-orientation values for 2027-03-01T00:01:00"),
              std::string::npos);
  }
  {
    app::Options o = good;
    o.fibre_factor = 0.5;
    EXPECT_NE(error_of(o).find("fibre factor"), std::string::npos);
    o = good;
    o.limits.range_cap_km = -1.0;
    EXPECT_NE(error_of(o).find("limits:"), std::string::npos);
    o = good;
    o.shells = {"60deg"};
    EXPECT_EQ(error_of(o), "no shell named 60deg");
    o = good;
    o.step_ns = 0;
    EXPECT_NE(error_of(o).find("step"), std::string::npos);
  }
  {
    // A ground directory missing a file, and one with a bad gateway record.
    const fs::path ground_dir = subdirectory(snapshot, "ground");
    fixture::copy_ground(ground_dir);
    fs::remove(ground_dir / "pop_codes.json");
    app::Options o = good;
    o.ground_dir = ground_dir;
    EXPECT_NE(error_of(o).find("pop_codes.json"), std::string::npos);
    fixture::copy_ground(ground_dir);
    {
      std::ofstream bad(ground_dir / "assumed_gateways.json");
      bad << R"([{"name": "No status", "lat": 1, "lon": 1}])";
    }
    EXPECT_NE(error_of(o).find("assumed_gateways.json: record 0 (No status)"), std::string::npos);
  }
  {
    // A city file without validation pairs, asked for validation pairs.
    const fs::path dir = subdirectory(snapshot, "cities");
    {
      std::ofstream cities(dir / "cities.json");
      cities << R"({"cities": [
        {"name": "A", "country": "US", "region": "US-NY", "geofeed_city": "Alpha", "lat": 40, "lon": -75},
        {"name": "B", "country": "US", "region": "US-IL", "geofeed_city": "Beta", "lat": 42, "lon": -88}],
        "pairs": [{"from": "A", "to": "B", "role": "study"}]})";
    }
    app::Options o = good;
    o.cities_file = dir / "cities.json";
    o.roles = app::RoleFilter::kValidation;
    EXPECT_NE(error_of(o).find("no city pair with role validation"), std::string::npos);
  }
}

// The whole program through run_main, with the CSV in a file.
struct MainResult {
  int status = -1;
  std::string out;
  std::string err;
};

MainResult run_main(const std::vector<std::string>& args, const app::RunHooks& hooks = {}) {
  std::vector<std::string_view> views(args.begin(), args.end());
  std::ostringstream out;
  std::ostringstream err;
  MainResult r;
  r.status = app::run_main(views, out, err, hooks);
  r.out = out.str();
  r.err = err.str();
  return r;
}

std::vector<std::string> main_args(const fs::path& snapshot, const fs::path& out,
                                   std::string_view model = "realistic") {
  return {"--snapshot",   snapshot.string(),
          "--ground",     fixture::kGroundDir.string(),
          "--cities",     fixture::kCities.string(),
          "--start",      std::string(fixture::kStart),
          "--duration-s", "120",
          "--step-s",     "60",
          "--pattern",    "three",
          "--model",      std::string(model),
          "--out",        out.string()};
}

TEST(TopologyMainTest, WritesTheFileAndTheSummary) {
  const Snapshot snapshot;
  const fs::path out = snapshot.dir.path() / "out.csv";
  const MainResult r = run_main(main_args(snapshot.dir.path(), out));
  EXPECT_EQ(r.status, 0) << r.err;
  EXPECT_NE(r.out.find("topology summary"), std::string::npos);
  EXPECT_EQ(r.err, "");
  EXPECT_EQ(parse_csv(read_file(out)).size(), 1U + 3 * 6);

  // To standard output: the summary moves to err.
  std::vector<std::string> args = main_args(snapshot.dir.path(), "-");
  const MainResult s = run_main(args);
  EXPECT_EQ(s.status, 0) << s.err;
  EXPECT_EQ(s.out, read_file(out));
  EXPECT_NE(s.err.find("topology summary"), std::string::npos);
}

TEST(TopologyMainTest, InvalidArgumentsAndFatalErrors) {
  const Snapshot snapshot;
  const fs::path out = snapshot.dir.path() / "out.csv";
  std::vector<std::string> args = main_args(snapshot.dir.path(), out);
  args.emplace_back("--bogus");
  const MainResult invalid = run_main(args);
  EXPECT_EQ(invalid.status, 2);
  EXPECT_NE(invalid.err.find("unknown argument \"--bogus\""), std::string::npos);
  EXPECT_FALSE(fs::exists(out));

  const MainResult help = run_main({"--help"});
  EXPECT_EQ(help.status, 0);
  EXPECT_EQ(help.out, app::usage());

  const MainResult missing = run_main(main_args(snapshot.dir.path() / "nope", out));
  EXPECT_EQ(missing.status, 1);
  EXPECT_NE(missing.err.find("topology: "), std::string::npos);
  EXPECT_FALSE(fs::exists(out));  // Nothing is created before the inputs are checked.

  const MainResult unwritable =
      run_main(main_args(snapshot.dir.path(), snapshot.dir.path() / "no_dir" / "out.csv"));
  EXPECT_EQ(unwritable.status, 1);
  EXPECT_NE(unwritable.err.find("cannot open"), std::string::npos);
}

// A satellite moved to the Earth's centre after its links were found up: the line-of-sight
// check catches every one of its up links, and the program exits 1.
TEST(TopologyMainTest, AFailedPhysicalCheckExits1) {
  const Snapshot snapshot;
  const fs::path out = snapshot.dir.path() / "out.csv";
  app::RunHooks hooks;
  hooks.after_links = [](topology::StepPositions& positions, topology::StepLinks&) {
    for (auto& p : positions.position_ecef_km) {
      if (p) {
        p = Eigen::Vector3d::Zero();
        break;
      }
    }
  };
  const MainResult r = run_main(main_args(snapshot.dir.path(), out), hooks);
  EXPECT_EQ(r.status, 1);
  EXPECT_NE(r.err.find("physical checks failed"), std::string::npos) << r.err;
  EXPECT_NE(r.out.find("checks: FAILED"), std::string::npos);
  EXPECT_EQ(r.out.find("line_of_sight 0,"), std::string::npos) << r.out;
  EXPECT_TRUE(fs::exists(out));  // The rows are still written.
}

TEST(TopologyMainTest, ADelayBelowTheLowerBoundExits1) {
  const Snapshot snapshot;
  const fs::path out = snapshot.dir.path() / "out.csv";
  for (const std::string_view model : {"realistic", "best-case"}) {
    app::RunHooks hooks;
    hooks.before_lower_bound = [](app::PathResult& r) {
      if (r.status == app::PathStatus::kOk) {
        r.one_way_s = 1e-6;
      }
    };
    const MainResult r = run_main(main_args(snapshot.dir.path(), out, model), hooks);
    EXPECT_EQ(r.status, 1) << model;
    EXPECT_NE(r.err.find("delays below the lower bound"), std::string::npos) << r.err;
    EXPECT_EQ(r.out.find("delays below the lower bound (must be 0): 0\n"), std::string::npos);
    EXPECT_NE(r.out.find("BELOW THE LOWER BOUND"), std::string::npos);
  }
}

// The number after `label` in a summary, or 0 (and a failure) if there is none.
std::size_t summary_count(const std::string& summary, std::string_view label) {
  const std::size_t at = summary.find(label);
  if (at == std::string::npos) {
    ADD_FAILURE() << "no " << label << " in " << summary;
    return 0;
  }
  return static_cast<std::size_t>(std::stoull(summary.substr(at + label.size())));
}

std::vector<std::string> with(std::vector<std::string> args,
                              const std::vector<std::string>& extra) {
  args.insert(args.end(), extra.begin(), extra.end());
  return args;
}

// --links-out writes the plan used, and a run reading it back with --links-file gives the same
// CSV, byte for byte; the summaries say where the links came from. Under six, the pattern with
// every kind of link (the library tests do the round trip for every pattern; each run here
// takes about 30 s under GCC's ASan).
TEST(TopologyMainTest, LinksOutThenLinksFileGivesTheSameCsv) {
  const Snapshot snapshot;
  {
    const std::string_view pattern = "six";
    const fs::path links = snapshot.dir.path() / ("links_" + std::string(pattern) + ".csv");
    const fs::path planned_csv = snapshot.dir.path() / "planned.csv";
    const fs::path read_csv = snapshot.dir.path() / "read.csv";
    std::vector<std::string> args = main_args(snapshot.dir.path(), planned_csv);
    args.at(13) = std::string(pattern);  // The value of --pattern.
    ASSERT_EQ(args.at(12), "--pattern");
    const MainResult planned = run_main(with(args, {"--links-out", links.string()}));
    ASSERT_EQ(planned.status, 0) << planned.err;
    EXPECT_NE(planned.out.find("  laser links written to " + links.string() + "\n"),
              std::string::npos);
    const std::size_t count = summary_count(planned.out, "  planned laser links: ");
    EXPECT_GT(count, 0U);
    // Two comment lines, the header, one line per link.
    const std::string text = read_file(links);
    EXPECT_EQ(static_cast<std::size_t>(std::count(text.begin(), text.end(), '\n')), count + 3);

    args.at(args.size() - 1) = read_csv.string();  // The value of --out.
    const MainResult read = run_main(with(args, {"--links-file", links.string()}));
    ASSERT_EQ(read.status, 0) << read.err;
    EXPECT_EQ(read_file(read_csv), read_file(planned_csv)) << pattern;
    EXPECT_EQ(summary_count(read.out, "  laser links read from the file: "), count);
    EXPECT_NE(read.out.find(", laser links from the file " + links.string() + " (at most "),
              std::string::npos);
    EXPECT_EQ(planned.out.find("from the file"), std::string::npos);
    EXPECT_NE(read.out.find("checks: PASSED"), std::string::npos);
  }
}

// A plus-grid plan (four links per satellite) read under three: refused without
// --max-lasers 4 (exit 1, nothing written), accepted with it.
TEST(TopologyMainTest, LinksFileLimitsAndRefusals) {
  const Snapshot snapshot;
  const fs::path links = snapshot.dir.path() / "links.csv";
  const fs::path out = snapshot.dir.path() / "out.csv";
  std::vector<std::string> plus_grid = main_args(snapshot.dir.path(), out);
  plus_grid.at(13) = "plus-grid";
  ASSERT_EQ(run_main(with(plus_grid, {"--links-out", links.string()})).status, 0);
  fs::remove(out);

  const std::vector<std::string> three = main_args(snapshot.dir.path(), out);
  const MainResult refused = run_main(with(three, {"--links-file", links.string()}));
  EXPECT_EQ(refused.status, 1);
  EXPECT_NE(refused.err.find("topology: --links-file " + links.string() + ": line "),
            std::string::npos)
      << refused.err;
  EXPECT_NE(refused.err.find("more than 3 links"), std::string::npos) << refused.err;
  EXPECT_FALSE(fs::exists(out));

  const MainResult allowed =
      run_main(with(three, {"--links-file", links.string(), "--max-lasers", "4"}));
  EXPECT_EQ(allowed.status, 0) << allowed.err;
  EXPECT_NE(allowed.out.find("(at most 4 per satellite)"), std::string::npos);
  EXPECT_NE(allowed.out.find("checks: PASSED"), std::string::npos);

  const fs::path bad = snapshot.dir.path() / "bad.csv";
  {
    std::ofstream file(bad);
    file << "norad_cat_id_a,norad_cat_id_b\n1,2\n";
  }
  const MainResult unknown = run_main(with(three, {"--links-file", bad.string()}));
  EXPECT_EQ(unknown.status, 1);
  EXPECT_NE(unknown.err.find("line 2: satellite 1 is not in the classification"), std::string::npos)
      << unknown.err;
  const MainResult unwritable =
      run_main(with(three, {"--links-out", (snapshot.dir.path() / "no_dir" / "l.csv").string()}));
  EXPECT_EQ(unwritable.status, 1);
  EXPECT_NE(unwritable.err.find("topology: --links-out cannot open"), std::string::npos);
}

// The mask overrides: a minimum of 20 deg equals the FCC mask of the fixture's shell (mean
// height 467.5 to 471 km: the 20 deg tier), so the CSV is the default one; Hypatia's range
// rule passes the checks. The summary names the override;
// bad values exit 2.
TEST(TopologyMainTest, MaskOverrides) {
  const Snapshot snapshot;
  const fs::path out = snapshot.dir.path() / "out.csv";
  const fs::path other = snapshot.dir.path() / "other.csv";
  const std::vector<std::string> args = main_args(snapshot.dir.path(), out);
  const MainResult normal = run_main(args);
  ASSERT_EQ(normal.status, 0) << normal.err;
  EXPECT_EQ(normal.out.find("override"), std::string::npos);
  const std::string normal_csv = read_file(out);

  std::vector<std::string> args_other = main_args(snapshot.dir.path(), other);
  const MainResult twenty = run_main(with(args_other, {"--min-elevation-deg", "20"}));
  ASSERT_EQ(twenty.status, 0) << twenty.err;
  EXPECT_EQ(read_file(other), normal_csv);
  EXPECT_NE(twenty.out.find(", ground mask override: one minimum elevation of 20.000 deg "
                            "replaces the FCC masks"),
            std::string::npos)
      << twenty.out;

  const MainResult range = run_main(with(args_other, {"--max-ground-range-km", "1089.686"}));
  ASSERT_EQ(range.status, 0) << range.err;
  EXPECT_NE(range.out.find(", ground mask override: any satellite within 1089.686 km, no "
                           "elevation test"),
            std::string::npos)
      << range.out;
  EXPECT_GT(summary_count(range.out, "ground link-steps: cities "), 0U);
  EXPECT_NE(range.out.find("checks: PASSED"), std::string::npos);

  fs::remove(other);
  for (const std::vector<std::string>& bad :
       {std::vector<std::string>{"--min-elevation-deg", "90"},
        std::vector<std::string>{"--max-ground-range-km", "0"},
        std::vector<std::string>{"--min-elevation-deg", "10", "--max-ground-range-km", "1000"},
        std::vector<std::string>{"--max-lasers", "4"}}) {
    const MainResult r = run_main(with(args_other, bad));
    EXPECT_EQ(r.status, 2) << bad[0];
    EXPECT_FALSE(fs::exists(other));
  }
}

}  // namespace
