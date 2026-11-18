// End to end: runs the built propagate executable (path from CMake, SATSIM_PROPAGATE_EXE) as
// a child process on the fixture snapshot.
//
// The child is started directly with posix_spawn, never through a shell, so under the
// valgrind step's --trace-children=yes the only traced child is the program itself. A
// Valgrind error there makes the child exit with Valgrind's --error-exitcode (99 in
// scripts/ci.sh), a status the program never uses, so every exit-status check below fails
// on it; the child's stderr, which carries Valgrind's report, is printed with each check.

#include <fcntl.h>
#include <gtest/gtest.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <filesystem>
#include <string>
#include <vector>

#include "propagate/csv.hpp"
#include "test_support.hpp"

extern char** environ;  // NOLINT(readability-redundant-declaration): POSIX, not in every header.

namespace {

using satsim::propagate::kCsvColumns;
using satsim::propagate::testing::parse_csv;
using satsim::propagate::testing::read_file;
using satsim::propagate::testing::ScratchDir;

struct Completed {
  int status = -1;  // Exit status, or -1 if the child did not exit normally.
  std::string out;
  std::string err;
};

// Runs the program with `args`, its stdout and stderr captured in files under `dir`.
Completed run_program(const std::vector<std::string>& args, const ScratchDir& dir) {
  const std::string out_path = (dir.path() / "stdout.txt").string();
  const std::string err_path = (dir.path() / "stderr.txt").string();
  std::vector<std::string> storage = {SATSIM_PROPAGATE_EXE};
  storage.insert(storage.end(), args.begin(), args.end());
  std::vector<char*> argv;
  argv.reserve(storage.size() + 1);
  for (std::string& s : storage) {
    argv.push_back(s.data());
  }
  argv.push_back(nullptr);

  posix_spawn_file_actions_t actions;
  posix_spawn_file_actions_init(&actions);
  posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO, out_path.c_str(),
                                   O_WRONLY | O_CREAT | O_TRUNC, 0644);
  posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, err_path.c_str(),
                                   O_WRONLY | O_CREAT | O_TRUNC, 0644);
  pid_t pid = 0;
  const int spawned = posix_spawn(&pid, argv[0], &actions, nullptr, argv.data(), environ);
  posix_spawn_file_actions_destroy(&actions);
  Completed completed;
  if (spawned != 0) {
    ADD_FAILURE() << "cannot start " << SATSIM_PROPAGATE_EXE << ": error " << spawned;
    return completed;
  }
  int wait_status = 0;
  if (waitpid(pid, &wait_status, 0) != pid) {
    ADD_FAILURE() << "waitpid failed";
    return completed;
  }
  if (WIFEXITED(wait_status)) {
    completed.status = WEXITSTATUS(wait_status);
  }
  completed.out = read_file(out_path);
  completed.err = read_file(err_path);
  return completed;
}

std::vector<std::string> fixture_args(const std::string& start, const std::string& out) {
  return {"--snapshot",   "tests/propagate/data/snapshot",
          "--start",      start,
          "--duration-s", "600",
          "--step-s",     "300",
          "--out",        out};
}

TEST(PropagateEndToEndTest, WritesTheCsvAndTheSummary) {
  const ScratchDir dir;
  const std::string csv_path = (dir.path() / "positions.csv").string();
  const Completed c = run_program(fixture_args("2026-11-17T18:00:00", csv_path), dir);
  ASSERT_EQ(c.status, 0) << c.err;
  EXPECT_EQ(c.err.find("propagate:"), std::string::npos) << c.err;
  EXPECT_NE(c.out.find("rows written: 18\n"), std::string::npos) << c.out;

  const auto records = parse_csv(read_file(csv_path));
  ASSERT_EQ(records.size(), 1 + 3 * 6U);
  EXPECT_TRUE(
      std::equal(records[0].begin(), records[0].end(), kCsvColumns.begin(), kCsvColumns.end()));
  for (const auto& record : records) {
    EXPECT_EQ(record.size(), kCsvColumns.size());
  }
}

TEST(PropagateEndToEndTest, CsvCanGoToStandardOutput) {
  const ScratchDir dir;
  const Completed c = run_program(fixture_args("2026-11-17T18:00:00", "-"), dir);
  ASSERT_EQ(c.status, 0) << c.err;
  EXPECT_EQ(parse_csv(c.out).size(), 1 + 3 * 6U);
  EXPECT_NE(c.err.find("rows written: 18\n"), std::string::npos) << c.err;
}

TEST(PropagateEndToEndTest, InvalidArgumentsExitWith2) {
  const ScratchDir dir;
  std::vector<std::string> args = fixture_args("2026-11-17T18:00:00", "-");
  args.at(7) = "0";  // --step-s 0
  const Completed c = run_program(args, dir);
  EXPECT_EQ(c.status, 2) << c.err;
  EXPECT_EQ(c.out, "");
  EXPECT_NE(c.err.find("propagate: --step-s \"0\""), std::string::npos) << c.err;
}

TEST(PropagateEndToEndTest, TimesOutsideTheEarthOrientationTableExitWith1AndWriteNothing) {
  const ScratchDir dir;
  const std::filesystem::path csv_path = dir.path() / "positions.csv";
  const Completed c = run_program(fixture_args("2027-01-05T00:00:00", csv_path.string()), dir);
  EXPECT_EQ(c.status, 1) << c.err;
  EXPECT_EQ(c.out, "");
  EXPECT_NE(c.err.find("propagate: no Earth-orientation values"), std::string::npos) << c.err;
  EXPECT_FALSE(std::filesystem::exists(csv_path));
}

TEST(PropagateEndToEndTest, HelpExitsWith0) {
  const ScratchDir dir;
  const Completed c = run_program({"--help"}, dir);
  EXPECT_EQ(c.status, 0) << c.err;
  EXPECT_EQ(c.out.rfind("usage: propagate", 0), 0U) << c.out;
}

}  // namespace
