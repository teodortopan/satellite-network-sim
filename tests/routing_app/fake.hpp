#pragma once

// Hand-made worlds for the routing program's graph and engine tests: every step's laser link
// states, ground links and directions of travel written out by hand, so that every expected
// route, cause and time can be worked out on paper. No orbit is propagated.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <utility>
#include <vector>

#include "routing/engine.hpp"
#include "routing/graph.hpp"
#include "satsim/time/utc_time.hpp"
#include "satsim/topology/ground_links.hpp"
#include "satsim/topology/link_plan.hpp"
#include "satsim/topology/link_state.hpp"

namespace satsim::routing_app::testing {

/// Any fixed instant (the engine never reads a step's time).
[[nodiscard]] time::UtcTime some_time();

/// A satellite a site sees: catalogue number 1000 + satellite.
[[nodiscard]] topology::VisibleSatellite seen(std::size_t satellite, double elevation_deg,
                                              double slant_km);

/// seen, in shell `shell`.
[[nodiscard]] topology::VisibleSatellite seen_in(std::size_t shell, std::size_t satellite,
                                                 double elevation_deg, double slant_km);

/// A link state: up with a length, or down for a reason.
[[nodiscard]] topology::LinkState up(double length_km);
[[nodiscard]] topology::LinkState down(
    topology::LinkDownReason reason = topology::LinkDownReason::kNoLineOfSight);

/// A plan holding only these links (satellite pairs), as the engine reads it.
[[nodiscard]] topology::LinkPlan plan_of(
    std::span<const std::pair<std::size_t, std::size_t>> links);

/// One step.
struct FakeStep {
  std::vector<topology::LinkState> links;  ///< One per plan link.
  std::vector<std::vector<topology::VisibleSatellite>> cities;
  std::vector<std::vector<topology::VisibleSatellite>> gateways;
  /// Satellites moving north at this step (all others move south).
  std::vector<std::size_t> north;
  /// StepGround's failure marks (empty: nothing failed). Default member initializers so that
  /// the worlds written before them need not name them.
  std::vector<char> satellite_down{};  // NOLINT(readability-redundant-member-init)
  std::vector<char> gateway_down{};    // NOLINT(readability-redundant-member-init)
};

/// The world: steps 0, 1, ... step_s apart. Steps past the last one see nothing.
class FakeWorld {
 public:
  FakeWorld(std::vector<FakeStep> steps, double step_s, std::size_t known_steps = 1);

  [[nodiscard]] std::size_t size() const noexcept { return steps_.size(); }
  [[nodiscard]] FakeStep& at(std::size_t k) { return steps_.at(k); }

  /// The look-ahead at step k.
  class View final : public Lookahead {
   public:
    View(const FakeWorld& world, std::size_t k) : world_(&world), k_(k) {}
    [[nodiscard]] std::size_t known_steps() const override;
    [[nodiscard]] StepGround step(std::size_t offset) const override;
    [[nodiscard]] std::vector<double> times_left_in_view_s(
        std::size_t site, std::span<const topology::VisibleSatellite> visible,
        double horizon_s) const override;
    [[nodiscard]] bool moving_north(std::size_t satellite) const override;

   private:
    const FakeWorld* world_;
    std::size_t k_;
  };

  [[nodiscard]] View view(std::size_t k) const { return {*this, k}; }

 private:
  std::vector<FakeStep> steps_;
  /// StepLinks per step (built from steps_ on construction).
  std::vector<topology::StepLinks> links_;
  double step_s_;
  std::size_t known_steps_;
};

/// `n` copies of `step`.
[[nodiscard]] std::vector<FakeStep> repeat(const FakeStep& step, std::size_t n);

/// Runs `engine` over every step of `world`, then finish(); the rows of each step.
[[nodiscard]] std::vector<std::vector<RouteRow>> run(Engine& engine, const FakeWorld& world);

}  // namespace satsim::routing_app::testing
