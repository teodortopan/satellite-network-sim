#include "satsim/topology/link_plan.hpp"

#include <Eigen/Core>
#include <Eigen/Geometry>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <numbers>
#include <optional>
#include <span>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "satsim/constellation/classification.hpp"
#include "satsim/constellation/geometry.hpp"
#include "satsim/topology/geometry.hpp"

namespace satsim::topology {

namespace {

using constellation::Classification;
using constellation::Plane;
using constellation::PlaneAssignment;

constexpr std::size_t kThreeLasers = 3;
constexpr std::size_t kPlusGridLasers = 4;
constexpr std::size_t kSixLasers = 6;

// The building state of one plan.
class Planner {
 public:
  Planner(const Classification& classification,
          std::span<const std::optional<Eigen::Vector3d>> position_ecef_km,
          const LinkPlanSettings& settings)
      : classification_(&classification), positions_(position_ecef_km), settings_(settings) {
    plan_.pattern = settings.pattern;
    plan_.lasers.resize(classification.satellites.size());
    for (std::size_t i = 0; i < classification.satellites.size(); ++i) {
      if (const auto* place =
              std::get_if<PlaneAssignment>(&classification.satellites[i].placement)) {
        for (const LaserSide side : laser_sides(settings.pattern, place->index_in_plane)) {
          plan_.lasers[i].push_back(Laser{.side = side, .link = std::nullopt, .unassigned = {}});
        }
      }
    }
  }

  LinkPlan run() && {
    const std::size_t shell_count = classification_->planes.size();
    plan_.neighbours.resize(shell_count);
    for (std::size_t shell = 0; shell < shell_count; ++shell) {
      plan_in_plane(shell);
      find_neighbours(shell);
      const auto& planes = classification_->planes[shell];
      const auto& neighbours = plan_.neighbours[shell];
      for (std::size_t p = 0; p < planes.size(); ++p) {
        if (const std::optional<std::size_t>& east = neighbours[p].east; east.has_value()) {
          match(shell, p, *east, LaserSide::kEast, LaserSide::kWest, LinkKind::kCrossPlane);
        }
      }
      for (std::size_t p = 0; p < planes.size(); ++p) {
        if (const std::optional<std::size_t>& second = neighbours[p].second_east;
            second.has_value()) {
          match(shell, p, *second, LaserSide::kSecondEast, LaserSide::kSecondWest,
                LinkKind::kSecondCrossPlane);
        }
      }
    }
    record_unassigned();
    return std::move(plan_);
  }

 private:
  // The laser of satellite `satellite` facing `side`, or nullptr if it has none.
  Laser* laser(std::size_t satellite, LaserSide side) {
    for (Laser& l : plan_.lasers[satellite]) {
      if (l.side == side) {
        return &l;
      }
    }
    return nullptr;
  }

  bool free(std::size_t satellite, LaserSide side) {
    const Laser* l = laser(satellite, side);
    return l != nullptr && !l->link.has_value();
  }

  [[nodiscard]] const Eigen::Vector3d* position(std::size_t satellite) const {
    if (satellite >= positions_.size()) {
      return nullptr;
    }
    const std::optional<Eigen::Vector3d>& p = positions_[satellite];
    return p.has_value() ? &*p : nullptr;
  }

  // Whether a link between a and b keeps line of sight for a whole orbit.
  [[nodiscard]] bool permanent(std::size_t a, std::size_t b) const {
    const std::optional<constellation::OrbitGeometry>& ga = classification_->satellites[a].geometry;
    const std::optional<constellation::OrbitGeometry>& gb = classification_->satellites[b].geometry;
    return ga.has_value() && gb.has_value() &&
           keeps_line_of_sight_for_an_orbit(*ga, *gb, settings_.atmosphere_km);
  }

  // Whether a and b already share a link: one of a's lasers (at most six) links to b.
  [[nodiscard]] bool linked(std::size_t a, std::size_t b) const {
    for (const Laser& l : plan_.lasers[a]) {
      if (l.link.has_value()) {
        const PlannedLink& link = plan_.links[*l.link];
        if (link.satellite_a == b || link.satellite_b == b) {
          return true;
        }
      }
    }
    return false;
  }

