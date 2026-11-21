#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "failures/geometry.hpp"
#include "failures/timeline.hpp"
#include "routing/graph.hpp"
#include "satsim/topology/ground_links.hpp"
#include "satsim/topology/link_plan.hpp"
#include "satsim/topology/link_state.hpp"

// Views and the overlay (Phase 4 spec 4.4; docs/phase-4-plan.md, design choice 3): what one
// actor believes at an instant, and a step's geometry turned into the network as that actor
// believes it, or as it truly is.

namespace satsim::failures_app {

/// What one actor knows: per fact of Timeline::facts (same index), the instant it knows it;
/// +infinity for never. A planner's, a node's after a flood: any actor's.
using Knowledge = std::vector<double>;

/// The planner's knowledge: every fact's planner_known_s.
[[nodiscard]] Knowledge planner_knowledge(const Timeline& timeline);

/// What an actor believes of one element: the latest fact about it that it knows.
enum class Belief : std::uint8_t { kNothing, kDown, kUp };

/// An actor's view at an instant.
struct ViewState {
  /// Per link of Timeline::links.
  std::vector<Belief> links;
  /// Per satellite and per gateway: the latest known fact about it.
  std::vector<Belief> satellites;
  std::vector<Belief> gateways;
  /// Per satellite: taken as down (non-zero): its latest fact is "down", or every planned
  /// laser link of it (Timeline::links before planned_count touching it; at least one) is
  /// kDown in `links` by a deciding fact that is not old and does not have the satellite
  /// among its detectors, and none of its repair links (from planned_count on) is kUp.
  std::vector<char> satellite_down;
  /// Per gateway: its latest fact is "down".
  std::vector<char> gateway_down;

  friend bool operator==(const ViewState&, const ViewState&) = default;
};

/// The view of an actor with `knowledge` (one entry per fact) at instant `t`: for each
/// subject, among the facts about it known by t (known instant <= t), the one with the latest
/// t_s decides (a later fact replaces an earlier one, spec 4.4); on equal t_s the later in the
/// facts list. Then the inference of spec 4.4 (satellite_down above).
[[nodiscard]] ViewState view_at(const Timeline& timeline, std::span<const double> knowledge,
                                double t);

/// One step's network for the routing graph, owning its data.
struct OverlaidStep {
  /// One state per link of Timeline::links: the geometric state with the force applied
  /// (topology::apply_link_force).
  topology::StepLinks links;
  /// The force that was applied, per link.
  std::vector<topology::LinkForce> force;
  /// Per city and gateway, the satellites it may use.
  std::vector<std::vector<topology::VisibleSatellite>> city_visible;
  std::vector<std::vector<topology::VisibleSatellite>> gateway_visible;
  /// Per satellite and gateway: non-zero for down.
  std::vector<char> satellite_down;
  std::vector<char> gateway_down;

  /// Points into this object: valid while it lives unchanged.
  [[nodiscard]] routing_app::StepGround ground() const noexcept;
};

/// The geometric state of every link of `links` at `step` (Geometry::link_state per link), with
/// the step's UTC time.
[[nodiscard]] topology::StepLinks geometric_states(Geometry& geometry,
                                                   std::span<const topology::PlannedLink> links,
                                                   std::int64_t step);

/// The network as `view` believes it, on one step's geometry (`step`) and the geometric states
/// of every link of the timeline at that step (`geometric`):
///  - a planned link is kInForce, or kFailed when the view holds it down or it touches a
///    satellite the view takes as down;
///  - a repair link is kNotInForce unless the view's latest fact of it is "up": then kInForce,
///    or kFailed if it touches a satellite taken as down;
///  - never kAcquiring;
///  - every satellite taken as down is removed from every city's and gateway's list, and a
///    gateway held as down has an empty list;
///  - the down marks are the view's.
[[nodiscard]] OverlaidStep overlay_view(const StepGeometry& step,
                                        const topology::StepLinks& geometric,
                                        const Timeline& timeline, const ViewState& view);

/// The true network at instant `t` on the geometry of step(t): the force of every link at t
/// (Timeline::force_at, acquiring included), and the satellites and gateways down at t
/// removed and marked as overlay_view does.
[[nodiscard]] OverlaidStep overlay_truth(const StepGeometry& step,
                                         const topology::StepLinks& geometric,
                                         const Timeline& timeline, double t);

}  // namespace satsim::failures_app
