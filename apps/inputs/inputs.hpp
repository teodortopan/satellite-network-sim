#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <iosfwd>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "satsim/constellation/classification.hpp"
#include "satsim/ground/distance.hpp"
#include "satsim/ground/home_pop.hpp"
#include "satsim/ground/sites.hpp"
#include "satsim/orbit/propagator.hpp"
#include "satsim/orbit/snapshot.hpp"
#include "satsim/time/earth_orientation.hpp"
#include "satsim/time/utc_time.hpp"
#include "satsim/topology/geometry.hpp"
#include "satsim/topology/link_plan.hpp"
#include "satsim/topology/link_state.hpp"
#include "satsim/topology/physical_checks.hpp"

// The inputs the network programs (topology, routing) share: the snapshot classified at the
// start, its laser plan (planned or read from a link file), the ground data, every city's home
// PoP and the selected city pairs. Loaded and checked once by prepare_inputs; nothing here
// steps through time (step_source.hpp does).

namespace satsim::app {

/// Which endpoint model a run uses (Phase 2 spec 4.6, Phase 3 spec 4.2).
enum class EndpointModel : std::uint8_t {
  /// "Starlink as an ISP": A's dish, its one serving satellite, lasers, a satellite a gateway
  /// sees, the gateway, fibre to A's home PoP, fibre to B.
  kRealistic,
  /// "Starlink as a backbone": dishes at both cities, each free to use any satellite at or
  /// above its mask, through space only.
  kBestCase,
};

/// "realistic", "best-case".
[[nodiscard]] std::string_view name_of(EndpointModel model) noexcept;
[[nodiscard]] std::optional<EndpointModel> parse_endpoint_model(std::string_view name) noexcept;

/// Where a gateway comes from: the community list (gateways.json) or a file of gateways
/// assumed to exist where that list has none (assumed_gateways.json).
enum class GatewaySource : std::uint8_t { kReal, kAssumed };

/// "real", "assumed".
[[nodiscard]] std::string_view name_of(GatewaySource source) noexcept;

/// An operational gateway the realistic model may use.
struct GatewaySite {
  ground::GroundSite site;
  GatewaySource source = GatewaySource::kReal;
};

/// Which city pairs of the city file a run computes.
enum class RoleFilter : std::uint8_t { kStudy, kValidation, kAll };

/// "study", "validation", "all".
[[nodiscard]] std::string_view name_of(RoleFilter roles) noexcept;

/// "study" or "validation".
[[nodiscard]] std::string_view role_name(ground::PairRole role) noexcept;

/// "city", "region", "country", "none".
[[nodiscard]] std::string_view name_of(ground::HomePopLevel level) noexcept;

/// The ground-data directory's files (written by python/fetch_ground.py). assumed_gateways.json
/// is optional: gateways assumed to exist where the community list has none, in the format of
/// gateways.json.
inline constexpr std::string_view kGatewaysFileName = "gateways.json";
inline constexpr std::string_view kAssumedGatewaysFileName = "assumed_gateways.json";
inline constexpr std::string_view kPopCodesFileName = "pop_codes.json";
inline constexpr std::string_view kPopBlocksFileName = "starlink_pops.csv";
inline constexpr std::string_view kGeofeedFileName = "starlink_geofeed.csv";

/// The settings that decide what is loaded. A program's Options holds these same members (and
/// its own); input_options_of copies them out.
struct InputOptions {
  /// Directory holding gp.json, supgp.json and finals2000A.all.
  std::filesystem::path snapshot_dir;
  /// Directory holding gateways.json, pop_codes.json, starlink_pops.csv, starlink_geofeed.csv
  /// and optionally assumed_gateways.json.
  std::filesystem::path ground_dir;
  /// The cities and pairs (cities.json).
  std::filesystem::path cities_file;
  /// First time step, and (without plan_time) the reference time of the classification and
  /// the link plan.
  time::UtcTime start;
  /// Length of the run and spacing of its steps (step_count).
  std::int64_t duration_ns = 0;
  std::int64_t step_ns = 0;
  topology::LaserPattern pattern = topology::LaserPattern::kThree;
  /// Names of the shells of `classification` to use; empty means every shell.
  std::vector<std::string> shells;
  RoleFilter roles = RoleFilter::kAll;
  topology::LinkLimits limits{};
  /// Fibre delay over light in a vacuum along the great circle; finite and >= 1.
  double fibre_factor = ground::kDefaultFibreRouteInflation;
  topology::ElevationMaskSettings mask{};
  /// Shells, planes and exclusions: the default, unless shells_file is set.
  constellation::ClassificationConfig classification = constellation::default_config();
  std::optional<std::filesystem::path> shells_file = std::nullopt;
  std::optional<std::filesystem::path> links_file = std::nullopt;
  std::optional<std::size_t> max_lasers = std::nullopt;
  /// The reference time of the classification, the positions at it and the laser plan
  /// (Phase 4 spec 4.10 --plan-time); nullopt means `start`. Any time the Earth-orientation
  /// table covers, before or after the start. A links file is read the same either way.
  std::optional<time::UtcTime> plan_time = std::nullopt;
  /// Steps after the last one whose Earth-orientation values must also exist: the look-ahead
  /// of a program that predicts beyond the run (0 for none).
  std::int64_t lookahead_steps = 0;
};

/// The InputOptions members of any options struct that has them under the same names.
template <typename O>
[[nodiscard]] InputOptions input_options_of(const O& o) {
  return InputOptions{.snapshot_dir = o.snapshot_dir,
                      .ground_dir = o.ground_dir,
                      .cities_file = o.cities_file,
                      .start = o.start,
                      .duration_ns = o.duration_ns,
                      .step_ns = o.step_ns,
                      .pattern = o.pattern,
                      .shells = o.shells,
                      .roles = o.roles,
                      .limits = o.limits,
                      .fibre_factor = o.fibre_factor,
                      .mask = o.mask,
                      .classification = o.classification,
                      .shells_file = o.shells_file,
                      .links_file = o.links_file,
                      .max_lasers = o.max_lasers,
                      .plan_time = o.plan_time,
                      .lookahead_steps = 0};
}

/// Copies parsed InputOptions into any options struct with the same members.
template <typename O>
void assign_input_options(O& o, const InputOptions& in) {
  o.snapshot_dir = in.snapshot_dir;
  o.ground_dir = in.ground_dir;
  o.cities_file = in.cities_file;
  o.start = in.start;
  o.duration_ns = in.duration_ns;
  o.step_ns = in.step_ns;
  o.pattern = in.pattern;
  o.shells = in.shells;
  o.roles = in.roles;
  o.limits = in.limits;
  o.fibre_factor = in.fibre_factor;
  o.mask = in.mask;
  o.classification = in.classification;
  o.shells_file = in.shells_file;
  o.links_file = in.links_file;
  o.max_lasers = in.max_lasers;
  o.plan_time = in.plan_time;
}

/// One direction of a selected city pair.
struct PairDirection {
  std::size_t from = 0;  ///< Index into Inputs::cities.
  std::size_t to = 0;
  ground::PairRole role = ground::PairRole::kStudy;
  double great_circle_km = 0.0;  ///< On the mean sphere (ground::great_circle_...).
  double lower_bound_s = 0.0;    ///< great_circle_km / c.
};

/// Every count of the ground data that the summaries report.
struct GroundCounts {
  std::size_t pairs_in_file = 0;
  std::size_t real_gateways = 0;  ///< Records of gateways.json, any status.
  std::size_t real_operational = 0;
  bool assumed_file = false;  ///< assumed_gateways.json exists.
  std::size_t assumed_gateways = 0;
  std::size_t assumed_operational = 0;
  std::size_t pops = 0;
  std::size_t pop_blocks = 0;
  std::size_t geofeed_blocks = 0;
  ground::PopAssignment assignment;
};

/// Inputs loaded and checked: stepping through time can no longer fail on them.
struct Inputs {
  /// The classification configuration used: InputOptions::classification, or the shells
  /// file's.
  constellation::ClassificationConfig config = constellation::default_config();
  std::int64_t time_count = 0;  ///< Number of time steps, at least 1.
  int fraction_digits = 0;      ///< Fraction digits of the utc columns.
  std::string first_time;
  std::string last_time;
  time::EarthOrientationTable earth_orientation;  ///< Covers every step and the look-ahead.

