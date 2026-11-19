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
  /// A cross-plane laser whose side has no neighbour plane (spec 4.3: the RAAN gap is over the
  /// limit, the shell has a single plane, or for two planes away the plane would be the
  /// satellite's own or a gap on the way is over the limit).
  kNoNeighbourPlane,
  /// A cross-plane laser whose neighbour plane exists, but whose satellite found no partner
  /// with a free laser facing back and line of sight at the reference time.
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
  /// Two consecutive planes are neighbours only if the RAAN gap between them is at most this
  /// many times the shell's median gap between consecutive planes (spec 4.3).
  double max_gap_over_median = 3.0;
  /// Line of sight at the reference time is required of cross-plane links (spec 4.3).
  double atmosphere_km = kDefaultAtmosphereKm;
};

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
  /// Per shell: the median RAAN gap between consecutive planes, radians, around the full
  /// circle (P gaps for P planes, including the one across RAAN 0). nullopt with fewer than two
  /// planes.
  std::vector<std::optional<double>> median_plane_gap_rad;

  friend bool operator==(const LinkPlan&, const LinkPlan&) = default;
};

/// Plans the laser links of every shell of `classification` from positions at the reference
/// time (`position_ecef_km`, one per classified satellite, all at one instant).
///
/// - In-plane, every pattern: satellite i of a plane of n >= 3 links to i + 1 (mod n), using
///   i's ahead and i+1's behind laser: n links, a ring. A plane of 2 has one link, a plane of
///   1 none (UnassignedReason::kPlaneTooSmall). In-plane links are planned without regard to
///   line of sight; one across a gap too long to bridge is planned and is down at every step.
/// - Neighbour planes: planes are in RAAN order (Classification::planes). With P >= 2 planes,
///   gap p is the RAAN from plane p to plane p+1 (mod P), in [0, 2*pi). Plane p's east
///   neighbour is p+1 (mod P) if gap p <= max_gap_over_median times the median of the P gaps,
///   and then p+1's west neighbour is p. A single plane has no neighbours. With 2 planes each
///   is the other's east and west neighbour. Two planes east is the east neighbour's east
///   neighbour, if both exist and it is not the plane itself (so never with 2 planes).
/// - Matching, for each plane p with an east neighbour q, in increasing p (then likewise for
///   two planes east): every pair (a in p, b in q) where a's east laser and b's west laser
///   are free, both have a position and a and b are not already linked is a candidate. Taken
///   in increasing distance, ties by a's then b's catalogue number, a pair is linked if both
///   lasers are still free, a and b are not yet linked, and the segment has line of sight.
///   "Not already linked" keeps a pair from being linked twice where the same two planes meet
///   on both sides (2 planes) or as both neighbour and second neighbour (3 or 4 planes).
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
