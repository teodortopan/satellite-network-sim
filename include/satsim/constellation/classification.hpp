#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "satsim/constellation/geometry.hpp"
#include "satsim/orbit/element_set.hpp"
#include "satsim/orbit/propagator.hpp"
#include "satsim/time/utc_time.hpp"

namespace satsim::constellation {

/// Why a satellite has no place (shell, plane, index in plane) in the network.
enum class ExclusionReason : std::uint8_t {
  /// SGP4 failed at or around the reference time (for example the satellite has decayed), or
  /// its propagator could not be built. There is no geometry.
  kPropagationFailed,
  /// Launched up to and including ClassificationConfig::last_launch_without_lasers: a
  /// first-generation satellite without inter-satellite lasers.
  kNoLasers,
  /// OBJECT_ID is missing or not an international designator, so the launch is unknown.
  kUnknownLaunch,
  /// In an excluded group labelled as the low group (ExcludedGroup).
  kLowGroup,
  /// In an excluded group labelled as an old height (ExcludedGroup): a shell's previous height,
  /// being phased out.
  kOldHeight,
  /// In an excluded group labelled as another height (ExcludedGroup): a populated height close
  /// to a working shell's, whose planes are not settled and drift in RAAN against the shell's.
  kOtherHeight,
  /// Inclination within a shell's band but mean height outside every shell's band and every
  /// excluded group: raising, lowering or decaying.
  kNotAtShellHeight,
  /// Inclination within no shell's band (and in no excluded group).
  kNoShell,
  /// In a shell, but in a group of planes-by-RAAN smaller than the shell's min_plane_size.
  kNoPlane,
};

/// Every reason, in enum order.
inline constexpr std::array<ExclusionReason, 9> kAllExclusionReasons = {
    ExclusionReason::kPropagationFailed, ExclusionReason::kNoLasers,
    ExclusionReason::kUnknownLaunch,     ExclusionReason::kLowGroup,
    ExclusionReason::kOldHeight,         ExclusionReason::kOtherHeight,
    ExclusionReason::kNotAtShellHeight,  ExclusionReason::kNoShell,
    ExclusionReason::kNoPlane,
};

/// A stable lower_snake_case name for files and logs, e.g. "no_lasers".
[[nodiscard]] std::string_view name_of(ExclusionReason reason) noexcept;

/// A working shell: satellites whose mean inclination and mean height fall in its bands are
/// grouped into planes by RAAN. Bands are in degrees and kilometres because they are input.
struct ShellDefinition {
  std::string name;
  double inclination_deg = 0.0;            ///< Centre of the inclination band.
  double inclination_tolerance_deg = 0.0;  ///< Half-width: |i - centre| <= tolerance.
  double min_height_km = 0.0;              ///< Mean height band [min, max).
  double max_height_km = 0.0;
  /// Satellites sorted by RAAN at the reference time are cut into planes wherever two
  /// neighbours are more than this far apart (cluster_by_circular_gaps).
  double plane_gap_threshold_deg = 0.0;
  /// Planes with fewer satellites are dropped; their satellites get kNoPlane.
  std::size_t min_plane_size = 1;
};

/// A labelled group of satellites to exclude, by mean inclination and mean height band.
struct ExcludedGroup {
  std::string name;
  ExclusionReason reason = ExclusionReason::kLowGroup;
  double min_inclination_deg = 0.0;  ///< Inclination band [min, max].
  double max_inclination_deg = 180.0;
  double min_height_km = 0.0;  ///< Mean height band [min, max).
  double max_height_km = 0.0;
};

/// Everything classify_constellation decides with. See default_config for the proposed values.
struct ClassificationConfig {
  /// Checked in order; a satellite goes to the first shell whose bands it is in.
  std::vector<ShellDefinition> shells;
  /// Checked in order, after the shells.
  std::vector<ExcludedGroup> excluded_groups;
  /// The last launch whose satellites have no lasers (inclusive).
  Launch last_launch_without_lasers;
  GeometrySampling sampling;
};

/// The proposed configuration for the Starlink snapshots of late 2026 (values and the evidence
/// for each in default_config's definition and docs/img/phase-2). Not final: the thresholds
/// await the project owner's approval.
[[nodiscard]] ClassificationConfig default_config();

/// nullopt if `config` is usable, else what is wrong: no shell or group name may be empty, every
/// band must have min < max and finite bounds, tolerances and thresholds must be positive,
/// min_plane_size at least 1, samples_per_orbit at least 4 and drift_baseline_s positive.
[[nodiscard]] std::optional<std::string> check_config(const ClassificationConfig& config);

/// A satellite's place in its shell.
struct PlaneAssignment {
  std::size_t plane_index = 0;     ///< Planes ordered by mean RAAN, from 0.
  std::size_t index_in_plane = 0;  ///< Ordered by argument of latitude, from 0.

