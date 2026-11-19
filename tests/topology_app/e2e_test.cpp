// End to end: runs the built topology executable (path from CMake, SATSIM_TOPOLOGY_EXE) as a
// child process on the synthetic snapshot and the ground fixture. Started directly with
// posix_spawn, never through a shell, so under the valgrind step's --trace-children=yes the
// program itself is checked, and a Valgrind error (exit code 99) fails every status check.

#include <fcntl.h>
#include <gtest/gtest.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#include <filesystem>
#include <string>
#include <vector>

#include "fixture.hpp"
#include "support/synthetic.hpp"
#include "test_support.hpp"

extern char** environ;  // NOLINT(readability-redundant-declaration): POSIX, not in every header.

namespace {

namespace fixture = satsim::topology_app::testing;
using satsim::propagate::testing::parse_csv;
using satsim::propagate::testing::read_file;
using satsim::propagate::testing::ScratchDir;

struct Completed {
  int status = -1;  // Exit status, or -1 if the child did not exit normally.
  std::string out;
  std::string err;
};

Completed run_program(const std::vector<std::string>& args, const ScratchDir& dir) {
  const std::string out_path = (dir.path() / "stdout.txt").string();
  const std::string err_path = (dir.path() / "stderr.txt").string();
  std::vector<std::string> storage = {SATSIM_TOPOLOGY_EXE};
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
    ADD_FAILURE() << "cannot start " << SATSIM_TOPOLOGY_EXE << ": error " << spawned;
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

std::vector<std::string> args(const std::filesystem::path& snapshot, const std::string& out) {
  return {"--snapshot",   snapshot.string(),
          "--ground",     fixture::kGroundDir.string(),
          "--cities",     fixture::kCities.string(),
          "--start",      std::string(fixture::kStart),
          "--duration-s", "60",
          "--step-s",     "30",
          "--pattern",    "plus-grid",
          "--model",      "best-case",
          "--out",        out};
}

TEST(TopologyEndToEndTest, RunsOnTheFixture) {
  const ScratchDir dir;
  const auto sets = satsim::constellation::testing::walker_element_sets(fixture::fixture_shell());
  fixture::write_snapshot(dir.path(), sets);
  const std::string csv_path = (dir.path() / "topology.csv").string();
  const Completed c = run_program(args(dir.path(), csv_path), dir);
  // Standard error is not checked to be empty: under the valgrind step it carries Valgrind's
  // report of the child (an error there shows as exit status 99).
  ASSERT_EQ(c.status, 0) << c.err;
  EXPECT_EQ(c.err.find("topology:"), std::string::npos) << c.err;
  EXPECT_NE(c.out.find("topology summary\n"), std::string::npos) << c.out;
  EXPECT_NE(c.out.find("checks: PASSED\n"), std::string::npos) << c.out;
  // 3 steps, 3 pairs, 2 directions, plus the header.
  EXPECT_EQ(parse_csv(read_file(csv_path)).size(), 19U);
}

TEST(TopologyEndToEndTest, ExitStatuses) {
  const ScratchDir dir;
  const Completed bad = run_program({"--pattern"}, dir);
  EXPECT_EQ(bad.status, 2) << bad.err;
  const Completed missing = run_program(args("no/such/dir", "-"), dir);
  EXPECT_EQ(missing.status, 1) << missing.err;
  EXPECT_EQ(missing.out, "");
  const Completed help = run_program({"-h"}, dir);
  EXPECT_EQ(help.status, 0) << help.err;
}

}  // namespace
