#include "satsim/core/angles.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cmath>
#include <limits>
#include <numbers>

namespace {

using satsim::deg_to_rad;
using satsim::rad_to_deg;
using satsim::wrap_two_pi;

constexpr double kPi = std::numbers::pi;
constexpr double kTwoPi = 2.0 * std::numbers::pi;
constexpr double kEps = std::numeric_limits<double>::epsilon();

// Known conversions. Each is one multiplication by a rounded constant, so the result is
// within a few ULPs of the exact value; EXPECT_DOUBLE_EQ allows 4 ULPs.
TEST(AnglesTest, DegToRadKnownValues) {
  EXPECT_EQ(deg_to_rad(0.0), 0.0);
  EXPECT_DOUBLE_EQ(deg_to_rad(90.0), kPi / 2.0);
  EXPECT_DOUBLE_EQ(deg_to_rad(180.0), kPi);
  EXPECT_DOUBLE_EQ(deg_to_rad(360.0), kTwoPi);
  EXPECT_DOUBLE_EQ(deg_to_rad(-45.0), -kPi / 4.0);
}

TEST(AnglesTest, RadToDegKnownValues) {
  EXPECT_EQ(rad_to_deg(0.0), 0.0);
  EXPECT_DOUBLE_EQ(rad_to_deg(kPi / 2.0), 90.0);
  EXPECT_DOUBLE_EQ(rad_to_deg(kPi), 180.0);
  EXPECT_DOUBLE_EQ(rad_to_deg(kTwoPi), 360.0);
  EXPECT_DOUBLE_EQ(rad_to_deg(-kPi / 4.0), -45.0);
}

// Two multiplications, each by a constant with up to 0.5 ULP error, plus two roundings:
// the relative error is bounded by about 4 epsilon. Allow 8 epsilon of relative error.
TEST(AnglesTest, DegRadRoundTrip) {
  constexpr std::array kDegrees{-720.5, -180.0, -1.0, 0.0, 1e-9, 0.25, 33.3, 359.999, 1e6};
  for (const double degrees : kDegrees) {
    EXPECT_NEAR(rad_to_deg(deg_to_rad(degrees)), degrees, 8.0 * kEps * std::abs(degrees))
        << "degrees = " << degrees;
  }
}

TEST(AnglesTest, WrapZeroAndFullTurnGiveZero) {
  EXPECT_EQ(wrap_two_pi(0.0), 0.0);
  EXPECT_EQ(wrap_two_pi(kTwoPi), 0.0);
  EXPECT_EQ(wrap_two_pi(-kTwoPi), 0.0);
  EXPECT_EQ(wrap_two_pi(4.0 * kTwoPi), 0.0);
}

TEST(AnglesTest, WrapNegativeZeroGivesPositiveZero) {
  EXPECT_FALSE(std::signbit(wrap_two_pi(-0.0)));
  EXPECT_FALSE(std::signbit(wrap_two_pi(-kTwoPi)));
}

TEST(AnglesTest, WrapLeavesInRangeValuesUnchanged) {
  EXPECT_EQ(wrap_two_pi(1.0), 1.0);
  EXPECT_EQ(wrap_two_pi(kPi), kPi);
  const double just_below = std::nextafter(kTwoPi, 0.0);
  EXPECT_EQ(wrap_two_pi(just_below), just_below);
}

// Adding 2*pi to a negative value rounds once, so the result is within half an ULP of
// the exact sum; EXPECT_DOUBLE_EQ covers that.
TEST(AnglesTest, WrapNegatives) {
  EXPECT_DOUBLE_EQ(wrap_two_pi(-kPi / 2.0), 3.0 * kPi / 2.0);
  EXPECT_DOUBLE_EQ(wrap_two_pi(-kPi), kPi);
  EXPECT_DOUBLE_EQ(wrap_two_pi(-1.0), kTwoPi - 1.0);
}

// Naive fmod + 2*pi rounds these up to exactly 2*pi, which is outside the range.
TEST(AnglesTest, WrapTinyNegativesStayBelowTwoPi) {
  constexpr std::array kTiny{-1e-20, -1e-300, -std::numeric_limits<double>::denorm_min(), -1e-17};
  for (const double radians : kTiny) {
    const double wrapped = wrap_two_pi(radians);
    EXPECT_GE(wrapped, 0.0) << "radians = " << radians;
    EXPECT_LT(wrapped, kTwoPi) << "radians = " << radians;
  }
}

// x + n*2*pi is computed in double before wrapping, so its rounding error (about
// ULP(n*2*pi), roughly 1e-12 at n = 1000) dominates. 1e-9 leaves a wide margin.
TEST(AnglesTest, WrapLargeMultiples) {
  constexpr double kOffset = 0.75;
  EXPECT_NEAR(wrap_two_pi(kOffset + 1000.0 * kTwoPi), kOffset, 1e-9);
  EXPECT_NEAR(wrap_two_pi(kOffset - 1000.0 * kTwoPi), kOffset, 1e-9);
  EXPECT_NEAR(wrap_two_pi(kOffset + 3.0 * kTwoPi), kOffset, 1e-12);
}

TEST(AnglesTest, WrapAlwaysInRange) {
  constexpr std::array kInputs{-1e9,  -12345.678,        -7.0, -kTwoPi + 1e-15, -1e-12,
                               1e-12, 6.283185307179586, 7.0,  12345.678,       1e9};
  for (const double radians : kInputs) {
    const double wrapped = wrap_two_pi(radians);
    EXPECT_GE(wrapped, 0.0) << "radians = " << radians;
    EXPECT_LT(wrapped, kTwoPi) << "radians = " << radians;
  }
}

}  // namespace
