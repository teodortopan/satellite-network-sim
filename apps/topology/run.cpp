#include "topology/run.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <map>
#include <numbers>
#include <optional>
#include <ostream>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "common/csv.hpp"
#include "common/statistics.hpp"
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
#include "satsim/topology/ground_links.hpp"
#include "satsim/topology/link_file.hpp"
#include "satsim/topology/link_plan.hpp"
#include "satsim/topology/link_state.hpp"
#include "satsim/topology/physical_checks.hpp"
#include "satsim/topology/positions.hpp"
#include "topology/arguments.hpp"
#include "topology/endpoints.hpp"

namespace satsim::topology_app {

namespace {

namespace fs = std::filesystem;
using app::append_csv_field;
using app::append_double;
using app::append_integer;
using app::fixed;

constexpr double kMsPerS = 1000.0;
constexpr std::size_t kMaxGatewaysListed = 5;

std::string_view role_name(ground::PairRole role) noexcept {
  return role == ground::PairRole::kStudy ? "study" : "validation";
}

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
std::optional<std::string> load_ground(const Options& options, PreparedRun& run) {
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

std::optional<std::string> check_options(const Options& options) {
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

void append_optional_double(std::string& line, bool set, double value) {
  line.push_back(',');
  if (set) {
    append_double(line, value);
  }
}

void append_satellite(std::string& line, const constellation::Classification& c,
                      const std::optional<std::size_t>& satellite) {
  line.push_back(',');
  if (satellite) {
    append_integer(line, c.satellites[*satellite].norad_cat_id);
  }
}

void write_header(std::ostream& csv) {
  std::string line;
  for (const std::string_view column : kCsvColumns) {
    if (!line.empty()) {
      line.push_back(',');
    }
    line.append(column);
  }
  line.push_back('\n');
  csv.write(line.data(), static_cast<std::streamsize>(line.size()));
}

// Sums one step's physical-check counts into the run's.
void add_checks(topology::PhysicalCheckCounts& total, const topology::PhysicalCheckCounts& step) {
  total.line_of_sight += step.line_of_sight;
  total.range_cap += step.range_cap;
  total.laser_budget += step.laser_budget;
  total.symmetry += step.symmetry;
  total.shells += step.shells;
  total.mask += step.mask;
  total.structure += step.structure;
}

std::string quantile_text(const std::vector<double>& values, int decimals, std::string_view unit) {
  const std::optional<app::Quantiles> q = app::quantiles(values);
  if (!q) {
    return "none";
  }
  const auto v = [&](double x) { return fixed(x, decimals) + std::string(unit); };
  return "min " + v(q->min) + ", median " + v(q->median) + ", p95 " + v(q->p95) + ", max " +
         v(q->max);
}

}  // namespace

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

PreparedRunOrError prepare_run(const Options& requested) {
  Options options = requested;
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
  PreparedRun run{.options = options,
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
  for (std::int64_t i = 0; i < run.time_count; ++i) {
    // The last step first, so a range far outside the table fails at once.
    const std::int64_t k = i == 0 ? run.time_count - 1 : i - 1;
    const time::UtcTime t = app::time_of_step(options.start, options.step_ns, k);
    if (!run.earth_orientation.at(t)) {
      return "no Earth-orientation values for " + app::format_utc_iso(t, run.fraction_digits) +
             " in " + eop_path.string() + " (values are never extrapolated)";
    }
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
      constellation::classify_constellation(sets, options.start, options.classification);
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

  const std::optional<time::EarthOrientation> eop_start = run.earth_orientation.at(options.start);
  if (!eop_start) {
    return std::string("no Earth-orientation values for the start");  // Checked above.
  }
  const topology::StepPositions at_start =
      topology::positions_at(run.propagators, options.start, *eop_start);
  run.start_propagation_failures = at_start.failed_count;
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
        run.working, at_start.position_ecef_km,
        {.pattern = options.pattern, .atmosphere_km = options.limits.atmosphere_km});
  }
  return run;
}

RunOutcome write_rows(const PreparedRun& run, std::ostream& csv, const RunHooks& hooks) {
  RunOutcome outcome;
  RunStatistics& stats = outcome.statistics;
  stats.directions.resize(run.directions.size());
  const Options& options = run.options;

  // The ground sites: every city, then every gateway; their visibility in one pass.
  std::vector<ground::GroundSite> sites;
  sites.reserve(run.cities.size() + run.gateways.size());
  std::vector<ground::GroundSite> city_sites;
  std::vector<std::optional<ground::GroundSite>> home_pop_sites;
  for (std::size_t c = 0; c < run.cities.size(); ++c) {
    sites.push_back(run.cities[c].site);
    city_sites.push_back(run.cities[c].site);
    const std::optional<ground::Pop>& pop = run.home_pops[c].pop;
    home_pop_sites.push_back(pop ? std::optional(pop->site) : std::nullopt);
  }
  for (const GatewaySite& gateway : run.gateways) {
    sites.push_back(gateway.site);
  }
  const topology::GroundVisibility visibility(sites, run.working, options.mask);
  std::vector<Direction> directions;
  directions.reserve(run.directions.size());
  for (const PairDirection& d : run.directions) {
    directions.push_back({.from = d.from, .to = d.to});
  }
  EndpointEvaluator evaluator(std::move(city_sites), std::move(home_pop_sites), run.gateways,
                              std::move(directions), run.working.satellites.size(),
                              {.model = options.model,
                               .fibre_factor = options.fibre_factor,
                               .serving_rule = options.serving_rule});
  topology::LinkStateEvaluator link_states(run.plan, options.limits);
  const bool realistic = options.model == EndpointModel::kRealistic;
  const std::string_view pattern_name = topology::name_of(options.pattern);
  const std::string_view model_name = name_of(options.model);
  const std::size_t city_count = run.cities.size();

  write_header(csv);
  std::string line;
  std::vector<PathResult> results;
  for (std::int64_t i = 0; i < run.time_count && csv; ++i) {
    const time::UtcTime utc = app::time_of_step(options.start, options.step_ns, i);
    const std::optional<time::EarthOrientation> eop = run.earth_orientation.at(utc);
    if (!eop) {  // prepare_run checked every step.
      outcome.error =
          "no Earth-orientation values for " + app::format_utc_iso(utc, run.fraction_digits);
      break;
    }
    topology::StepPositions positions = topology::positions_at(run.propagators, utc, *eop);
    topology::StepLinks links = link_states.step(positions);
    if (hooks.after_links) {
      hooks.after_links(positions, links);
    }
    const std::vector<std::vector<topology::VisibleSatellite>> visible = visibility.at(positions);
    add_checks(stats.checks,
               topology::run_physical_checks(run.working, run.plan, links, positions,
                                             options.limits, sites, visible, options.mask));
    stats.propagation_failures += positions.failed_count;
    stats.link_steps_up += topology::count_up(links);
    const auto down = topology::count_down_by_reason(links);
    for (std::size_t r = 0; r < down.size(); ++r) {
      stats.link_steps_down[r] += down[r];
    }
    const std::span<const std::vector<topology::VisibleSatellite>> all(visible);
    for (std::size_t s = 0; s < all.size(); ++s) {
      (s < city_count ? stats.city_link_steps : stats.gateway_link_steps) += all[s].size();
    }
    if (auto error = evaluator.evaluate(run.plan, links, all.first(city_count),
                                        all.subspan(city_count), results)) {
      outcome.error = "step " + app::format_utc_iso(utc, run.fraction_digits) + ": " + *error;
      break;
    }
    ++stats.steps;

    const std::string time_text = app::format_utc_iso(utc, run.fraction_digits);
    for (std::size_t d = 0; d < results.size(); ++d) {
      PathResult& r = results[d];
      if (hooks.before_lower_bound) {
        hooks.before_lower_bound(r);
      }
      const PairDirection& pd = run.directions[d];
      DirectionStatistics& ds = stats.directions[d];
      ++ds.steps_by_status.at(static_cast<std::size_t>(r.status));
      const bool ok = r.status == PathStatus::kOk;
      if (ok) {
        if (r.one_way_s < pd.lower_bound_s) {
          ++stats.lower_bound_failures;
          ++ds.lower_bound_failures;
        }
        ds.one_way_ms.push_back(r.one_way_s * kMsPerS);
        ds.stretch.push_back(r.one_way_s / pd.lower_bound_s);
        ++ds.laser_hop_steps[r.laser_hops];
        if (r.gateway) {
          const GatewaySite& g = run.gateways[*r.gateway];
          ++ds.gateway_steps[g.site.name + " (" + std::string(name_of(g.source)) + ")"];
        }
      }
    }
    for (std::size_t d = 0; d < results.size(); ++d) {
      const PathResult& r = results[d];
      const PairDirection& pd = run.directions[d];
      const bool ok = r.status == PathStatus::kOk;
      line.clear();
      line.append(time_text);
      line.push_back(',');
      append_csv_field(line, run.cities[pd.from].site.name);
      line.push_back(',');
      append_csv_field(line, run.cities[pd.to].site.name);
      line.push_back(',');
      line.append(role_name(pd.role));
      line.push_back(',');
      line.append(pattern_name);
      line.push_back(',');
      line.append(model_name);
      line.push_back(',');
      line.append(name_of(r.status));
      append_optional_double(line, ok, r.one_way_s * kMsPerS);
      // Directions come in pairs (from-to, to-from): the reverse of d is d ^ 1.
      const PathResult& reverse = results[d ^ 1U];
      if (!realistic) {
        append_optional_double(line, ok, 2.0 * r.one_way_s * kMsPerS);
      } else {
        append_optional_double(line, ok && reverse.status == PathStatus::kOk,
                               (r.one_way_s + reverse.one_way_s) * kMsPerS);
      }
      line.push_back(',');
      if (ok) {
        append_integer(line, static_cast<std::int64_t>(r.laser_hops));
      }
      append_optional_double(line, ok, r.path_km());
      append_optional_double(line, ok, r.uplink_km);
      append_optional_double(line, ok, r.laser_km);
      append_optional_double(line, ok, r.downlink_km);
      append_optional_double(line, ok, r.fibre_km);
      append_optional_double(line, true, pd.great_circle_km);
      append_optional_double(line, true, pd.lower_bound_s * kMsPerS);
      append_optional_double(line, ok, r.one_way_s / pd.lower_bound_s);
      append_satellite(line, run.working, r.serving_satellite);
      append_satellite(line, run.working, r.gateway_satellite);
      line.push_back(',');
      if (r.gateway) {
        append_csv_field(line, run.gateways[*r.gateway].site.name);
      }
      line.push_back(',');
      if (r.gateway) {
        line.append(name_of(run.gateways[*r.gateway].source));
      }
      line.push_back(',');
      if (realistic && run.home_pops[pd.from].pop) {
        append_csv_field(line, run.home_pops[pd.from].pop->code);
      }
      line.push_back(',');
      if (realistic) {
        line.append(name_of(run.home_pops[pd.from].level));
      }
      append_satellite(line, run.working, r.first_satellite);
      append_satellite(line, run.working, r.last_satellite);
      line.push_back('\n');
      csv.write(line.data(), static_cast<std::streamsize>(line.size()));
      ++stats.rows_written;
    }
  }
  csv.flush();
  if (!csv && !outcome.error) {
    outcome.error = "writing the CSV failed";
  }
  return outcome;
}

void write_summary(const PreparedRun& run, const RunStatistics& stats, std::ostream& out) {
  const Options& o = run.options;
  const auto& config = o.classification;
  out << "topology summary\n";
  out << "  inputs: snapshot " << o.snapshot_dir.string() << ", ground " << o.ground_dir.string()
      << ", cities " << o.cities_file.string() << ", shells "
      << (o.shells_file ? "file " + o.shells_file->string() : std::string("built-in"));
  if (o.links_file) {
    out << ", laser links from the file " << o.links_file->string() << " (at most "
        << topology::max_lasers_of(run.plan) << " per satellite)";
  }
  out << '\n';
  out << "  times: " << run.time_count << ", from " << run.first_time << " to " << run.last_time
      << " UTC every " << app::seconds_text(o.step_ns) << " s (steps done: " << stats.steps
      << ")\n";
  out << "  settings: pattern " << topology::name_of(o.pattern) << ", model " << name_of(o.model)
      << ", roles " << name_of(o.roles) << ", serving rule highest-elevation, fibre factor "
      << fixed(o.fibre_factor, 3) << ", range cap "
      << (o.limits.range_cap_km ? fixed(*o.limits.range_cap_km, 1) + " km" : "none")
      << ", Sun exclusion "
      << (o.limits.sun_exclusion_rad
              ? fixed(*o.limits.sun_exclusion_rad * 180.0 / std::numbers::pi, 2) + " deg"
              : "off")
      << ", lock-on " << fixed(o.limits.lock_on_s, 3) << " s, atmosphere "
      << fixed(o.limits.atmosphere_km, 0) << " km, high-latitude 5 deg mask "
      << (o.mask.high_latitude_allowance ? "on" : "off");
  if (o.mask.min_elevation_rad) {
    out << ", ground mask override: one minimum elevation of "
        << fixed(*o.mask.min_elevation_rad * 180.0 / std::numbers::pi, 3)
        << " deg replaces the FCC masks";
  }
  if (o.mask.max_ground_range_km) {
    out << ", ground mask override: any satellite within " << fixed(*o.mask.max_ground_range_km, 3)
        << " km, no elevation test";
  }
  out << '\n';
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
  out << "  propagation failures at the start: " << run.start_propagation_failures
      << ", over the run: " << stats.propagation_failures << " satellite-steps\n";

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
  out << "  link-steps up: " << stats.link_steps_up << ", down:";
  for (const topology::LinkDownReason reason : topology::kAllLinkDownReasons) {
    out << ' ' << topology::name_of(reason) << ' '
        << stats.link_steps_down.at(static_cast<std::size_t>(reason));
  }
  out << '\n';
  out << "  ground link-steps: cities " << stats.city_link_steps << ", gateways "
      << stats.gateway_link_steps << '\n';

  const GroundCounts& g = run.ground;
  out << "  ground data: " << run.cities.size() << " cities, " << g.pairs_in_file
      << " pairs in the file (" << run.directions.size() / 2 << " selected); gateways real "
      << g.real_gateways << " (" << g.real_operational << " operational), assumed ";
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

  out << "  paths per pair and direction:\n";
  for (std::size_t d = 0; d < run.directions.size(); ++d) {
    const PairDirection& pd = run.directions[d];
    const DirectionStatistics& ds = stats.directions.at(d);
    out << "    " << run.cities[pd.from].site.name << " -> " << run.cities[pd.to].site.name << " ("
        << role_name(pd.role) << ", " << fixed(pd.great_circle_km, 0) << " km, lower bound "
        << fixed(pd.lower_bound_s * kMsPerS, 3) << " ms):";
    for (const PathStatus status : kAllPathStatuses) {
      const std::size_t n = ds.steps_by_status.at(static_cast<std::size_t>(status));
      if (n > 0 || status == PathStatus::kOk) {
        out << ' ' << name_of(status) << ' ' << n;
      }
    }
    out << '\n';
    out << "      one-way: " << quantile_text(ds.one_way_ms, 3, " ms") << '\n';
    out << "      stretch: " << quantile_text(ds.stretch, 3, "") << '\n';
    if (!ds.laser_hop_steps.empty()) {
      out << "      laser hops (steps):";
      for (const auto& [hops, n] : ds.laser_hop_steps) {
        out << ' ' << hops << ':' << n;
      }
      out << '\n';
    }
    if (!ds.gateway_steps.empty()) {
      std::vector<std::pair<std::string, std::size_t>> used(ds.gateway_steps.begin(),
                                                            ds.gateway_steps.end());
      std::stable_sort(used.begin(), used.end(),
                       [](const auto& a, const auto& b) { return a.second > b.second; });
      out << "      gateways (" << used.size() << "):";
      for (std::size_t k = 0; k < std::min(used.size(), kMaxGatewaysListed); ++k) {
        out << (k == 0 ? " " : "; ") << used[k].first << ' ' << used[k].second;
      }
      if (used.size() > kMaxGatewaysListed) {
        out << "; ...";
      }
      out << '\n';
    }
    if (ds.lower_bound_failures > 0) {
      out << "      BELOW THE LOWER BOUND: " << ds.lower_bound_failures << " steps\n";
    }
  }

  const topology::PhysicalCheckCounts& k = stats.checks;
  out << "  physical-check failures (must be 0): line_of_sight " << k.line_of_sight
      << ", range_cap " << k.range_cap << ", laser_budget " << k.laser_budget << ", symmetry "
      << k.symmetry << ", shells " << k.shells << ", mask " << k.mask << ", structure "
      << k.structure << '\n';
  out << "  delays below the lower bound (must be 0): " << stats.lower_bound_failures << '\n';
  if (o.links_out) {
    out << "  laser links written to " << o.links_out->string() << '\n';
  }
  out << "  rows written: " << stats.rows_written << '\n';
  out << "  checks: " << (stats.passed() ? "PASSED" : "FAILED") << '\n';
}

int run_main(std::span<const std::string_view> args, std::ostream& out, std::ostream& err,
             const RunHooks& hooks) {
  const ParsedArguments parsed = parse_arguments(args);
  if (parsed.help) {
    out << usage();
    return 0;
  }
  if (!parsed.options) {
    err << "topology: " << parsed.error.value_or("invalid arguments") << '\n'
        << "Run 'topology --help' for usage.\n";
    return 2;
  }
  const Options& options = *parsed.options;
  PreparedRunOrError prepared = prepare_run(options);
  if (const auto* error = std::get_if<std::string>(&prepared)) {
    err << "topology: " << *error << '\n';
    return 1;
  }
  const PreparedRun& run = std::get<PreparedRun>(prepared);
  if (options.links_out) {
    if (auto error = topology::write_link_plan_file(run.plan, run.working, *options.links_out)) {
      err << "topology: --links-out " << *error << '\n';
      return 1;
    }
  }

  RunOutcome outcome;
  if (options.out == "-") {
    outcome = write_rows(run, out, hooks);
    write_summary(run, outcome.statistics, err);
  } else {
    std::ofstream file(options.out, std::ios::binary | std::ios::trunc);
    if (!file.is_open()) {
      err << "topology: cannot open " << options.out << " for writing\n";
      return 1;
    }
    outcome = write_rows(run, file, hooks);
    file.close();
    if (!file && !outcome.error) {
      outcome.error = "writing the CSV failed";
    }
    write_summary(run, outcome.statistics, out);
  }
  if (outcome.error) {
    err << "topology: " << *outcome.error << " (" << options.out << ")\n";
    return 1;
  }
  if (!outcome.statistics.passed()) {
    err << "topology: physical checks failed: " << outcome.statistics.checks.total()
        << " violations, " << outcome.statistics.lower_bound_failures
        << " delays below the lower bound\n";
    return 1;
  }
  return 0;
}

}  // namespace satsim::topology_app
