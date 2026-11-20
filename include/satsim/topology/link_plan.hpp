#pragma once

#include <Eigen/Core>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

#include "satsim/constellation/classification.hpp"
#include "satsim/topology/geometry.hpp"

namespace satsim::topology {

// The laser link plan (spec 4.3): which satellites are linked, decided once at the reference
// time and kept for the whole run. Whether a planned link is up at a given step is
// link_state.hpp's business.

/// Which lasers face where (spec 4.3).
enum class LaserPattern : std::uint8_t {
  /// Ahead, behind, and one cross-plane laser: satellites at even index_in_plane face east,
  /// odd face west. 3 lasers.
  kThree,
  /// Ahead, behind, east, west. 4 lasers.
  kPlusGrid,
  /// Ahead, behind, east, west, two planes east, two planes west. 6 lasers.
  kSix,
};

inline constexpr std::array<LaserPattern, 3> kAllLaserPatterns = {
    LaserPattern::kThree, LaserPattern::kPlusGrid, LaserPattern::kSix};

/// "three", "plus-grid", "six".
[[nodiscard]] std::string_view name_of(LaserPattern pattern) noexcept;

/// The pattern named by name_of, or nullopt.
[[nodiscard]] std::optional<LaserPattern> parse_laser_pattern(std::string_view name) noexcept;

/// Lasers per satellite: 3, 4 or 6.
[[nodiscard]] std::size_t laser_budget(LaserPattern pattern) noexcept;

/// Where one laser terminal points. East is toward the next plane in RAAN order (plane p+1),
/// ahead toward the next satellite in the plane (index_in_plane + 1, the direction of motion).
enum class LaserSide : std::uint8_t {
  kAhead,
  kBehind,
  kEast,
  kWest,
  kSecondEast,  ///< Two planes east (six only).
  kSecondWest,  ///< Two planes west (six only).
};

/// "ahead", "behind", "east", "west", "second_east", "second_west".
[[nodiscard]] std::string_view name_of(LaserSide side) noexcept;

/// The lasers of a satellite at `index_in_plane` under `pattern`, in this order: ahead,
/// behind, then the cross-plane sides in LaserSide order.
[[nodiscard]] std::vector<LaserSide> laser_sides(LaserPattern pattern, std::size_t index_in_plane);

enum class LinkKind : std::uint8_t {
  kInPlane,           ///< Between consecutive satellites of one plane.
  kCrossPlane,        ///< Between neighbouring planes.
  kSecondCrossPlane,  ///< Between planes two apart (six only).
};

inline constexpr std::array<LinkKind, 3> kAllLinkKinds = {LinkKind::kInPlane, LinkKind::kCrossPlane,
                                                          LinkKind::kSecondCrossPlane};

/// "in_plane", "cross_plane", "second_cross_plane".
[[nodiscard]] std::string_view name_of(LinkKind kind) noexcept;

/// One planned laser link. Satellites are indices into Classification::satellites.
struct PlannedLink {
  /// For in-plane links the satellite behind (its kAhead laser); for cross-plane links the
  /// satellite in the western plane (its kEast or kSecondEast laser).
  std::size_t satellite_a = 0;
  /// The other end: its kBehind, kWest or kSecondWest laser.
  std::size_t satellite_b = 0;
  LaserSide side_a = LaserSide::kAhead;
  LaserSide side_b = LaserSide::kBehind;
  LinkKind kind = LinkKind::kInPlane;
  std::size_t shell_index = 0;

