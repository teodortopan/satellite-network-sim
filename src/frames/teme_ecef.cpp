#include "satsim/frames/teme_ecef.hpp"

#include <Eigen/Core>
#include <cmath>

#include "satsim/time/earth_orientation.hpp"
#include "satsim/time/gmst.hpp"
#include "satsim/time/time_scales.hpp"
#include "satsim/time/utc_time.hpp"

namespace satsim::frames {

Eigen::Vector3d teme_to_pef(const Eigen::Vector3d& position_teme_km,
                            const time::UtcTime& ut1) noexcept {
  // R3(theta) = [[cos, sin, 0], [-sin, cos, 0], [0, 0, 1]]: the Earth-fixed axes have turned
  // by +theta, so a fixed inertial vector appears turned by -theta.
  const double gmst = time::gmst_rad(ut1);
  const double c = std::cos(gmst);
  const double s = std::sin(gmst);
  const double x = position_teme_km.x();
  const double y = position_teme_km.y();
  return {c * x + s * y, -s * x + c * y, position_teme_km.z()};
}

Eigen::Vector3d pef_to_itrf(const Eigen::Vector3d& position_pef_km, double polar_x_rad,
                            double polar_y_rad) noexcept {
  // ROT1(-y_p) = [[1, 0, 0], [0, cy, -sy], [0, sy, cy]] is applied first, then
  // ROT2(-x_p) = [[cx, 0, sx], [0, 1, 0], [-sx, 0, cx]].
  const double cx = std::cos(polar_x_rad);
  const double sx = std::sin(polar_x_rad);
  const double cy = std::cos(polar_y_rad);
  const double sy = std::sin(polar_y_rad);
  const double x = position_pef_km.x();
  const double y1 = cy * position_pef_km.y() - sy * position_pef_km.z();
  const double z1 = sy * position_pef_km.y() + cy * position_pef_km.z();
  return {cx * x + sx * z1, y1, -sx * x + cx * z1};
}

Eigen::Vector3d teme_to_ecef(const Eigen::Vector3d& position_teme_km, const time::UtcTime& utc,
                             const time::EarthOrientation& earth_orientation) noexcept {
  const time::UtcTime ut1 = time::ut1_from_utc(utc, earth_orientation.ut1_minus_utc_s);
  return pef_to_itrf(teme_to_pef(position_teme_km, ut1), earth_orientation.polar_x_rad,
                     earth_orientation.polar_y_rad);
}

}  // namespace satsim::frames
