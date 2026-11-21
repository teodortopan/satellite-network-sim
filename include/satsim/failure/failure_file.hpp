#pragma once

#include <cstdint>
#include <filesystem>
#include <iosfwd>
#include <optional>
#include <span>
#include <string>
#include <variant>

#include "satsim/constellation/classification.hpp"
#include "satsim/failure/schedule.hpp"
#include "satsim/time/utc_time.hpp"
#include "satsim/topology/link_plan.hpp"

namespace satsim::failure {

// Failure files (docs/phase-4-failures.md 4.2): a run's failure schedule as text.
//
// Format. UTF-8 text, lines ending in "\n" (a "\r" before it is ignored). Lines starting with
// '#' are comments, anywhere. No empty lines. The first other line is the header
//   kind,norad_cat_id_a,norad_cat_id_b,gateway,start_utc,length_s
// and every other line is one failure with those six fields, unused ones empty:
//  - kind: satellite, laser_link or gateway (name_of(ElementKind));
//  - satellite: norad_cat_id_a only (decimal digits, no sign);
//  - laser_link: norad_cat_id_a and norad_cat_id_b, the two ends of a link of the laser plan,
//    in either order;
//  - gateway: the gateway field only, a gateway's name exactly as the run's gateway list has
//    it. That one field may be quoted, and is written quoted when the name holds a comma or a
//    double quote ("New York, NY"): it is enclosed in double quotes and every double quote
//    inside is doubled. No other field is ever quoted;
//  - start_utc: the failure instant, YYYY-MM-DDTHH:MM:SS with an optional fraction of 1 to 9
//    digits after a '.', UTC (time::parse_iso_civil_time, the form the programs take for
//    --start). It must be a step of the run (the run's start + k * step for a whole k >= 0;
//    no end of the run is checked here) or before the run's start;
//  - length_s: empty for a permanent failure, else the seconds the element stays down, a
//    decimal number greater than 0. A length of 0 is refused: a failure that ends the instant
//    it starts does nothing.
// write_failure_schedule writes one comment line, the header, then the failures in schedule
// order; times with the fewest fraction digits that are exact, lengths in the shortest form
// that reads back as the same double.
//
// Round trip. Reading what write_failure_schedule wrote, with the same classification, plan,
// gateway names, run start and step, gives an equal schedule (operator==).

/// A schedule read from a file, or why it was refused.
using FailureScheduleOrError = std::variant<FailureSchedule, std::string>;

/// Writes `schedule` in the format above: catalogue numbers from classification.satellites,
/// a link's ends as plan.links has them (satellite_a first), gateway names from
/// `gateway_names`. Returns false, writing nothing, if a gateway name is empty or holds a line
/// break (the format has no way to write it), and false if the stream fails.
bool write_failure_schedule(std::ostream& out, const FailureSchedule& schedule,
                            const constellation::Classification& classification,
                            const topology::LinkPlan& plan,
                            std::span<const std::string> gateway_names);

/// write_failure_schedule to a file (created or truncated); nullopt on success, else a message
/// naming the file.
[[nodiscard]] std::optional<std::string> write_failure_schedule_file(
    const std::filesystem::path& path, const FailureSchedule& schedule,
    const constellation::Classification& classification, const topology::LinkPlan& plan,
    std::span<const std::string> gateway_names);

/// Reads a failure file for a run that starts at `run_start_utc` and steps every `step_ns`
/// nanoseconds (> 0), against its classification, laser plan and gateway list
/// (`gateway_names`, index = gateway index). The whole file or nothing: the first problem
/// found refuses it, with a message naming its line ("line 7: ..."):
///  - a malformed line (not six fields, a quoted field not closed or followed by more text, a
///    quote inside an unquoted field, a quoted field other than the gateway), an unknown kind,
///    a catalogue number that is not decimal digits, or a header that is not the one above;
///  - fields that do not fit the kind (one that must be set is empty, one that must be empty
///    is set);
///  - a catalogue number that is not in the classification, or is a satellite in no plane
///    (excluded, with its reason);
///  - a laser link joining a satellite to itself, or two satellites without a link in the
///    plan;
///  - a gateway name that no gateway has, or that more than one gateway has;
///  - a start that is not a UTC time, is neither a step of the run nor before its start, or is
///    more than 1e18 ns (about 31.7 years, the longest run the programs accept) from the run's
///    start;
///  - a length that is not a decimal number, is negative, is zero, or is so short against the
///    start that start_s + length_s == start_s;
///  - the same element failing twice at overlapping times (the intervals [start, end) meet;
///    a permanent failure lasts for ever), naming the line of the first.
/// A file with a header and no failure gives an empty schedule. An empty file, or one without
/// a header, is refused. A step_ns <= 0 is refused.
[[nodiscard]] FailureScheduleOrError read_failure_schedule(
    std::istream& in, const constellation::Classification& classification,
    const topology::LinkPlan& plan, std::span<const std::string> gateway_names,
    const time::UtcTime& run_start_utc, std::int64_t step_ns);

/// read_failure_schedule from a file; every message starts with the path, and a file that is
/// missing or cannot be opened is refused.
[[nodiscard]] FailureScheduleOrError read_failure_schedule_file(
    const std::filesystem::path& path, const constellation::Classification& classification,
    const topology::LinkPlan& plan, std::span<const std::string> gateway_names,
    const time::UtcTime& run_start_utc, std::int64_t step_ns);

}  // namespace satsim::failure
