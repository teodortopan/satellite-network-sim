#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "inputs/inputs.hpp"
#include "satsim/ground/distance.hpp"
#include "satsim/ground/sites.hpp"
#include "satsim/network/graph.hpp"
#include "satsim/network/shortest_path.hpp"
#include "satsim/topology/ground_links.hpp"
#include "satsim/topology/link_plan.hpp"
#include "satsim/topology/link_state.hpp"

// The two endpoint models of spec 4.6: how a city pair's one-way delay is read off one time
// step's network. Kept in the program's library, on top of the generic shortest path
// (satsim::network) and the step's links (satsim::topology).
//
// Every link's delay is its length divided by the speed of light in a vacuum
// (kSpeedOfLightVacuumKmPerS), lasers and radio alike; fibre is ground::fibre_delay_s over the
// great-circle distance with the run's fibre factor.

namespace satsim::topology_app {

/// Which endpoint model a run uses (spec 4.6); shared with the routing program.
using app::EndpointModel;
using app::name_of;
using app::parse_endpoint_model;

/// The outcome of one direction at one step.
enum class PathStatus : std::uint8_t {
  kOk,
  /// Realistic: the sending city has no home PoP (no Starlink service there).
  kNoService,
  /// Realistic: the sending city sees no satellite at or above its mask. Best case: either
  /// city sees none.
  kNoCoverage,
  /// Realistic: no satellite that an operational gateway sees can be reached over up laser
  /// links from the serving satellite (or there is no gateway).
  kNoGateway,
  /// Best case: both cities see satellites, but no chain of up laser links joins them.
  kNoPath,
};

inline constexpr std::array<PathStatus, 5> kAllPathStatuses = {
    PathStatus::kOk, PathStatus::kNoService, PathStatus::kNoCoverage, PathStatus::kNoGateway,
    PathStatus::kNoPath};

/// "ok", "no_service", "no_coverage", "no_gateway", "no_path".
[[nodiscard]] std::string_view name_of(PathStatus status) noexcept;

/// Where a gateway comes from, and an operational gateway (shared with the routing program).
using app::GatewaySite;
using app::GatewaySource;

/// One direction of a city pair: from (the Starlink customer in the realistic model) to.
/// Indices into the evaluator's cities.
struct Direction {
  std::size_t from = 0;
  std::size_t to = 0;
};

/// The result of one direction at one step. Satellites are indices into the classification
/// (Classification::satellites). Every length and delay is set (and meaningful) only when
/// status is kOk; otherwise they stay 0 and the optional fields are empty, except
/// serving_satellite, which is also set for kNoGateway (the satellite that found no gateway).
struct PathResult {
  PathStatus status = PathStatus::kNoPath;
  double one_way_s = 0.0;
  /// Laser links on the path: 0 for a bent pipe (one satellite up and down).
  std::size_t laser_hops = 0;
  double uplink_km = 0.0;    ///< Sending dish to the first satellite (slant range).
  double laser_km = 0.0;     ///< Sum of the laser links' lengths.
  double downlink_km = 0.0;  ///< Last satellite to the gateway (realistic) or the dish.
  /// Realistic: great-circle km gateway to home PoP plus home PoP to the destination city.
  /// The fibre delay is fibre_km * fibre factor / c. Best case: 0.
  double fibre_km = 0.0;

  /// Realistic: the sending city's serving satellite, the satellite the gateway is reached
  /// through, and the gateway (index into the evaluator's gateways).
  std::optional<std::size_t> serving_satellite;
  std::optional<std::size_t> gateway_satellite;
  std::optional<std::size_t> gateway;
  /// Best case: the first satellite (seen by the sending city) and the last (seen by the
  /// receiving city).
  std::optional<std::size_t> first_satellite;
  std::optional<std::size_t> last_satellite;

  /// uplink + laser + downlink + fibre.
  [[nodiscard]] double path_km() const noexcept {
    return uplink_km + laser_km + downlink_km + fibre_km;
  }
};

/// Settings of the endpoint models.
struct EndpointSettings {
  EndpointModel model = EndpointModel::kRealistic;
  /// ground::fibre_delay_s's inflation. Precondition: finite and >= 1.
  double fibre_factor = ground::kDefaultFibreRouteInflation;
  topology::ServingRule serving_rule = topology::ServingRule::kHighestElevation;
};

/// The graph of one time step, as the endpoint models read it. Nodes 0 .. S-1 are the
/// satellites (classification indices). Each city c then has two nodes: its dish sending
/// (S + 2c), with an edge to every satellite it sees, and its dish receiving (S + 2c + 1),
/// with an edge from every satellite it sees. No edge enters a sending node or leaves a
/// receiving one, so no path passes through a city: the best case stays in space. Laser
/// links that are up are undirected edges between satellites.
class StepNetwork {
 public:
  StepNetwork(std::size_t satellite_count, std::size_t city_count);

