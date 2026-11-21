#pragma once

// The loop's StepWorld on any hand-made Geometry (fake_geometry.hpp): offset j is step k + j,
// `known` steps known from now, every satellite in view for the whole horizon, none moving
// north.

#include <cstddef>
#include <cstdint>
#include <map>
#include <span>
#include <vector>

#include "failures/geometry.hpp"
#include "failures/loop.hpp"
#include "failures/timeline.hpp"
#include "failures/view.hpp"
#include "satsim/topology/ground_links.hpp"
#include "satsim/topology/link_state.hpp"

namespace satsim::failures_app::testing {

class GeometryWorld final : public StepWorld {
 public:
  /// `known`: the steps known from now (1 for the start planner; the re-plan interval for the
  /// whole-interval planner).
  GeometryWorld(Geometry& geometry, const Timeline& timeline, std::size_t known = 1)
      : geometry_(&geometry), timeline_(&timeline), known_(known) {}
  void at(std::int64_t k) { k_ = k; }
  [[nodiscard]] std::size_t known_steps() const override { return known_; }
  [[nodiscard]] const StepGeometry& geometry(std::size_t offset) const override {
    return geometry_->at(k_ + static_cast<std::int64_t>(offset));
  }
  [[nodiscard]] const topology::StepLinks& geometric(std::size_t offset) const override {
    const std::int64_t k = k_ + static_cast<std::int64_t>(offset);
    auto it = cache_.find(k);
    if (it == cache_.end()) {
      it = cache_.emplace(k, geometric_states(*geometry_, timeline_->links, k)).first;
    }
    return it->second;
  }
  [[nodiscard]] std::vector<double> times_left_in_view_s(
      std::size_t /*site*/, std::span<const topology::VisibleSatellite> visible,
      double horizon_s) const override {
    return std::vector<double>(visible.size(), horizon_s);
  }
  [[nodiscard]] bool moving_north(std::size_t /*satellite*/) const override { return false; }

 private:
  Geometry* geometry_;
  const Timeline* timeline_;
  std::size_t known_;
  std::int64_t k_ = 0;
  mutable std::map<std::int64_t, topology::StepLinks> cache_;
};

}  // namespace satsim::failures_app::testing
