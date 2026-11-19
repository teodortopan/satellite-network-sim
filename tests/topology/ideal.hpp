#pragma once

// Ideal shells for the topology tests, classified by construction rather than by
// classify_constellation (which measures every orbit over a full period and is slow under the
// sanitizers and Valgrind): satellite s of plane p of a Walker shell is put at plane p, index
// s. Positions are SGP4's at the reference time, rotated to ITRF with zero Earth orientation.

#include <Eigen/Core>
#include <cstddef>
#include <functional>
#include <optional>
#include <span>
#include <vector>

#include "satsim/constellation/classification.hpp"
#include "satsim/ground/sites.hpp"
#include "satsim/orbit/element_set.hpp"
#include "satsim/time/utc_time.hpp"
#include "satsim/topology/positions.hpp"
#include "support/synthetic.hpp"

namespace satsim::topology::testing {

using constellation::testing::WalkerShell;

/// Which satellites to leave out: (shell, plane, index in the Walker shell).
using SatellitePredicate = std::function<bool(std::size_t, std::size_t, std::size_t)>;

struct IdealOptions {
  /// Satellites removed altogether (absent from the classification).
  SatellitePredicate drop;
  /// Satellites kept in the classification but excluded (ExclusionReason::kNoLasers), so in no
  /// plane.
  SatellitePredicate exclude;
};

struct Ideal {
  constellation::Classification classification;
  std::vector<orbit::ElementSet> element_sets;  ///< In classification order.
  /// At the reference time, one per classified satellite.
  std::vector<std::optional<Eigen::Vector3d>> positions_ecef_km;
};

/// Walker shells, one shell each (their first_id ranges must not overlap and must increase, so
/// that classification order is catalogue order). Every member's mean height is the shell's
/// height_km. The reference time is each shell's epoch (they should all have the same one).
[[nodiscard]] Ideal ideal_shells(std::span<const WalkerShell> shells,
                                 const IdealOptions& options = {});

/// StepPositions at `utc` holding `positions` and zero Earth orientation.
[[nodiscard]] StepPositions step_positions(
    const time::UtcTime& utc, const std::vector<std::optional<Eigen::Vector3d>>& positions);

/// The Walker shells' common epoch.
[[nodiscard]] time::UtcTime epoch_of(const WalkerShell& shell);

}  // namespace satsim::topology::testing
