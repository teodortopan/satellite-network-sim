#pragma once

#include <Eigen/Core>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "inputs/inputs.hpp"
#include "satsim/ground/sites.hpp"
#include "satsim/time/utc_time.hpp"
#include "satsim/topology/ground_links.hpp"
#include "satsim/topology/link_state.hpp"

// The geometry of a run at any step, in random access (Phase 4 spec 4.3: the true network at
// an instant is the geometry of the last step at or before it). An interface, so that the
// timeline and overlay tests can give hand-made geometry.
//
// Steps are numbered from 0 at the run's start; step k is at start + k * step.

namespace satsim::failures_app {

/// What a run's geometry is at one step.
struct StepGeometry {
  /// The step's UTC time (the overlays copy it into their StepLinks).
  time::UtcTime utc;
  /// Per satellite of the classification: its Earth-fixed position, nullopt without one.
  std::vector<std::optional<Eigen::Vector3d>> position_ecef_km;
  /// Per city and per gateway: the satellites it may use, in increasing satellite index
  /// (topology::GroundVisibility::at).
  std::vector<std::vector<topology::VisibleSatellite>> city_visible;
  std::vector<std::vector<topology::VisibleSatellite>> gateway_visible;
};

class Geometry {
 public:
  Geometry() = default;
  virtual ~Geometry() = default;
  Geometry(const Geometry&) = delete;
  Geometry& operator=(const Geometry&) = delete;
  Geometry(Geometry&&) = delete;
  Geometry& operator=(Geometry&&) = delete;

  /// The geometry of step `step` (>= 0), in any order. The reference stays valid at least
  /// until the next call of at() or link_state() for another step.
  [[nodiscard]] virtual const StepGeometry& at(std::int64_t step) = 0;

  /// The geometric state of a laser link between satellites a and b at `step`
  /// (topology::geometric_link_state with the run's limits; no lock-on).
  [[nodiscard]] virtual topology::LinkState link_state(std::int64_t step, std::size_t a,
                                                       std::size_t b) = 0;
};

/// The real geometry of a run, from the prepared inputs, exactly as app::StepSource makes it:
/// positions by topology::positions_at with the inputs' propagators and the Earth orientation
/// of the step's UTC time (start + step * step_ns, app::time_of_step); ground links by one
/// topology::GroundVisibility over every city then every gateway with options.mask; a link's
/// state by topology::geometric_link_state with options.limits and, when the Sun exclusion is
/// on, frames::sun_direction_ecef of the step (as topology::LinkStateEvaluator). Lock-on is not
/// applied (the failures program's lock-on is the acquisition time).
///
/// The last `cache_steps` steps asked for are kept. A step with no Earth-orientation values
/// (prepare_inputs checked every step of the run and its look-ahead, so only a step beyond
/// them) has no positions and no ground links, and error() names it.
class SnapshotGeometry final : public Geometry {
 public:
  /// `inputs` must outlive the geometry. cache_steps >= 1 (raised to 1).
  SnapshotGeometry(const app::Inputs& inputs, const app::InputOptions& options,
                   std::size_t cache_steps = 4);

  [[nodiscard]] const StepGeometry& at(std::int64_t step) override;
  [[nodiscard]] topology::LinkState link_state(std::int64_t step, std::size_t a,
                                               std::size_t b) override;

  /// The first step asked for that had no Earth-orientation values, or nullopt.
  [[nodiscard]] const std::optional<std::string>& error() const noexcept { return error_; }

 private:
  struct Cached {
    std::int64_t step = 0;
    StepGeometry geometry;
    std::optional<Eigen::Vector3d> sun_direction_ecef = std::nullopt;
  };

  const Cached& cached(std::int64_t step);

  const app::Inputs* inputs_;
  time::UtcTime start_;
  std::int64_t step_ns_;
  topology::LinkLimits limits_;
  std::vector<ground::GroundSite> sites_;
  std::size_t city_count_;
  topology::GroundVisibility visibility_;
  std::size_t cache_steps_;
  /// Most recently used last.
  std::vector<std::unique_ptr<Cached>> cache_;
  std::optional<std::string> error_;
};

}  // namespace satsim::failures_app
