#include "failures/view.hpp"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <utility>
#include <vector>

#include "failures/geometry.hpp"
#include "failures/timeline.hpp"
#include "routing/graph.hpp"
#include "satsim/topology/ground_links.hpp"
#include "satsim/topology/link_plan.hpp"
#include "satsim/topology/link_state.hpp"

namespace satsim::failures_app {

namespace {

using topology::LinkForce;
using topology::VisibleSatellite;

bool marked(const std::vector<char>& marks, std::size_t i) noexcept {
  return i < marks.size() && marks[i] != 0;
}

bool touches(const topology::PlannedLink& link, const std::vector<char>& satellite_down) {
  return marked(satellite_down, link.satellite_a) || marked(satellite_down, link.satellite_b);
}

// The step's lists without the satellites marked down; a gateway marked down gets none.
OverlaidStep with_ground(const StepGeometry& step, const topology::StepLinks& geometric,
                         std::vector<LinkForce> force, std::vector<char> satellite_down,
                         std::vector<char> gateway_down) {
  OverlaidStep result{.links = geometric,
                      .force = std::move(force),
                      .city_visible = {},
                      .gateway_visible = {},
                      .satellite_down = {},
                      .gateway_down = {}};
  result.links.utc = step.utc;
  topology::apply_link_force(result.links, result.force);
  const auto keep = [&](const std::vector<VisibleSatellite>& visible) {
    std::vector<VisibleSatellite> kept;
    kept.reserve(visible.size());
    for (const VisibleSatellite& v : visible) {
      if (!marked(satellite_down, v.satellite)) {
        kept.push_back(v);
      }
    }
    return kept;
  };
  result.city_visible.reserve(step.city_visible.size());
  for (const auto& visible : step.city_visible) {
    result.city_visible.push_back(keep(visible));
  }
  result.gateway_visible.reserve(step.gateway_visible.size());
  for (std::size_t g = 0; g < step.gateway_visible.size(); ++g) {
    result.gateway_visible.push_back(marked(gateway_down, g) ? std::vector<VisibleSatellite>{}
                                                             : keep(step.gateway_visible[g]));
  }
  result.satellite_down = std::move(satellite_down);
  result.gateway_down = std::move(gateway_down);
  return result;
}

}  // namespace

Knowledge planner_knowledge(const Timeline& timeline) {
  Knowledge knowledge;
  knowledge.reserve(timeline.facts.size());
  for (const Fact& fact : timeline.facts) {
    knowledge.push_back(fact.planner_known_s);
  }
  return knowledge;
}

ViewState view_at(const Timeline& timeline, std::span<const double> knowledge, double t) {
  const ControlNodes& nodes = timeline.nodes;
  ViewState view;
  view.links.assign(timeline.links.size(), Belief::kNothing);
  view.satellites.assign(nodes.satellites, Belief::kNothing);
  view.gateways.assign(nodes.gateways, Belief::kNothing);
  // The deciding fact's t_s per subject.
  constexpr double kNone = -std::numeric_limits<double>::infinity();
  std::vector<double> link_t(view.links.size(), kNone);
  std::vector<double> satellite_t(view.satellites.size(), kNone);
  std::vector<double> gateway_t(view.gateways.size(), kNone);
  std::vector<bool> link_set(view.links.size(), false);
  std::vector<bool> satellite_set(view.satellites.size(), false);
  std::vector<bool> gateway_set(view.gateways.size(), false);
  for (std::size_t f = 0; f < timeline.facts.size() && f < knowledge.size(); ++f) {
    if (!(knowledge[f] <= t)) {
      continue;
    }
    const Fact& fact = timeline.facts[f];
    std::vector<Belief>* beliefs = &view.links;
    std::vector<double>* latest = &link_t;
    std::vector<bool>* set = &link_set;
    if (fact.subject.kind == SubjectKind::kSatellite) {
      beliefs = &view.satellites;
      latest = &satellite_t;
      set = &satellite_set;
    } else if (fact.subject.kind == SubjectKind::kGateway) {
      beliefs = &view.gateways;
      latest = &gateway_t;
      set = &gateway_set;
    }
    const std::size_t i = fact.subject.index;
    if (i >= beliefs->size()) {
      continue;
    }
    // Later in the list wins a tie: >=.
    if (!(*set)[i] || fact.t_s >= (*latest)[i]) {
      (*set)[i] = true;
      (*latest)[i] = fact.t_s;
      (*beliefs)[i] = fact.down ? Belief::kDown : Belief::kUp;
    }
  }
  view.satellite_down.assign(view.satellites.size(), 0);
  for (std::size_t s = 0; s < view.satellites.size(); ++s) {
    view.satellite_down[s] = static_cast<char>(view.satellites[s] == Belief::kDown);
  }
  // The inference: every planned laser link of a satellite held down.
  std::vector<std::size_t> planned(view.satellites.size(), 0);
  std::vector<std::size_t> held_down(view.satellites.size(), 0);
  for (std::size_t i = 0; i < timeline.planned_count && i < timeline.links.size(); ++i) {
    const topology::PlannedLink& link = timeline.links[i];
    for (const std::size_t s : {link.satellite_a, link.satellite_b}) {
      if (s < planned.size()) {
        ++planned[s];
        if (view.links[i] == Belief::kDown) {
          ++held_down[s];
        }
      }
    }
  }
  for (std::size_t s = 0; s < view.satellites.size(); ++s) {
    if (planned[s] > 0 && held_down[s] == planned[s]) {
      view.satellite_down[s] = 1;
    }
  }
  view.gateway_down.assign(view.gateways.size(), 0);
  for (std::size_t g = 0; g < view.gateways.size(); ++g) {
    view.gateway_down[g] = static_cast<char>(view.gateways[g] == Belief::kDown);
  }
  return view;
}

routing_app::StepGround OverlaidStep::ground() const noexcept {
  return routing_app::StepGround{.links = &links,
                                 .city_visible = city_visible,
                                 .gateway_visible = gateway_visible,
                                 .satellite_down = satellite_down,
                                 .gateway_down = gateway_down};
}

topology::StepLinks geometric_states(Geometry& geometry,
                                     std::span<const topology::PlannedLink> links,
                                     std::int64_t step) {
  topology::StepLinks result{.utc = geometry.at(step).utc, .states = {}};
  result.states.reserve(links.size());
  for (const topology::PlannedLink& link : links) {
    result.states.push_back(geometry.link_state(step, link.satellite_a, link.satellite_b));
  }
  return result;
}

OverlaidStep overlay_view(const StepGeometry& step, const topology::StepLinks& geometric,
                          const Timeline& timeline, const ViewState& view) {
  std::vector<LinkForce> force(timeline.links.size(), LinkForce::kInForce);
  for (std::size_t i = 0; i < timeline.links.size(); ++i) {
    const bool on_down_satellite = touches(timeline.links[i], view.satellite_down);
    const Belief belief = i < view.links.size() ? view.links[i] : Belief::kNothing;
    if (i < timeline.planned_count) {
      force[i] =
          belief == Belief::kDown || on_down_satellite ? LinkForce::kFailed : LinkForce::kInForce;
    } else if (belief == Belief::kUp) {
      force[i] = on_down_satellite ? LinkForce::kFailed : LinkForce::kInForce;
    } else {
      force[i] = LinkForce::kNotInForce;
    }
  }
  return with_ground(step, geometric, std::move(force), view.satellite_down, view.gateway_down);
}

OverlaidStep overlay_truth(const StepGeometry& step, const topology::StepLinks& geometric,
                           const Timeline& timeline, double t) {
  return with_ground(step, geometric, timeline.force_at(t), timeline.satellite_down_at(t),
                     timeline.gateway_down_at(t));
}

}  // namespace satsim::failures_app
