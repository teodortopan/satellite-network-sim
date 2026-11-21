#pragma once

// Hand-made geometry for the failures program's tests: visible lists and laser link states
// written out by hand, so that every detection and report instant can be worked out on paper,
// and a geometry adapter on ideal Walker shells (tests/topology/ideal.hpp) for re-matching,
// which needs real orbits.

#include <Eigen/Core>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <tuple>
#include <utility>
#include <vector>

#include "failures/geometry.hpp"
#include "satsim/orbit/propagator.hpp"
#include "satsim/time/utc_time.hpp"
#include "satsim/topology/ground_links.hpp"
#include "satsim/topology/link_state.hpp"
#include "topology/ideal.hpp"

namespace satsim::failures_app::testing {

/// Any fixed instant (2026-11-18T21:15:00 UTC).
[[nodiscard]] time::UtcTime some_time();

/// A satellite a site sees at `slant_km` (catalogue number 1000 + satellite).
[[nodiscard]] topology::VisibleSatellite seen(std::size_t satellite, double slant_km);

/// Geometry written by hand.
///  - at(k): steps[k] if set, else `base`.
///  - link_state(k, a, b): states[(k, min, max)] if set, else up with lengths_km[(min, max)]
///    (1000 km if not set).
class FakeGeometry final : public Geometry {
 public:
  StepGeometry base{
      .utc = some_time(), .position_ecef_km = {}, .city_visible = {}, .gateway_visible = {}};
  std::map<std::int64_t, StepGeometry> steps;
  std::map<std::pair<std::size_t, std::size_t>, double> lengths_km;
  std::map<std::tuple<std::int64_t, std::size_t, std::size_t>, topology::LinkState> states;

  /// The link between a and b is down (no line of sight) at steps from .. to - 1.
  void down_between(std::size_t a, std::size_t b, std::int64_t from, std::int64_t to);
  void set_length(std::size_t a, std::size_t b, double km);

  [[nodiscard]] const StepGeometry& at(std::int64_t step) override;
  [[nodiscard]] topology::LinkState link_state(std::int64_t step, std::size_t a,
                                               std::size_t b) override;
};

/// The geometry of ideal shells: positions by SGP4 at the reference time + k * step_s, rotated
/// to ITRF with zero Earth orientation (as ideal_shells does at the reference time); no ground
/// sites; link states by topology::geometric_link_state with default limits.
class IdealGeometry final : public Geometry {
 public:
  IdealGeometry(const topology::testing::Ideal& ideal, const time::UtcTime& reference,
                double step_s);

  [[nodiscard]] const StepGeometry& at(std::int64_t step) override;
  [[nodiscard]] topology::LinkState link_state(std::int64_t step, std::size_t a,
                                               std::size_t b) override;

 private:
  std::vector<orbit::Propagator> propagators_;
  time::UtcTime reference_;
  double step_s_;
  std::map<std::int64_t, std::unique_ptr<StepGeometry>> cache_;
};

}  // namespace satsim::failures_app::testing
