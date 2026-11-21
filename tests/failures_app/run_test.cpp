// The failures program as a library and as a program, on the topology program's synthetic
// snapshot (an ideal Walker shell of 160 satellites) and its small ground fixture: with no
// failure every lane gives the routing program's rows (spec 4.2, 6.2); with one, every output
// file in its format, the checks passing, the refusals of the run.

#include "failures/run.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "failures/arguments.hpp"
#include "routing/arguments.hpp"
#include "routing/run.hpp"
#include "satsim/routing/strategy.hpp"
#include "support/synthetic.hpp"
#include "test_support.hpp"
#include "topology_app/fixture.hpp"

namespace {

namespace fs = std::filesystem;
namespace fixture = satsim::topology_app::testing;
namespace fa = satsim::failures_app;
namespace ra = satsim::routing_app;
using satsim::propagate::testing::parse_csv;
using satsim::propagate::testing::read_file;
using satsim::propagate::testing::ScratchDir;
using Rows = std::vector<std::vector<std::string>>;

struct Snapshot {
  ScratchDir dir;
  Snapshot() {
    fixture::write_snapshot(
        dir.path(), satsim::constellation::testing::walker_element_sets(fixture::fixture_shell()));
  }
};

const Snapshot& snapshot() {
  static const Snapshot s;
  return s;
}

std::vector<std::string> args(const fs::path& out, std::string_view model) {
  return {"--snapshot",
          snapshot().dir.path().string(),
          "--ground",
          fixture::kGroundDir.string(),
          "--cities",
          fixture::kCities.string(),
          "--start",
          std::string(fixture::kStart),
          "--duration-s",
          "60",
          "--step-s",
          "15",
          "--pattern",
          "three",
          "--model",
          std::string(model),
          "--strategies",
          "lowest-delay,keep-until-broken",
          "--replan-s",
          "15",
          "--serving-slot-s",
          "15",
          "--out",
          out.string(),
          "--reactions",
          "planner,local-repair"};
}

fa::Options options_of(const std::vector<std::string>& a) {
  const std::vector<std::string_view> views(a.begin(), a.end());
  const fa::ParsedArguments parsed = fa::parse_arguments(views);
  if (!parsed.options) {
    ADD_FAILURE() << parsed.error.value_or("");
    std::abort();
  }
  return *parsed.options;
}

fa::FailureRun prepare(const fa::Options& o) {
  fa::FailureRunOrError r = fa::prepare_failure_run(o);
  if (const auto* error = std::get_if<std::string>(&r)) {
    ADD_FAILURE() << *error;
    std::abort();
  }
  return std::move(std::get<fa::FailureRun>(r));
}

int run_main(const std::vector<std::string>& a, std::string* err_text = nullptr) {
  const std::vector<std::string_view> views(a.begin(), a.end());
  std::ostringstream out;
  std::ostringstream err;
  const int status = fa::run_main(views, out, err);
  if (err_text != nullptr) {
    *err_text = err.str() + out.str();
  }
  return status;
}

std::string header_of(std::span<const std::string_view> columns) {
  std::string h;
  for (const std::string_view c : columns) {
    h += (h.empty() ? "" : ",") + std::string(c);
  }
  return h;
}

// With no failure file every lane's Phase 3 columns are the routing program's, in both models
// (spec 4.2): the rows of one step and direction come lane by lane.
TEST(FailuresRunTest, NoFailureEveryLaneEqualsTheRoutingProgram) {
  for (const std::string_view model : {"realistic", "best-case"}) {
    SCOPED_TRACE(model);
    const fa::Options o = options_of(args("-", model));
    const fa::FailureRun r = prepare(o);
    std::ostringstream routes;
    std::ostringstream network;
    const fa::FailureOutcome outcome = fa::write_failure_routes(r, routes, &network);
    ASSERT_FALSE(outcome.error.has_value()) << outcome.error.value_or("");
    EXPECT_TRUE(outcome.statistics.passed());

    ra::PreparedRunOrError rp = ra::prepare_run(o.routing);
    ASSERT_TRUE(std::holds_alternative<ra::PreparedRun>(rp));
    std::ostringstream routing_csv;
    const ra::RunOutcome ro = ra::write_routes(std::get<ra::PreparedRun>(rp), routing_csv);
    ASSERT_FALSE(ro.error.has_value());

    const Rows f = parse_csv(routes.str());
    const Rows g = parse_csv(routing_csv.str());
    ASSERT_EQ(f.size() - 1, 4 * (g.size() - 1));
    std::map<std::string, std::size_t> lanes;
    for (std::size_t i = 1; i < f.size(); ++i) {
      const std::vector<std::string>& row = f[i];
      ASSERT_EQ(row.size(), ra::kRoutesColumns.size() + 3);
      const std::vector<std::string> phase3(
          row.begin(), row.begin() + static_cast<std::ptrdiff_t>(ra::kRoutesColumns.size()));
      EXPECT_EQ(phase3, g.at(1 + (i - 1) / 4)) << i;
      ++lanes[row[ra::kRoutesColumns.size()]];
      EXPECT_EQ(row[ra::kRoutesColumns.size() + 1] == "delivered", row[7] == "ok") << i;
      EXPECT_EQ(row.back(), "");
    }
    EXPECT_EQ(lanes, (std::map<std::string, std::size_t>{{"baseline", g.size() - 1},
                                                         {"local-repair", g.size() - 1},
                                                         {"planner", g.size() - 1},
                                                         {"reference", g.size() - 1}}));
    // No failure: no t_f, so no measure.
    for (const fa::LaneMeasures& m : outcome.statistics.measures) {
      EXPECT_FALSE(m.black_hole_s.has_value());
    }
    // The network CSV: 5 steps, scope all then each shell of the default classification
    // (the fixture's 160 satellites are all in 53deg); one piece of 160 at every step.
    const Rows n = parse_csv(network.str());
    const std::size_t scopes = 5;
    ASSERT_EQ(n.size(), 1U + 5 * scopes);
    EXPECT_EQ(n[0], (std::vector<std::string>{"utc", "scope", "working_satellites", "pieces",
                                              "largest_piece", "largest_piece_share"}));
    EXPECT_EQ(n[1],
              (std::vector<std::string>{"2026-11-18T12:00:00", "all", "160", "1", "160", "1"}));
    EXPECT_EQ(n[2],
              (std::vector<std::string>{"2026-11-18T12:00:00", "53deg", "160", "1", "160", "1"}));
    EXPECT_EQ(n[3][2], "0");
    EXPECT_EQ(n[3][5], "none");
  }
}

// One satellite fails: the first satellite of the first direction's lowest-delay route at
// 15 s. The program writes every file in its format, its checks pass, the planner lane is a
// black hole from 15 s to the plan at 30 s and the local-repair lane from 15 s to 15.1 s.
TEST(FailuresRunTest, OneFailureEveryFile) {
  ScratchDir dir;
  // The baseline route at 15 s, from a run without failures.
  const fa::FailureRun clean = prepare(options_of(args("-", "best-case")));
  std::ostringstream clean_routes;
  ASSERT_FALSE(fa::write_failure_routes(clean, clean_routes, nullptr).error.has_value());
  const Rows c = parse_csv(clean_routes.str());
  std::string first;
  for (std::size_t i = 1; i < c.size(); ++i) {
    if (c[i][0] == "2026-11-18T12:00:15" && c[i][6] == "lowest-delay" && c[i][21] == "baseline") {
      first = c[i][13];
      break;
    }
  }
  ASSERT_FALSE(first.empty());
  const fs::path failures = dir.path() / "failures.csv";
  {
    std::ofstream file(failures);
    file << "kind,norad_cat_id_a,norad_cat_id_b,gateway,start_utc,length_s\n"
         << "satellite," << first << ",,,2026-11-18T12:00:15,\n";
  }
  std::vector<std::string> a = args(dir.path() / "routes.csv", "best-case");
  for (const std::string_view extra :
       {"--failures", "", "--events-out", "events.csv", "--facts-out", "facts.csv",
        "--measures-out", "measures.csv", "--network-out", "network.csv", "--drops-out",
        "drops.csv"}) {
    a.emplace_back(extra);
  }
  for (std::size_t i = a.size() - 12; i < a.size(); i += 2) {
    a[i + 1] = i == a.size() - 12 ? failures.string() : (dir.path() / a[i + 1]).string();
  }
  std::string err;
  ASSERT_EQ(run_main(a, &err), 0) << err;
  EXPECT_NE(err.find("  checks: PASSED"), std::string::npos) << err;
  EXPECT_NE(err.find("failures (Phase 4):"), std::string::npos);

  const Rows events = parse_csv(read_file(dir.path() / "events.csv"));
  EXPECT_EQ(header_of(fa::kEventsColumns),
            read_file(dir.path() / "events.csv").substr(0, header_of(fa::kEventsColumns).size()));
  const auto lane = [&](std::string_view reaction) {
    Rows result;
    for (std::size_t i = 1; i < events.size(); ++i) {
      if (events[i][2] == c[1][1] && events[i][3] == c[1][2] && events[i][7] == "lowest-delay" &&
          events[i][8] == reaction) {
        result.push_back(events[i]);
      }
    }
    return result;
  };
  const std::string element = "satellite:" + first;
  const Rows planner = lane("planner");
  ASSERT_GE(planner.size(), 2U);
  const auto at = [](const Rows& rows, std::string_view start) {
    return std::find_if(rows.begin(), rows.end(), [&](const auto& row) { return row[0] == start; });
  };
  ASSERT_NE(at(planner, "15"), planner.end());
  EXPECT_EQ((*at(planner, "15"))[1], "30");
  EXPECT_EQ((*at(planner, "15"))[9], "black_hole");
  EXPECT_EQ((*at(planner, "15"))[10], "failed_satellite");
  EXPECT_EQ((*at(planner, "15"))[11], element);
  const Rows repair = lane("local-repair");
  ASSERT_NE(at(repair, "15"), repair.end());
  EXPECT_EQ((*at(repair, "15"))[1], "15.1");
  EXPECT_EQ((*at(repair, "15"))[9], "black_hole");
  ASSERT_NE(at(repair, "15.1"), repair.end());
  EXPECT_EQ((*at(repair, "15.1"))[9], "delivered");

  const std::string facts_text = read_file(dir.path() / "facts.csv");
  EXPECT_EQ(facts_text.substr(0, header_of(fa::kFactsColumns).size()),
            header_of(fa::kFactsColumns));
  const Rows facts = parse_csv(facts_text);
  ASSERT_GE(facts.size(), 2U);
  EXPECT_EQ(facts[1][0], "0");
  EXPECT_EQ(facts[1][1], element);
  EXPECT_EQ(facts[1][2], "down");
  EXPECT_EQ(facts[1][3], "15");
  EXPECT_EQ(facts[1][4], "no");
  EXPECT_EQ(facts[1][9], "");  // No flooding: the flood columns are empty.
  for (std::size_t i = 2; i < facts.size(); ++i) {
    EXPECT_EQ(facts[i][1].substr(0, 5), "link:");
    EXPECT_EQ(facts[i][5].substr(0, 10), "satellite:");
    EXPECT_NE(facts[i][5].find("@15.1"), std::string::npos);
  }

  const std::string measures_text = read_file(dir.path() / "measures.csv");
  EXPECT_EQ(measures_text.substr(0, header_of(fa::kMeasuresColumns).size()),
            header_of(fa::kMeasuresColumns));
  const Rows measures = parse_csv(measures_text);
  // Two study pairs, one validation pair, both directions; two strategies; two reactions.
  ASSERT_EQ(measures.size(), 1U + 6 * 2 * 2);
  EXPECT_EQ(measures[1][6], "planner");
  EXPECT_EQ(measures[1][7], "15");
  EXPECT_EQ(measures[1][8], "15");    // Black hole until the plan at 30 s.
  EXPECT_EQ(measures[1][9], "none");  // No loop without flooding.
  EXPECT_EQ(measures[2][6], "local-repair");
  EXPECT_EQ(measures[2][8], "0.09999999999999964");  // 15.1 - 15.

  const Rows network = parse_csv(read_file(dir.path() / "network.csv"));
  ASSERT_EQ(network.size(), 1U + 5 * 5);
  // Per step the scope all, then each shell: 160 working at 0 s, 159 from 15 s.
  EXPECT_EQ(network[1][1], "all");
  EXPECT_EQ(network[1][2], "160");
  EXPECT_EQ(network[6][1], "all");
  EXPECT_EQ(network[6][2], "159");
  EXPECT_EQ(network[7][1], "53deg");
  EXPECT_EQ(network[7][2], "159");
  EXPECT_TRUE(fs::exists(dir.path() / "drops.csv"));
  const std::string routes_text = read_file(dir.path() / "routes.csv");
  EXPECT_EQ(routes_text.substr(0, routes_text.find('\n')),
            header_of(ra::kRoutesColumns) + ",reaction,state,failed_element");
}

TEST(FailuresRunTest, RefusalsOfTheRun) {
  std::vector<std::string> a = args("-", "best-case");
  a.back() = "planner,flooding";
  std::string err;
  EXPECT_EQ(run_main(a, &err), 2);
  EXPECT_NE(err.find("flooding is not available yet"), std::string::npos) << err;
  a.back() = "planner";
  a.insert(a.end(), {"--failures", "/nonexistent/failures.csv"});
  EXPECT_EQ(run_main(a, &err), 2);
  EXPECT_NE(err.find("--failures"), std::string::npos) << err;
}

}  // namespace
