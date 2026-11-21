#include "inputs/inputs.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <numbers>
#include <optional>
#include <ostream>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <variant>
#include <vector>

#include "common/csv.hpp"
#include "common/steps.hpp"
#include "satsim/constellation/classification.hpp"
#include "satsim/constellation/config_file.hpp"
#include "satsim/core/physical_constants.hpp"
#include "satsim/ground/distance.hpp"
#include "satsim/ground/home_pop.hpp"
#include "satsim/ground/loaders.hpp"
#include "satsim/ground/sites.hpp"
#include "satsim/orbit/element_selection.hpp"
#include "satsim/orbit/element_set.hpp"
#include "satsim/orbit/snapshot.hpp"
#include "satsim/time/earth_orientation.hpp"
#include "satsim/time/utc_time.hpp"
#include "satsim/topology/geometry.hpp"
#include "satsim/topology/link_file.hpp"
#include "satsim/topology/link_plan.hpp"
#include "satsim/topology/link_state.hpp"
#include "satsim/topology/physical_checks.hpp"
#include "satsim/topology/positions.hpp"

namespace satsim::app {

namespace {

namespace fs = std::filesystem;

// "<path>: record 3 (Akron, OH): message (and 2 more)" for the first bad record of a JSON
// file, or nullopt if there is none.
template <typename Errors>
std::optional<std::string> first_record_error(const fs::path& path, const Errors& errors,
                                              std::string_view what) {
  if (errors.empty()) {
    return std::nullopt;
  }
  const auto& e = errors.front();
  std::string message = path.string() + ": " + std::string(what) + " " + std::to_string(e.index);
  if (!e.name.empty()) {
    message += " (" + e.name + ")";
  }
  message += ": " + e.message;
  if (errors.size() > 1) {
    message += " (and " + std::to_string(errors.size() - 1) + " more)";
  }
  return message;
}

std::optional<std::string> first_line_error(const fs::path& path,
                                            const std::vector<ground::LineError>& errors) {
  if (errors.empty()) {
    return std::nullopt;
  }
  std::string message = path.string() + ": line " + std::to_string(errors.front().line) + ": " +
                        errors.front().message;
  if (errors.size() > 1) {
    message += " (and " + std::to_string(errors.size() - 1) + " more)";
  }
  return message;
}

// Loads gateways.json or assumed_gateways.json; the operational ones are appended.
std::optional<std::string> load_gateway_file(const fs::path& path, GatewaySource source,
                                             std::vector<GatewaySite>& gateways,
                                             std::size_t& loaded, std::size_t& operational) {
  ground::JsonLoadResult<ground::Gateway> result = ground::load_gateways(path);
  if (result.file_error) {
    return result.file_error;
  }
  if (auto error = first_record_error(path, result.record_errors, "record")) {
    return error;
  }
  loaded = result.records.size();
  const std::vector<ground::Gateway> selected = ground::select_gateways(result.records);
  operational = selected.size();
  for (const ground::Gateway& gateway : selected) {
    gateways.push_back(GatewaySite{.site = gateway.site, .source = source});
  }
  return std::nullopt;
}

// Loads the cities, gateways and PoPs, finds every city's home PoP and selects the pairs.
std::optional<std::string> load_ground(const InputOptions& options, Inputs& run) {
  const fs::path& dir = options.ground_dir;
  ground::CitiesLoadResult cities = ground::load_cities(options.cities_file);
  if (cities.file_error) {
    return cities.file_error;
  }
  if (auto error = first_record_error(options.cities_file, cities.city_errors, "city")) {
    return error;
  }
  if (auto error = first_record_error(options.cities_file, cities.pair_errors, "pair")) {
    return error;
  }

  GroundCounts& counts = run.ground;
  if (auto error = load_gateway_file(dir / kGatewaysFileName, GatewaySource::kReal, run.gateways,
                                     counts.real_gateways, counts.real_operational)) {
    return error;
  }
  const fs::path assumed_path = dir / kAssumedGatewaysFileName;
  std::error_code ec;
  counts.assumed_file = fs::exists(assumed_path, ec);
  if (counts.assumed_file) {
    if (auto error = load_gateway_file(assumed_path, GatewaySource::kAssumed, run.gateways,
                                       counts.assumed_gateways, counts.assumed_operational)) {
      return error;
    }
  }

  const fs::path pops_path = dir / kPopCodesFileName;
  ground::JsonLoadResult<ground::Pop> pops = ground::load_pop_codes(pops_path);
  if (pops.file_error) {
    return pops.file_error;
  }
  if (auto error = first_record_error(pops_path, pops.record_errors, "record")) {
    return error;
  }
  const fs::path blocks_path = dir / kPopBlocksFileName;
  ground::CsvLoadResult<ground::PopBlock> blocks = ground::load_starlink_pops(blocks_path);
  if (blocks.file_error) {
    return blocks.file_error;
  }
  if (auto error = first_line_error(blocks_path, blocks.line_errors)) {
    return error;
  }
  const fs::path geofeed_path = dir / kGeofeedFileName;
  ground::CsvLoadResult<ground::GeofeedEntry> geofeed = ground::load_starlink_geofeed(geofeed_path);
  if (geofeed.file_error) {
    return geofeed.file_error;
  }
  if (auto error = first_line_error(geofeed_path, geofeed.line_errors)) {
    return error;
  }
  counts.pops = pops.records.size();
  counts.pop_blocks = blocks.records.size();
  counts.geofeed_blocks = geofeed.records.size();
  const ground::HomePopFinder finder(geofeed.records, blocks.records, pops.records);
  counts.assignment = finder.assignment();

  run.cities = std::move(cities.cities);
  run.home_pops.reserve(run.cities.size());
  for (const ground::City& city : run.cities) {
    run.home_pops.push_back(finder.find(city));
  }

  counts.pairs_in_file = cities.pairs.size();
  const auto index_of = [&](const std::string& name) {
    const auto it = std::find_if(run.cities.begin(), run.cities.end(),
                                 [&](const ground::City& c) { return c.site.name == name; });
    return static_cast<std::size_t>(it - run.cities.begin());  // The loader checked the names.
  };
  for (const ground::CityPair& pair : cities.pairs) {
    const bool wanted =
        options.roles == RoleFilter::kAll ||
        (options.roles == RoleFilter::kStudy) == (pair.role == ground::PairRole::kStudy);
    if (!wanted) {
      continue;
    }
    const std::size_t a = index_of(pair.from);
    const std::size_t b = index_of(pair.to);
    const double km =
        ground::great_circle_distance_on_mean_sphere_km(run.cities[a].site, run.cities[b].site);
    const double lower_bound_s = km / kSpeedOfLightVacuumKmPerS;
    for (const auto& [from, to] : {std::pair{a, b}, std::pair{b, a}}) {
      run.directions.push_back(PairDirection{.from = from,
                                             .to = to,
                                             .role = pair.role,
                                             .great_circle_km = km,
                                             .lower_bound_s = lower_bound_s});
    }
  }
  if (run.directions.empty()) {
    return options.cities_file.string() + ": no city pair with role " +
           std::string(name_of(options.roles));
  }
  return std::nullopt;
}

// The classification with the planes of unused shells removed.
constellation::Classification working_classification(const constellation::Classification& full,
                                                     const std::vector<bool>& shell_used) {
  constellation::Classification working = full;
  for (constellation::ClassifiedSatellite& satellite : working.satellites) {
    if (std::holds_alternative<constellation::PlaneAssignment>(satellite.placement) &&
        satellite.shell_index && !shell_used[*satellite.shell_index]) {
      satellite.placement = constellation::ExclusionReason::kNoPlane;
    }
  }
  for (std::size_t s = 0; s < working.planes.size(); ++s) {
    if (!shell_used[s]) {
      working.planes[s].clear();
    }
  }
  return working;
}

std::optional<std::string> check_options(const InputOptions& options) {
  if (options.step_ns <= 0 || options.duration_ns < 0 ||
      options.duration_ns > app::kMaxDurationNs) {
    return "step must be positive and duration in [0, 1e9] s";
  }
  if (auto error = topology::check_limits(options.limits)) {
    return "limits: " + *error;
  }
  if (auto error = topology::check_mask_settings(options.mask)) {
    return "ground mask: " + *error;
  }
  if (options.max_lasers && (*options.max_lasers == 0 || !options.links_file)) {
    return "max lasers: must be at least 1, and only with a links file";
  }
  if (!(options.fibre_factor >= 1.0) || !std::isfinite(options.fibre_factor)) {
    return "the fibre factor must be finite and at least 1";
  }
  if (auto error = constellation::check_config(options.classification)) {
    return "classification: " + *error;
  }
  for (const std::string& name : options.shells) {
    const auto& shells = options.classification.shells;
    if (std::none_of(shells.begin(), shells.end(),
                     [&](const auto& shell) { return shell.name == name; })) {
      return "no shell named " + name;
    }
  }
  return std::nullopt;
}

}  // namespace

std::string_view name_of(EndpointModel model) noexcept {
  return model == EndpointModel::kRealistic ? "realistic" : "best-case";
}

std::optional<EndpointModel> parse_endpoint_model(std::string_view name) noexcept {
  if (name == "realistic") {
    return EndpointModel::kRealistic;
  }
  if (name == "best-case") {
    return EndpointModel::kBestCase;
  }
  return std::nullopt;
}

std::string_view name_of(GatewaySource source) noexcept {
  return source == GatewaySource::kReal ? "real" : "assumed";
}

std::string_view name_of(RoleFilter roles) noexcept {
  switch (roles) {
    case RoleFilter::kStudy:
      return "study";
    case RoleFilter::kValidation:
      return "validation";
    case RoleFilter::kAll:
      return "all";
  }
  return "unknown";
}

std::string_view role_name(ground::PairRole role) noexcept {
  return role == ground::PairRole::kStudy ? "study" : "validation";
}

std::string_view name_of(ground::HomePopLevel level) noexcept {
  switch (level) {
    case ground::HomePopLevel::kCity:
      return "city";
    case ground::HomePopLevel::kRegion:
      return "region";
    case ground::HomePopLevel::kCountry:
      return "country";
    case ground::HomePopLevel::kNone:
      return "none";
  }
  return "unknown";
}

InputsOrError prepare_inputs(const InputOptions& options_given) {
  InputOptions options = options_given;
  if (options.shells_file) {
    constellation::ClassificationConfigOrError config =
        constellation::load_classification_config(*options.shells_file);
    if (auto* error = std::get_if<std::string>(&config)) {
      return "--shells-file " + *error;
    }
    options.classification = std::move(std::get<constellation::ClassificationConfig>(config));
  }
  if (auto error = check_options(options)) {
    return std::move(*error);
  }
  if (options.lookahead_steps < 0) {
    return std::string("the look-ahead must not be negative");
  }
  const fs::path& dir = options.snapshot_dir;
  constexpr std::array<std::string_view, 3> kSnapshotFiles = {orbit::kStandardElementsFileName,
                                                              orbit::kSupplementalElementsFileName,
                                                              orbit::kEarthOrientationFileName};
  if (auto error = orbit::check_snapshot_directory(dir, kSnapshotFiles)) {
    return std::move(*error);
  }
  constexpr std::array<std::string_view, 4> kGroundFiles = {kGatewaysFileName, kPopCodesFileName,
                                                            kPopBlocksFileName, kGeofeedFileName};
  std::error_code ec;
  std::string missing;
  for (const std::string_view name : kGroundFiles) {
    if (!fs::is_regular_file(options.ground_dir / name, ec)) {
      missing += (missing.empty() ? "" : ", ") + std::string(name);
    }
  }
  if (!missing.empty()) {
    return "ground-data directory " + options.ground_dir.string() + " has no " + missing;
  }
  if (!fs::is_regular_file(options.cities_file, ec)) {
    return options.cities_file.string() + ": no such file";
  }

  // Earth orientation first: quick to load, and every step can be checked before anything
  // slow is parsed.
  const fs::path eop_path = dir / orbit::kEarthOrientationFileName;
  time::EarthOrientationLoadResult eop = time::load_earth_orientation(eop_path);
  if (!eop.table) {
    return eop.error.value_or(eop_path.string() + ": could not be loaded");
  }
  Inputs run{.config = options.classification,
             .time_count = app::step_count(options.duration_ns, options.step_ns),
             .fraction_digits = app::fraction_digits_for(options.start, options.step_ns),
             .first_time = {},
             .last_time = {},
             .earth_orientation = std::move(*eop.table),
             .standard_file = {},
             .supplemental_file = {},
             .superseded = 0,
             .classification = {},
             .shell_used = {},
             .working = {},
             .propagators = {},
             .start_propagation_failures = 0,
             .plan = {},
             .cities = {},
             .home_pops = {},
             .gateways = {},
             .ground = {},
             .directions = {}};
  const time::UtcTime last = app::time_of_step(options.start, options.step_ns, run.time_count - 1);
  run.first_time = app::format_utc_iso(options.start, run.fraction_digits);
  run.last_time = app::format_utc_iso(last, run.fraction_digits);
  const std::int64_t checked_count = run.time_count + options.lookahead_steps;
  if (options.lookahead_steps > 0 && (checked_count - 1) > app::kMaxDurationNs / options.step_ns) {
    return std::string("the look-ahead goes beyond the longest run");
  }
  for (std::int64_t i = 0; i < checked_count; ++i) {
    // The last step first, so a range far outside the table fails at once.
    const std::int64_t k = i == 0 ? checked_count - 1 : i - 1;
    const time::UtcTime t = app::time_of_step(options.start, options.step_ns, k);
    if (!run.earth_orientation.at(t)) {
      return "no Earth-orientation values for " + app::format_utc_iso(t, run.fraction_digits) +
             " in " + eop_path.string() + " (values are never extrapolated)";
    }
  }
  // The reference time of the shells, planes and laser plan.
  const time::UtcTime reference = options.plan_time.value_or(options.start);
  if (options.plan_time && !run.earth_orientation.at(*options.plan_time)) {
    return "no Earth-orientation values for the plan time " +
           app::format_utc_iso(*options.plan_time,
                               app::fraction_digits_for(*options.plan_time, options.step_ns)) +
           " in " + eop_path.string() + " (values are never extrapolated)";
  }

  if (auto error = load_ground(options, run)) {
    return std::move(*error);
  }

  orbit::SnapshotElementsOrError loaded = orbit::load_snapshot_elements(dir);
  if (auto* error = std::get_if<std::string>(&loaded)) {
    return std::move(*error);
  }
  auto& elements = std::get<orbit::SnapshotElements>(loaded);
  run.standard_file = std::move(elements.standard_file);
  run.supplemental_file = std::move(elements.supplemental_file);
  run.superseded = elements.selection.superseded.size();
  std::vector<orbit::ElementSet> sets;
  sets.reserve(elements.selection.selected.size());
  for (const orbit::SourcedElementSet& s : elements.selection.selected) {
    sets.push_back(s.elements);
  }
  constellation::ClassificationOrError classified =
      constellation::classify_constellation(sets, reference, options.classification);
  if (auto* error = std::get_if<std::string>(&classified)) {
    return "classification: " + *error;  // Unreachable: the config was checked.
  }
  run.classification = std::move(std::get<constellation::Classification>(classified));

  const auto& shells = options.classification.shells;
  run.shell_used.assign(shells.size(), options.shells.empty());
  for (std::size_t s = 0; s < shells.size(); ++s) {
    if (std::find(options.shells.begin(), options.shells.end(), shells[s].name) !=
        options.shells.end()) {
      run.shell_used[s] = true;
    }
  }
  run.working = working_classification(run.classification, run.shell_used);
  run.propagators = topology::plane_member_propagators(run.working, sets);

  const std::optional<time::EarthOrientation> eop_reference = run.earth_orientation.at(reference);
  if (!eop_reference) {
    return std::string("no Earth-orientation values for the plan time");  // Checked above.
  }
  const topology::StepPositions at_reference =
      topology::positions_at(run.propagators, reference, *eop_reference);
  run.start_propagation_failures = at_reference.failed_count;
  if (options.links_file) {
    topology::LinkPlanOrError read = topology::read_link_plan_file(
        *options.links_file, run.working,
        {.pattern = options.pattern, .max_lasers = options.max_lasers});
    if (auto* error = std::get_if<std::string>(&read)) {
      return "--links-file " + *error;
    }
    run.plan = std::move(std::get<topology::LinkPlan>(read));
  } else {
    run.plan = topology::plan_laser_links(
        run.working, at_reference.position_ecef_km,
        {.pattern = options.pattern, .atmosphere_km = options.limits.atmosphere_km});
  }
  return run;
}

void add_checks(topology::PhysicalCheckCounts& total, const topology::PhysicalCheckCounts& step) {
  total.line_of_sight += step.line_of_sight;
  total.range_cap += step.range_cap;
  total.laser_budget += step.laser_budget;
  total.symmetry += step.symmetry;
  total.shells += step.shells;
  total.mask += step.mask;
  total.structure += step.structure;
}

void write_inputs_line(std::ostream& out, const InputOptions& options, const Inputs& inputs) {
  const InputOptions& o = options;
  const Inputs& run = inputs;
  out << "  inputs: snapshot " << o.snapshot_dir.string() << ", ground " << o.ground_dir.string()
      << ", cities " << o.cities_file.string() << ", shells "
      << (o.shells_file ? "file " + o.shells_file->string() : std::string("built-in"));
  if (o.plan_time) {
    out << ", plan time "
        << format_utc_iso(*o.plan_time, std::max(run.fraction_digits,
                                                 fraction_digits_for(*o.plan_time, o.step_ns)))
        << " UTC";
  }
  if (o.links_file) {
    out << ", laser links from the file " << o.links_file->string() << " (at most "
        << topology::max_lasers_of(run.plan) << " per satellite)";
  }
  out << '\n';
}

std::string limits_text(const InputOptions& o) {
  std::string text =
      "fibre factor " + fixed(o.fibre_factor, 3) + ", range cap " +
      (o.limits.range_cap_km ? fixed(*o.limits.range_cap_km, 1) + " km" : "none") +
      ", Sun exclusion " +
      (o.limits.sun_exclusion_rad
           ? fixed(*o.limits.sun_exclusion_rad * 180.0 / std::numbers::pi, 2) + " deg"
           : "off") +
      ", lock-on " + fixed(o.limits.lock_on_s, 3) + " s, atmosphere " +
      fixed(o.limits.atmosphere_km, 0) + " km, high-latitude 5 deg mask " +
      (o.mask.high_latitude_allowance ? "on" : "off");
  if (o.mask.min_elevation_rad) {
    text += ", ground mask override: one minimum elevation of " +
            fixed(*o.mask.min_elevation_rad * 180.0 / std::numbers::pi, 3) +
            " deg replaces the FCC masks";
  }
  if (o.mask.max_ground_range_km) {
    text += ", ground mask override: any satellite within " +
            fixed(*o.mask.max_ground_range_km, 3) + " km, no elevation test";
  }
  return text;
}

void write_satellite_lines(std::ostream& out, const Inputs& inputs) {
  const Inputs& run = inputs;
  const auto& config = run.config;
  out << "  element files: gp.json " << run.standard_file.records_loaded << " records ("
      << run.standard_file.record_errors.size() << " errors), supgp.json "
      << run.supplemental_file.records_loaded << " records ("
      << run.supplemental_file.record_errors.size() << " errors), " << run.superseded
      << " superseded\n";

  // Satellites per shell and exclusions, from the full classification.
  const constellation::Classification& c = run.classification;
  std::vector<std::size_t> in_planes(config.shells.size(), 0);
  std::map<constellation::ExclusionReason, std::size_t> reasons;
  for (const constellation::ClassifiedSatellite& sat : c.satellites) {
    if (std::holds_alternative<constellation::PlaneAssignment>(sat.placement)) {
      ++in_planes.at(sat.shell_index.value_or(0));
    } else {
      ++reasons[std::get<constellation::ExclusionReason>(sat.placement)];
    }
  }
  out << "  satellites: " << c.satellites.size() << '\n';
  std::size_t used_satellites = 0;
  for (std::size_t s = 0; s < config.shells.size(); ++s) {
    out << "    shell " << config.shells[s].name << ": " << in_planes[s] << " satellites in "
        << c.planes[s].size() << " planes" << (run.shell_used[s] ? "" : " (not used in this run)")
        << '\n';
    if (run.shell_used[s]) {
      used_satellites += in_planes[s];
    }
  }
  out << "    satellites carrying traffic: " << used_satellites << '\n';
  std::size_t excluded = 0;
  for (const auto& [reason, count] : reasons) {
    excluded += count;
  }
  out << "  satellites excluded: " << excluded << '\n';
  for (const constellation::ExclusionReason reason : constellation::kAllExclusionReasons) {
    const auto it = reasons.find(reason);
    out << "    " << constellation::name_of(reason) << ": "
        << (it == reasons.end() ? 0 : it->second) << '\n';
  }
}

void write_plan_lines(std::ostream& out, const InputOptions& options, const Inputs& inputs) {
  const InputOptions& o = options;
  const Inputs& run = inputs;
  out << (o.links_file ? "  laser links read from the file: " : "  planned laser links: ")
      << run.plan.links.size() << '\n';
  const auto kinds = topology::count_links_by_kind(run.plan);
  for (const topology::LinkKind kind : topology::kAllLinkKinds) {
    out << "    " << topology::name_of(kind) << ": " << kinds.at(static_cast<std::size_t>(kind))
        << '\n';
  }
  const auto unassigned = topology::count_unassigned_lasers(run.plan);
  out << "  lasers without a link:";
  for (const topology::UnassignedReason reason : topology::kAllUnassignedReasons) {
    out << ' ' << topology::name_of(reason) << ' '
        << unassigned.at(static_cast<std::size_t>(reason));
  }
  out << '\n';
}

void write_link_step_lines(
    std::ostream& out, std::size_t link_steps_up,
    const std::array<std::size_t, topology::kAllLinkDownReasons.size()>& link_steps_down,
    std::size_t city_link_steps, std::size_t gateway_link_steps) {
  out << "  link-steps up: " << link_steps_up << ", down:";
  for (const topology::LinkDownReason reason : topology::kAllLinkDownReasons) {
    out << ' ' << topology::name_of(reason) << ' '
        << link_steps_down.at(static_cast<std::size_t>(reason));
  }
  out << '\n';
  out << "  ground link-steps: cities " << city_link_steps << ", gateways " << gateway_link_steps
      << '\n';
}

void write_ground_lines(std::ostream& out, const Inputs& inputs, std::size_t pairs_selected) {
  const Inputs& run = inputs;
  const GroundCounts& g = run.ground;
  out << "  ground data: " << run.cities.size() << " cities, " << g.pairs_in_file
      << " pairs in the file (" << pairs_selected << " selected); gateways real " << g.real_gateways
      << " (" << g.real_operational << " operational), assumed ";
  if (g.assumed_file) {
    out << g.assumed_gateways << " (" << g.assumed_operational << " operational)";
  } else {
    out << "none (no " << kAssumedGatewaysFileName << ")";
  }
  out << "; " << g.pops << " PoPs, " << g.pop_blocks << " PoP blocks, " << g.geofeed_blocks
      << " geofeed blocks (" << g.assignment.mapped_entries << " mapped to a PoP)\n";
  out << "  home PoPs:\n";
  for (std::size_t i = 0; i < run.cities.size(); ++i) {
    const ground::HomePop& home = run.home_pops[i];
    out << "    " << run.cities[i].site.name << ": "
        << (home.pop ? home.pop->code + " (" + home.pop->city + ")" : std::string("none"))
        << ", level " << name_of(home.level) << '\n';
  }
}

void write_physical_check_line(std::ostream& out, const topology::PhysicalCheckCounts& k) {
  out << "  physical-check failures (must be 0): line_of_sight " << k.line_of_sight
      << ", range_cap " << k.range_cap << ", laser_budget " << k.laser_budget << ", symmetry "
      << k.symmetry << ", shells " << k.shells << ", mask " << k.mask << ", structure "
      << k.structure << '\n';
}

}  // namespace satsim::app
