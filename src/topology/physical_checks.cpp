#include "satsim/topology/physical_checks.hpp"

#include <Eigen/Core>
#include <Eigen/Geometry>
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <optional>
#include <span>
#include <utility>
#include <variant>
#include <vector>

#include "satsim/constellation/classification.hpp"
#include "satsim/frames/wgs84.hpp"
#include "satsim/ground/sites.hpp"
#include "satsim/topology/geometry.hpp"
#include "satsim/topology/ground_links.hpp"
#include "satsim/topology/link_plan.hpp"
#include "satsim/topology/link_state.hpp"
#include "satsim/topology/positions.hpp"

namespace satsim::topology {

namespace {

constexpr double kAngleToleranceRad = 1e-9;
constexpr double kLengthToleranceKm = 1e-9;

// The distance from the origin to the segment ab, computed differently from the builder's
// segment_min_radius_km (which projects onto the segment): the distance to the infinite line
// is |a x b| / |b - a|, and it applies only when the foot of the perpendicular lies between a
// and b, which is when the angles at both a and b are acute (a . (a - b) > 0 and
// b . (b - a) > 0). Otherwise the nearer endpoint is closest.
double independent_min_radius_km(const Eigen::Vector3d& a, const Eigen::Vector3d& b) {
  const Eigen::Vector3d ab = b - a;
  const double length = ab.norm();
  if (length == 0.0 || a.dot(-ab) <= 0.0 || b.dot(ab) <= 0.0) {
    return std::min(a.norm(), b.norm());
  }
  return a.cross(b).norm() / length;
}

// The elevation of `satellite` from a site at geodetic latitude `lat`, longitude `lon` on the
// ellipsoid, through the local east-north-up frame, rather than the builder's normal vector:
// the up component of the site-to-satellite vector against its length.
double independent_elevation_rad(const ground::GroundSite& site, const Eigen::Vector3d& satellite) {
  const double sin_lat = std::sin(site.latitude_rad);
  const double cos_lat = std::cos(site.latitude_rad);
  const double sin_lon = std::sin(site.longitude_rad);
  const double cos_lon = std::cos(site.longitude_rad);
  // Site position recomputed from its geodetic coordinates (height 0).
  const double n_km = frames::kWgs84EquatorialRadiusKm /
                      std::sqrt(1.0 - (frames::kWgs84EccentricitySquared * sin_lat * sin_lat));
  const Eigen::Vector3d site_km(n_km * cos_lat * cos_lon, n_km * cos_lat * sin_lon,
                                n_km * (1.0 - frames::kWgs84EccentricitySquared) * sin_lat);
  const Eigen::Vector3d d = satellite - site_km;
  const double east = (-sin_lon * d.x()) + (cos_lon * d.y());
  const double north =
      (-sin_lat * cos_lon * d.x()) - (sin_lat * sin_lon * d.y()) + (cos_lat * d.z());
  const double up = (cos_lat * cos_lon * d.x()) + (cos_lat * sin_lon * d.y()) + (sin_lat * d.z());
  return std::atan2(up, std::hypot(east, north));
}

const Eigen::Vector3d* position_of(const StepPositions& positions, std::size_t satellite) {
  if (satellite >= positions.position_ecef_km.size()) {
    return nullptr;
  }
  const std::optional<Eigen::Vector3d>& p = positions.position_ecef_km[satellite];
  return p.has_value() ? &*p : nullptr;
}

// The shell of a plane member, or nullopt for anything else (out of range, excluded, or a
// plane member without a shell, which classification never produces).
std::optional<std::size_t> member_shell(const constellation::Classification& c,
                                        std::size_t satellite) {
  if (satellite >= c.satellites.size() ||
      !std::holds_alternative<constellation::PlaneAssignment>(c.satellites[satellite].placement)) {
    return std::nullopt;
  }
  return c.satellites[satellite].shell_index;
}

}  // namespace

PhysicalCheckCounts run_physical_checks(const constellation::Classification& classification,
                                        const LinkPlan& plan, const StepLinks& links,
                                        const StepPositions& positions, const LinkLimits& limits,
                                        std::span<const ground::GroundSite> sites,
                                        std::span<const std::vector<VisibleSatellite>> visible,
                                        const ElevationMaskSettings& mask_settings) {
  PhysicalCheckCounts counts;
  const std::size_t satellite_count = classification.satellites.size();
  if (links.states.size() != plan.links.size() || plan.lasers.size() != satellite_count ||
      positions.position_ecef_km.size() != satellite_count || visible.size() != sites.size()) {
    ++counts.structure;
    return counts;
  }

  const double min_radius_km = line_of_sight_radius_km(limits.atmosphere_km);
  std::vector<std::size_t> links_per_satellite(satellite_count, 0);
  std::vector<std::pair<std::size_t, std::size_t>> pairs;
  pairs.reserve(plan.links.size());
  for (std::size_t i = 0; i < plan.links.size(); ++i) {
    const PlannedLink& link = plan.links[i];
    const std::size_t a = link.satellite_a;
    const std::size_t b = link.satellite_b;
    if (a >= satellite_count || b >= satellite_count) {
      ++counts.structure;
      continue;
    }
    ++links_per_satellite[a];
    ++links_per_satellite[b];

    // Symmetry: no self-link, no pair twice, and each end records this link exactly once, at
    // the side the link says.
    if (a == b) {
      ++counts.symmetry;
    }
    pairs.emplace_back(std::min(a, b), std::max(a, b));
    for (const auto& [end, side] : {std::pair{a, link.side_a}, std::pair{b, link.side_b}}) {
      const auto records = std::ranges::count_if(
          plan.lasers[end], [&](const Laser& l) { return l.link == i && l.side == side; });
      if (records != 1) {
        ++counts.symmetry;
      }
    }

    // Shells: both ends plane members of the same shell, the link's recorded shell.
    const std::optional<std::size_t> shell_a = member_shell(classification, a);
    const std::optional<std::size_t> shell_b = member_shell(classification, b);
    if (!shell_a.has_value() || !shell_b.has_value() || *shell_a != *shell_b ||
        *shell_a != link.shell_index) {
      ++counts.shells;
    }

    // Line of sight and range, for links marked up.
    if (!links.states[i].up()) {
      continue;
    }
    const auto* pa = position_of(positions, a);
    const auto* pb = position_of(positions, b);
    if (pa == nullptr || pb == nullptr) {
      ++counts.line_of_sight;
      continue;
    }
    if (independent_min_radius_km(*pa, *pb) < min_radius_km - kLengthToleranceKm) {
      ++counts.line_of_sight;
    }
    const std::optional<double>& cap_km = limits.range_cap_km;
    if (cap_km.has_value() && (*pa - *pb).norm() > *cap_km + kLengthToleranceKm) {
      ++counts.range_cap;
    }
  }

  // A pair of satellites linked more than once.
  std::ranges::sort(pairs);
  for (std::size_t i = 1; i < pairs.size(); ++i) {
    if (pairs[i] == pairs[i - 1]) {
      ++counts.symmetry;
    }
  }

  // Every laser record must name an existing link that has this satellite at that side.
  for (std::size_t s = 0; s < satellite_count; ++s) {
    for (const Laser& l : plan.lasers[s]) {
      if (!l.link) {
        continue;
      }
      if (*l.link >= plan.links.size()) {
        ++counts.symmetry;
        continue;
      }
      const PlannedLink& link = plan.links[*l.link];
      const bool at_a = link.satellite_a == s && link.side_a == l.side;
      const bool at_b = link.satellite_b == s && link.side_b == l.side;
      if (!at_a && !at_b) {
        ++counts.symmetry;
      }
    }
  }

  const std::size_t budget = laser_budget(plan.pattern);
  counts.laser_budget = static_cast<std::size_t>(
      std::ranges::count_if(links_per_satellite, [&](std::size_t n) { return n > budget; }));

  // Ground links.
  const std::vector<std::optional<double>> heights = shell_mean_heights_km(classification);
  for (std::size_t site = 0; site < sites.size(); ++site) {
    for (const VisibleSatellite& v : visible[site]) {
      const auto* p = position_of(positions, v.satellite);
      const std::optional<std::size_t> shell = member_shell(classification, v.satellite);
      if (!shell.has_value() || p == nullptr || *shell >= heights.size()) {
        ++counts.mask;
        continue;
      }
      const double mask = elevation_mask_rad(heights[*shell].value_or(0.0),
                                             sites[site].latitude_rad, mask_settings);
      if (independent_elevation_rad(sites[site], *p) < mask - kAngleToleranceRad) {
        ++counts.mask;
      }
    }
  }
  return counts;
}

}  // namespace satsim::topology
