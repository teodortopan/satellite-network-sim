#include "satsim/constellation/config_file.hpp"

#include <gtest/gtest.h>

#include <filesystem>
#include <sstream>
#include <string>
#include <string_view>
#include <variant>

#include "satsim/constellation/classification.hpp"

namespace {

using satsim::constellation::ClassificationConfig;
using satsim::constellation::ClassificationConfigOrError;
using satsim::constellation::default_config;
using satsim::constellation::ExclusionReason;
using satsim::constellation::load_classification_config;
using satsim::constellation::parse_classification_config;

ClassificationConfigOrError parse(std::string_view text) {
  std::istringstream in{std::string(text)};
  return parse_classification_config(in);
}

std::string error_of(std::string_view text) {
  const ClassificationConfigOrError result = parse(text);
  if (const auto* error = std::get_if<std::string>(&result)) {
    return *error;
  }
  ADD_FAILURE() << "accepted: " << text;
  return {};
}

constexpr std::string_view kShell =
    R"({"name": "ideal", "inclination_deg": 53.0, "inclination_tolerance_deg": 0.3,
        "min_height_km": 500.0, "max_height_km": 530.0, "plane_gap_threshold_deg": 2.0,
        "min_plane_size": 10})";

std::string with_shells(std::string_view shells, std::string_view extra = "") {
  return "{\"shells\": [" + std::string(shells) + "]" + std::string(extra) + "}";
}

TEST(ClassificationConfigFile, ReadsShellsAndKeepsTheOtherDefaults) {
  const ClassificationConfigOrError result = parse(with_shells(kShell));
  ASSERT_TRUE(std::holds_alternative<ClassificationConfig>(result))
      << std::get<std::string>(result);
  const auto& config = std::get<ClassificationConfig>(result);
  ASSERT_EQ(config.shells.size(), 1U);
  const auto& shell = config.shells[0];
  EXPECT_EQ(shell.name, "ideal");
  EXPECT_DOUBLE_EQ(shell.inclination_deg, 53.0);
  EXPECT_DOUBLE_EQ(shell.inclination_tolerance_deg, 0.3);
  EXPECT_DOUBLE_EQ(shell.min_height_km, 500.0);
  EXPECT_DOUBLE_EQ(shell.max_height_km, 530.0);
  EXPECT_DOUBLE_EQ(shell.plane_gap_threshold_deg, 2.0);
  EXPECT_EQ(shell.min_plane_size, 10U);
  // No excluded groups unless the file has some; the rest is the default.
  EXPECT_TRUE(config.excluded_groups.empty());
  const ClassificationConfig defaults = default_config();
  EXPECT_EQ(config.last_launch_without_lasers, defaults.last_launch_without_lasers);
  EXPECT_EQ(config.sampling.samples_per_orbit, defaults.sampling.samples_per_orbit);
}

TEST(ClassificationConfigFile, ReadsExcludedGroupsInOrder) {
  const ClassificationConfigOrError result = parse(with_shells(kShell,
                                                               R"(, "excluded_groups": [
           {"name": "low", "reason": "low_group", "min_inclination_deg": 0,
            "max_inclination_deg": 180, "min_height_km": 350, "max_height_km": 375},
           {"name": "old", "reason": "old_height", "min_inclination_deg": 69.7,
            "max_inclination_deg": 70.3, "min_height_km": 515, "max_height_km": 560}])"));
  ASSERT_TRUE(std::holds_alternative<ClassificationConfig>(result))
      << std::get<std::string>(result);
  const auto& groups = std::get<ClassificationConfig>(result).excluded_groups;
  ASSERT_EQ(groups.size(), 2U);
  EXPECT_EQ(groups[0].name, "low");
  EXPECT_EQ(groups[0].reason, ExclusionReason::kLowGroup);
  EXPECT_EQ(groups[1].reason, ExclusionReason::kOldHeight);
  EXPECT_DOUBLE_EQ(groups[1].min_inclination_deg, 69.7);
  EXPECT_DOUBLE_EQ(groups[1].max_height_km, 560.0);
}

TEST(ClassificationConfigFile, RejectsWhatItCannotUse) {
  EXPECT_NE(error_of("not json").find("not valid JSON"), std::string::npos);
  EXPECT_NE(error_of("[]").find("JSON object"), std::string::npos);
  EXPECT_NE(error_of("{}").find("non-empty array"), std::string::npos);
  EXPECT_NE(error_of(R"({"shells": []})").find("non-empty array"), std::string::npos);
  EXPECT_NE(error_of(with_shells(kShell, R"(, "shell_count": 1)")).find("unknown key"),
            std::string::npos);
  // A misspelt key inside a shell is named, not ignored.
  std::string misspelt(kShell);
  misspelt.replace(misspelt.find("min_plane_size"), 14, "min_plane_sise");
  const std::string misspelt_error = error_of(with_shells(misspelt));
  EXPECT_NE(misspelt_error.find("shells[0]"), std::string::npos) << misspelt_error;
  EXPECT_NE(misspelt_error.find("min_plane_sise"), std::string::npos) << misspelt_error;
  // Wrong types.
  std::string text_number(kShell);
  text_number.replace(text_number.find("53.0"), 4, "\"53\"");
  EXPECT_NE(error_of(with_shells(text_number)).find("must be a number"), std::string::npos);
  std::string fractional_size(kShell);
  fractional_size.replace(fractional_size.find("\"min_plane_size\": 10"), 20,
                          "\"min_plane_size\": 1.5");
  EXPECT_NE(error_of(with_shells(fractional_size)).find("whole number"), std::string::npos);
  // Two shells of one name.
  EXPECT_NE(
      error_of(with_shells(std::string(kShell) + "," + std::string(kShell))).find("used twice"),
      std::string::npos);
  // A band check_config rejects.
  std::string inverted(kShell);
  inverted.replace(inverted.find("530.0"), 5, "400.0");
  EXPECT_NE(error_of(with_shells(inverted)).find("height band"), std::string::npos);
  // An unknown or unusable exclusion reason.
  const std::string group_prefix =
      R"(, "excluded_groups": [{"name": "g", "min_inclination_deg": 0, "max_inclination_deg": 1,
           "min_height_km": 1, "max_height_km": 2, "reason": )";
  EXPECT_NE(error_of(with_shells(kShell, group_prefix + "\"weather\"}]")).find("not an exclusion"),
            std::string::npos);
  EXPECT_NE(error_of(with_shells(kShell, group_prefix + "\"no_plane\"}]")).find("reason cannot"),
            std::string::npos);
}

TEST(ClassificationConfigFile, LoadNamesTheFile) {
  const ClassificationConfigOrError missing =
      load_classification_config(std::filesystem::path("no/such/shells.json"));
  ASSERT_TRUE(std::holds_alternative<std::string>(missing));
  EXPECT_NE(std::get<std::string>(missing).find("no/such/shells.json"), std::string::npos);
}

}  // namespace
