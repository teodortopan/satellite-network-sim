#include "satsim/routing/change.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <optional>
#include <vector>

namespace {

using satsim::routing::Change;
using satsim::routing::ChangeKind;
using satsim::routing::classify_change;
using satsim::routing::DirectionStep;
using satsim::routing::name_of;

const std::vector<std::size_t> kRouteA{0, 3, 4, 1};
const std::vector<std::size_t> kRouteA2{0, 3, 4, 1};  // The same nodes, another vector.
const std::vector<std::size_t> kRouteB{0, 5, 4, 1};
const std::vector<std::size_t> kNoRoute;

DirectionStep up(const std::vector<std::size_t>& route, double delay_s) {
  return {.route = route, .up = true, .delay_s = delay_s};
}

DirectionStep down(const std::vector<std::size_t>& route) {
  return {.route = route, .up = false, .delay_s = 0.0};
}

TEST(ChangeTest, SameRouteBothUpIsNone) {
  // The delay may move with the satellites: still no change.
  const Change c = classify_change(up(kRouteA, 0.030), up(kRouteA2, 0.031));
  EXPECT_EQ(c.kind, ChangeKind::kNone);
  EXPECT_EQ(c.size_s, std::nullopt);
}

TEST(ChangeTest, ANewRouteWithLowerDelayIsABadChange) {
  // 0.375 -> 0.25: size 0.25 - 0.375 = -0.125 (exact in binary).
  const Change c = classify_change(up(kRouteA, 0.375), up(kRouteB, 0.25));
  EXPECT_EQ(c.kind, ChangeKind::kBadChange);
  EXPECT_EQ(c.size_s, std::optional(-0.125));
}

TEST(ChangeTest, ANewRouteWithEqualOrHigherDelayIsADelayIncrease) {
  const Change higher = classify_change(up(kRouteA, 0.25), up(kRouteB, 0.375));
  EXPECT_EQ(higher.kind, ChangeKind::kDelayIncreaseChange);
  EXPECT_EQ(higher.size_s, std::optional(0.125));
  const Change equal = classify_change(up(kRouteA, 0.25), up(kRouteB, 0.25));
  EXPECT_EQ(equal.kind, ChangeKind::kDelayIncreaseChange);
  EXPECT_EQ(equal.size_s, std::optional(0.0));
}

TEST(ChangeTest, UpThenDownIsABreak) {
  for (const DirectionStep& current : {down(kRouteA), down(kNoRoute), down(kRouteB)}) {
    const Change c = classify_change(up(kRouteA, 0.03), current);
    EXPECT_EQ(c.kind, ChangeKind::kBreak);
    EXPECT_EQ(c.size_s, std::nullopt);
  }
}

TEST(ChangeTest, DownThenUpIsARecover) {
  // The same route coming back up, a new route after no route, or after another route.
  for (const DirectionStep& previous : {down(kRouteA), down(kNoRoute), down(kRouteB)}) {
    const Change c = classify_change(previous, up(kRouteA, 0.03));
    EXPECT_EQ(c.kind, ChangeKind::kRecover);
    EXPECT_EQ(c.size_s, std::nullopt);
  }
}

TEST(ChangeTest, DownThenDownIsStillDown) {
  EXPECT_EQ(classify_change(down(kNoRoute), down(kNoRoute)).kind, ChangeKind::kStillDown);
  EXPECT_EQ(classify_change(down(kRouteA), down(kRouteB)).kind, ChangeKind::kStillDown);
  EXPECT_EQ(classify_change(down(kRouteA), down(kNoRoute)).size_s, std::nullopt);
}

TEST(ChangeTest, Names) {
  EXPECT_EQ(name_of(ChangeKind::kNone), "none");
  EXPECT_EQ(name_of(ChangeKind::kBadChange), "bad_change");
  EXPECT_EQ(name_of(ChangeKind::kDelayIncreaseChange), "delay_increase_change");
  EXPECT_EQ(name_of(ChangeKind::kBreak), "break");
  EXPECT_EQ(name_of(ChangeKind::kRecover), "recover");
  EXPECT_EQ(name_of(ChangeKind::kStillDown), "still_down");
}

}  // namespace
