#include "satsim/constellation/geometry.hpp"

#include <Eigen/Core>
#include <Eigen/Geometry>
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <numbers>
#include <numeric>
#include <optional>
#include <span>
#include <string_view>
#include <variant>
#include <vector>

#include "satsim/core/angles.hpp"
#include "satsim/frames/geodetic.hpp"
#include "satsim/orbit/propagator.hpp"
#include "satsim/time/utc_time.hpp"

namespace satsim::constellation {

namespace {

constexpr double kTwoPi = 2.0 * std::numbers::pi;
constexpr double kSecondsPerDay = 86400.0;
// Below this, sin(inclination) is treated as zero: the orbit is equatorial and has no node.
constexpr double kEquatorialSine = 1e-12;

// The unit vector towards the ascending node of the plane with unit normal `normal`.
Eigen::Vector3d node_direction(const Eigen::Vector3d& normal) noexcept {
  const Eigen::Vector3d node(-normal.y(), normal.x(), 0.0);  // z x normal.
  const double length = node.norm();
  if (length <= kEquatorialSine) {
    return Eigen::Vector3d::UnitX();
  }
  return node / length;
}

// The states of one window: `count` samples evenly spaced over `period_s`, centred on `centre`.
struct Window {
  std::vector<Eigen::Vector3d> positions_km;
  std::vector<double> offsets_s;  // From the centre.
  Eigen::Vector3d normal_sum = Eigen::Vector3d::Zero();
  double height_sum_km = 0.0;
  double radius_sum_km = 0.0;
};

std::variant<Window, orbit::PropagationError> sample_window(const orbit::Propagator& propagator,
                                                            const time::UtcTime& centre,
                                                            double period_s, std::size_t count) {
  Window window;
  window.positions_km.reserve(count);
  window.offsets_s.reserve(count);
  const auto n = static_cast<double>(count);
  for (std::size_t k = 0; k < count; ++k) {
    const double offset_s = ((static_cast<double>(k) + 0.5) / n - 0.5) * period_s;
    const orbit::StateOrError result = propagator.propagate(centre.plus_seconds(offset_s));
    if (const auto* error = std::get_if<orbit::PropagationError>(&result)) {
      return *error;
    }
    const auto& state = std::get<orbit::TemeState>(result);
    window.normal_sum += orbit_normal_teme(state.position_km, state.velocity_km_s);
    // Geodetic height is unchanged by the rotation about z between TEME and PEF; see
    // OrbitGeometry::mean_height_km.
    window.height_sum_km += frames::ecef_to_geodetic(state.position_km).height_km;
    window.radius_sum_km += state.position_km.norm();
    window.positions_km.push_back(state.position_km);
    window.offsets_s.push_back(offset_s);
  }
  return window;
}

}  // namespace

Eigen::Vector3d orbit_normal_teme(const Eigen::Vector3d& position_teme_km,
                                  const Eigen::Vector3d& velocity_teme_km_s) noexcept {
  return position_teme_km.cross(velocity_teme_km_s).normalized();
}

double inclination_rad(const Eigen::Vector3d& normal_teme) noexcept {
  return std::acos(std::clamp(normal_teme.z(), -1.0, 1.0));
}

double raan_teme_rad(const Eigen::Vector3d& normal_teme) noexcept {
  const Eigen::Vector3d node = node_direction(normal_teme);
  return wrap_two_pi(std::atan2(node.y(), node.x()));
}

double argument_of_latitude_rad(const Eigen::Vector3d& position_teme_km,
                                const Eigen::Vector3d& normal_teme) noexcept {
  const Eigen::Vector3d node = node_direction(normal_teme);
  // In-plane axis 90 degrees ahead of the node, in the direction of motion.
  const Eigen::Vector3d ahead = normal_teme.cross(node);
  return wrap_two_pi(std::atan2(position_teme_km.dot(ahead), position_teme_km.dot(node)));
}

double wrap_pi(double radians) noexcept {
  return wrap_two_pi(radians + std::numbers::pi) - std::numbers::pi;
}

std::optional<double> circular_mean_rad(std::span<const double> angles_rad) noexcept {
  if (angles_rad.empty()) {
    return std::nullopt;
  }
  double sum_cos = 0.0;
  double sum_sin = 0.0;
  for (const double a : angles_rad) {
    sum_cos += std::cos(a);
    sum_sin += std::sin(a);
  }
  if (std::hypot(sum_cos, sum_sin) <= 1e-12 * static_cast<double>(angles_rad.size())) {
    return std::nullopt;
  }
  return wrap_two_pi(std::atan2(sum_sin, sum_cos));
}

std::vector<std::vector<std::size_t>> cluster_by_circular_gaps(
    std::span<const double> angles_rad, std::span<const std::int64_t> tie_keys,
    double gap_threshold_rad) {
  const std::size_t n = angles_rad.size();
  std::vector<std::vector<std::size_t>> groups;
  if (n == 0) {
    return groups;
  }
  std::vector<double> wrapped(n);
  std::transform(angles_rad.begin(), angles_rad.end(), wrapped.begin(),
                 [](double a) { return wrap_two_pi(a); });
  std::vector<std::size_t> order(n);
  std::iota(order.begin(), order.end(), std::size_t{0});
  std::sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) {
    if (wrapped[a] != wrapped[b]) {
      return wrapped[a] < wrapped[b];
    }
    if (tie_keys[a] != tie_keys[b]) {
      return tie_keys[a] < tie_keys[b];
    }
    return a < b;
  });

  // gap_before[i]: the gap from the previous member in sorted order to member i, the first
  // member's measured from the last one across 0 / 2*pi.
  std::vector<bool> starts_group(n, false);
  bool any_cut = false;
  for (std::size_t i = 0; i < n; ++i) {
    const double previous = i == 0 ? wrapped[order[n - 1]] - kTwoPi : wrapped[order[i - 1]];
    if (n > 1 && wrapped[order[i]] - previous > gap_threshold_rad) {
      starts_group[i] = true;
      any_cut = true;
    }
  }
  if (!any_cut) {
    groups.emplace_back(order.begin(), order.end());
    return groups;
  }
  // Start at the first cut, go once around the circle.
  const std::size_t first = static_cast<std::size_t>(
      std::find(starts_group.begin(), starts_group.end(), true) - starts_group.begin());
  for (std::size_t step = 0; step < n; ++step) {
    const std::size_t i = (first + step) % n;
    if (starts_group[i]) {
      groups.emplace_back();
    }
    groups.back().push_back(order[i]);
  }
  return groups;
}

