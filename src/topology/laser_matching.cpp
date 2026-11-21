#include "laser_matching.hpp"

#include <Eigen/Core>
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <vector>

#include "satsim/constellation/classification.hpp"
#include "satsim/topology/geometry.hpp"
#include "satsim/topology/link_plan.hpp"

namespace satsim::topology::detail {

MatchEnd match_end(std::size_t satellite, const Eigen::Vector3d& position_ecef_km,
                   double limit_km) noexcept {
  const double x = position_ecef_km.x();
  const double y = position_ecef_km.y();
  const double z = position_ecef_km.z();
  const double r_squared = (x * x) + (y * y) + (z * z);
  const double t_squared = r_squared - (limit_km * limit_km);
  return MatchEnd{.satellite = satellite,
                  .x = x,
                  .y = y,
                  .z = z,
                  .tangent_km = t_squared > 0.0 ? std::sqrt(t_squared) : 0.0};
}

std::optional<double> in_sight_distance_km(const MatchEnd& a, const MatchEnd& b,
                                           double limit_km) noexcept {
  const double dx = b.x - a.x;
  const double dy = b.y - a.y;
  const double dz = b.z - a.z;
  const double length_squared = (dx * dx) + (dy * dy) + (dz * dz);
  const double reach_km = a.tangent_km + b.tangent_km;
  // Two satellites farther apart than the sum of their tangent lengths (max_link_length_km)
  // cannot see each other: a cheap first rejection.
  if (length_squared > reach_km * reach_km) {
    return std::nullopt;
  }
  // The exact test, as segment_min_radius_km computes it: the point of the segment closest to
  // the centre, a + t d with t clamped to [0, 1].
  const double t =
      length_squared > 0.0
          ? std::clamp(-((a.x * dx) + (a.y * dy) + (a.z * dz)) / length_squared, 0.0, 1.0)
          : 0.0;
  const double cx = a.x + (t * dx);
  const double cy = a.y + (t * dy);
  const double cz = a.z + (t * dz);
  if (std::sqrt((cx * cx) + (cy * cy) + (cz * cz)) < limit_km) {
    return std::nullopt;
  }
  return std::sqrt(length_squared);
}

bool permanent(const constellation::Classification& classification, std::size_t a, std::size_t b,
               double atmosphere_km) {
  const std::optional<constellation::OrbitGeometry>& ga = classification.satellites[a].geometry;
  const std::optional<constellation::OrbitGeometry>& gb = classification.satellites[b].geometry;
  return ga.has_value() && gb.has_value() &&
         keeps_line_of_sight_for_an_orbit(*ga, *gb, atmosphere_km);
}

void match_across_planes(const constellation::Classification& classification,
                         std::span<const std::optional<Eigen::Vector3d>> position_ecef_km,
                         double atmosphere_km, std::span<const std::size_t> from_members,
                         std::span<const std::size_t> to_members,
                         const std::function<bool(std::size_t)>& from_free,
                         const std::function<bool(std::size_t)>& to_free,
                         const std::function<bool(std::size_t, std::size_t)>& linked,
                         const std::function<void(std::size_t, std::size_t)>& accept) {
  struct Candidate {
    double distance_km;
    std::int64_t id_a;
    std::int64_t id_b;
    std::size_t a;
    std::size_t b;
  };
  const double limit_km = line_of_sight_radius_km(atmosphere_km);
  // Each satellite with a position and a free laser on the side wanted.
  const auto ends = [&](std::span<const std::size_t> members,
                        const std::function<bool(std::size_t)>& free) {
    std::vector<MatchEnd> out;
    out.reserve(members.size());
    for (const std::size_t s : members) {
      if (s < position_ecef_km.size() && position_ecef_km[s].has_value() && free(s)) {
        out.push_back(match_end(s, *position_ecef_km[s], limit_km));
      }
    }
    return out;
  };
  const std::vector<MatchEnd> from_ends = ends(from_members, from_free);
  const std::vector<MatchEnd> to_ends = ends(to_members, to_free);
  std::vector<Candidate> candidates;
  for (const MatchEnd& ea : from_ends) {
    for (const MatchEnd& eb : to_ends) {
      if (ea.satellite == eb.satellite) {
        continue;
      }
      // Line of sight now does not change during the matching, so a pair without it can never
      // be accepted and is left out before sorting.
      const std::optional<double> distance_km = in_sight_distance_km(ea, eb, limit_km);
      if (!distance_km.has_value() || linked(ea.satellite, eb.satellite)) {
        continue;
      }
      candidates.push_back(Candidate{.distance_km = *distance_km,
                                     .id_a = classification.satellites[ea.satellite].norad_cat_id,
                                     .id_b = classification.satellites[eb.satellite].norad_cat_id,
                                     .a = ea.satellite,
                                     .b = eb.satellite});
    }
  }
  std::ranges::sort(candidates, [](const Candidate& x, const Candidate& y) {
    if (x.distance_km != y.distance_km) {
      return x.distance_km < y.distance_km;
    }
    if (x.id_a != y.id_a) {
      return x.id_a < y.id_a;
    }
    return x.id_b < y.id_b;
  });
  // Permanence depends only on the pair, so it is tested last, and only for a pair that would
  // otherwise be linked: the result is the same as testing every candidate first.
  for (const Candidate& c : candidates) {
    if (from_free(c.a) && to_free(c.b) && !linked(c.a, c.b) &&
        permanent(classification, c.a, c.b, atmosphere_km)) {
      accept(c.a, c.b);
    }
  }
}

}  // namespace satsim::topology::detail