  void add_link(std::size_t shell, std::size_t a, LaserSide side_a, std::size_t b, LaserSide side_b,
                LinkKind kind) {
    const std::size_t index = plan_.links.size();
    plan_.links.push_back(PlannedLink{.satellite_a = a,
                                      .satellite_b = b,
                                      .side_a = side_a,
                                      .side_b = side_b,
                                      .kind = kind,
                                      .shell_index = shell});
    laser(a, side_a)->link = index;
    laser(b, side_b)->link = index;
  }

  void plan_in_plane(std::size_t shell) {
    for (const Plane& plane : classification_->planes[shell]) {
      const std::vector<std::size_t>& m = plane.members;
      const std::size_t n = m.size();
      if (n < 2) {
        continue;
      }
      // A plane of two has a single link; a ring of n >= 3 has n.
      const std::size_t count = n == 2 ? 1 : n;
      for (std::size_t i = 0; i < count; ++i) {
        add_link(shell, m[i], LaserSide::kAhead, m[(i + 1) % n], LaserSide::kBehind,
                 LinkKind::kInPlane);
      }
    }
  }

  void find_neighbours(std::size_t shell) {
    const std::vector<Plane>& planes = classification_->planes[shell];
    const std::size_t count = planes.size();
    std::vector<PlaneNeighbours>& neighbours = plan_.neighbours[shell];
    neighbours.assign(count, PlaneNeighbours{});
    if (count < 2) {
      return;
    }
    for (std::size_t p = 0; p < count; ++p) {
      const std::size_t east = (p + 1) % count;
      neighbours[p].east = east;
      neighbours[east].west = p;
    }
    for (std::size_t p = 0; p < count; ++p) {
      const std::optional<std::size_t> east = neighbours[p].east;
      if (!east.has_value()) {
        continue;
      }
      const std::optional<std::size_t> second = neighbours[*east].east;
      if (second.has_value() && *second != p) {
        neighbours[p].second_east = second;
        neighbours[*second].second_west = p;
      }
    }
  }

