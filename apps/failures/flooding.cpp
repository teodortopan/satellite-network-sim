#include "failures/flooding.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <utility>
#include <vector>

#include "failures/control_graph.hpp"
#include "failures/geometry.hpp"
#include "failures/timeline.hpp"
#include "failures/view.hpp"
#include "routing/graph.hpp"
#include "satsim/network/graph.hpp"
#include "satsim/network/many_sources.hpp"
#include "satsim/network/paths_to.hpp"
#include "satsim/network/pieces.hpp"
#include "satsim/network/shortest_path.hpp"
#include "satsim/topology/link_state.hpp"

namespace satsim::failures_app {

namespace {

using routing_app::NodeKind;
using routing_app::NodeMeaning;

constexpr double kInfinity = std::numeric_limits<double>::infinity();

bool marked(const std::vector<char>& marks, std::size_t i) noexcept {
  return i < marks.size() && marks[i] != 0;
}

// The messages graph of the truth at t, cached by instant.
class TruthGraphs {
 public:
  TruthGraphs(const Timeline& timeline, const TimelineSettings& settings, Geometry& geometry)
      : timeline_(&timeline), settings_(&settings), geometry_(&geometry) {}

  const network::Graph& at(double t) {
    auto it = graphs_.find(t);
    if (it == graphs_.end()) {
      const std::int64_t k = step_of(t, settings_->step_s);
      const topology::StepLinks geometric = geometric_states(*geometry_, timeline_->links, k);
      const OverlaidStep truth = overlay_truth(geometry_->at(k), geometric, *timeline_, t);
      it = graphs_
               .emplace(t, control_graph(timeline_->nodes, timeline_->links, truth.ground(),
                                         settings_->model, settings_->hop_delay_s))
               .first;
    }
    return it->second;
  }

