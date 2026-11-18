// Loads the full committed Starlink snapshot. Built only without sanitizers and not run under
// Valgrind (see tests/orbit/CMakeLists.txt for why).

#include <gtest/gtest.h>

#include <cstddef>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <string>

#include "satsim/orbit/omm_loader.hpp"

namespace {

using satsim::orbit::load_omm_json;
using satsim::orbit::LoadResult;

const std::filesystem::path kSnapshotDir = "data/starlink/2026-11-18";

// Every real record must load, and the counts must match what fetch_elements.py recorded.
TEST(OmmSnapshotTest, FullSnapshotLoadsWithoutErrors) {
  std::ifstream metadata_file(kSnapshotDir / "metadata.json");
  ASSERT_TRUE(metadata_file.is_open());
  const nlohmann::json metadata = nlohmann::json::parse(metadata_file, nullptr, false);
  ASSERT_TRUE(metadata.is_object());

  for (const std::string name : {"gp.json", "supgp.json"}) {
    const LoadResult result = load_omm_json(kSnapshotDir / name);
    ASSERT_FALSE(result.file_error.has_value()) << result.file_error.value_or("");
    EXPECT_TRUE(result.record_errors.empty())
        << name << ": " << result.record_errors.size() << " errors, first: "
        << (result.record_errors.empty() ? "" : result.record_errors[0].message);
    ASSERT_TRUE(metadata.contains(name) && metadata[name].contains("records"));
    const auto expected = metadata[name]["records"].get<std::size_t>();
    EXPECT_GT(expected, 1000U) << name;
    EXPECT_EQ(result.element_sets.size(), expected) << name;
  }
}

}  // namespace