  /// Rebuilds the edges for one step: the up links of `plan` (states in `links`, one per
  /// planned link, length set when up) and the cities' ground links (`city_visible[c]`, one
  /// list per city, slant ranges in km). Returns an error if a delay is refused by the graph
  /// (a negative or non-finite length) or the sizes disagree; the graph is then unusable.
  [[nodiscard]] std::optional<std::string> build(
      const topology::LinkPlan& plan, const topology::StepLinks& links,
      std::span<const std::vector<topology::VisibleSatellite>> city_visible);

  [[nodiscard]] const network::Graph& graph() const noexcept { return graph_; }
  [[nodiscard]] std::size_t satellite_count() const noexcept { return satellite_count_; }
  [[nodiscard]] std::size_t sending_node(std::size_t city) const noexcept {
    return satellite_count_ + 2 * city;
  }
  [[nodiscard]] std::size_t receiving_node(std::size_t city) const noexcept {
    return satellite_count_ + 2 * city + 1;
  }

 private:
  std::size_t satellite_count_;
  std::size_t city_count_;
  network::Graph graph_;
};

/// Computes every direction's result at one step, under one endpoint model.
///
/// Best case, A to B: the shortest path from A's sending node to B's receiving node. One
/// Dijkstra per distinct sending city per step.
///
/// Realistic, A to B, in this order:
///  1. A without a home PoP: kNoService.
///  2. A's serving satellite s (topology::pick_serving_satellite under the serving rule among
///     A's visible satellites); none: kNoCoverage.
///  3. One Dijkstra from s over the laser links. For every operational gateway g and every
///     satellite v that g sees at this step (v == s is the bent pipe, zero laser hops), the
///     candidate cost is delay(s, v) + slant(v, g) / c + fibre(g, A's home PoP). The gateway
///     chosen is the candidate with the smallest cost; ties go to fewer laser hops, then the
///     lower gateway index, then the lower satellite index. None reachable: kNoGateway.
///  4. The one-way delay is slant(A, s) / c + that cost + fibre(A's home PoP, B).
/// B's home PoP and B's satellites play no part in A to B: B is a server on the internet. The
/// reverse direction is its own Direction.
class EndpointEvaluator {
 public:
  /// `cities` are the sites of every city a direction names; `home_pops[c]` the home PoP site
  /// of city c (nullopt: no service), used by the realistic model only; `gateways` the
  /// operational gateways, used by the realistic model only (the order fixes ties).
  /// Preconditions: home_pops.size() == cities.size(), every direction names cities in range,
  /// settings.fibre_factor finite and >= 1.
  EndpointEvaluator(std::vector<ground::GroundSite> cities,
                    std::vector<std::optional<ground::GroundSite>> home_pops,
                    std::vector<GatewaySite> gateways, std::vector<Direction> directions,
                    std::size_t satellite_count, const EndpointSettings& settings);

  /// One result per direction (constructor order) at one step. `city_visible[c]` are city c's
  /// visible satellites and `gateway_visible[g]` gateway g's (GroundVisibility::at, the same
  /// order as the constructor's lists). Returns an error only if the step's graph cannot be
  /// built (StepNetwork::build).
  [[nodiscard]] std::optional<std::string> evaluate(
      const topology::LinkPlan& plan, const topology::StepLinks& links,
      std::span<const std::vector<topology::VisibleSatellite>> city_visible,
      std::span<const std::vector<topology::VisibleSatellite>> gateway_visible,
      std::vector<PathResult>& results);

  [[nodiscard]] const std::vector<Direction>& directions() const noexcept { return directions_; }
  [[nodiscard]] const std::vector<GatewaySite>& gateways() const noexcept { return gateways_; }
  [[nodiscard]] const EndpointSettings& settings() const noexcept { return settings_; }

 private:
  [[nodiscard]] PathResult best_case(
      const Direction& d, const network::ShortestPaths& from_a,
      std::span<const std::vector<topology::VisibleSatellite>> city_visible) const;
  [[nodiscard]] PathResult realistic(
      std::size_t direction, const std::optional<topology::VisibleSatellite>& serving,
      const network::ShortestPaths* from_serving,
      std::span<const std::vector<topology::VisibleSatellite>> gateway_visible) const;

  std::vector<ground::GroundSite> cities_;
  std::vector<std::optional<ground::GroundSite>> home_pops_;
  std::vector<GatewaySite> gateways_;
  std::vector<Direction> directions_;
  EndpointSettings settings_;
  StepNetwork network_;
  /// [city * gateways + g]: great-circle km from gateway g to city's home PoP (NaN without
  /// one).
  std::vector<double> gateway_to_pop_km_;
  /// [direction]: great-circle km from the sending city's home PoP to the receiving city.
  std::vector<double> pop_to_destination_km_;
};

}  // namespace satsim::topology_app