 private:
  const Timeline* timeline_;
  const TimelineSettings* settings_;
  Geometry* geometry_;
  std::map<double, network::Graph> graphs_;
};

}  // namespace

std::size_t Floods::messages() const noexcept {
  std::size_t total = 0;
  for (const Flood& f : facts) {
    total += f.messages;
  }
  return total;
}

Floods flood_facts(const Timeline& timeline, const TimelineSettings& settings, Geometry& geometry,
                   const AfterFlood& after_flood) {
  const double end_s = static_cast<double>(settings.step_count - 1) * settings.step_s;
  const std::size_t n = timeline.nodes.node_count();
  TruthGraphs graphs(timeline, settings, geometry);
  Floods result;
  result.facts.reserve(timeline.facts.size());
  for (std::size_t f = 0; f < timeline.facts.size(); ++f) {
    const Fact& fact = timeline.facts[f];
    Flood flood;
    std::vector<const Detection*> detectors;
    if (!fact.old) {
      for (const Detection& d : fact.detectors) {
        if (d.t_s <= end_s) {
          detectors.push_back(&d);
        }
      }
    }
    if (detectors.empty()) {
      if (after_flood) {
        after_flood(f, flood);
      }
      result.facts.push_back(std::move(flood));
      continue;
    }
    double t0 = kInfinity;
    for (const Detection* d : detectors) {
      t0 = std::min(t0, d->t_s);
    }
    const network::Graph& graph = graphs.at(t0);
    std::vector<network::Start> starts;
    std::map<std::size_t, double> own_s;  // A detector's own detection instant.
    for (const Detection* d : detectors) {
      starts.push_back({.node = d->node, .delay_s = d->t_s - t0});
      own_s.emplace(d->node, d->t_s);
    }
    const network::ManySourcePaths paths =
        network::shortest_paths_from_many(graph, starts, settings.times.hop_processing_s);
    flood.learn_s.assign(n, kInfinity);
    for (std::size_t v = 0; v < n; ++v) {
      if (!paths.reachable(v)) {
        continue;
      }
      const bool stays_start = paths.predecessor[v] == network::kNoNode;
      flood.learn_s[v] = stays_start ? own_s.at(v) : t0 + paths.delay_s[v];
      ++flood.nodes;
      flood.last_s = std::max(flood.last_s.value_or(-kInfinity), flood.learn_s[v]);
      const std::size_t edges = graph.edges_from(v).size();
      flood.messages += stays_start ? edges : edges - 1;
    }
    if (after_flood) {
      after_flood(f, flood);
    }
    // The check: every node joined to a start by the messages graph learns.
    std::vector<network::NodePair> pairs;
    for (std::size_t u = 0; u < n; ++u) {
      for (const network::Edge& e : graph.edges_from(u)) {
        if (u < e.to) {
          pairs.emplace_back(u, e.to);
        }
      }
    }
    const network::Pieces pieces = network::connected_pieces(n, pairs);
    std::set<std::size_t> joined;
    for (const network::Start& s : starts) {
      joined.insert(pieces.piece[s.node]);
    }
    for (std::size_t v = 0; v < n; ++v) {
      const bool learns = v < flood.learn_s.size() && std::isfinite(flood.learn_s[v]);
      if (joined.count(pieces.piece[v]) != 0 && !learns) {
        ++result.unreached;
      }
    }
    result.facts.push_back(std::move(flood));
  }
  return result;
}

ViewState all_in_force_view(const Timeline& timeline) {
  ViewState view;
  view.links.assign(timeline.links.size(), Belief::kNothing);
  for (std::size_t i = timeline.planned_count; i < view.links.size(); ++i) {
    view.links[i] = Belief::kUp;
  }
  view.satellites.assign(timeline.nodes.satellites, Belief::kNothing);
  view.gateways.assign(timeline.nodes.gateways, Belief::kNothing);
  view.satellite_down.assign(timeline.nodes.satellites, 0);
  view.gateway_down.assign(timeline.nodes.gateways, 0);
  return view;
}

network::Graph view_graph(const network::Graph& superset, const routing_app::GraphLayout& layout,
                          const Timeline& timeline, const ViewState& view) {
  // (min, max) satellite pair -> whether some link joining them is believed to carry traffic.
  std::map<std::pair<std::size_t, std::size_t>, bool> carries;
  for (std::size_t i = 0; i < timeline.links.size(); ++i) {
    const topology::PlannedLink& link = timeline.links[i];
    const Belief belief = i < view.links.size() ? view.links[i] : Belief::kNothing;
    const bool believed =
        i < timeline.planned_count ? belief != Belief::kDown : belief == Belief::kUp;
    bool& entry = carries[std::minmax(link.satellite_a, link.satellite_b)];
    entry = entry || believed;
  }
  const auto satellite_down = [&](std::size_t s) { return marked(view.satellite_down, s); };
  const auto gateway_down = [&](std::size_t g) { return marked(view.gateway_down, g); };
  network::Graph result(superset.node_count());
  for (std::size_t u = 0; u < superset.node_count(); ++u) {
    const NodeMeaning mu = layout.meaning(u);
    for (const network::Edge& e : superset.edges_from(u)) {
      const NodeMeaning mv = layout.meaning(e.to);
      bool keep = true;
      if (mu.kind == NodeKind::kSatellite && mv.kind == NodeKind::kSatellite) {
        const auto it = carries.find(std::minmax(mu.index, mv.index));
        keep = it != carries.end() && it->second && !satellite_down(mu.index) &&
               !satellite_down(mv.index);
      } else if (mu.kind == NodeKind::kSatellite && mv.kind == NodeKind::kGateway) {
        keep = !satellite_down(mu.index) && !gateway_down(mv.index);
      } else if (mu.kind == NodeKind::kGateway && mv.kind == NodeKind::kEnd) {
        keep = !gateway_down(mu.index);
      } else if (mu.kind == NodeKind::kSatellite) {
        keep = !satellite_down(mu.index);  // To a receiving dish.
      } else if (mv.kind == NodeKind::kSatellite) {
        keep = !satellite_down(mv.index);  // From an open or serving dish.
      }
      if (keep) {
        // Copied from a graph that accepted it: never refused.
        static_cast<void>(result.add_edge(u, e.to, e.delay_s));
      }
    }
  }
  return result;
}

FloodKnowledge::FloodKnowledge(const Timeline& timeline, const Floods& floods, double compute_s,
                               UseHook hook)
    : timeline_(&timeline),
      floods_(&floods),
      compute_s_(compute_s),
      hook_(std::move(hook)),
      nodes_(timeline.nodes.node_count()) {
  for (std::size_t f = 0; f < floods.facts.size(); ++f) {
    for (std::size_t v = 0; v < floods.facts[f].learn_s.size(); ++v) {
      const double t = use_s(f, v);
      if (std::isfinite(t)) {
        last_use_ = std::max(last_use_.value_or(t), t);
      }
    }
  }
}

double FloodKnowledge::use_s(std::size_t fact, std::size_t node) const {
  double t = kInfinity;
  if (timeline_->facts.at(fact).old) {
    t = 0.0;
  } else if (fact < floods_->facts.size() && node < floods_->facts[fact].learn_s.size()) {
    const double learn_s = floods_->facts[fact].learn_s[node];
    if (std::isfinite(learn_s)) {
      t = learn_s + compute_s_;
    }
  }
  return hook_ ? hook_(fact, node, t) : t;
}

FloodKnowledge::Uses& FloodKnowledge::uses_of(std::size_t node) {
  Uses& uses = nodes_.at(node);
  if (!uses.built) {
    uses.built = true;
    for (std::size_t f = 0; f < timeline_->facts.size(); ++f) {
      const double t = use_s(f, node);
      if (std::isfinite(t)) {
        uses.by_time.emplace_back(t, f);
      }
    }
    std::sort(uses.by_time.begin(), uses.by_time.end());
  }
  return uses;
}

const std::vector<std::size_t>& FloodKnowledge::facts_at(std::size_t node, double t) {
  Uses& uses = uses_of(node);
  const auto end = std::upper_bound(
      uses.by_time.begin(), uses.by_time.end(), t,
      [](double value, const std::pair<double, std::size_t>& u) { return value < u.first; });
  const auto count = static_cast<std::size_t>(end - uses.by_time.begin());
  auto it = uses.sets.find(count);
  if (it == uses.sets.end()) {
    std::vector<std::size_t> facts;
    facts.reserve(count);
    for (auto u = uses.by_time.begin(); u != end; ++u) {
      facts.push_back(u->second);
    }
    std::sort(facts.begin(), facts.end());
    // The check, from the floods themselves.
    for (const std::size_t f : facts) {
      if (timeline_->facts.at(f).old) {
        continue;
      }
      const std::vector<double>& learn = floods_->facts.at(f).learn_s;
      if (node >= learn.size() || !(t >= learn[node] + compute_s_)) {
        ++early_;
      }
    }
    it = uses.sets.emplace(count, std::move(facts)).first;
  }
  return it->second;
}

std::optional<double> FloodKnowledge::next_use(std::size_t node, double after) {
  const Uses& uses = uses_of(node);
  const auto it = std::upper_bound(
      uses.by_time.begin(), uses.by_time.end(), after,
      [](double value, const std::pair<double, std::size_t>& u) { return value < u.first; });
  return it == uses.by_time.end() ? std::nullopt : std::optional(it->first);
}

FloodTables::FloodTables(const Timeline& timeline, routing_app::GraphLayout layout)
    : timeline_(&timeline), layout_(layout) {}

void FloodTables::plan(network::Graph superset) {
  superset_ = std::move(superset);
  for (auto& [facts, entry] : entries_) {
    entry->graph.reset();
    entry->trees.clear();
  }
}

FloodTables::Entry& FloodTables::entry(const std::vector<std::size_t>& facts) {
  std::unique_ptr<Entry>& e = entries_[facts];
  if (!e) {
    e = std::make_unique<Entry>();
    std::vector<double> knowledge(timeline_->facts.size(), kInfinity);
    for (const std::size_t f : facts) {
      knowledge.at(f) = 0.0;
    }
    e->view = view_at(*timeline_, knowledge, 0.0);
  }
  return *e;
}

const ViewState& FloodTables::view(const std::vector<std::size_t>& facts) {
  return entry(facts).view;
}

const network::Graph& FloodTables::graph(const std::vector<std::size_t>& facts) {
  Entry& e = entry(facts);
  if (!e.graph) {
    e.graph = std::make_unique<network::Graph>(view_graph(superset_, layout_, *timeline_, e.view));
  }
  return *e.graph;
}

const network::PathsTo& FloodTables::tree(const std::vector<std::size_t>& facts,
                                          std::size_t target) {
  Entry& e = entry(facts);
  auto it = e.trees.find(target);
  if (it == e.trees.end()) {
    it = e.trees.emplace(target, network::shortest_paths_to(graph(facts), target)).first;
  }
  return it->second;
}

}  // namespace satsim::failures_app