  friend bool operator==(const PlaneAssignment&, const PlaneAssignment&) = default;
};

/// One satellite's classification.
struct ClassifiedSatellite {
  std::int64_t norad_cat_id = 0;
  std::string object_name;
  std::string object_id;
  /// Set exactly when the reason is kPropagationFailed.
  std::optional<orbit::PropagationError> propagation_error;
  /// Absent exactly when propagation failed.
  std::optional<OrbitGeometry> geometry;
  /// The first shell whose inclination and height bands contain the geometry, whatever the
  /// exclusion reason (so first-generation satellites in a shell can be counted).
  std::optional<std::size_t> shell_index;
  /// Exactly one of a place in a plane (then shell_index is set) or why there is none.
  std::variant<PlaneAssignment, ExclusionReason> placement;
};

/// One plane of a shell.
struct Plane {
  /// Circular mean of the members' RAAN, in [0, 2*pi).
  double mean_raan_teme_rad = 0.0;
  /// Indices into Classification::satellites, in index_in_plane order.
  std::vector<std::size_t> members;

  friend bool operator==(const Plane&, const Plane&) = default;
};

/// The outcome of classify_constellation.
struct Classification {
  /// One per input element set, ascending catalogue number.
  std::vector<ClassifiedSatellite> satellites;
  /// planes[s] are the planes of config.shells[s], ascending mean RAAN.
  std::vector<std::vector<Plane>> planes;
};

/// A classification, or why the configuration cannot be used (check_config).
using ClassificationOrError = std::variant<Classification, std::string>;

/// Classifies every satellite into a shell, plane and index in plane, or an exclusion reason.
///
/// 1. Each element set's propagator is built and its geometry measured at `reference_utc` over
///    one orbital period (86400 / mean motion seconds; measure_orbit_geometry). Every satellite
///    is thus compared at the same instant, whatever its element epoch.
/// 2. Reasons, first match wins: propagation failed; unknown launch; no lasers; then by geometry
///    the first shell whose bands contain it (a candidate for a plane), else the first excluded
///    group, else not at a shell's height if the inclination is within a shell's band, else no
///    shell.
/// 3. Per shell, the candidates are grouped by RAAN (cluster_by_circular_gaps with the shell's
///    threshold). Groups smaller than min_plane_size are dropped (kNoPlane). The remaining
///    planes are numbered in ascending mean RAAN, ties by smallest member catalogue number.
///    Within a plane, satellites are numbered in ascending argument of latitude from 0
///    (the ascending node), ties by catalogue number.
///
/// Deterministic: the result does not depend on the order of `element_sets`. Precondition:
/// catalogue numbers are unique (select_element_sets guarantees it).
[[nodiscard]] ClassificationOrError classify_constellation(
    std::span<const orbit::ElementSet> element_sets, const time::UtcTime& reference_utc,
    const ClassificationConfig& config);

}  // namespace satsim::constellation
