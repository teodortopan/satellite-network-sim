#pragma once

#include <Eigen/Core>
#include <cmath>

// Elementary frame rotations, internal to satsim::frames. ROTi(a) gives the coordinates of
// a fixed vector in axes turned by +a about axis i (Vallado's ROT1/ROT2/ROT3; ERFA's
// eraRx/eraRy/eraRz). A vector at angle phi from the x-axis in the old axes is at phi - a in
// the new ones.
namespace satsim::frames::detail {

inline Eigen::Matrix3d rot1(double angle_rad) noexcept {
  const double c = std::cos(angle_rad);
  const double s = std::sin(angle_rad);
  Eigen::Matrix3d m;
  m << 1.0, 0.0, 0.0, 0.0, c, s, 0.0, -s, c;
  return m;
}

inline Eigen::Matrix3d rot2(double angle_rad) noexcept {
  const double c = std::cos(angle_rad);
  const double s = std::sin(angle_rad);
  Eigen::Matrix3d m;
  m << c, 0.0, -s, 0.0, 1.0, 0.0, s, 0.0, c;
  return m;
}

inline Eigen::Matrix3d rot3(double angle_rad) noexcept {
  const double c = std::cos(angle_rad);
  const double s = std::sin(angle_rad);
  Eigen::Matrix3d m;
  m << c, s, 0.0, -s, c, 0.0, 0.0, 0.0, 1.0;
  return m;
}

}  // namespace satsim::frames::detail
