#include "satsim/core/angles.hpp"

#include <cmath>
#include <numbers>

namespace satsim {

namespace {
constexpr double kTwoPi = 2.0 * std::numbers::pi;
constexpr double kDegPerRad = 180.0 / std::numbers::pi;
constexpr double kRadPerDeg = std::numbers::pi / 180.0;
}  // namespace

double deg_to_rad(double degrees) noexcept { return degrees * kRadPerDeg; }

double rad_to_deg(double radians) noexcept { return radians * kDegPerRad; }

double wrap_two_pi(double radians) noexcept {
  // fmod is exact and returns a value in (-2*pi, 2*pi) with the sign of the input.
  double wrapped = std::fmod(radians, kTwoPi);
  if (wrapped < 0.0) {
    wrapped += kTwoPi;
  }
  // Adding 2*pi to a tiny negative remainder (e.g. -1e-20) rounds to exactly 2*pi;
  // the nearest representable value inside the range is then 0.
  if (wrapped >= kTwoPi) {
    wrapped = 0.0;
  }
  // Normalise -0.0 (from fmod of a negative multiple of 2*pi) to +0.0.
  return wrapped == 0.0 ? 0.0 : wrapped;
}

}  // namespace satsim
