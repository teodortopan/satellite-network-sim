#pragma once

#include <Eigen/Core>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <variant>
#include <vector>

#include "satsim/orbit/propagator.hpp"
#include "satsim/time/utc_time.hpp"

namespace satsim::constellation {

// Orbit geometry derived from SGP4 positions and velocities in TEME, never from the element
// sets' mean elements (which mean something only inside SGP4, and are at different epochs).
//
// TEME's z axis is the true pole of date and its x axis the mean equinox, so inclination and
// right ascension of the ascending node (RAAN) below are measured against the true equator of
// date, from the mean equinox. That frame is the same for every satellite at one instant, which
// is all comparing planes needs.

/// The unit orbit normal r x v / |r x v| of a TEME state: perpendicular to the orbit plane, on
/// the side from which the satellite is seen moving anticlockwise. Precondition: position and
/// velocity are not parallel (true of any orbit).
[[nodiscard]] Eigen::Vector3d orbit_normal_teme(const Eigen::Vector3d& position_teme_km,
                                                const Eigen::Vector3d& velocity_teme_km_s) noexcept;

/// Inclination of the plane with unit normal `normal_teme`: the angle between the normal and
/// TEME's z axis, in [0, pi]. Above pi/2 the orbit is retrograde.
[[nodiscard]] double inclination_rad(const Eigen::Vector3d& normal_teme) noexcept;

/// Right ascension of the ascending node of the plane with unit normal `normal_teme`, in
/// [0, 2*pi): the angle from TEME's x axis to the node vector z x normal, about z. The node is
/// undefined for an equatorial orbit (inclination 0 or pi, to within 1e-12 rad); its RAAN is
/// then 0 by convention, so the node direction is the x axis.
[[nodiscard]] double raan_teme_rad(const Eigen::Vector3d& normal_teme) noexcept;

/// Argument of latitude of a TEME position in the plane with unit normal `normal_teme`, in
/// [0, 2*pi): the angle from the ascending node (direction of RAAN, see raan_teme_rad) to the
/// position, measured in the direction of motion. The position's component along the normal
/// (zero when the normal is the satellite's own) is ignored.
[[nodiscard]] double argument_of_latitude_rad(const Eigen::Vector3d& position_teme_km,
                                              const Eigen::Vector3d& normal_teme) noexcept;

/// `radians` wrapped to [-pi, pi). For differences of circular quantities.
[[nodiscard]] double wrap_pi(double radians) noexcept;

/// The circular mean of `angles_rad` in [0, 2*pi): the direction of the sum of their unit
/// vectors. nullopt if the input is empty or the sum vanishes (to within 1e-12 of the count),
/// where no mean direction exists.
[[nodiscard]] std::optional<double> circular_mean_rad(std::span<const double> angles_rad) noexcept;

/// Groups angles on the circle by gaps: sorted around the circle, two neighbours are in the
/// same group when the gap between them is at most `gap_threshold_rad`, including across
/// 0 / 2*pi (a group straddling 0 is one group). Returns groups of indices into `angles_rad`.
/// Within a group, indices go around the circle in increasing angle starting after the gap
/// before the group; equal angles keep the order of `tie_keys` (then of index). Groups are
/// ordered by their first member's position in the sorted order. If no gap exceeds the
/// threshold, everything is one group (starting at the smallest angle). Angles are wrapped to
/// [0, 2*pi) first. `tie_keys` has one entry per angle (pass the catalogue numbers).
[[nodiscard]] std::vector<std::vector<std::size_t>> cluster_by_circular_gaps(
    std::span<const double> angles_rad, std::span<const std::int64_t> tie_keys,
    double gap_threshold_rad);

/// How measure_orbit_geometry samples the orbit.
struct GeometrySampling {
  /// Samples per window. Each window is one orbital period long and centred on its time, with
  /// samples evenly spaced (offsets (k + 1/2) / n - 1/2 periods, k = 0 .. n-1). An even
  /// sampling of a full period averages out every harmonic of the orbit phase below n.
  std::size_t samples_per_orbit = 32;
  /// The second window, for the RAAN drift rate, is centred this long after the reference time.
  double drift_baseline_s = 86400.0;
};

/// The geometry of one satellite's orbit at a reference time.
///
/// "Mean" values are averages over one orbital period centred on the reference time, which
/// removes most of the oscillation SGP4's short-period terms put into osculating values (the
/// in-plane wobble of the J2 bulge and the eccentricity). The osculating values at the reference
/// time itself are kept alongside, so the effect of averaging can be measured.
struct OrbitGeometry {
  /// Unit normal of the mean plane: the normalised average of the unit normals r x v.
  Eigen::Vector3d mean_normal_teme;
  /// From mean_normal_teme (inclination_rad, raan_teme_rad).
  double inclination_rad = 0.0;
  double raan_teme_rad = 0.0;
  /// Argument of latitude at the reference time, in the mean plane: the samples' arguments of
  /// latitude, unwrapped into one increasing sequence and fitted with a straight line in time
  /// (least squares), evaluated at the reference time. In [0, 2*pi).
  double argument_of_latitude_rad = 0.0;
  /// Mean over the window of the WGS-84 geodetic height (above the ellipsoid, along its normal).
  /// The height is computed from the TEME position directly: geodetic height does not change
  /// under a rotation about the z axis, which is all that separates TEME from the Earth-fixed
  /// PEF frame (GMST), and PEF and ITRF differ only by polar motion (under 3e-6 rad), which
  /// moves the height by centimetres. So no Earth-orientation data is needed.
  double mean_height_km = 0.0;
  /// Mean over the window of the distance from the Earth's centre.
  double mean_radius_km = 0.0;
  /// Osculating values at the reference time itself (from that one state's own normal).
  double osculating_inclination_rad = 0.0;
  double osculating_raan_teme_rad = 0.0;
  double osculating_argument_of_latitude_rad = 0.0;
  /// RAAN drift: the mean RAAN of a second window (GeometrySampling::drift_baseline_s later)
  /// minus raan_teme_rad, wrapped to [-pi, pi), per day. nullopt if the second window could not
  /// be propagated (for example the satellite decays in between). Negative for prograde orbits
  /// (the J2 bulge makes their planes regress).
  std::optional<double> raan_drift_rad_per_day;
};

/// The geometry or why it could not be measured: the first propagation error met.
using OrbitGeometryOrError = std::variant<OrbitGeometry, orbit::PropagationError>;

/// Propagates `propagator` over one orbital period of `period_s` seconds centred on
/// `reference_utc` (and over the drift window) and derives the geometry above. Any failed
/// propagation in the main window, or at the reference time, gives that error.
/// Preconditions: period_s > 0, sampling.samples_per_orbit >= 4, drift_baseline_s > 0.
[[nodiscard]] OrbitGeometryOrError measure_orbit_geometry(const orbit::Propagator& propagator,
                                                          const time::UtcTime& reference_utc,
                                                          double period_s,
                                                          const GeometrySampling& sampling);

/// A launch, from an international designator ("2021-044AB" is year 2021, launch 44).
struct Launch {
  int year = 0;
  int number = 0;

  friend auto operator<=>(const Launch&, const Launch&) = default;
};

/// The launch of an international designator (COSPAR ID, OMM's OBJECT_ID) of the form
/// YYYY-NNN followed by one to three capital letters (the piece). nullopt for anything else.
[[nodiscard]] std::optional<Launch> launch_of(std::string_view international_designator) noexcept;

}  // namespace satsim::constellation
