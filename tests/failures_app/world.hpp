#pragma once

// The hand-made world of the timeline and view tests, on the failure tests' small network
// (tests/failure/fixture.hpp): 22 satellites (16 in shell 0, 6 in shell 1), the six planned
// links
//   0: 0-1 (1000 km)   1: 1-2 (1500 km)   2: 0-4 (2000 km)
//   3: 4-5 (1000 km)   4: 1-5 (1000 km)   5: 16-17 (1000 km)
// two cities and two gateways, seen at every step as
//   city 0:    satellite 1 at 500 km, satellite 5 at 550 km
//   city 1:    satellite 16 at 600 km
//   gateway 0: satellite 1 at 600 km, satellite 2 at 700 km
//   gateway 1: satellite 5 at 800 km, satellite 16 at 900 km
// Control-graph nodes: satellites 0 .. 21, dishes 22 and 23, gateways 24 and 25.
// Settings: 1 s steps, 100 steps, hop delay 0.002 s, the default reaction times (detection
// 0.1 s, per hop 0.001 s, planner processing 0, acquisition 30 s), re-pointing off.

#include <cstddef>
#include <vector>

#include "failures/timeline.hpp"
#include "fake_geometry.hpp"
#include "inputs/inputs.hpp"
#include "satsim/core/physical_constants.hpp"
#include "satsim/failure/schedule.hpp"

namespace satsim::failures_app::testing {

inline constexpr double kHopDelayS = 0.002;

/// The cost of one control-graph hop over `km` as shortest_paths_from_many forms it:
/// (km / c + hop delay) + per-hop time.
[[nodiscard]] inline double hop_s(double km, double hop_processing_s = 0.001) {
  return (km / kSpeedOfLightVacuumKmPerS + kHopDelayS) + hop_processing_s;
}

inline constexpr std::size_t kDish0 = 22;
inline constexpr std::size_t kDish1 = 23;
inline constexpr std::size_t kGateway0 = 24;
inline constexpr std::size_t kGateway1 = 25;

struct World {
  FakeGeometry geometry;
  TimelineSettings settings;

  explicit World(app::EndpointModel model = app::EndpointModel::kBestCase);

  /// build_timeline on the small network with `failures`.
  [[nodiscard]] Timeline timeline(const std::vector<failure::Failure>& failures);
};

}  // namespace satsim::failures_app::testing
