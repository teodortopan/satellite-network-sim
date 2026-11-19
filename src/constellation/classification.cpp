#include "satsim/constellation/classification.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <numeric>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "satsim/constellation/geometry.hpp"
#include "satsim/core/angles.hpp"
#include "satsim/orbit/element_set.hpp"
#include "satsim/orbit/propagator.hpp"
#include "satsim/time/utc_time.hpp"

namespace satsim::constellation {

namespace {

constexpr double kSecondsPerDay = 86400.0;

bool in_inclination_band(const ShellDefinition& shell, double inclination_deg) noexcept {
  return std::abs(inclination_deg - shell.inclination_deg) <= shell.inclination_tolerance_deg;
}

bool in_height_band(double min_km, double max_km, double height_km) noexcept {
  return height_km >= min_km && height_km < max_km;
}

bool is_finite_band(double min, double max) noexcept {
  return std::isfinite(min) && std::isfinite(max) && min < max;
}

// The reason a satellite with this geometry and launch gets before planes are formed, or
// nullopt for a plane candidate. Sets `shell_index` from the geometry alone.
std::optional<ExclusionReason> reason_before_planes(const ClassificationConfig& config,
                                                    const OrbitGeometry& geometry,
                                                    const std::optional<Launch>& launch,
                                                    std::optional<std::size_t>& shell_index) {
  const double inclination_deg = rad_to_deg(geometry.inclination_rad);
  const double height_km = geometry.mean_height_km;
  for (std::size_t s = 0; s < config.shells.size(); ++s) {
    const ShellDefinition& shell = config.shells[s];
    if (in_inclination_band(shell, inclination_deg) &&
        in_height_band(shell.min_height_km, shell.max_height_km, height_km)) {
      shell_index = s;
      break;
    }
  }
  if (!launch) {
    return ExclusionReason::kUnknownLaunch;
  }
  if (*launch <= config.last_launch_without_lasers) {
    return ExclusionReason::kNoLasers;
  }
  if (shell_index) {
    return std::nullopt;
  }
  for (const ExcludedGroup& group : config.excluded_groups) {
    if (inclination_deg >= group.min_inclination_deg &&
        inclination_deg <= group.max_inclination_deg &&
        in_height_band(group.min_height_km, group.max_height_km, height_km)) {
      return group.reason;
    }
  }
  const bool shell_inclination = std::any_of(
      config.shells.begin(), config.shells.end(),
      [&](const ShellDefinition& shell) { return in_inclination_band(shell, inclination_deg); });
  return shell_inclination ? ExclusionReason::kNotAtShellHeight : ExclusionReason::kNoShell;
}

// Forms the planes of one shell from its candidates (indices into `satellites`).
std::vector<Plane> form_planes(const ShellDefinition& shell,
                               const std::vector<std::size_t>& candidates,
                               std::vector<ClassifiedSatellite>& satellites) {
  std::vector<double> raans;
  std::vector<std::int64_t> ids;
  raans.reserve(candidates.size());
  ids.reserve(candidates.size());
  for (const std::size_t i : candidates) {
    raans.push_back(satellites[i].geometry->raan_teme_rad);
    ids.push_back(satellites[i].norad_cat_id);
  }
  const std::vector<std::vector<std::size_t>> groups =
      cluster_by_circular_gaps(raans, ids, deg_to_rad(shell.plane_gap_threshold_deg));

  struct Candidate {
    Plane plane;
    std::int64_t smallest_id = 0;
  };
  std::vector<Candidate> kept;
  for (const std::vector<std::size_t>& group : groups) {
    if (group.size() < shell.min_plane_size) {
      for (const std::size_t g : group) {
        satellites[candidates[g]].placement = ExclusionReason::kNoPlane;
      }
      continue;
    }
    std::vector<double> group_raans;
    group_raans.reserve(group.size());
    Candidate candidate;
    candidate.smallest_id = ids[group.front()];
    for (const std::size_t g : group) {
      group_raans.push_back(raans[g]);
      candidate.plane.members.push_back(candidates[g]);
      candidate.smallest_id = std::min(candidate.smallest_id, ids[g]);
    }
    // A group cut out by gaps spans less than a full circle, so its mean exists; the first
    // member's RAAN is only a fallback for a degenerate group spread evenly around the circle.
    candidate.plane.mean_raan_teme_rad =
        circular_mean_rad(group_raans).value_or(wrap_two_pi(group_raans.front()));
    std::sort(candidate.plane.members.begin(), candidate.plane.members.end(),
              [&](std::size_t a, std::size_t b) {
                const double ua = satellites[a].geometry->argument_of_latitude_rad;
                const double ub = satellites[b].geometry->argument_of_latitude_rad;
                if (ua != ub) {
                  return ua < ub;
                }
                return satellites[a].norad_cat_id < satellites[b].norad_cat_id;
              });
    kept.push_back(std::move(candidate));
  }
  std::sort(kept.begin(), kept.end(), [](const Candidate& a, const Candidate& b) {
    if (a.plane.mean_raan_teme_rad != b.plane.mean_raan_teme_rad) {
      return a.plane.mean_raan_teme_rad < b.plane.mean_raan_teme_rad;
    }
    return a.smallest_id < b.smallest_id;
  });

  std::vector<Plane> planes;
  planes.reserve(kept.size());
  for (Candidate& candidate : kept) {
    const std::size_t plane_index = planes.size();
    for (std::size_t k = 0; k < candidate.plane.members.size(); ++k) {
      satellites[candidate.plane.members[k]].placement =
          PlaneAssignment{.plane_index = plane_index, .index_in_plane = k};
    }
    planes.push_back(std::move(candidate.plane));
  }
  return planes;
}

}  // namespace

std::string_view name_of(ExclusionReason reason) noexcept {
  switch (reason) {
    case ExclusionReason::kPropagationFailed:
      return "propagation_failed";
    case ExclusionReason::kNoLasers:
      return "no_lasers";
    case ExclusionReason::kUnknownLaunch:
      return "unknown_launch";
    case ExclusionReason::kLowGroup:
      return "low_group";
    case ExclusionReason::kOldHeight:
      return "old_height";
    case ExclusionReason::kOtherHeight:
      return "other_height";
    case ExclusionReason::kNotAtShellHeight:
      return "not_at_shell_height";
    case ExclusionReason::kNoShell:
      return "no_shell";
    case ExclusionReason::kNoPlane:
      return "no_plane";
  }
  return "unknown";
}

// Proposed values, from the study of the 2026-11-18 snapshot at 2026-11-18T21:15:00 UTC
// (python/plot_constellation.py, docs/img/phase-2). Heights are mean WGS-84 geodetic heights
// over one orbit (OrbitGeometry::mean_height_km), not heights estimated from mean motion.
//
// - Inclination: every working shell's satellites lie within 0.01 deg of its centre; +-0.3 deg
//   is wide enough not to matter and keeps clear of the other shells and of the
//   first-generation satellites at 53.0 to 53.1 deg (excluded anyway, as no lasers).
// - Height bands: each shell's satellites keep within about 1 km of one height. The bands are
//   a few km wide around it, so satellites raising or lowering through the shell, and nearby
//   populated heights, are not taken for members: at a different height a satellite's position
//   along the orbit slides against the shell's by about 1.2 deg per day per km.
// - Plane gap thresholds: inside the range over which the number of planes does not change,
//   which differs per shell because the plane spacing does (53 deg: 1.9 to 2.1 deg, with legacy
//   groups 0.67 deg apart; 43 deg: 3.6 deg; 97 deg: alternately 0.92 and 2.95 deg; 70 deg:
//   13.35 deg).
// - Minimum plane size 10: a plane of fewer satellites has a gap of 36 deg or more along the
//   orbit.
ClassificationConfig default_config() {
  ClassificationConfig config;
  config.shells = {
      {.name = "53deg",
       .inclination_deg = 53.16,
       .inclination_tolerance_deg = 0.3,
       .min_height_km = 467.5,
       .max_height_km = 471.0,
       .plane_gap_threshold_deg = 0.4,
       .min_plane_size = 10},
      {.name = "43deg",
       .inclination_deg = 43.0,
       .inclination_tolerance_deg = 0.3,
       .min_height_km = 485.5,
       .max_height_km = 489.0,
       .plane_gap_threshold_deg = 1.0,
       .min_plane_size = 10},
      {.name = "97deg",
       .inclination_deg = 97.28,
       .inclination_tolerance_deg = 0.3,
       .min_height_km = 476.0,
       .max_height_km = 479.0,
       .plane_gap_threshold_deg = 0.4,
       .min_plane_size = 10},
      {.name = "70deg",
       .inclination_deg = 70.0,
       .inclination_tolerance_deg = 0.3,
       .min_height_km = 483.5,
       .max_height_km = 489.0,
       .plane_gap_threshold_deg = 2.0,
       .min_plane_size = 10},
  };
  config.excluded_groups = {
      // 43 deg at 355-360 km, 97 deg at 355-358, 53 deg at 365-368, 70 deg at 372-375.
      {.name = "low group",
       .reason = ExclusionReason::kLowGroup,
       .min_inclination_deg = 0.0,
       .max_inclination_deg = 180.0,
       .min_height_km = 350.0,
       .max_height_km = 375.0},
      // 70 deg at 520-556 km, most at 535: the shell's previous height.
      {.name = "70deg at the old height",
       .reason = ExclusionReason::kOldHeight,
       .min_inclination_deg = 69.7,
       .max_inclination_deg = 70.3,
       .min_height_km = 515.0,
       .max_height_km = 560.0},
      // 53 deg at 477-478 km (about 470 satellites), 8 km above the shell: few full planes, and
      // their RAAN drifts about 0.03 deg/day against the shell's planes.
      {.name = "53deg at 477 km",
       .reason = ExclusionReason::kOtherHeight,
       .min_inclination_deg = 52.86,
       .max_inclination_deg = 53.46,
       .min_height_km = 476.0,
       .max_height_km = 479.0},
  };
  config.last_launch_without_lasers = Launch{.year = 2021, .number = 44};
  config.sampling = GeometrySampling{};
  return config;
}

std::optional<std::string> check_config(const ClassificationConfig& config) {
  for (const ShellDefinition& shell : config.shells) {
    const std::string label = "shell \"" + shell.name + "\": ";
    if (shell.name.empty()) {
      return std::string("a shell has no name");
    }
    if (!std::isfinite(shell.inclination_deg) || !(shell.inclination_tolerance_deg > 0.0) ||
        !std::isfinite(shell.inclination_tolerance_deg)) {
      return label + "inclination and a positive tolerance needed";
    }
    if (!is_finite_band(shell.min_height_km, shell.max_height_km)) {
      return label + "height band needs finite min < max";
    }
    if (!(shell.plane_gap_threshold_deg > 0.0) || !std::isfinite(shell.plane_gap_threshold_deg)) {
      return label + "plane gap threshold must be positive";
    }
    if (shell.min_plane_size < 1) {
      return label + "min_plane_size must be at least 1";
    }
  }
  for (const ExcludedGroup& group : config.excluded_groups) {
    const std::string label = "excluded group \"" + group.name + "\": ";
    if (group.name.empty()) {
      return std::string("an excluded group has no name");
    }
    if (!std::isfinite(group.min_inclination_deg) || !std::isfinite(group.max_inclination_deg) ||
        group.min_inclination_deg > group.max_inclination_deg) {
      return label + "inclination band needs finite min <= max";
    }
    if (!is_finite_band(group.min_height_km, group.max_height_km)) {
      return label + "height band needs finite min < max";
    }
    if (group.reason == ExclusionReason::kPropagationFailed ||
        group.reason == ExclusionReason::kNoPlane) {
      return label + "reason cannot be " + std::string(name_of(group.reason));
    }
  }
  constexpr std::size_t kMinSamples = 4;
  if (config.sampling.samples_per_orbit < kMinSamples) {
    return std::string("samples_per_orbit must be at least 4");
  }
  if (!(config.sampling.drift_baseline_s > 0.0) ||
      !std::isfinite(config.sampling.drift_baseline_s)) {
    return std::string("drift_baseline_s must be positive");
  }
  return std::nullopt;
}

ClassificationOrError classify_constellation(std::span<const orbit::ElementSet> element_sets,
                                             const time::UtcTime& reference_utc,
                                             const ClassificationConfig& config) {
  if (std::optional<std::string> error = check_config(config)) {
    return std::move(*error);
  }
  std::vector<std::size_t> order(element_sets.size());
  std::iota(order.begin(), order.end(), std::size_t{0});
  std::stable_sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) {
    return element_sets[a].norad_cat_id < element_sets[b].norad_cat_id;
  });

  Classification result;
  result.satellites.reserve(element_sets.size());
  std::vector<std::vector<std::size_t>> candidates(config.shells.size());
  for (const std::size_t input_index : order) {
    const orbit::ElementSet& elements = element_sets[input_index];
    ClassifiedSatellite satellite{
        .norad_cat_id = elements.norad_cat_id,
        .object_name = elements.object_name,
        .object_id = elements.object_id,
        .propagation_error = std::nullopt,
        .geometry = std::nullopt,
        .shell_index = std::nullopt,
        .placement = ExclusionReason::kPropagationFailed,
    };
    const orbit::PropagatorOrError built = orbit::Propagator::create(elements);
    if (const auto* error = std::get_if<orbit::PropagationError>(&built)) {
      satellite.propagation_error = *error;
      result.satellites.push_back(std::move(satellite));
      continue;
    }
    // Only the length of the averaging window comes from the elements: one period of the
    // mean motion (positive, or the propagator could not have been built).
    const double period_s = kSecondsPerDay / elements.mean_motion_rev_per_day;
    OrbitGeometryOrError measured = measure_orbit_geometry(
        std::get<orbit::Propagator>(built), reference_utc, period_s, config.sampling);
    if (const auto* error = std::get_if<orbit::PropagationError>(&measured)) {
      satellite.propagation_error = *error;
      result.satellites.push_back(std::move(satellite));
      continue;
    }
    satellite.geometry = std::get<OrbitGeometry>(measured);
    const std::optional<ExclusionReason> reason = reason_before_planes(
        config, *satellite.geometry, launch_of(elements.object_id), satellite.shell_index);
    if (reason) {
      satellite.placement = *reason;
    } else if (satellite.shell_index) {  // No reason means a shell.
      candidates[*satellite.shell_index].push_back(result.satellites.size());
    }
    result.satellites.push_back(std::move(satellite));
  }

  result.planes.reserve(config.shells.size());
  for (std::size_t s = 0; s < config.shells.size(); ++s) {
    result.planes.push_back(form_planes(config.shells[s], candidates[s], result.satellites));
  }
  return result;
}

}  // namespace satsim::constellation
