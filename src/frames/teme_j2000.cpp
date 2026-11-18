#include "satsim/frames/teme_j2000.hpp"

#include <Eigen/Core>

#include "rotation.hpp"
#include "satsim/frames/precession_nutation.hpp"
#include "satsim/time/utc_time.hpp"

namespace satsim::frames {

Eigen::Matrix3d teme_to_j2000_matrix(const time::UtcTime& tt) noexcept {
  return teme_to_j2000_matrix(tt, nutation_iau1980(tt));
}

Eigen::Matrix3d teme_to_j2000_matrix(const time::UtcTime& tt, const Nutation& nutation) noexcept {
  const double mean_obliquity = mean_obliquity_iau1980_rad(tt);
  const double equation_of_equinoxes = teme_equation_of_equinoxes_rad(mean_obliquity, nutation);
  // r_J2000 = P^T N^T R3(-eqe) r_TEME.
  return precession_matrix_iau1976(tt).transpose() *
         (nutation_matrix(mean_obliquity, nutation).transpose() *
          detail::rot3(-equation_of_equinoxes));
}

Eigen::Vector3d teme_to_j2000(const Eigen::Vector3d& vector_teme,
                              const time::UtcTime& tt) noexcept {
  return teme_to_j2000_matrix(tt) * vector_teme;
}

Eigen::Vector3d j2000_to_teme(const Eigen::Vector3d& vector_j2000,
                              const time::UtcTime& tt) noexcept {
  return teme_to_j2000_matrix(tt).transpose() * vector_j2000;
}

}  // namespace satsim::frames