  // Greedy shortest-first matching of plane `from`'s `side_from` lasers with plane `to`'s
  // `side_to` lasers.
  void match(std::size_t shell, std::size_t from, std::size_t to, LaserSide side_from,
             LaserSide side_to, LinkKind kind) {
    struct Candidate {
      double distance_km;
      std::int64_t id_a;
      std::int64_t id_b;
      std::size_t a;
      std::size_t b;
    };
    const auto& planes = classification_->planes[shell];
    // Each satellite with a position and a free laser on the side wanted, as plain doubles: the
    // loop below runs over every pair of the two planes, so it avoids Eigen expressions, which
    // dominate the cost in unoptimised sanitizer builds.
    struct End {
      std::size_t satellite;
      double x, y, z;
      double tangent_km;  // Tangent length to the limiting sphere (0 if inside it).
    };
    const double limit_km = line_of_sight_radius_km(settings_.atmosphere_km);
    const auto ends = [&](const std::vector<std::size_t>& members, LaserSide side) {
      std::vector<End> out;
      out.reserve(members.size());
      for (const std::size_t s : members) {
        const Eigen::Vector3d* p = position(s);
        if (p != nullptr && free(s, side)) {
          const double x = p->x();
          const double y = p->y();
          const double z = p->z();
          const double r_squared = (x * x) + (y * y) + (z * z);
          const double t_squared = r_squared - (limit_km * limit_km);
          out.push_back(End{.satellite = s,
                            .x = x,
                            .y = y,
                            .z = z,
                            .tangent_km = t_squared > 0.0 ? std::sqrt(t_squared) : 0.0});
        }
      }
      return out;
    };
    const std::vector<End> from_ends = ends(planes[from].members, side_from);
    const std::vector<End> to_ends = ends(planes[to].members, side_to);
    std::vector<Candidate> candidates;
    for (const End& ea : from_ends) {
      for (const End& eb : to_ends) {
        const double dx = eb.x - ea.x;
        const double dy = eb.y - ea.y;
        const double dz = eb.z - ea.z;
        const double length_squared = (dx * dx) + (dy * dy) + (dz * dz);
        const double reach_km = ea.tangent_km + eb.tangent_km;
        // Two satellites farther apart than the sum of their tangent lengths
        // (max_link_length_km) cannot see each other: a cheap first rejection.
        if (ea.satellite == eb.satellite || length_squared > reach_km * reach_km) {
          continue;
        }
        // The exact test, as segment_min_radius_km computes it: the point of the segment
        // closest to the centre, a + t d with t clamped to [0, 1]. Line of sight at the
        // reference time does not change during the matching, so a pair without it can never
        // be accepted and is left out before sorting.
        const double t =
            length_squared > 0.0
                ? std::clamp(-((ea.x * dx) + (ea.y * dy) + (ea.z * dz)) / length_squared, 0.0, 1.0)
                : 0.0;
        const double cx = ea.x + (t * dx);
        const double cy = ea.y + (t * dy);
        const double cz = ea.z + (t * dz);
        if (std::sqrt((cx * cx) + (cy * cy) + (cz * cz)) < limit_km ||
            linked(ea.satellite, eb.satellite)) {
          continue;
        }
        candidates.push_back(
            Candidate{.distance_km = std::sqrt(length_squared),
                      .id_a = classification_->satellites[ea.satellite].norad_cat_id,
                      .id_b = classification_->satellites[eb.satellite].norad_cat_id,
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
    // Permanence depends only on the pair, so it is tested last, and only for a pair that
    // would otherwise be linked: the result is the same as testing every candidate first.
    for (const Candidate& c : candidates) {
      if (free(c.a, side_from) && free(c.b, side_to) && !linked(c.a, c.b) && permanent(c.a, c.b)) {
        add_link(shell, c.a, side_from, c.b, side_to, kind);
      }
    }
  }

  void record_unassigned() {
    for (std::size_t i = 0; i < classification_->satellites.size(); ++i) {
      const constellation::ClassifiedSatellite& satellite = classification_->satellites[i];
      const auto* place = std::get_if<PlaneAssignment>(&satellite.placement);
      if (place == nullptr || !satellite.shell_index) {
        continue;
      }
      const PlaneNeighbours& n = plan_.neighbours[*satellite.shell_index][place->plane_index];
      for (Laser& l : plan_.lasers[i]) {
        if (l.link) {
          continue;
        }
        std::optional<std::size_t> neighbour;
        switch (l.side) {
          case LaserSide::kAhead:
          case LaserSide::kBehind:
            l.unassigned = UnassignedReason::kPlaneTooSmall;
            continue;
          case LaserSide::kEast:
            neighbour = n.east;
            break;
          case LaserSide::kWest:
            neighbour = n.west;
            break;
          case LaserSide::kSecondEast:
            neighbour = n.second_east;
            break;
          case LaserSide::kSecondWest:
            neighbour = n.second_west;
            break;
        }
        l.unassigned =
            neighbour ? UnassignedReason::kNoFreePartner : UnassignedReason::kNoNeighbourPlane;
      }
    }
  }

  const Classification* classification_;
  std::span<const std::optional<Eigen::Vector3d>> positions_;
  LinkPlanSettings settings_;
  LinkPlan plan_;
};

}  // namespace

bool keeps_line_of_sight_for_an_orbit(const constellation::OrbitGeometry& a,
                                      const constellation::OrbitGeometry& b, double atmosphere_km) {
  // Each orbit's ascending node and the direction 90 deg further along it, in TEME: a point at
  // argument of latitude u is radius * (cos u * node + sin u * along).
  struct CircularOrbit {
    Eigen::Vector3d node_teme;
    Eigen::Vector3d along_teme;
    double radius_km;
    double argument_of_latitude_rad;
  };
  const auto circular = [](const constellation::OrbitGeometry& g) {
    const Eigen::Vector3d node_teme(std::cos(g.raan_teme_rad), std::sin(g.raan_teme_rad), 0.0);
    return CircularOrbit{.node_teme = node_teme,
                         .along_teme = g.mean_normal_teme.cross(node_teme),
                         .radius_km = g.mean_radius_km,
                         .argument_of_latitude_rad = g.argument_of_latitude_rad};
  };
  const CircularOrbit orbit_a = circular(a);
  const CircularOrbit orbit_b = circular(b);
  const auto position_teme_km = [](const CircularOrbit& o, double advance_rad) {
    const double u = o.argument_of_latitude_rad + advance_rad;
    return Eigen::Vector3d(o.radius_km *
                           ((std::cos(u) * o.node_teme) + (std::sin(u) * o.along_teme)));
  };
  for (std::size_t k = 0; k < kPermanenceSamplesPerOrbit; ++k) {
    const double advance_rad = 2.0 * std::numbers::pi * static_cast<double>(k) /
                               static_cast<double>(kPermanenceSamplesPerOrbit);
    // has_line_of_sight is written for Earth-fixed positions; it uses only distances from the
    // Earth's centre, which TEME and ITRF share.
    if (!has_line_of_sight(position_teme_km(orbit_a, advance_rad),
                           position_teme_km(orbit_b, advance_rad), atmosphere_km)) {
      return false;
    }
  }
  return true;
}

std::string_view name_of(LaserPattern pattern) noexcept {
  switch (pattern) {
    case LaserPattern::kThree:
      return "three";
    case LaserPattern::kPlusGrid:
      return "plus-grid";
    case LaserPattern::kSix:
      return "six";
  }
  return "?";
}

std::optional<LaserPattern> parse_laser_pattern(std::string_view name) noexcept {
  for (const LaserPattern pattern : kAllLaserPatterns) {
    if (name == name_of(pattern)) {
      return pattern;
    }
  }
  return std::nullopt;
}

std::size_t laser_budget(LaserPattern pattern) noexcept {
  switch (pattern) {
    case LaserPattern::kThree:
      return kThreeLasers;
    case LaserPattern::kPlusGrid:
      return kPlusGridLasers;
    case LaserPattern::kSix:
      return kSixLasers;
  }
  return 0;
}

std::string_view name_of(LaserSide side) noexcept {
  switch (side) {
    case LaserSide::kAhead:
      return "ahead";
    case LaserSide::kBehind:
      return "behind";
    case LaserSide::kEast:
      return "east";
    case LaserSide::kWest:
      return "west";
    case LaserSide::kSecondEast:
      return "second_east";
    case LaserSide::kSecondWest:
      return "second_west";
  }
  return "?";
}

std::vector<LaserSide> laser_sides(LaserPattern pattern, std::size_t index_in_plane) {
  switch (pattern) {
    case LaserPattern::kThree:
      return {LaserSide::kAhead, LaserSide::kBehind,
              index_in_plane % 2 == 0 ? LaserSide::kEast : LaserSide::kWest};
    case LaserPattern::kPlusGrid:
      return {LaserSide::kAhead, LaserSide::kBehind, LaserSide::kEast, LaserSide::kWest};
    case LaserPattern::kSix:
      return {LaserSide::kAhead, LaserSide::kBehind,     LaserSide::kEast,
              LaserSide::kWest,  LaserSide::kSecondEast, LaserSide::kSecondWest};
  }
  return {};
}

std::string_view name_of(LinkKind kind) noexcept {
  switch (kind) {
    case LinkKind::kInPlane:
      return "in_plane";
    case LinkKind::kCrossPlane:
      return "cross_plane";
    case LinkKind::kSecondCrossPlane:
      return "second_cross_plane";
  }
  return "?";
}

std::string_view name_of(UnassignedReason reason) noexcept {
  switch (reason) {
    case UnassignedReason::kPlaneTooSmall:
      return "plane_too_small";
    case UnassignedReason::kNoNeighbourPlane:
      return "no_neighbour_plane";
    case UnassignedReason::kNoFreePartner:
      return "no_free_partner";
  }
  return "?";
}

LinkPlan plan_laser_links(const Classification& classification,
                          std::span<const std::optional<Eigen::Vector3d>> position_ecef_km,
                          const LinkPlanSettings& settings) {
  return Planner(classification, position_ecef_km, settings).run();
}

std::array<std::size_t, 3> count_links_by_kind(const LinkPlan& plan) noexcept {
  std::array<std::size_t, 3> counts{};
  for (const PlannedLink& link : plan.links) {
    ++counts[static_cast<std::size_t>(link.kind)];
  }
  return counts;
}

std::array<std::size_t, 3> count_unassigned_lasers(const LinkPlan& plan) noexcept {
  std::array<std::size_t, 3> counts{};
  for (const std::vector<Laser>& lasers : plan.lasers) {
    for (const Laser& l : lasers) {
      if (l.unassigned) {
        ++counts[static_cast<std::size_t>(*l.unassigned)];
      }
    }
  }
  return counts;
}

}  // namespace satsim::topology
