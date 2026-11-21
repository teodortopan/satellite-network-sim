#include "satsim/topology/link_repair.hpp"

#include <Eigen/Core>
#include <algorithm>
#include <cstddef>
#include <optional>
#include <set>
#include <span>
#include <utility>
#include <vector>

#include "laser_matching.hpp"
#include "satsim/constellation/classification.hpp"
#include "satsim/topology/geometry.hpp"
#include "satsim/topology/link_plan.hpp"

namespace satsim::topology {

namespace {

using constellation::Classification;
using constellation::Plane;

// The matching state of one call.
class Repairer {
 public:
  Repairer(const Classification& classification, const LinkPlan& plan, const RepairInput& input)
      : classification_(&classification), plan_(&plan), input_(&input) {
    used_.resize(plan.lasers.size());
    for (std::size_t s = 0; s < plan.lasers.size(); ++s) {
      used_[s].assign(plan.lasers[s].size(), 0);
    }
    for (const auto& [a, b] : input.linked) {
      linked_.insert(std::minmax(a, b));
    }
  }

  std::vector<PlannedLink> run() && {
    const double limit_km = line_of_sight_radius_km(input_->atmosphere_km);
    for (std::size_t shell = 0; shell < classification_->planes.size(); ++shell) {
      const std::vector<Plane>& planes = classification_->planes[shell];
      for (const Plane& plane : planes) {
        in_plane(shell, plane, limit_km);
      }
      if (shell >= plan_->neighbours.size() || plan_->neighbours[shell].size() != planes.size()) {
        continue;
      }
      const auto& neighbours = plan_->neighbours[shell];
      for (std::size_t p = 0; p < planes.size(); ++p) {
        if (const std::optional<std::size_t>& east = neighbours[p].east;
            east.has_value() && *east < planes.size()) {
          across(shell, planes[p], planes[*east], LaserSide::kEast, LaserSide::kWest,
                 LinkKind::kCrossPlane);
        }
      }
      for (std::size_t p = 0; p < planes.size(); ++p) {
        if (const std::optional<std::size_t>& second = neighbours[p].second_east;
            second.has_value() && *second < planes.size()) {
          across(shell, planes[p], planes[*second], LaserSide::kSecondEast, LaserSide::kSecondWest,
                 LinkKind::kSecondCrossPlane);
        }
      }
    }
    return std::move(links_);
  }

 private:
  [[nodiscard]] bool down(std::size_t satellite) const {
    return satellite >= input_->satellite_down.size() || input_->satellite_down[satellite] != 0;
  }

  [[nodiscard]] const Eigen::Vector3d* position(std::size_t satellite) const {
    if (satellite >= input_->position_ecef_km.size()) {
      return nullptr;
    }
    const std::optional<Eigen::Vector3d>& p = input_->position_ecef_km[satellite];
    return p.has_value() ? &*p : nullptr;
  }

  // The index of satellite's laser facing `side` in plan.lasers[satellite], or nullopt.
  [[nodiscard]] std::optional<std::size_t> laser_index(std::size_t satellite,
                                                       LaserSide side) const {
    if (satellite >= plan_->lasers.size()) {
      return std::nullopt;
    }
    const std::vector<Laser>& lasers = plan_->lasers[satellite];
    for (std::size_t k = 0; k < lasers.size(); ++k) {
      if (lasers[k].side == side) {
        return k;
      }
    }
    return std::nullopt;
  }

  // Whether satellite's laser facing `side` takes part and is not yet used.
  [[nodiscard]] bool available(std::size_t satellite, LaserSide side) const {
    const std::optional<std::size_t> k = laser_index(satellite, side);
    if (!k.has_value() || down(satellite) || position(satellite) == nullptr ||
        satellite >= input_->laser_free.size()) {
      return false;
    }
    const std::vector<char>& free = input_->laser_free[satellite];
    return *k < free.size() && free[*k] != 0 && used_[satellite][*k] == 0;
  }

  [[nodiscard]] bool linked(std::size_t a, std::size_t b) const {
    return linked_.contains(std::minmax(a, b));
  }

  void add(std::size_t shell, std::size_t a, LaserSide side_a, std::size_t b, LaserSide side_b,
           LinkKind kind) {
    links_.push_back(PlannedLink{.satellite_a = a,
                                 .satellite_b = b,
                                 .side_a = side_a,
                                 .side_b = side_b,
                                 .kind = kind,
                                 .shell_index = shell});
    used_[a][laser_index(a, side_a).value_or(0)] = 1;
    used_[b][laser_index(b, side_b).value_or(0)] = 1;
    linked_.insert(std::minmax(a, b));
  }

  // Bridges every gap of the plane's ring left by satellites that are down.
  void in_plane(std::size_t shell, const Plane& plane, double limit_km) {
    std::vector<std::size_t> ring;
    for (const std::size_t s : plane.members) {
      if (!down(s)) {
        ring.push_back(s);
      }
    }
    const std::size_t n = ring.size();
    for (std::size_t k = 0; k < n; ++k) {
      const std::size_t i = ring[k];
      const std::size_t j = ring[(k + 1) % n];
      if (i == j || !available(i, LaserSide::kAhead) || !available(j, LaserSide::kBehind) ||
          linked(i, j)) {
        continue;
      }
      // available() guarantees both positions.
      const detail::MatchEnd ei = detail::match_end(i, *position(i), limit_km);
      const detail::MatchEnd ej = detail::match_end(j, *position(j), limit_km);
      if (detail::in_sight_distance_km(ei, ej, limit_km).has_value() &&
          detail::permanent(*classification_, i, j, input_->atmosphere_km)) {
        add(shell, i, LaserSide::kAhead, j, LaserSide::kBehind, LinkKind::kInPlane);
      }
    }
  }

  void across(std::size_t shell, const Plane& from, const Plane& to, LaserSide side_from,
              LaserSide side_to, LinkKind kind) {
    detail::match_across_planes(
        *classification_, input_->position_ecef_km, input_->atmosphere_km, from.members, to.members,
        [&](std::size_t s) { return available(s, side_from); },
        [&](std::size_t s) { return available(s, side_to); },
        [&](std::size_t a, std::size_t b) { return linked(a, b); },
        [&](std::size_t a, std::size_t b) { add(shell, a, side_from, b, side_to, kind); });
  }

  const Classification* classification_;
  const LinkPlan* plan_;
  const RepairInput* input_;
  std::vector<std::vector<char>> used_;
  std::set<std::pair<std::size_t, std::size_t>> linked_;
  std::vector<PlannedLink> links_;
};

}  // namespace

std::vector<PlannedLink> match_free_lasers(const Classification& classification,
                                           const LinkPlan& plan, const RepairInput& input) {
  return Repairer(classification, plan, input).run();
}

}  // namespace satsim::topology
