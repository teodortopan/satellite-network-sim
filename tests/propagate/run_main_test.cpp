// run_main, the whole program minus the process: streams, exit status and output file.

#include <gtest/gtest.h>

#include <filesystem>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include "propagate/run.hpp"
#include "test_support.hpp"

namespace {

using satsim::propagate::testing::parse_csv;
using satsim::propagate::testing::read_file;
using satsim::propagate::testing::ScratchDir;

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
  result.status = satsim::propagate::run_main(views, out, err);
  result.out = out.str();
  result.err = err.str();
  return result;
}

std::vector<std::string> fixture_args(const std::string& start, const std::string& out) {
  return {"--snapshot",   "tests/propagate/data/snapshot",
          "--start",      start,
          "--duration-s", "600",
          "--step-s",     "300",
          "--out",        out};
}

TEST(PropagateMainTest, WritesTheCsvFileAndTheSummaryToOut) {
  const ScratchDir dir;
  const std::string csv_path = (dir.path() / "positions.csv").string();
  const Result r = run_main(fixture_args("2026-11-17T18:00:00", csv_path));
  EXPECT_EQ(r.status, 0) << r.err;
  EXPECT_EQ(r.err, "");
  EXPECT_NE(r.out.find("rows written: 18\n"), std::string::npos) << r.out;
  EXPECT_EQ(parse_csv(read_file(csv_path)).size(), 19U);
}

TEST(PropagateMainTest, CsvToStandardOutputMovesTheSummaryToErr) {
  const Result r = run_main(fixture_args("2026-11-17T18:00:00", "-"));
  EXPECT_EQ(r.status, 0) << r.err;
  EXPECT_EQ(parse_csv(r.out).size(), 19U);
  EXPECT_NE(r.err.find("rows written: 18\n"), std::string::npos) << r.err;
}

TEST(PropagateMainTest, InvalidArgumentsExitWith2AndAMessage) {
  const Result r = run_main({"--snapshot", "x", "--bogus"});
  EXPECT_EQ(r.status, 2);
  EXPECT_EQ(r.out, "");
  EXPECT_EQ(r.err, "propagate: unknown argument \"--bogus\"\nRun 'propagate --help' for usage.\n");
}

TEST(PropagateMainTest, HelpGoesToOutAndExits0) {
  const Result r = run_main({"--help"});
  EXPECT_EQ(r.status, 0);
  EXPECT_EQ(r.out, satsim::propagate::usage());
  EXPECT_EQ(r.err, "");
}

// The run fails on its inputs before the output file is created.
TEST(PropagateMainTest, FatalErrorExits1AndCreatesNoFile) {
  const ScratchDir dir;
  const std::filesystem::path csv_path = dir.path() / "positions.csv";
  const Result r = run_main(fixture_args("2027-01-05T00:00:00", csv_path.string()));
  EXPECT_EQ(r.status, 1);
  EXPECT_EQ(r.out, "");
  EXPECT_EQ(r.err.rfind("propagate: no Earth-orientation values for 2027-01-05T00:00:00", 0), 0U)
      << r.err;
  EXPECT_FALSE(std::filesystem::exists(csv_path));
}

TEST(PropagateMainTest, UnwritableOutputExits1) {
  const ScratchDir dir;
  const Result r =
      run_main(fixture_args("2026-11-17T18:00:00", (dir.path() / "no" / "such.csv").string()));
  EXPECT_EQ(r.status, 1);
  EXPECT_NE(r.err.find("cannot open"), std::string::npos) << r.err;
}

}  // namespace