  orbit::ElementFileLoad standard_file{};
  orbit::ElementFileLoad supplemental_file{};
  std::size_t superseded = 0;
  /// The classification of every satellite at the plan time (the start by default).
  constellation::Classification classification{};
  /// Per shell of `config`: whether the run uses it.
  std::vector<bool> shell_used;
  /// `classification` with the planes of unused shells removed (their satellites then carry
  /// no traffic: they are given ExclusionReason::kNoPlane here, and only here). Everything
  /// that steps through time uses this one.
  constellation::Classification working{};
  /// One per satellite of `working`: set for the plane members.
  std::vector<std::optional<orbit::Propagator>> propagators;
  /// Positions failed at the plan time (the start by default).
  std::size_t start_propagation_failures = 0;
  /// Planned at the plan time (the start by default), or read from the links file.
  topology::LinkPlan plan{};

  /// Every city of the city file, in file order, and each one's home PoP.
  std::vector<ground::City> cities;
  std::vector<ground::HomePop> home_pops;
  /// The operational gateways: gateways.json's in file order, then assumed_gateways.json's.
  std::vector<GatewaySite> gateways;
  GroundCounts ground{};
  /// Two per selected pair: from-to, then to-from.
  std::vector<PairDirection> directions;
};

using InputsOrError = std::variant<Inputs, std::string>;

/// Loads and checks everything a run needs. With options.shells_file, the shells and excluded
/// groups are read from that file first (Inputs::config then holds them). Fails, with a message
/// naming the file, time or value at fault, if: the shells file cannot be read or is not a
/// usable configuration, an option is out of range (step, duration, limits, ground mask, fibre
/// factor, max lasers, a shell name), the snapshot directory or one of its files is missing, a
/// step (or a look-ahead step, or the plan time) has no Earth-orientation values, a ground file
/// is missing or has any bad record, no pair has the selected role, an element file cannot be
/// loaded as a whole, or options.links_file is refused (topology::read_link_plan_file). Then
/// classifies the snapshot at the plan time (options.plan_time, else the start), propagates
/// every plane member there and plans the laser links, or reads them from options.links_file
/// against `working` with the pattern and options.max_lasers. The steps of the run still start
/// at options.start.
[[nodiscard]] InputsOrError prepare_inputs(const InputOptions& options_given);

/// Sums one step's physical-check counts into a run's.
void add_checks(topology::PhysicalCheckCounts& total, const topology::PhysicalCheckCounts& step);

// Summary lines both programs write, each ending in a newline.

/// "  inputs: snapshot ..., ground ..., cities ..., shells ...[, plan time ... UTC][, laser
/// links from the file ...]"; the plan time only when options.plan_time is set.
void write_inputs_line(std::ostream& out, const InputOptions& options, const Inputs& inputs);

/// "fibre factor 1.980, range cap none, Sun exclusion off, lock-on 0.000 s, atmosphere 80 km,
/// high-latitude 5 deg mask off" and the ground-mask override if any (no newline).
[[nodiscard]] std::string limits_text(const InputOptions& options);

/// The element files, the satellites per shell and the exclusions.
void write_satellite_lines(std::ostream& out, const Inputs& inputs);

/// The laser plan: links by kind and lasers without a link.
void write_plan_lines(std::ostream& out, const InputOptions& options, const Inputs& inputs);

/// Link-steps up and down (indexed by LinkDownReason) and ground link-steps.
void write_link_step_lines(
    std::ostream& out, std::size_t link_steps_up,
    const std::array<std::size_t, topology::kAllLinkDownReasons.size()>& link_steps_down,
    std::size_t city_link_steps, std::size_t gateway_link_steps);

/// The ground data and every city's home PoP; `pairs_selected` is the number of pairs run.
void write_ground_lines(std::ostream& out, const Inputs& inputs, std::size_t pairs_selected);

/// The physical-check failures line.
void write_physical_check_line(std::ostream& out, const topology::PhysicalCheckCounts& k);

}  // namespace satsim::app
