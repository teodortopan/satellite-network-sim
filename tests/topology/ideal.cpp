#include "ideal.hpp"

#include <Eigen/Core>
#include <cstddef>
#include <optional>
#include <span>
#include <utility>
#include <variant>
#include <vector>

#include "satsim/constellation/classification.hpp"
#include "satsim/constellation/geometry.hpp"
#include "satsim/frames/teme_ecef.hpp"
#include "satsim/orbit/element_set.hpp"
#include "satsim/orbit/propagator.hpp"
#include "satsim/time/earth_orientation.hpp"
#include "satsim/time/utc_time.hpp"
#include "satsim/topology/positions.hpp"
#include "support/synthetic.hpp"

namespace satsim::topology::testing {

time::UtcTime epoch_of(const WalkerShell& shell) {
  return constellation::testing::utc(shell.epoch);
}

Ideal ideal_shells(std::span<const WalkerShell> shells, const IdealOptions& options) {
  Ideal ideal;
  ideal.classification.planes.resize(shells.size());
  for (std::size_t shell = 0; shell < shells.size(); ++shell) {
    const WalkerShell& w = shells[shell];
    const time::UtcTime reference = epoch_of(w);
    const std::vector<orbit::ElementSet> sets = constellation::testing::walker_element_sets(w);
    std::vector<constellation::Plane>& planes = ideal.classification.planes[shell];
    planes.resize(w.planes);
    for (std::size_t p = 0; p < w.planes; ++p) {
      planes[p].mean_raan_teme_rad = sets[p * w.per_plane].raan_rad;
      for (std::size_t s = 0; s < w.per_plane; ++s) {
        if (options.drop && options.drop(shell, p, s)) {
          continue;
        }
        const orbit::ElementSet& set = sets[(p * w.per_plane) + s];
        const std::size_t index = ideal.classification.satellites.size();
        constellation::ClassifiedSatellite satellite;
        satellite.norad_cat_id = set.norad_cat_id;
        satellite.object_name = set.object_name;
        satellite.object_id = set.object_id;
        satellite.shell_index = shell;
        if (options.exclude && options.exclude(shell, p, s)) {
          satellite.placement = constellation::ExclusionReason::kNoLasers;
        } else {
          satellite.placement = constellation::PlaneAssignment{
              .plane_index = p, .index_in_plane = planes[p].members.size()};
          planes[p].members.push_back(index);
        }
        const orbit::PropagatorOrError built = orbit::Propagator::create(set);
        const orbit::StateOrError state = std::get<orbit::Propagator>(built).propagate(reference);
        const auto& teme = std::get<orbit::TemeState>(state);
        // The orbit plane and argument of latitude from the state at the reference time (the
        // osculating values; classify_constellation would average them over an orbit).
        constellation::OrbitGeometry geometry;
        geometry.mean_normal_teme =
            constellation::orbit_normal_teme(teme.position_km, teme.velocity_km_s);
        geometry.inclination_rad = constellation::inclination_rad(geometry.mean_normal_teme);
        geometry.raan_teme_rad = constellation::raan_teme_rad(geometry.mean_normal_teme);
        geometry.argument_of_latitude_rad =
            constellation::argument_of_latitude_rad(teme.position_km, geometry.mean_normal_teme);
        geometry.mean_height_km = w.height_km;
        geometry.mean_radius_km = 6378.135 + w.height_km;
        satellite.geometry = geometry;
        ideal.classification.satellites.push_back(satellite);
        ideal.element_sets.push_back(set);
        ideal.positions_ecef_km.emplace_back(
            frames::teme_to_ecef(teme.position_km, reference, time::EarthOrientation{}));
      }
    }
    // Planes left empty by drop or exclude are not planes: remove them and renumber.
    std::vector<constellation::Plane> kept;
    for (constellation::Plane& plane : planes) {
      if (plane.members.empty()) {
        continue;
      }
      for (const std::size_t member : plane.members) {
        std::get<constellation::PlaneAssignment>(ideal.classification.satellites[member].placement)
            .plane_index = kept.size();
      }
      kept.push_back(std::move(plane));
    }
    planes = std::move(kept);
  }
  return ideal;
}

StepPositions step_positions(const time::UtcTime& utc,
                             const std::vector<std::optional<Eigen::Vector3d>>& positions) {
  return StepPositions{
      .utc = utc,
      .earth_orientation = {},
      .position_ecef_km = positions,
      .propagation_error = std::vector<std::optional<orbit::PropagationError>>(positions.size()),
      .failed_count = 0};
}

}  // namespace satsim::topology::testing