OrbitGeometryOrError measure_orbit_geometry(const orbit::Propagator& propagator,
                                            const time::UtcTime& reference_utc, double period_s,
                                            const GeometrySampling& sampling) {
  auto sampled = sample_window(propagator, reference_utc, period_s, sampling.samples_per_orbit);
  if (const auto* error = std::get_if<orbit::PropagationError>(&sampled)) {
    return *error;
  }
  const Window& window = std::get<Window>(sampled);
  const orbit::StateOrError at_reference = propagator.propagate(reference_utc);
  if (const auto* error = std::get_if<orbit::PropagationError>(&at_reference)) {
    return *error;
  }
  const auto& state = std::get<orbit::TemeState>(at_reference);

  const auto n = static_cast<double>(sampling.samples_per_orbit);
  OrbitGeometry geometry;
  geometry.mean_normal_teme = window.normal_sum.normalized();
  geometry.inclination_rad = inclination_rad(geometry.mean_normal_teme);
  geometry.raan_teme_rad = raan_teme_rad(geometry.mean_normal_teme);
  geometry.mean_height_km = window.height_sum_km / n;
  geometry.mean_radius_km = window.radius_sum_km / n;

  // The offsets are symmetric about the reference time (they sum to zero), so the least-squares
  // line through (offset, unwrapped argument of latitude) takes the mean value there.
  double unwrapped = argument_of_latitude_rad(window.positions_km[0], geometry.mean_normal_teme);
  double previous = unwrapped;
  double sum = unwrapped;
  for (std::size_t k = 1; k < window.positions_km.size(); ++k) {
    const double u = argument_of_latitude_rad(window.positions_km[k], geometry.mean_normal_teme);
    unwrapped += wrap_pi(u - previous);  // One step is 2*pi / n < pi ahead.
    previous = u;
    sum += unwrapped;
  }
  geometry.argument_of_latitude_rad = wrap_two_pi(sum / n);

  const Eigen::Vector3d normal = orbit_normal_teme(state.position_km, state.velocity_km_s);
  geometry.osculating_inclination_rad = inclination_rad(normal);
  geometry.osculating_raan_teme_rad = raan_teme_rad(normal);
  geometry.osculating_argument_of_latitude_rad =
      argument_of_latitude_rad(state.position_km, normal);

  const auto later =
      sample_window(propagator, reference_utc.plus_seconds(sampling.drift_baseline_s), period_s,
                    sampling.samples_per_orbit);
  if (const auto* drift_window = std::get_if<Window>(&later)) {
    const double later_raan_rad = raan_teme_rad(drift_window->normal_sum.normalized());
    geometry.raan_drift_rad_per_day = wrap_pi(later_raan_rad - geometry.raan_teme_rad) /
                                      (sampling.drift_baseline_s / kSecondsPerDay);
  }
  return geometry;
}

std::optional<Launch> launch_of(std::string_view international_designator) noexcept {
  const std::string_view id = international_designator;
  constexpr std::size_t kPieceStart = 8;  // "YYYY-NNN" then a piece of one to three letters.
  constexpr std::size_t kMaxLength = kPieceStart + 3;
  if (id.size() <= kPieceStart || id.size() > kMaxLength || id[4] != '-') {
    return std::nullopt;
  }
  const auto digits = [&](std::size_t begin, std::size_t end) -> std::optional<int> {
    int value = 0;
    for (std::size_t i = begin; i < end; ++i) {
      if (id[i] < '0' || id[i] > '9') {
        return std::nullopt;
      }
      value = value * 10 + (id[i] - '0');
    }
    return value;
  };
  const std::optional<int> year = digits(0, 4);
  const std::optional<int> number = digits(5, kPieceStart);
  if (!year || !number) {
    return std::nullopt;
  }
  for (std::size_t i = kPieceStart; i < id.size(); ++i) {
    if (id[i] < 'A' || id[i] > 'Z') {
      return std::nullopt;
    }
  }
  return Launch{.year = *year, .number = *number};
}

}  // namespace satsim::constellation
