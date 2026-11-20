#pragma once

#include <Eigen/Core>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

#include "satsim/constellation/classification.hpp"
#include "satsim/ground/sites.hpp"
#include "satsim/topology/geometry.hpp"
#include "satsim/topology/positions.hpp"

namespace satsim::topology {

// Ground links (spec 4.4): which satellites a ground site can use at a time step.

/// A satellite a site can use at one step.
struct VisibleSatellite {
  std::size_t satellite = 0;  ///< Index into Classification::satellites.
  std::int64_t norad_cat_id = 0;
  std::size_t shell_index = 0;
  double elevation_rad = 0.0;
  double slant_range_km = 0.0;
  double mask_rad = 0.0;  ///< The mask it was held to (shell height and site latitude).

  friend bool operator==(const VisibleSatellite&, const VisibleSatellite&) = default;
};

/// Default horizon of the time left in view (Phase 3 spec 4.4), s.
inline constexpr double kDefaultViewHorizonS = 900.0;

/// How many future positions time_left_in_view_s reads: ceil(horizon_s / step_s), the steps
/// k = 0 .. n-1 before the horizon. Precondition: step_s > 0 and horizon_s >= 0, both finite.
[[nodiscard]] std::size_t view_steps(double step_s,
                                     double horizon_s = kDefaultViewHorizonS) noexcept;

/// The time left in view of one satellite from one site (Phase 3 spec 4.4). future_ecef_km[k]
/// is the satellite's Earth-fixed position k steps from now (k = 0 is now), step_s apart, or
/// nullopt where its propagation failed. With k the first step at which the site may not use
/// it (ground_link_allowed with mask_rad and settings; a missing position ends the view),
/// returns min(k * step_s, horizon_s); if it may at every step k < view_steps(step_s,
/// horizon_s), horizon_s ("still in view at the horizon counts as the horizon"). So a satellite
/// not in view now has 0, and one in view now and out at the next step has step_s. Reads at
/// most view_steps entries; with fewer, the steps after the last count as out of view.
[[nodiscard]] double time_left_in_view_s(
    const SiteGeometry& site, double mask_rad,
    std::span<const std::optional<Eigen::Vector3d>> future_ecef_km, double step_s,
    double horizon_s = kDefaultViewHorizonS, const ElevationMaskSettings& settings = {});

/// Finds, for a fixed list of ground sites, the plane members each site may use.
///
/// The mask depends on the satellite's shell (its mean height, shell_mean_heights_km) and the
/// site's latitude (elevation_mask_rad); it is computed once per site and shell. A shell with
/// no plane member has no mask and no satellites. With mask_settings.min_elevation_rad that one
/// angle is every mask; with mask_settings.max_ground_range_km a site may use every plane
/// member within that slant range (ground_link_allowed), and VisibleSatellite::mask_rad and
/// mask_rad() are -pi/2 (no elevation test).
///
/// Speed: per step the plane members are sorted by their Earth-fixed z, and for each site
/// only those within the largest possible slant range in z are tested; each of those is first
/// tested without trigonometry or square roots (up . d >= 0 and (up . d)^2 >= |d|^2 sin^2
/// mask, which is exactly elevation >= mask for a mask in [0, pi/2)), and only satellites
/// passing that get the exact look angle. The final decision is made on the exact elevation.
class GroundVisibility {
 public:
  GroundVisibility(std::span<const ground::GroundSite> sites,
                   const constellation::Classification& classification,
                   const ElevationMaskSettings& mask_settings = {});

  /// For each site (in constructor order), the plane members at or above its mask at the
  /// step, in increasing satellite index (so increasing catalogue number). Satellites without
  /// a position at the step are skipped.
  [[nodiscard]] std::vector<std::vector<VisibleSatellite>> at(const StepPositions& positions) const;

  [[nodiscard]] std::size_t site_count() const noexcept { return sites_.size(); }

  /// The mask of `site` for satellites of `shell`; nullopt for a shell with no plane member.
  [[nodiscard]] std::optional<double> mask_rad(std::size_t site, std::size_t shell) const;

  /// time_left_in_view_s of `satellite` (index into Classification::satellites) from `site`,
  /// under this visibility's mask for the satellite's shell and its mask settings, so that a
  /// satellite at() lists at a step has a time left of at least one step from that step. 0 for
  /// a satellite that is not a plane member. future_ecef_km: that satellite's positions.
  [[nodiscard]] double time_left_in_view_s(
      std::size_t site, std::size_t satellite,
      std::span<const std::optional<Eigen::Vector3d>> future_ecef_km, double step_s,
      double horizon_s = kDefaultViewHorizonS) const;

  /// The time left in view of each satellite of `visible` (normally at(steps[0])[site]), in
  /// that order: steps[k] are the positions k steps from now, step_s apart (at least
  /// view_steps(step_s, horizon_s) of them for a full answer).
  [[nodiscard]] std::vector<double> times_left_in_view_s(
      std::size_t site, std::span<const VisibleSatellite> visible,
      std::span<const StepPositions> steps, double step_s,
      double horizon_s = kDefaultViewHorizonS) const;

 private:
  struct Member {
    std::size_t satellite;
    std::int64_t norad_cat_id;
    std::size_t shell_index;
  };

  std::vector<SiteGeometry> sites_;
  ElevationMaskSettings mask_settings_;
  /// In increasing satellite index.
  std::vector<Member> members_;
  std::size_t shell_count_ = 0;
  /// masks_[site * shell_count_ + shell]; NaN for a shell with no plane member.
  std::vector<double> masks_rad_;
  std::vector<double> sin_squared_masks_;
  /// Per site: the smallest mask over shells with members.
  std::vector<double> min_mask_rad_;
};

/// How one serving satellite is chosen among the visible ones (spec 4.5).
enum class ServingRule : std::uint8_t {
  kHighestElevation,  ///< The highest elevation; ties to the lower catalogue number.
};

/// The serving satellite of a site at a step under `rule`, or nullopt if none is visible.
[[nodiscard]] std::optional<VisibleSatellite> pick_serving_satellite(
    std::span<const VisibleSatellite> visible, ServingRule rule = ServingRule::kHighestElevation);

}  // namespace satsim::topology
