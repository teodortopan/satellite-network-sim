#pragma once

#include <cstddef>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

#include "failures/geometry.hpp"
#include "failures/timeline.hpp"
#include "failures/view.hpp"
#include "routing/graph.hpp"
#include "satsim/network/graph.hpp"
#include "satsim/network/paths_to.hpp"

// The flooding reaction mode's knowledge (Phase 4 spec 4.5, 6.1; docs/phase-4-plan.md, design
// choice 5): when each node learns each fact, when it uses it in its tables, and its tables.
//
// Floods. A fact that is not old and has a detector whose detection instant is not after the
// run's end ((step_count - 1) * step_s, spec 4.4 Details) floods. Let t0 be the earliest such
// detection instant. The flood is worked out entirely on the messages graph (control_graph) of
// the true network at t0, on the geometry of step(t0) (overlay_truth at t0):
// network::shortest_paths_from_many with one start per such detector, of delay
// (its detection instant - t0), and extra_per_edge_s = hop_processing_s. A node that stays a
// start learns at its own detection instant; any other node it reaches at t0 + its total; a
// node it does not reach never learns. A detection after the run's end starts nothing; a fact
// with no other detector has no flood (no node learns it, 0 nodes, 0 messages, never).
// Messages: every node that learns sends the fact once on each of its edges of that messages
// graph but the one it first came in on (to its predecessor, one edge even if there are
// parallel ones); a node that stays a start sends on all its edges.
//
// Use. A node uses a fact in its tables from its learning instant plus compute_s (spec 4.5);
// an old fact from instant 0 (every node has it from the first step). In the realistic model
// dishes have no edge in the messages graph, so they learn nothing by flood.
//
// Tables (spec 4.5). At every plan a superset planning graph is built from geometry alone: the
// planning graph of that plan's window (as routing_app::Engine plans) on the network in which
// every run link counts as in force and nothing is down. A node's view graph is that graph
// without the edges its view (view_at of the facts it uses) rules out (view_graph). Its table
// towards a target is network::shortest_paths_to on its view graph. Views, view graphs and
// trees are cached by the set of facts used, which many nodes share.

namespace satsim::failures_app {

/// One fact's flood.
struct Flood {
  /// Per control-graph node (Timeline::nodes): the instant it learns the fact, +infinity for
  /// never. Empty when the fact starts no flood.
  std::vector<double> learn_s;
  /// Nodes that learn it, detectors included.
  std::size_t nodes = 0;
  /// The last learning instant; nullopt without a flood.
  std::optional<double> last_s = std::nullopt;
  /// Every sending of the fact on a link.
  std::size_t messages = 0;
};

struct Floods {
  /// Per fact of Timeline::facts.
  std::vector<Flood> facts;
  /// The check of spec 6.1 "when a flood is done, every node that the network joins to a
  /// detecting node has the fact": nodes in a piece (network::connected_pieces of the flood's
  /// messages graph) that holds one of its starts, which never learn it. 0 for a correct run.
  std::size_t unreached = 0;

  /// Every message of every flood.
  [[nodiscard]] std::size_t messages() const noexcept;
};

/// Test seam: changes a flood once it is worked out (fact index, flood).
using AfterFlood = std::function<void(std::size_t, Flood&)>;

/// The flood of every fact of `timeline` (see the header comment), on `geometry` with the
/// settings the timeline was built with.
[[nodiscard]] Floods flood_facts(const Timeline& timeline, const TimelineSettings& settings,
                                 Geometry& geometry, const AfterFlood& after_flood = {});

/// The view graph (spec 4.5): `superset` (a GraphBuilder graph of `layout`) with these edges
/// left out, the others copied in the same order (node by node, each node's edges in order):
///  - a laser edge between satellites a and b unless some link of the run joining them
///    (Timeline::links, either way round) is believed to carry traffic, a planned link
///    (index < planned_count) whose belief is not kDown or a repair link whose belief is kUp,
///    or if `view` takes a or b as down;
///  - an edge between a dish (open, receiving or serving) and a satellite the view takes as
///    down;
///  - an edge from a satellite to a gateway if the view takes either as down;
///  - an edge from a gateway the view takes as down to an end node.
/// With nothing believed down and no repair link it is `superset` exactly.
[[nodiscard]] network::Graph view_graph(const network::Graph& superset,
                                        const routing_app::GraphLayout& layout,
                                        const Timeline& timeline, const ViewState& view);

/// The view of the superset network: every planned link with no belief, every repair link
/// believed up, nothing down. overlay_view of it puts every run link in force.
[[nodiscard]] ViewState all_in_force_view(const Timeline& timeline);

/// When each node uses each fact.
class FloodKnowledge {
 public:
  /// Test seam: changes a use instant (fact, node, instant) -> instant.
  using UseHook = std::function<double(std::size_t, std::size_t, double)>;

