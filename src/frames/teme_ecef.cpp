#include "satsim/frames/teme_ecef.hpp"

#include <Eigen/Core>
#include <cmath>

#include "satsim/time/gmst.hpp"
#include "satsim/time/utc_time.hpp"

namespace satsim::frames {

Eigen::Vector3d teme_to_ecef(const Eigen::Vector3d& position_teme_km,
                             const time::UtcTime& t) noexcept {
  // R3(theta) = [[cos, sin, 0], [-sin, cos, 0], [0, 0, 1]]: the Earth-fixed axes have turned
  // by +theta, so a fixed inertial vector appears turned by -theta.
  const double gmst = time::gmst_rad(t);
  const double c = std::cos(gmst);
  const double s = std::sin(gmst);
  const double x = position_teme_km.x();
  const double y = position_teme_km.y();
  return {c * x + s * y, -s * x + c * y, position_teme_km.z()};
}

}  // namespace satsim::frames
