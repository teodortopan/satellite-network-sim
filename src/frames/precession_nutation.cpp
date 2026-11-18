#include "satsim/frames/precession_nutation.hpp"

#include <Eigen/Core>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <numbers>
#include <ranges>

#include "nutation_iau1980_series.hpp"
#include "rotation.hpp"
#include "satsim/time/utc_time.hpp"

namespace satsim::frames {

namespace {

constexpr double kPi = std::numbers::pi;
constexpr double kTwoPi = 2.0 * kPi;
constexpr double kArcsecondToRad = kPi / (180.0 * 3600.0);
constexpr double kTenthMilliarcsecondToRad = kArcsecondToRad / 1e4;
constexpr double kJdJ2000 = 2451545.0;
constexpr double kDaysPerJulianCentury = 36525.0;

// Julian centuries of TT since J2000.0, from the two-part date: jd_whole - J2000 is exact
// (both are whole or half days far below 2^52), so only the final sum and division round.
double centuries_since_j2000(const time::UtcTime& tt) noexcept {
  return ((tt.jd_whole() - kJdJ2000) + tt.jd_fraction()) / kDaysPerJulianCentury;
}

// Angle wrapped to [-pi, pi), as ERFA's eraAnpm does.
double wrap_pi(double angle) noexcept {
  double wrapped = std::fmod(angle, kTwoPi);
  if (std::abs(wrapped) >= kPi) {
    wrapped -= std::copysign(kTwoPi, angle);
  }
  return wrapped;
}

// The five Delaunay arguments of the IAU 1980 theory, in radians (Explanatory Supplement
// 1992, section 3.222; same expressions as ERFA nut80.c). Each is a polynomial in arcseconds
// plus a whole number of revolutions per century; the revolutions are reduced with fmod
// before scaling so the large multiple of 2 pi never enters the sum.
struct DelaunayArguments {
  double l;        // Mean anomaly of the Moon.
  double l_prime;  // Mean anomaly of the Sun.
  double f;        // Mean argument of latitude of the Moon (L - Omega).
  double d;        // Mean elongation of the Moon from the Sun.
  double omega;    // Longitude of the Moon's mean ascending node.
};

DelaunayArguments delaunay_arguments(double t) noexcept {
  const auto argument = [t](double c0, double c1, double c2, double c3, double revolutions) {
    return wrap_pi((c0 + (c1 + (c2 + c3 * t) * t) * t) * kArcsecondToRad +
                   std::fmod(revolutions * t, 1.0) * kTwoPi);
  };
  return {
      .l = argument(485866.733, 715922.633, 31.310, 0.064, 1325.0),
      // NOLINTNEXTLINE(modernize-use-std-numbers): IAU coefficient, not Euler's constant.
      .l_prime = argument(1287099.804, 1292581.224, -0.577, -0.012, 99.0),
      .f = argument(335778.877, 295263.137, -13.257, 0.011, 1342.0),
      .d = argument(1072261.307, 1105601.328, -6.891, 0.019, 1236.0),
      .omega = argument(450160.280, -482890.539, 7.455, 0.008, -5.0),
  };
}

// Accumulates one term into (d_psi, d_eps) in units of 0.1 mas.
void add_term(const detail::NutationTerm& term, const DelaunayArguments& a, double t,
              double& longitude, double& obliquity) noexcept {
  const double argument = static_cast<double>(term.l) * a.l +
                          static_cast<double>(term.l_prime) * a.l_prime +
                          static_cast<double>(term.f) * a.f + static_cast<double>(term.d) * a.d +
                          static_cast<double>(term.omega) * a.omega;
  longitude += (term.longitude_sin + term.longitude_sin_per_century * t) * std::sin(argument);
  obliquity += (term.obliquity_cos + term.obliquity_cos_per_century * t) * std::cos(argument);
}

// Indices of the series sorted by decreasing |longitude amplitude|; ties keep table order.
constexpr std::array<std::size_t, kNutationIau1980TermCount> by_decreasing_amplitude() {
  std::array<std::size_t, kNutationIau1980TermCount> order{};
  for (std::size_t i = 0; i < order.size(); ++i) {
    order[i] = i;
  }
  const auto amplitude = [](std::size_t i) {
    const double value = detail::kNutationIau1980Series[i].longitude_sin;
    return value < 0.0 ? -value : value;
  };
  // std::sort is constexpr (std::stable_sort is not), so ties are broken by index explicitly.
  std::sort(order.begin(), order.end(), [&amplitude](std::size_t a, std::size_t b) {
    return amplitude(a) > amplitude(b) || (amplitude(a) == amplitude(b) && a < b);
  });
  return order;
}

constexpr std::array<std::size_t, kNutationIau1980TermCount> kTermsByAmplitude =
    by_decreasing_amplitude();

}  // namespace

PrecessionAngles precession_iau1976(const time::UtcTime& tt) noexcept {
  // Lieske (1979) with the start epoch at J2000.0 (t0 = 0), as eraPrec76(J2000, 0, date):
  // all terms in t0 vanish.
  const double t = centuries_since_j2000(tt);
  const double t_rad = t * kArcsecondToRad;
  constexpr double kW = 2306.2181;
  return {
      .zeta_rad = (kW + (0.30188 + 0.017998 * t) * t) * t_rad,
      .theta_rad = (2004.3109 + (-0.42665 - 0.041833 * t) * t) * t_rad,
      .z_rad = (kW + (1.09468 + 0.018203 * t) * t) * t_rad,
  };
}

Eigen::Matrix3d precession_matrix_iau1976(const time::UtcTime& tt) noexcept {
  const PrecessionAngles angles = precession_iau1976(tt);
  return detail::rot3(-angles.z_rad) *
         (detail::rot2(angles.theta_rad) * detail::rot3(-angles.zeta_rad));
}

double mean_obliquity_iau1980_rad(const time::UtcTime& tt) noexcept {
  const double t = centuries_since_j2000(tt);
  return kArcsecondToRad * (84381.448 + (-46.8150 + (-0.00059 + 0.001813 * t) * t) * t);
}

Nutation nutation_iau1980(const time::UtcTime& tt) noexcept {
  const double t = centuries_since_j2000(tt);
  const DelaunayArguments arguments = delaunay_arguments(t);
  double longitude = 0.0;
  double obliquity = 0.0;
  // Smallest terms first, as ERFA does, so they are not lost against the largest.
  for (const detail::NutationTerm& term : std::views::reverse(detail::kNutationIau1980Series)) {
    add_term(term, arguments, t, longitude, obliquity);
  }
  return {.longitude_rad = longitude * kTenthMilliarcsecondToRad,
          .obliquity_rad = obliquity * kTenthMilliarcsecondToRad};
}

Nutation nutation_iau1980_largest_terms(const time::UtcTime& tt, std::size_t term_count) noexcept {
  const double t = centuries_since_j2000(tt);
  const DelaunayArguments arguments = delaunay_arguments(t);
  double longitude = 0.0;
  double obliquity = 0.0;
  for (std::size_t k = std::min(term_count, kNutationIau1980TermCount); k > 0; --k) {
    add_term(detail::kNutationIau1980Series[kTermsByAmplitude[k - 1]], arguments, t, longitude,
             obliquity);
  }
  return {.longitude_rad = longitude * kTenthMilliarcsecondToRad,
          .obliquity_rad = obliquity * kTenthMilliarcsecondToRad};
}

Eigen::Matrix3d nutation_matrix(double mean_obliquity_rad, const Nutation& nutation) noexcept {
  return detail::rot1(-(mean_obliquity_rad + nutation.obliquity_rad)) *
         (detail::rot3(-nutation.longitude_rad) * detail::rot1(mean_obliquity_rad));
}

double teme_equation_of_equinoxes_rad(double mean_obliquity_rad,
                                      const Nutation& nutation) noexcept {
  return nutation.longitude_rad * std::cos(mean_obliquity_rad);
}

}  // namespace satsim::frames