  /// `timeline` and `floods` must outlive the object.
  FloodKnowledge(const Timeline& timeline, const Floods& floods, double compute_s,
                 UseHook hook = {});

  /// The instant control node `node` starts using fact `fact`: 0 for an old fact; the node's
  /// learning instant + compute_s when it learns it; +infinity otherwise.
  [[nodiscard]] double use_s(std::size_t fact, std::size_t node) const;

  /// The facts `node` uses at `t` (use_s <= t), in increasing fact order. Valid while the
  /// object lives.
  [[nodiscard]] const std::vector<std::size_t>& facts_at(std::size_t node, double t);

  /// The first instant after `after` at which `node` starts using a fact; nullopt for none.
  [[nodiscard]] std::optional<double> next_use(std::size_t node, double after);

  /// The latest finite use instant of any fact by any node; nullopt for none.
  [[nodiscard]] std::optional<double> last_use() const noexcept { return last_use_; }

  /// The check of spec 6.1 "no table before the node's learning time plus the compute time":
  /// sets of facts asked for at an instant before some member's learning instant (from the
  /// floods, written apart from use_s) + compute_s. 0 for a correct run.
  [[nodiscard]] std::size_t early() const noexcept { return early_; }

 private:
  struct Uses {
    bool built = false;
    /// (use instant, fact), finite only, increasing.
    std::vector<std::pair<double, std::size_t>> by_time;
    /// Number of facts in use -> that set, sorted.
    std::map<std::size_t, std::vector<std::size_t>> sets;
  };
  Uses& uses_of(std::size_t node);

  const Timeline* timeline_;
  const Floods* floods_;
  double compute_s_;
  UseHook hook_;
  std::vector<Uses> nodes_;
  std::optional<double> last_use_ = std::nullopt;
  std::size_t early_ = 0;
};

/// Views, view graphs and tables of fact sets, for the current plan.
class FloodTables {
 public:
  /// `timeline` must outlive the object.
  FloodTables(const Timeline& timeline, routing_app::GraphLayout layout);

  /// A new plan: its superset planning graph. Drops every view graph and tree.
  void plan(network::Graph superset);

  /// The view of a set of facts (sorted fact indices): view_at with each of them known.
  [[nodiscard]] const ViewState& view(const std::vector<std::size_t>& facts);
  /// Its view graph under the current plan.
  [[nodiscard]] const network::Graph& graph(const std::vector<std::size_t>& facts);
  /// Its tree towards `target` (network::shortest_paths_to on its view graph). Valid until the
  /// next plan.
  [[nodiscard]] const network::PathsTo& tree(const std::vector<std::size_t>& facts,
                                             std::size_t target);

 private:
  struct Entry {
    ViewState view;
    std::unique_ptr<network::Graph> graph;
    std::map<std::size_t, network::PathsTo> trees;
  };
  Entry& entry(const std::vector<std::size_t>& facts);

  const Timeline* timeline_;
  routing_app::GraphLayout layout_;
  network::Graph superset_;
  std::map<std::vector<std::size_t>, std::unique_ptr<Entry>> entries_;
};

}  // namespace satsim::failures_app
