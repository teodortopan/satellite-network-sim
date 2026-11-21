#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "inputs/inputs.hpp"
#include "satsim/network/graph.hpp"
#include "satsim/topology/ground_links.hpp"
#include "satsim/topology/link_plan.hpp"
#include "satsim/topology/link_state.hpp"

// The one graph of Phase 3 spec 4.2, for both endpoint models, built at every step. Pure: no
// I/O, no propagation. Every edge's delay is its length over the speed of light in a vacuum
// plus the hop delay (radio and lasers), or the fibre delay (gateway to end node).

namespace satsim::routing_app {

using app::EndpointModel;

/// What a node stands for.
enum class NodeKind : std::uint8_t {
  kSatellite,      ///< index: the satellite (Classification::satellites).
  kOpenDish,       ///< index: the city. Edges to every satellite the city sees.
  kReceivingDish,  ///< index: the city (best case). Edges from every satellite the city sees.
  kGateway,        ///< index: the gateway (realistic).
  kEnd,            ///< index: the direction (realistic). Edges from every gateway.
  kServingDish,    ///< index: the dish (realistic). One edge, to its serving satellite.
};

struct NodeMeaning {
  NodeKind kind = NodeKind::kSatellite;
  std::size_t index = 0;

  friend bool operator==(const NodeMeaning&, const NodeMeaning&) = default;
};

/// Node numbering. S satellites, C cities, and in the realistic model G gateways, D
/// directions and N serving dishes (0 of each in the best case):
///   satellite i            i
///   open dish of city c    S + 2c        (the topology program's sending node)
///   receiving dish of c    S + 2c + 1    (the topology program's receiving node)
///   gateway g              S + 2C + g
///   end of direction d     S + 2C + G + d
///   serving dish i         S + 2C + G + D + i
/// The best case is therefore numbered exactly as the topology program's graph, so the two
/// give the same shortest paths with the same ties.
class GraphLayout {
 public:
  GraphLayout() = default;
  GraphLayout(EndpointModel model, std::size_t satellites, std::size_t cities, std::size_t gateways,
              std::size_t directions, std::size_t serving_dishes) noexcept;

  [[nodiscard]] EndpointModel model() const noexcept { return model_; }
  [[nodiscard]] std::size_t satellite_count() const noexcept { return satellites_; }
  [[nodiscard]] std::size_t city_count() const noexcept { return cities_; }
  [[nodiscard]] std::size_t gateway_count() const noexcept { return gateways_; }
  [[nodiscard]] std::size_t direction_count() const noexcept { return directions_; }
  [[nodiscard]] std::size_t serving_dish_count() const noexcept { return serving_dishes_; }
  [[nodiscard]] std::size_t node_count() const noexcept;

  [[nodiscard]] std::size_t satellite(std::size_t i) const noexcept { return i; }
  [[nodiscard]] std::size_t open_dish(std::size_t city) const noexcept {
    return satellites_ + 2 * city;
  }
  [[nodiscard]] std::size_t receiving_dish(std::size_t city) const noexcept {
    return satellites_ + 2 * city + 1;
  }
  [[nodiscard]] std::size_t gateway(std::size_t g) const noexcept {
    return satellites_ + 2 * cities_ + g;
  }
  [[nodiscard]] std::size_t end(std::size_t direction) const noexcept {
    return satellites_ + 2 * cities_ + gateways_ + direction;
  }
  [[nodiscard]] std::size_t serving_dish(std::size_t dish) const noexcept {
    return satellites_ + 2 * cities_ + gateways_ + directions_ + dish;
  }

  /// Precondition: node < node_count().
  [[nodiscard]] NodeMeaning meaning(std::size_t node) const noexcept;

 private:
  EndpointModel model_ = EndpointModel::kBestCase;
  std::size_t satellites_ = 0;
  std::size_t cities_ = 0;
  std::size_t gateways_ = 0;
  std::size_t directions_ = 0;
  std::size_t serving_dishes_ = 0;
};

/// One step's network as the graph reads it.
struct StepGround {
  const topology::StepLinks* links = nullptr;  ///< One state per planned link.
  /// Per city (C lists) and per gateway (G lists): the satellites it may use, in increasing
  /// satellite index (GroundVisibility::at).
  std::span<const std::vector<topology::VisibleSatellite>> city_visible;
  std::span<const std::vector<topology::VisibleSatellite>> gateway_visible;
};

/// The visible satellite `satellite` of `visible` (sorted by satellite), or nullptr.
[[nodiscard]] const topology::VisibleSatellite* find_visible(
    std::span<const topology::VisibleSatellite> visible, std::size_t satellite) noexcept;

/// Builds the graph of spec 4.2 for one step (or, for the whole-interval planner, the edges
/// that exist at every step of an interval).
class GraphBuilder {
 public:
  /// `plan`: the laser links (must outlive the builder). `end_delays_s[d * G + g]`: the fibre
  /// delay from gateway g to direction d's end node, the fibre from g to the sender's home PoP
  /// plus the fibre from the home PoP to the destination (NaN: no edge, a sender without a
  /// home PoP); realistic only, D × G values. `dish_cities[i]`: the city of serving dish i.
  /// hop_delay_s: added to every radio and laser edge (finite, >= 0).
  GraphBuilder(GraphLayout layout, const topology::LinkPlan& plan, std::vector<double> end_delays_s,
               std::vector<std::size_t> dish_cities, double hop_delay_s);

  [[nodiscard]] const GraphLayout& layout() const noexcept { return layout_; }

  /// The edges of steps[0] that exist at every step of `steps` (one step: just its graph),
  /// with steps[0]'s delays, into `graph` (resized to layout().node_count(), edges cleared):
  ///  - satellites: an undirected edge for every planned link that is up;
  ///  - open dish of every city: an edge to every satellite the city sees;
  ///  - best case: from every satellite a city sees to its receiving dish;
  ///  - realistic: from every satellite gateway g sees to gateway g; from every gateway to
  ///    every end node whose end delay is set; from serving dish i to serving[i], if set and
  ///    city dish_cities[i] sees it (tested only at the first serving_steps steps: the rest of
  ///    the slot; at later steps the dish may have another satellite, which is not predicted).
  ///    With serving_dishes_open, each serving dish instead has an edge to every satellite
  ///    its city sees (the with-route rule's "if the dish could use any visible satellite").
  /// Returns an error if a delay is refused or the sizes disagree; the graph is then unusable.
  [[nodiscard]] std::optional<std::string> build(
      std::span<const StepGround> steps, std::span<const std::optional<std::size_t>> serving,
      std::size_t serving_steps, bool serving_dishes_open, network::Graph& graph) const;

 private:
  GraphLayout layout_;
  const topology::LinkPlan* plan_;
  std::vector<double> end_delays_s_;
  std::vector<std::size_t> dish_cities_;
  double hop_delay_s_;
};

}  // namespace satsim::routing_app
