#pragma once

#include <cstddef>
#include <filesystem>
#include <iosfwd>
#include <optional>
#include <string>
#include <variant>

#include "satsim/constellation/classification.hpp"
#include "satsim/topology/link_plan.hpp"

namespace satsim::topology {

// Link files (spec 4.9): a laser link plan as text, so that a run can use links planned
// elsewhere (a published setup's) or the links of an earlier run.
//
// Format. UTF-8 text, lines ending in "\n" (a "\r" before it is ignored). Lines starting with
// '#' are comments, anywhere. The first other line is the header, either
//   norad_cat_id_a,norad_cat_id_b
// or
//   norad_cat_id_a,norad_cat_id_b,side_a,side_b
// and every other line is one laser link with as many fields as the header: two catalogue
// numbers (decimal digits, no sign) and, with the long header, the laser side used at each end
// (name_of(LaserSide): ahead, behind, east, west, second_east, second_west), which must be one
// of the pairs ahead/behind, east/west, second_east/second_west. No quoting, no spaces, no
// empty lines. write_link_plan always writes the long header, two comment lines first.
//
// Reading. A file is someone else's link set, so the planner's rules (spec 4.3) are not
// applied: a link may join any two plane members, in any planes or shells. The plan read has:
//  - links: in file order; satellite_a and satellite_b as written; the sides as written, or,
//    without side columns, ahead/behind when both ends are in one plane of one shell, else
//    east/west (labels only: nothing downstream depends on them); kind from the sides
//    (ahead/behind in_plane, east/west cross_plane, second_east/second_west
//    second_cross_plane); shell_index satellite_a's shell.
//  - lasers: for each plane member, laser_sides(pattern, index_in_plane) first, each linked to
//    the first link in file order that uses that side at that satellite; a link end whose side
//    the satellite has no free laser for gets a laser of its own, appended. A laser left
//    unlinked records why: an ahead or behind laser in a plane of one or two satellites
//    plane_too_small, a cross-plane laser whose plane has no neighbour on that side
//    no_neighbour_plane, any other no_free_partner (exactly the planner's reasons).
//  - neighbours: plane_neighbours of each shell; pattern: the setting; max_lasers: the
//    setting's limit if it is not laser_budget(pattern), else nullopt; cross_shell_links: true
//    exactly when a link joins two shells.
//
// Round trip. Reading what write_link_plan wrote for a planned plan, with that plan's pattern
// and no other limit, against the classification it was planned on, gives a plan equal to it
// (operator==: same links in the same order with the same sides, kinds and shells, same
// lasers, neighbours and settings), so link states evaluated on either are equal at every
// step.

/// Settings of read_link_plan.
struct LinkFileSettings {
  /// The lasers each plane member starts with (laser_sides) and the plan's pattern.
  LaserPattern pattern = LaserPattern::kThree;
  /// The most links one satellite may have; nullopt means laser_budget(pattern).
  std::optional<std::size_t> max_lasers = std::nullopt;
};

/// A plan read from a file, or why it was refused.
using LinkPlanOrError = std::variant<LinkPlan, std::string>;

/// Writes `plan` (planned or read on `classification`) in the format above: catalogue numbers
/// from classification.satellites. Returns false if the stream fails.
bool write_link_plan(const LinkPlan& plan, const constellation::Classification& classification,
                     std::ostream& out);

/// write_link_plan to a file (created or truncated); nullopt on success, else a message naming
/// the file.
[[nodiscard]] std::optional<std::string> write_link_plan_file(
    const LinkPlan& plan, const constellation::Classification& classification,
    const std::filesystem::path& path);

/// Reads a link file against `classification`. The whole file or nothing: the first problem
/// found refuses it, with a message naming its line ("line 7: ..."):
///  - a line that is malformed (wrong number of fields, a catalogue number that is not
///    decimal digits, an unknown side or sides that are not a pair), or a header that is not
///    one of the two above;
///  - a catalogue number that is not in the classification, or is a satellite in no plane
///    (excluded, with its reason);
///  - a link joining a satellite to itself;
///  - a link that appears twice, in either order (naming the line of the first);
///  - a satellite with more links than the limit (naming the line of the one too many);
///  - no header or no link at all (an empty file).
[[nodiscard]] LinkPlanOrError read_link_plan(std::istream& in,
                                             const constellation::Classification& classification,
                                             const LinkFileSettings& settings);

/// read_link_plan from a file; every message starts with the path, and a file that is missing
/// or cannot be opened is refused.
[[nodiscard]] LinkPlanOrError read_link_plan_file(
    const std::filesystem::path& path, const constellation::Classification& classification,
    const LinkFileSettings& settings);

}  // namespace satsim::topology
