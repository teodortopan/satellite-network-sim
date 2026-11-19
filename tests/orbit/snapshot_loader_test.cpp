// The snapshot loader (satsim/orbit/snapshot.hpp), on the propagate program's fixture snapshot
// and on scratch directories.

#include <gtest/gtest.h>
#include <unistd.h>

#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <variant>
#include <vector>

#include "satsim/orbit/element_selection.hpp"
#include "satsim/orbit/element_set.hpp"
#include "satsim/orbit/propagator.hpp"
#include "satsim/orbit/snapshot.hpp"

namespace {

namespace fs = std::filesystem;
using satsim::orbit::build_propagators;
using satsim::orbit::BuiltPropagators;
using satsim::orbit::check_snapshot_directory;
using satsim::orbit::ElementSource;
using satsim::orbit::load_snapshot_elements;
using satsim::orbit::PropagationError;
using satsim::orbit::SnapshotElements;
using satsim::orbit::SourcedElementSet;

// gp.json: 4 records; supgp.json: 5 records; 6 satellites in all
// (tests/propagate/test_support.hpp).
const fs::path kFixture = "tests/propagate/data/snapshot";
constexpr std::array<std::string_view, 3> kAllFiles = {"gp.json", "supgp.json", "finals2000A.all"};

// A fresh directory, removed with its contents on destruction.
class ScratchDir {
 public:
  ScratchDir() {
    const auto* info = ::testing::UnitTest::GetInstance()->current_test_info();
    path_ = fs::temp_directory_path() /
            ("satsim_snapshot_" + std::string(info->name()) + "_" + std::to_string(::getpid()));
    fs::remove_all(path_);
    fs::create_directories(path_);
  }
  ScratchDir(const ScratchDir&) = delete;
  ScratchDir& operator=(const ScratchDir&) = delete;
  ScratchDir(ScratchDir&&) = delete;
  ScratchDir& operator=(ScratchDir&&) = delete;
  ~ScratchDir() {
    std::error_code ec;
    fs::remove_all(path_, ec);
  }
  [[nodiscard]] const fs::path& path() const { return path_; }

 private:
  fs::path path_;
};

void write_file(const fs::path& path, std::string_view content) {
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  out << content;
}

TEST(SnapshotLoaderTest, FixtureDirectoryHasEveryFile) {
  EXPECT_EQ(check_snapshot_directory(kFixture, kAllFiles), std::nullopt);
}

TEST(SnapshotLoaderTest, MissingDirectory) {
  const std::optional<std::string> error = check_snapshot_directory("no/such/snapshot", kAllFiles);
  EXPECT_EQ(error.value_or("(none)"), "snapshot directory no/such/snapshot does not exist");
}

TEST(SnapshotLoaderTest, NotADirectory) {
  const ScratchDir dir;
  write_file(dir.path() / "file", "x");
  const std::optional<std::string> error = check_snapshot_directory(dir.path() / "file", kAllFiles);
  EXPECT_EQ(error.value_or("(none)"),
            "snapshot " + (dir.path() / "file").string() + " is not a directory");
}

TEST(SnapshotLoaderTest, MissingFilesAreAllNamed) {
  const ScratchDir dir;
  write_file(dir.path() / "supgp.json", "[]");
  const std::optional<std::string> error = check_snapshot_directory(dir.path(), kAllFiles);
  EXPECT_EQ(error.value_or("(none)"),
            "snapshot directory " + dir.path().string() + " has no gp.json, finals2000A.all");
  // Only the files asked for are required.
  constexpr std::array<std::string_view, 1> kOnlySupplemental = {"supgp.json"};
  EXPECT_EQ(check_snapshot_directory(dir.path(), kOnlySupplemental), std::nullopt);
}

TEST(SnapshotLoaderTest, LoadsAndSelectsTheFixture) {
  const auto loaded = load_snapshot_elements(kFixture);
  ASSERT_TRUE(std::holds_alternative<SnapshotElements>(loaded));
  const auto& elements = std::get<SnapshotElements>(loaded);
  EXPECT_EQ(elements.standard_file.records_loaded, 4U);
  EXPECT_EQ(elements.supplemental_file.records_loaded, 5U);
  EXPECT_TRUE(elements.standard_file.record_errors.empty());
  EXPECT_TRUE(elements.supplemental_file.record_errors.empty());
  std::vector<std::int64_t> ids;
  for (const SourcedElementSet& s : elements.selection.selected) {
    ids.push_back(s.elements.norad_cat_id);
    // 44753 is only in the standard file.
    EXPECT_EQ(s.source, s.elements.norad_cat_id == 44753 ? ElementSource::kStandard
                                                         : ElementSource::kSupplemental);
  }
  EXPECT_EQ(ids, (std::vector<std::int64_t>{44714, 44753, 55278, 100001, 799501782, 799501787}));
  EXPECT_TRUE(elements.selection.superseded.empty());
}

TEST(SnapshotLoaderTest, UnusableFileIsAnErrorNamingIt) {
  const ScratchDir dir;
  write_file(dir.path() / "gp.json", "[]");
  write_file(dir.path() / "supgp.json", "{not json");
  const auto loaded = load_snapshot_elements(dir.path());
  ASSERT_TRUE(std::holds_alternative<std::string>(loaded));
  EXPECT_NE(std::get<std::string>(loaded).find("supgp.json"), std::string::npos)
      << std::get<std::string>(loaded);
}

TEST(SnapshotLoaderTest, BadRecordsAreCountedNotFatal) {
  const ScratchDir dir;
  write_file(dir.path() / "gp.json", R"([{"NORAD_CAT_ID": 1}])");
  write_file(dir.path() / "supgp.json", "[]");
  const auto loaded = load_snapshot_elements(dir.path());
  ASSERT_TRUE(std::holds_alternative<SnapshotElements>(loaded));
  const auto& elements = std::get<SnapshotElements>(loaded);
  EXPECT_EQ(elements.standard_file.records_loaded, 0U);
  EXPECT_EQ(elements.standard_file.record_errors.size(), 1U);
  EXPECT_TRUE(elements.selection.selected.empty());
}

TEST(SnapshotLoaderTest, BuildPropagatorsSeparatesFailuresKeepingOrder) {
  const auto loaded = load_snapshot_elements(kFixture);
  ASSERT_TRUE(std::holds_alternative<SnapshotElements>(loaded));
  std::vector<SourcedElementSet> sets = std::get<SnapshotElements>(loaded).selection.selected;
  sets[2].elements.eccentricity = 2.0;  // 55278 cannot be built.
  const BuiltPropagators built = build_propagators(sets);
  ASSERT_EQ(built.satellites.size(), 5U);
  ASSERT_EQ(built.failures.size(), 1U);
  EXPECT_EQ(built.failures[0].element_set.elements.norad_cat_id, 55278);
  EXPECT_EQ(built.failures[0].error, PropagationError::kInvalidElements);
  EXPECT_EQ(built.satellites[0].element_set.elements.norad_cat_id, 44714);
  EXPECT_EQ(built.satellites[2].element_set.elements.norad_cat_id, 100001);
  const auto epoch =
      satsim::time::UtcTime::from_civil(built.satellites[2].element_set.elements.epoch);
  EXPECT_EQ(epoch, built.satellites[2].propagator.epoch());
}

}  // namespace