  friend bool operator==(const PlannedLink&, const PlannedLink&) = default;
};

/// Why a laser has no link.
enum class UnassignedReason : std::uint8_t {
  /// An in-plane laser in a plane of one or two satellites: a plane of one has nobody to link
  /// to, and in a plane of two the two satellites share one link (0's ahead to 1's behind),
  /// never two, so 0's behind and 1's ahead stay unused.
  kPlaneTooSmall,
  /// A cross-plane laser whose side has no neighbour plane (spec 4.3): the shell has a single
  /// plane, or, for two planes away, the shell has two planes (the plane would be the
  /// satellite's own).
  kNoNeighbourPlane,
  /// A cross-plane laser whose neighbour plane exists, but whose satellite found no partner
  /// with a free laser facing back, line of sight at the reference time and line of sight for
  /// a whole orbit (keeps_line_of_sight_for_an_orbit). Across a RAAN gap too wide for any
  /// permanent link, every laser facing the gap ends here.
  kNoFreePartner,
};

inline constexpr std::array<UnassignedReason, 3> kAllUnassignedReasons = {
    UnassignedReason::kPlaneTooSmall, UnassignedReason::kNoNeighbourPlane,
    UnassignedReason::kNoFreePartner};

/// "plane_too_small", "no_neighbour_plane", "no_free_partner".
[[nodiscard]] std::string_view name_of(UnassignedReason reason) noexcept;

/// One laser terminal of a satellite: exactly one of link and unassigned is set.
struct Laser {
  LaserSide side = LaserSide::kAhead;
  std::optional<std::size_t> link = std::nullopt;  ///< Index into LinkPlan::links.
  std::optional<UnassignedReason> unassigned = std::nullopt;

  friend bool operator==(const Laser&, const Laser&) = default;
};

/// The neighbouring planes of one plane (indices into Classification::planes[shell]).
struct PlaneNeighbours {
  std::optional<std::size_t> east = std::nullopt;
  std::optional<std::size_t> west = std::nullopt;
  std::optional<std::size_t> second_east = std::nullopt;
  std::optional<std::size_t> second_west = std::nullopt;

  friend bool operator==(const PlaneNeighbours&, const PlaneNeighbours&) = default;
};

/// Settings of plan_laser_links.
struct LinkPlanSettings {
  LaserPattern pattern = LaserPattern::kThree;
  /// Line of sight at the reference time and for a whole orbit is required of cross-plane
  /// links (spec 4.3).
  double atmosphere_km = kDefaultAtmosphereKm;
};

/// Points of one orbit at which keeps_line_of_sight_for_an_orbit tests the link.
inline constexpr std::size_t kPermanenceSamplesPerOrbit = 360;

/// Whether a laser link between two satellites keeps line of sight for a whole orbit (spec
/// 4.3), in a model: each satellite on a circular orbit of radius mean_radius_km in its own
/// mean plane (mean_normal_teme, raan_teme_rad), both moving at the same angular rate from
/// their arguments of latitude at the reference time. The segment between the two model
/// positions must clear line_of_sight_radius_km(atmosphere_km) at each of
/// kPermanenceSamplesPerOrbit equally spaced points of one orbit, the first at the reference
/// time. Two circular orbits at one rate return to the same relative geometry after one orbit,
/// so one orbit covers every time. The test is done in TEME, where the geometry is held:
/// line of sight does not change under a rotation about the Earth's centre, so the answer is
/// the one the Earth-fixed frame would give.
[[nodiscard]] bool keeps_line_of_sight_for_an_orbit(const constellation::OrbitGeometry& a,
                                                    const constellation::OrbitGeometry& b,
                                                    double atmosphere_km = kDefaultAtmosphereKm);

/// The plan.
struct LinkPlan {
  LaserPattern pattern = LaserPattern::kThree;
  /// Ordered by shell; within a shell all in-plane links (plane, then index of satellite_a),
  /// then cross-plane links (western plane, then the order they were accepted), then
  /// second-cross-plane links likewise.
  std::vector<PlannedLink> links;
  /// One entry per satellite of the classification: its lasers in laser_sides order; empty for
  /// a satellite that is not a plane member.
  std::vector<std::vector<Laser>> lasers;
  /// neighbours[s][p]: plane p of shell s.
  std::vector<std::vector<PlaneNeighbours>> neighbours;
  /// The most links one satellite may have, if not laser_budget(pattern) (max_lasers_of).
  /// nullopt for every planned plan; set only by read_link_plan for a file read with another
  /// limit (spec 4.9, --max-lasers).
  std::optional<std::size_t> max_lasers = std::nullopt;
  /// Whether a link may join two shells. False for every planned plan (lasers stay within a
  /// shell, spec 4.3); read_link_plan sets it exactly when the file has such a link, since a
  /// file is someone else's link set (spec 4.9). The physical checks count a link joining two
  /// shells only when this is false.
  bool cross_shell_links = false;

