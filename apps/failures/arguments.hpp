#pragma once

#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "failures/settings.hpp"
#include "routing/arguments.hpp"

// The failures program's command line (Phase 4 spec 4.10): everything the routing program
// takes, and the failure file, the reaction modes and the times of spec 4.9.

namespace satsim::failures_app {

/// What one run of the failures program computes and where it writes it. Built by
/// parse_arguments, which has checked every value; tests may also fill one in directly.
struct Options {
  /// Every option of the routing program, parsed and checked as that program does
  /// (routing.out is the routes CSV, routing.drops_out the baseline's drops CSV).
  routing_app::Options routing;
  /// --failures: the failure file; without it the schedule is empty.
  std::optional<std::filesystem::path> failures = std::nullopt;
  /// --reactions, in the order given, each at most once.
  std::vector<Reaction> reactions;
  /// --replan-on-report: the planner re-plans every direction the instant it learns a fact.
  bool replan_on_report = false;
  /// --detection-s, --hop-processing-s, --compute-s, --planner-processing-s,
  /// --acquisition-s and --repoint.
  ReactionTimes times{};
  /// --events-out, --facts-out, --floods-out, --measures-out, --network-out: not written when
  /// unset (--floods-out is accepted and not written yet: flooding is not available).
  std::optional<std::filesystem::path> events_out = std::nullopt;
  std::optional<std::filesystem::path> facts_out = std::nullopt;
  std::optional<std::filesystem::path> floods_out = std::nullopt;
  std::optional<std::filesystem::path> measures_out = std::nullopt;
  std::optional<std::filesystem::path> network_out = std::nullopt;
};

/// The outcome of parse_arguments: exactly one of options, help or error.
struct ParsedArguments {
  std::optional<Options> options;
  bool help = false;
  std::optional<std::string> error;
};

/// Parses the command line, without the program name. --help or -h anywhere wins. Rejected,
/// besides everything the routing program rejects (routing_app::parse_routing_options, with the
/// same messages; --reactions is required too): an empty --failures, a reaction list that is
/// empty, has an empty or unknown item or names one twice, a --replan-on-report or --repoint
/// other than on or off, a time of spec 4.9 that is not a finite number of seconds >= 0, an
/// empty --events-out, --facts-out, --floods-out, --measures-out or --network-out or one of "-", a
/// --lock-on-s other than 0 (the acquisition time is this program's lock-on) and --serving
/// with-route (that rule gives every strategy a dish of its own; the reaction modes share one dish
/// per city). `flooding` is accepted here; the run refuses it (run_main).
[[nodiscard]] ParsedArguments parse_arguments(std::span<const std::string_view> args);

/// The --help text.
[[nodiscard]] std::string_view usage() noexcept;

}  // namespace satsim::failures_app
