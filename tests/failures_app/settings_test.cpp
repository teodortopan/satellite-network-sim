#include "failures/settings.hpp"

#include <gtest/gtest.h>

#include <limits>
#include <optional>
#include <string>

namespace {

using satsim::failures_app::check_reaction_times;
using satsim::failures_app::ReactionTimes;

TEST(SettingsTest, DefaultsAreTheSpecs) {
  const ReactionTimes t;
  EXPECT_EQ(t.detection_s, 0.1);
  EXPECT_EQ(t.hop_processing_s, 0.001);
  EXPECT_EQ(t.compute_s, 0.1);
  EXPECT_EQ(t.planner_processing_s, 0.0);
  EXPECT_EQ(t.acquisition_s, 30.0);
  EXPECT_FALSE(t.repoint);
  EXPECT_EQ(check_reaction_times(t), std::nullopt);
}

TEST(SettingsTest, EveryTimeMustBeFiniteAndNotNegative) {
  EXPECT_EQ(check_reaction_times(ReactionTimes{.detection_s = 0.0,
                                               .hop_processing_s = 0.0,
                                               .compute_s = 0.0,
                                               .planner_processing_s = 0.0,
                                               .acquisition_s = 0.0,
                                               .repoint = true}),
            std::nullopt);
  ReactionTimes t;
  t.detection_s = -0.1;
  EXPECT_EQ(check_reaction_times(t), std::string("detection_s must be finite and at least 0"));
  t = {};
  t.hop_processing_s = std::numeric_limits<double>::infinity();
  EXPECT_EQ(check_reaction_times(t), std::string("hop_processing_s must be finite and at least 0"));
  t = {};
  t.compute_s = std::numeric_limits<double>::quiet_NaN();
  EXPECT_EQ(check_reaction_times(t), std::string("compute_s must be finite and at least 0"));
  t = {};
  t.planner_processing_s = -1.0;
  EXPECT_EQ(check_reaction_times(t),
            std::string("planner_processing_s must be finite and at least 0"));
  t = {};
  t.acquisition_s = -2.0;
  EXPECT_EQ(check_reaction_times(t), std::string("acquisition_s must be finite and at least 0"));
}

}  // namespace
