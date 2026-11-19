#pragma once

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

/// Finds, for a fixed list of ground sites, the plane members at or above each site's mask.
///
/// The mask depends on the satellite's shell (its mean height, shell_mean_heights_km) and the
/// site's latitude (elevation_mask_rad); it is computed once per site and shell. A shell with
/// no plane member has no mask and no satellites.
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

 private:
  struct Member {
    std::size_t satellite;
    std::int64_t norad_cat_id;
    std::size_t shell_index;
  };

  std::vector<SiteGeometry> sites_;
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