  friend bool operator==(const LinkPlan&, const LinkPlan&) = default;
};

/// The most links one satellite of `plan` may have: plan.max_lasers if set, else
/// laser_budget(plan.pattern).
[[nodiscard]] std::size_t max_lasers_of(const LinkPlan& plan) noexcept;

/// The neighbouring planes of every plane of one shell (`planes`, in RAAN order), as
/// plan_laser_links finds them (below): with P >= 2 planes, plane p's east is p+1 (mod P) and
/// p+1's west is p; two planes east is p+2 (mod P) unless that is p itself.
[[nodiscard]] std::vector<PlaneNeighbours> plane_neighbours(
    std::span<const constellation::Plane> planes);

/// Plans the laser links of every shell of `classification` from positions at the reference
/// time (`position_ecef_km`, one per classified satellite, all at one instant).
///
/// - In-plane, every pattern: satellite i of a plane of n >= 3 links to i + 1 (mod n), using
///   i's ahead and i+1's behind laser: n links, a ring. A plane of 2 has one link, a plane of
///   1 none (UnassignedReason::kPlaneTooSmall). In-plane links are planned without regard to
///   line of sight; one across a gap too long to bridge is planned and is down at every step.
/// - Neighbour planes: planes are in RAAN order (Classification::planes). With P >= 2 planes,
///   plane p's east neighbour is p+1 (mod P), and p+1's west neighbour is p, however wide the
///   RAAN gap between them: a gap too wide to bridge leaves no link through the permanence
///   test below. A single plane has no neighbours. With 2 planes each is the other's east and
///   west neighbour. Two planes east is p+2 (mod P), if that is not the plane itself (so never
///   with 2 planes).
/// - Matching, for each plane p in increasing p, with its east neighbour q (then likewise for
///   two planes east): every pair (a in p, b in q) where a's east laser and b's west laser
///   are free, both have a position and a and b are not already linked is a candidate. Taken
///   in increasing distance, ties by a's then b's catalogue number, a pair is linked if both
///   lasers are still free, a and b are not yet linked, the segment has line of sight at the
///   reference time, and the link keeps line of sight for a whole orbit
///   (keeps_line_of_sight_for_an_orbit, from the satellites' OrbitGeometry; a satellite
///   without geometry has no permanent link). "Not already linked" keeps a pair from being
///   linked twice where the same two planes meet on both sides (2 planes) or as both
///   neighbour and second neighbour (3 or 4 planes).
/// - Lasers never leave a shell and excluded satellites get none. Every satellite gets exactly
///   laser_budget(pattern) lasers, each linked at most once, so the budget holds by
///   construction. A laser left without a link records why.
///
/// Deterministic: depends only on the classification (whose order is canonical) and the
/// positions.
[[nodiscard]] LinkPlan plan_laser_links(
    const constellation::Classification& classification,
    std::span<const std::optional<Eigen::Vector3d>> position_ecef_km,
    const LinkPlanSettings& settings);

/// Links of the plan by kind, indexed by LinkKind.
[[nodiscard]] std::array<std::size_t, 3> count_links_by_kind(const LinkPlan& plan) noexcept;

/// Lasers without a link, indexed by UnassignedReason.
[[nodiscard]] std::array<std::size_t, 3> count_unassigned_lasers(const LinkPlan& plan) noexcept;

}  // namespace satsim::topology
