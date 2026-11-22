#include "failures/run.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <map>
#include <memory>
#include <optional>
#include <ostream>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <variant>
#include <vector>

#include "common/csv.hpp"
#include "common/statistics.hpp"
#include "failures/arguments.hpp"
#include "failures/flooding.hpp"
#include "failures/geometry.hpp"
#include "failures/loop.hpp"
#include "failures/measures.hpp"
#include "failures/reports.hpp"
#include "failures/settings.hpp"
#include "failures/timeline.hpp"
#include "failures/view.hpp"
#include "inputs/inputs.hpp"
#include "inputs/step_source.hpp"
#include "routing/engine.hpp"
#include "routing/graph.hpp"
#include "routing/run.hpp"
#include "routing/settings.hpp"
#include "satsim/failure/failure_file.hpp"
#include "satsim/failure/schedule.hpp"
#include "satsim/network/pieces.hpp"
#include "satsim/routing/strategy.hpp"
#include "satsim/topology/ground_links.hpp"
#include "satsim/topology/link_file.hpp"
#include "satsim/topology/link_plan.hpp"
#include "satsim/topology/link_state.hpp"
#include "satsim/topology/physical_checks.hpp"
#include "satsim/topology/positions.hpp"

namespace satsim::failures_app {

namespace {

using app::append_csv_field;
using app::append_double;
using app::append_integer;
using app::fixed;
using routing_app::NodeKind;
using routing_app::NodeMeaning;

constexpr double kMsPerS = 1000.0;
constexpr double kNsPerS = 1e9;

routing_app::EngineSettings engine_settings_of(const routing_app::Options& o) {
  return routing_app::EngineSettings{.strategies = o.strategies,
                                     .step_ns = o.step_ns,
                                     .replan_ns = o.replan_ns,
                                     .slot_ns = o.slot_ns,
                                     .planner = o.planner,
                                     .serving = o.serving,
                                     .backup = o.backup,
                                     .hop_delay_s = o.hop_delay_s};
}

double step_seconds(const routing_app::Options& o) {
  return static_cast<double>(o.step_ns) / kNsPerS;
}

// The loop's StepWorld on the program's StepSource (whose plan is the run's link list).
class SourceWorld final : public StepWorld {
 public:
  SourceWorld(const app::StepSource& steps, std::size_t city_count, double step_s)
      : steps_(&steps), city_count_(city_count), step_s_(step_s) {}

  [[nodiscard]] std::size_t known_steps() const override { return steps_->full_window(); }

  [[nodiscard]] const StepGeometry& geometry(std::size_t offset) const override {
    const std::int64_t step = steps_->index() + static_cast<std::int64_t>(offset);
    // Drop the steps already passed.
    cache_.erase(cache_.begin(), cache_.lower_bound(steps_->index()));
    std::unique_ptr<StepGeometry>& entry = cache_[step];
    if (!entry) {
      const auto& visible = steps_->visible(offset);
      const auto cities = static_cast<std::ptrdiff_t>(city_count_);
      entry = std::make_unique<StepGeometry>(
          StepGeometry{.utc = steps_->links(offset).utc,
                       .position_ecef_km = {},
                       .city_visible = {visible.begin(), visible.begin() + cities},
                       .gateway_visible = {visible.begin() + cities, visible.end()}});
    }
    return *entry;
  }

  [[nodiscard]] const topology::StepLinks& geometric(std::size_t offset) const override {
    return steps_->links(offset);
  }

  [[nodiscard]] std::vector<double> times_left_in_view_s(
      std::size_t site, std::span<const topology::VisibleSatellite> visible,
      double horizon_s) const override {
    return steps_->visibility().times_left_in_view_s(site, visible, steps_->position_steps(),
                                                     step_s_, horizon_s);
  }

  [[nodiscard]] bool moving_north(std::size_t satellite) const override {
    if (steps_->position_window() < 2) {
      return false;
    }
    const auto& now = steps_->positions(0).position_ecef_km.at(satellite);
    const auto& next = steps_->positions(1).position_ecef_km.at(satellite);
    return now && next && next->z() > now->z();
  }

 private:
  const app::StepSource* steps_;
  std::size_t city_count_;
  double step_s_;
  mutable std::map<std::int64_t, std::unique_ptr<StepGeometry>> cache_;
};

void write_header(std::ostream& out, std::span<const std::string_view> columns,
                  std::span<const std::string_view> more = {}) {
  std::string line;
  for (const auto& group : {columns, more}) {
    for (const std::string_view column : group) {
      if (!line.empty()) {
        line.push_back(',');
      }
      line.append(column);
    }
  }
  line.push_back('\n');
  out.write(line.data(), static_cast<std::streamsize>(line.size()));
}

void write_text(std::ostream& out, const std::string& line) {
  out.write(line.data(), static_cast<std::streamsize>(line.size()));
}

std::int64_t norad(const FailureRun& r, std::size_t satellite) {
  return r.run.working.satellites.at(satellite).norad_cat_id;
}

void append_optional(std::string& line, std::optional<double> value) {
  if (value) {
    append_double(line, *value);
  } else {
    line.append("none");
  }
}

// The direction's columns from, to, role, pattern, model.
void append_direction(std::string& line, const FailureRun& r, std::size_t direction) {
  const app::PairDirection& pd = r.run.directions.at(direction);
  append_csv_field(line, r.run.cities[pd.from].site.name);
  line.push_back(',');
  append_csv_field(line, r.run.cities[pd.to].site.name);
  line.push_back(',');
  line.append(app::role_name(pd.role));
  line.push_back(',');
  line.append(topology::name_of(r.options.routing.pattern));
  line.push_back(',');
  line.append(app::name_of(r.options.routing.model));
}

std::string quantiles_text(const std::vector<double>& values, int decimals, std::string_view unit) {
  const std::optional<app::Quantiles> q = app::quantiles(values);
  if (!q) {
    return "none";
  }
  const auto v = [&](double x) { return fixed(x, decimals) + std::string(unit); };
  return "min " + v(q->min) + ", median " + v(q->median) + ", max " + v(q->max) + " (" +
         std::to_string(q->count) + ")";
}

std::string seconds_or_none(std::optional<double> s) {
  return s ? fixed(*s, 3) + " s" : std::string("none");
}

bool lists(const Options& o, Reaction reaction) {
  return std::find(o.reactions.begin(), o.reactions.end(), reaction) != o.reactions.end();
}

// A control-graph node as the facts and floods CSVs write it.
std::string node_text(const FailureRun& r, std::size_t node) {
  const ControlNodes& nodes = r.timeline.nodes;
  std::string text;
  if (nodes.is_satellite(node)) {
    text = "satellite:";
    append_integer(text, r.run.working.satellites.at(node).norad_cat_id);
  } else if (nodes.is_dish(node)) {
    text = "city:" + r.run.cities.at(node - nodes.satellites).site.name;
  } else {
    text = "gateway:" + r.run.gateways.at(node - nodes.satellites - nodes.cities).site.name;
  }
  return text;
}

// Whether lane (engine kind, strategy) has a row in the measures and the summary.
bool measured(const FailureStatistics& stats, std::size_t e, std::size_t s) {
  const LaneKind kind = stats.kinds.at(e);
  return kind == LaneKind::kPlanner || kind == LaneKind::kLocalRepair ||
         (kind == LaneKind::kFlooding && stats.flooding_strategy == s);
}

}  // namespace

std::string element_text(const FailureRun& r, const Subject& subject) {
  std::string text;
  switch (subject.kind) {
    case SubjectKind::kSatellite:
      text = "satellite:";
      append_integer(text, norad(r, subject.index));
      break;
    case SubjectKind::kLink: {
      const topology::PlannedLink& link = r.timeline.links.at(subject.index);
      const std::int64_t a = norad(r, link.satellite_a);
      const std::int64_t b = norad(r, link.satellite_b);
      text = "link:";
      append_integer(text, std::min(a, b));
      text.push_back('-');
      append_integer(text, std::max(a, b));
      break;
    }
    case SubjectKind::kGateway:
      text = "gateway:" + r.run.gateways.at(subject.index).site.name;
      break;
  }
  return text;
}

std::optional<std::string> refused(const Options& options) {
  if (lists(options, Reaction::kFlooding) &&
      std::none_of(options.routing.strategies.begin(), options.routing.strategies.end(),
                   [](const routing::Strategy& s) {
                     return s.kind == routing::StrategyKind::kLowestDelay;
                   })) {
    return std::string(
        "--reactions flooding needs lowest-delay in --strategies (the flooding lanes are "
        "lowest-delay's, measured against its baseline and reference)");
  }
  return std::nullopt;
}

FailureRunOrError prepare_failure_run(const Options& options) {
  if (auto error = check_reaction_times(options.times)) {
    return std::move(*error);
  }
  routing_app::PreparedRunOrError prepared = routing_app::prepare_run(options.routing);
  if (auto* error = std::get_if<std::string>(&prepared)) {
    return std::move(*error);
  }
  FailureRun r{.run = std::move(std::get<routing_app::PreparedRun>(prepared)),
               .options = options,
               .schedule = {},
               .timeline_settings = {},
               .timeline = {},
               .reports = {},
               .floods = {}};
  r.options.routing = r.run.options;
  std::vector<std::string> gateway_names;
  gateway_names.reserve(r.run.gateways.size());
  for (const app::GatewaySite& g : r.run.gateways) {
    gateway_names.push_back(g.site.name);
  }
  if (options.failures) {
    failure::FailureScheduleOrError read = failure::read_failure_schedule_file(
        *options.failures, r.run.working, r.run.plan, gateway_names, r.run.options.start,
        r.run.options.step_ns);
    if (auto* error = std::get_if<std::string>(&read)) {
      return "--failures " + *error;
    }
    r.schedule = std::move(std::get<failure::FailureSchedule>(read));
  }
  const app::InputOptions inputs = app::input_options_of(r.run.options);
  SnapshotGeometry geometry(r.run, inputs);
  r.timeline_settings = TimelineSettings{.times = options.times,
                                         .model = r.run.options.model,
                                         .hop_delay_s = r.run.options.hop_delay_s,
                                         .step_s = step_seconds(r.run.options),
                                         .step_count = r.run.time_count,
                                         .gateway_count = r.run.gateways.size(),
                                         .atmosphere_km = r.run.options.limits.atmosphere_km};
  r.timeline = build_timeline(r.run.working, r.run.plan, r.schedule, r.timeline_settings, geometry);
  r.reports = fact_reports(r.timeline, r.timeline_settings, geometry);
  if (lists(options, Reaction::kFlooding)) {
    r.floods = flood_facts(r.timeline, r.timeline_settings, geometry);
  }
  if (const std::optional<std::string>& error = geometry.error()) {
    return *error;
  }
  return r;
}

FailureOutcome write_failure_routes(const FailureRun& r, std::ostream& routes,
                                    std::ostream* network, const FailureHooks& hooks) {
  FailureOutcome outcome;
  FailureStatistics& stats = outcome.statistics;
  routing_app::RunStatistics& rs = stats.routing;
  const routing_app::PreparedRun& run = r.run;
  const routing_app::Options& o = run.options;
  const routing_app::EngineSettings settings = engine_settings_of(o);
  const app::InputOptions inputs = app::input_options_of(o);
  const double step_s = step_seconds(o);

  const LoopSettings loop_settings{.engine = settings,
                                   .reactions = r.options.reactions,
                                   .replan_on_report = r.options.replan_on_report,
                                   .times = r.options.times,
                                   .step_count = run.time_count,
                                   .floods = &r.floods};
  FailureLoop loop(r.timeline, routing_app::network_of(run), loop_settings, hooks.loop);
  for (std::size_t e = 0; e < loop.engine_count(); ++e) {
    stats.kinds.push_back(loop.kind(e));
  }
  // The steps carry a state for every link of the run's list.
  routing_app::PreparedRun stepping = run;
  stepping.plan.links = r.timeline.links;
  app::StepSource steps(stepping, inputs, routing_app::position_steps_needed(settings, o.model),
                        routing_app::full_steps_needed(settings), hooks.after_links);
  const SourceWorld world(steps, run.cities.size(), step_s);
  const routing_app::GraphLayout& layout = loop.engine(0).layout();
  const std::size_t city_count = run.cities.size();
  const std::size_t planned = r.timeline.planned_count;
  std::vector<std::string> strategy_names;
  strategy_names.reserve(o.strategies.size());
  for (const routing::Strategy& s : o.strategies) {
    strategy_names.push_back(routing::name_of(s));
  }
  rs.ideal_ms.resize(run.directions.size());

  // The network CSV's scopes: all, then each used shell.
  std::vector<std::pair<std::string, std::optional<std::size_t>>> scopes = {{"all", std::nullopt}};
  for (std::size_t shell = 0; shell < run.config.shells.size(); ++shell) {
    if (shell < run.shell_used.size() && run.shell_used[shell]) {
      scopes.emplace_back(run.config.shells[shell].name, shell);
    }
  }

  write_header(routes, routing_app::kRoutesColumns, kRoutesExtraColumns);
  if (network != nullptr) {
    write_header(*network, kNetworkColumns);
  }
  std::vector<LaneRow> rows;
  std::string line;
  for (std::int64_t i = 0; i < run.time_count && routes; ++i) {
    if (auto error = steps.advance()) {
      outcome.error = std::move(*error);
      break;
    }
    const topology::StepPositions& positions = steps.positions();
    const topology::StepLinks& all_links = steps.links();
    topology::StepLinks links{.utc = all_links.utc, .states = {}};
    links.states.assign(all_links.states.begin(),
                        all_links.states.begin() + static_cast<std::ptrdiff_t>(planned));
    const std::vector<std::vector<topology::VisibleSatellite>>& visible = steps.visible();
    app::add_checks(rs.physical,
                    topology::run_physical_checks(run.working, run.plan, links, positions, o.limits,
                                                  steps.sites(), visible, o.mask));
    rs.propagation_failures += positions.failed_count;
    rs.link_steps_up += topology::count_up(links);
    const auto down = topology::count_down_by_reason(links);
    for (std::size_t k = 0; k < down.size(); ++k) {
      rs.link_steps_down.at(k) += down.at(k);
    }
    for (std::size_t s = 0; s < visible.size(); ++s) {
      (s < city_count ? rs.city_link_steps : rs.gateway_link_steps) += visible[s].size();
    }
    const std::string time_text = app::format_utc_iso(positions.utc, run.fraction_digits);
    if (auto error = loop.step(i, world, rows)) {
      outcome.error = "step " + time_text + ": " + *error;
      break;
    }
    ++rs.steps;

    // The truth at the step: the force checks, the link-steps by force, the network CSV.
    const OverlaidStep& truth = loop.truth();
    const topology::ForceCheckCounts force =
        topology::run_force_checks(run.working, r.timeline.links, planned, truth.force, truth.links,
                                   positions, truth.satellite_down, o.limits);
    stats.force.laser_reuse += force.laser_reuse;
    stats.force.repair_shell += force.repair_shell;
    stats.force.failed_satellite_link += force.failed_satellite_link;
    stats.force.line_of_sight += force.line_of_sight;
    stats.force.structure += force.structure;
    for (const topology::LinkForce f : truth.force) {
      ++stats.link_steps_by_force.at(static_cast<std::size_t>(f));
    }
    if (network != nullptr) {
      std::vector<network::NodePair> edges;
      for (std::size_t j = 0; j < truth.links.states.size(); ++j) {
        if (truth.links.states[j].up()) {
          edges.emplace_back(r.timeline.links[j].satellite_a, r.timeline.links[j].satellite_b);
        }
      }
      for (const auto& [name, shell] : scopes) {
        std::vector<std::uint8_t> present(run.working.satellites.size(), 0);
        std::size_t working = 0;
        for (std::size_t s = 0; s < present.size(); ++s) {
          const auto& sat = run.working.satellites[s];
          const bool member = std::holds_alternative<constellation::PlaneAssignment>(sat.placement);
          const bool in_scope = !shell || sat.shell_index == shell;
          const bool is_down = s < truth.satellite_down.size() && truth.satellite_down[s] != 0;
          if (member && in_scope && !is_down) {
            present[s] = 1;
            ++working;
          }
        }
        const network::Pieces pieces = network::connected_pieces(present.size(), edges, present);
        const std::size_t largest =
            pieces.size.empty() ? 0 : *std::max_element(pieces.size.begin(), pieces.size.end());
        line.clear();
        line.append(time_text);
        line.push_back(',');
        append_csv_field(line, name);
        line.push_back(',');
        append_integer(line, static_cast<std::int64_t>(working));
        line.push_back(',');
        append_integer(line, static_cast<std::int64_t>(pieces.count()));
        line.push_back(',');
        append_integer(line, static_cast<std::int64_t>(largest));
        line.push_back(',');
        if (working > 0) {
          append_double(line, static_cast<double>(largest) / static_cast<double>(working));
        } else {
          line.append("none");
        }
        line.push_back('\n');
        write_text(*network, line);
      }
    }

    for (const LaneRow& lr : rows) {
      const routing_app::RouteRow& row = lr.row;
      if (lr.engine == 0 && row.strategy == 0 && row.ideal_s) {
        rs.ideal_ms[row.direction].push_back(*row.ideal_s * kMsPerS);
      }
      line.clear();
      routing_app::append_route_columns(line, run, layout, time_text, strategy_names[row.strategy],
                                        row);
      line.push_back(',');
      line.append(name_of(loop.kind(lr.engine)));
      line.push_back(',');
      line.append(name_of(lr.status.state));
      line.push_back(',');
      if (lr.status.state == DirectionState::kBlackHole && lr.status.element) {
        append_csv_field(line, element_text(r, *lr.status.element));
      }
      line.push_back('\n');
      write_text(routes, line);
      ++rs.rows_written;
    }
  }
  loop.finish();
  routes.flush();
  if (!routes && !outcome.error) {
    outcome.error = "writing the routes CSV failed";
  }
  if (network != nullptr) {
    network->flush();
    if (!*network && !outcome.error) {
      outcome.error = "writing the network CSV failed";
    }
  }
  const routing_app::Engine& baseline = loop.engine(0);
  rs.routes = baseline.checks();
  rs.measures = baseline.measures();
  rs.drop_events = baseline.drop_events();
  std::sort(rs.drop_events.begin(), rs.drop_events.end(),
            [](const routing_app::DropEvent& a, const routing_app::DropEvent& b) {
              return std::tie(a.start_step, a.direction, a.strategy) <
                     std::tie(b.start_step, b.direction, b.strategy);
            });
  for (std::size_t e = 1; e < loop.engine_count(); ++e) {
    const routing_app::RouteChecks& c = loop.engine(e).checks();
    stats.other_routes.ok_route_not_in_graph += c.ok_route_not_in_graph;
    stats.other_routes.below_ideal += c.below_ideal;
    stats.other_routes.below_lower_bound += c.below_lower_bound;
    stats.other_routes.drop_without_cause += c.drop_without_cause;
    stats.other_routes.bad_route_shape += c.bad_route_shape;
  }
  stats.failures = loop.checks();
  stats.failures.flood_unreached = r.floods.unreached;
  stats.flooding_strategy = loop.flooding_strategy();
  stats.intervals = loop.intervals();
  stats.t_f = failure_instant(r.schedule);
  stats.measures =
      lane_measures(stats.intervals, loop.step_delays(), loop.engine_count(), run.directions.size(),
                    o.strategies.size(), step_s, loop.end_s(), stats.t_f);
  stats.repairs = loop.repairs();
  return outcome;
}

bool write_events(const FailureRun& r, const FailureStatistics& stats, std::ostream& out) {
  const routing_app::GraphLayout layout(
      r.run.options.model, r.run.working.satellites.size(), r.run.cities.size(),
      r.run.options.model == app::EndpointModel::kRealistic ? r.run.gateways.size() : 0,
      r.run.options.model == app::EndpointModel::kRealistic ? r.run.directions.size() : 0,
      r.run.options.model == app::EndpointModel::kRealistic ? r.run.cities.size() : 0);
  write_header(out, kEventsColumns);
  std::string line;
  for (const Interval& iv : stats.intervals) {
    line.clear();
    append_double(line, iv.start_s);
    line.push_back(',');
    append_double(line, iv.end_s);
    line.push_back(',');
    append_direction(line, r, iv.direction);
    line.push_back(',');
    line.append(routing::name_of(r.run.options.strategies.at(iv.strategy)));
    line.push_back(',');
    line.append(name_of(stats.kinds.at(iv.engine)));
    line.push_back(',');
    line.append(name_of(iv.status.state));
    line.push_back(',');
    if (iv.status.cause) {
      line.append(routing_app::name_of(*iv.status.cause));
    }
    line.push_back(',');
    if (iv.status.state == DirectionState::kBlackHole && iv.status.element) {
      append_csv_field(line, element_text(r, *iv.status.element));
    }
    line.push_back(',');
    if (iv.status.state == DirectionState::kDelivered && iv.status.delay_s) {
      append_double(line, *iv.status.delay_s * kMsPerS);
    }
    line.push_back(',');
    std::optional<std::size_t> gateway;
    bool first = true;
    for (const std::size_t node : iv.status.route) {
      if (node >= layout.node_count()) {
        continue;
      }
      const NodeMeaning m = layout.meaning(node);
      if (m.kind == NodeKind::kSatellite) {
        if (!first) {
          line.push_back(' ');
        }
        append_integer(line, norad(r, m.index));
        first = false;
      } else if (m.kind == NodeKind::kGateway) {
        gateway = m.index;
      }
    }
    line.push_back(',');
    if (iv.serving) {
      append_integer(line, norad(r, iv.serving.value_or(0)));
    }
    line.push_back(',');
    if (gateway) {
      append_csv_field(line, r.run.gateways.at(gateway.value_or(0)).site.name);
    }
    line.push_back('\n');
    write_text(out, line);
  }
  out.flush();
  return static_cast<bool>(out);
}

bool write_facts(const FailureRun& r, std::ostream& out) {
  write_header(out, kFactsColumns);
  const bool flooding = lists(r.options, Reaction::kFlooding);
  std::string line;
  for (std::size_t f = 0; f < r.timeline.facts.size(); ++f) {
    const Fact& fact = r.timeline.facts[f];
    line.clear();
    append_integer(line, static_cast<std::int64_t>(f));
    line.push_back(',');
    append_csv_field(line, element_text(r, fact.subject));
    line.append(fact.down ? ",down," : ",up,");
    append_double(line, fact.t_s);
    line.append(fact.old ? ",yes," : ",no,");
    std::string detectors;
    for (const Detection& d : fact.detectors) {
      if (!detectors.empty()) {
        detectors.push_back(';');
      }
      detectors.append(node_text(r, d.node));
      detectors.push_back('@');
      append_double(detectors, d.t_s);
    }
    append_csv_field(line, detectors);
    line.push_back(',');
    if (std::isfinite(fact.planner_known_s)) {
      append_double(line, fact.planner_known_s);
    } else {
      line.append("never");
    }
    line.push_back(',');
    const FactReports& reports = r.reports.at(f);
    append_integer(line, static_cast<std::int64_t>(reports.reports));
    line.push_back(',');
    if (reports.first_hops) {
      append_integer(line, static_cast<std::int64_t>(*reports.first_hops));
    }
    // flood_nodes, flood_last_s, messages: flooding only.
    line.push_back(',');
    if (flooding) {
      const Flood& flood = r.floods.facts.at(f);
      append_integer(line, static_cast<std::int64_t>(flood.nodes));
      line.push_back(',');
      if (flood.last_s) {
        append_double(line, *flood.last_s);
      } else {
        line.append("never");
      }
      line.push_back(',');
      append_integer(line, static_cast<std::int64_t>(flood.messages));
    } else {
      line.append(",,");
    }
    line.push_back('\n');
    write_text(out, line);
  }
  out.flush();
  return static_cast<bool>(out);
}

bool write_floods(const FailureRun& r, std::ostream& out) {
  write_header(out, kFloodsColumns);
  std::string line;
  for (std::size_t f = 0; f < r.floods.facts.size(); ++f) {
    const std::vector<double>& learn = r.floods.facts[f].learn_s;
    for (std::size_t node = 0; node < learn.size(); ++node) {
      if (!std::isfinite(learn[node])) {
        continue;
      }
      line.clear();
      append_integer(line, static_cast<std::int64_t>(f));
      line.push_back(',');
      append_csv_field(line, node_text(r, node));
      line.push_back(',');
      append_double(line, learn[node]);
      line.push_back('\n');
      write_text(out, line);
    }
  }
  out.flush();
  return static_cast<bool>(out);
}

bool write_measures(const FailureRun& r, const FailureStatistics& stats, std::ostream& out) {
  write_header(out, kMeasuresColumns);
  std::size_t reports = 0;
  std::size_t messages = 0;
  for (const FactReports& f : r.reports) {
    reports += f.reports;
    messages += f.hops_total;
  }
  const std::size_t d_count = r.run.directions.size();
  const std::size_t s_count = r.run.options.strategies.size();
  std::string line;
  for (std::size_t d = 0; d < d_count; ++d) {
    for (std::size_t s = 0; s < s_count; ++s) {
      for (std::size_t e = 0; e < stats.kinds.size(); ++e) {
        if (!measured(stats, e, s)) {
          continue;
        }
        const bool flooding = stats.kinds[e] == LaneKind::kFlooding;
        const LaneMeasures& m = stats.measures.at((e * d_count + d) * s_count + s);
        const std::optional<app::Quantiles> q = app::quantiles(m.extra_ms);
        line.clear();
        append_direction(line, r, d);
        line.push_back(',');
        line.append(routing::name_of(r.run.options.strategies[s]));
        line.push_back(',');
        line.append(name_of(stats.kinds[e]));
        for (const std::optional<double> v :
             {stats.t_f, m.black_hole_s, m.loop_s, m.lost_s, m.restore_s, m.settle_s,
              q ? std::optional(q->min) : std::nullopt, q ? std::optional(q->median) : std::nullopt,
              q ? std::optional(q->max) : std::nullopt, m.extra_settled_ms}) {
          line.push_back(',');
          append_optional(line, v);
        }
        line.push_back(',');
        if (flooding) {
          line.append("none");
        } else {
          append_integer(line, static_cast<std::int64_t>(reports));
        }
        line.push_back(',');
        append_integer(line, static_cast<std::int64_t>(flooding ? r.floods.messages() : messages));
        line.push_back('\n');
        write_text(out, line);
      }
    }
  }
  out.flush();
  return static_cast<bool>(out);
}

void write_failure_summary(const FailureRun& r, const FailureStatistics& stats, std::ostream& out) {
  routing_app::write_summary_body(r.run, stats.routing, out);
  const Options& o = r.options;
  const ReactionTimes& t = o.times;
  out << "  failures (Phase 4):\n";
  out << "    reactions:";
  for (const Reaction reaction : o.reactions) {
    out << ' ' << name_of(reaction);
  }
  out << "; replan on report " << (o.replan_on_report ? "on" : "off") << ", repoint "
      << (t.repoint ? "on" : "off") << "; detection " << fixed(t.detection_s, 3) << " s, per hop "
      << fixed(t.hop_processing_s, 3) << " s, compute " << fixed(t.compute_s, 3)
      << " s, planner processing " << fixed(t.planner_processing_s, 3) << " s, acquisition "
      << fixed(t.acquisition_s, 3) << " s\n";
  out << "    schedule: " << r.schedule.failures.size() << " failures";
  if (o.failures) {
    out << " from " << o.failures->string();
  }
  out << "; failure instant t_f " << seconds_or_none(stats.t_f) << '\n';
  for (const failure::Failure& f : r.schedule.failures) {
    Subject subject{.kind = SubjectKind::kSatellite, .index = f.element};
    if (f.kind == failure::ElementKind::kLaserLink) {
      subject.kind = SubjectKind::kLink;
    } else if (f.kind == failure::ElementKind::kGateway) {
      subject.kind = SubjectKind::kGateway;
    }
    out << "      " << element_text(r, subject) << " at " << fixed(f.start_s, 3) << " s"
        << (f.start_s < 0.0 ? " (old)" : "") << ", "
        << (f.length_s ? "for " + fixed(*f.length_s, 3) + " s" : std::string("permanent")) << '\n';
  }
  std::size_t old = 0;
  for (const Fact& f : r.timeline.facts) {
    old += f.old ? 1 : 0;
  }
  const std::size_t made = r.timeline.links.size() - r.timeline.planned_count;
  std::size_t dropped = 0;
  for (std::size_t j = r.timeline.planned_count; j < r.timeline.links.size(); ++j) {
    const auto& changes = r.timeline.force[j];
    for (std::size_t c = 1; c < changes.size(); ++c) {
      if (changes[c].force == topology::LinkForce::kNotInForce) {
        ++dropped;
      }
    }
  }
  out << "    timeline: facts " << r.timeline.facts.size() << " (old " << old
      << "), repair links made " << made << ", dropped " << dropped << "; link-steps:";
  for (const topology::LinkForce f : topology::kAllLinkForces) {
    out << ' ' << topology::name_of(f) << ' '
        << stats.link_steps_by_force.at(static_cast<std::size_t>(f));
  }
  out << '\n';
  std::size_t reports = 0;
  std::size_t messages = 0;
  std::vector<double> first_hops;
  for (const FactReports& f : r.reports) {
    reports += f.reports;
    messages += f.hops_total;
    if (f.first_hops) {
      first_hops.push_back(static_cast<double>(*f.first_hops));
    }
  }
  out << "    reports: " << reports << ", their hops " << messages
      << "; hops of the first report per fact: " << quantiles_text(first_hops, 0, "")
      << "; local repairs " << stats.repairs << '\n';
  if (lists(o, Reaction::kFlooding)) {
    std::size_t flooded = 0;
    std::vector<double> nodes;
    std::vector<double> sent;
    for (const Flood& f : r.floods.facts) {
      if (f.learn_s.empty()) {
        continue;
      }
      ++flooded;
      nodes.push_back(static_cast<double>(f.nodes));
      sent.push_back(static_cast<double>(f.messages));
    }
    out << "    floods: " << flooded << " of " << r.floods.facts.size() << " facts, messages "
        << r.floods.messages() << "; nodes per flood " << quantiles_text(nodes, 0, "")
        << "; messages per flood " << quantiles_text(sent, 0, "") << '\n';
  }
  out << "    measures from t_f (per direction, strategy and reaction mode):\n";
  const std::size_t d_count = r.run.directions.size();
  const std::size_t s_count = r.run.options.strategies.size();
  for (std::size_t d = 0; d < d_count; ++d) {
    const app::PairDirection& pd = r.run.directions[d];
    out << "      " << r.run.cities[pd.from].site.name << " -> " << r.run.cities[pd.to].site.name
        << '\n';
    for (std::size_t s = 0; s < s_count; ++s) {
      for (std::size_t e = 0; e < stats.kinds.size(); ++e) {
        if (!measured(stats, e, s)) {
          continue;
        }
        const LaneMeasures& m = stats.measures.at((e * d_count + d) * s_count + s);
        out << "        " << routing::name_of(r.run.options.strategies[s]) << ", "
            << name_of(stats.kinds[e]) << ": black hole " << seconds_or_none(m.black_hole_s)
            << ", lost " << seconds_or_none(m.lost_s) << ", restore "
            << seconds_or_none(m.restore_s) << ", settle " << seconds_or_none(m.settle_s)
            << "; extra delay " << quantiles_text(m.extra_ms, 3, " ms") << ", once settled "
            << (m.extra_settled_ms ? fixed(*m.extra_settled_ms, 3) + " ms" : std::string("none"))
            << "; loop " << seconds_or_none(m.loop_s) << '\n';
      }
    }
  }
  const FailureChecks& c = stats.failures;
  out << "    failure-check failures (must be 0): delivered_not_up " << c.delivered_not_up
      << ", black_hole_not_failed " << c.black_hole_not_failed << ", fact_used_early "
      << c.fact_used_early << ", repair_too_early " << c.repair_too_early << ", settled_differs "
      << c.settled_differs << ", below_baseline_ideal " << c.below_baseline_ideal
      << ", loop_not_repeated " << c.loop_not_repeated << ", table_fact_early "
      << c.table_fact_early << ", flood_unreached " << c.flood_unreached
      << ", settled_tables_differ " << c.settled_tables_differ << '\n';
  const routing_app::RouteChecks& rc = stats.other_routes;
  out << "    route-check failures of the other lanes (must be 0): ok_route_not_in_graph "
      << rc.ok_route_not_in_graph << ", below_ideal " << rc.below_ideal << ", below_lower_bound "
      << rc.below_lower_bound << ", drop_without_cause " << rc.drop_without_cause
      << ", bad_route_shape " << rc.bad_route_shape << '\n';
  const topology::ForceCheckCounts& f = stats.force;
  out << "    force-check failures (must be 0): laser_reuse " << f.laser_reuse << ", repair_shell "
      << f.repair_shell << ", failed_satellite_link " << f.failed_satellite_link
      << ", line_of_sight " << f.line_of_sight << ", structure " << f.structure << '\n';
  out << "    intervals: " << stats.intervals.size() << '\n';
  out << "  checks: " << (stats.passed() ? "PASSED" : "FAILED") << '\n';
}

int run_main(std::span<const std::string_view> args, std::ostream& out, std::ostream& err,
             const FailureHooks& hooks) {
  const ParsedArguments parsed = parse_arguments(args);
  if (parsed.help) {
    out << usage();
    return 0;
  }
  if (!parsed.options) {
    err << "failures: " << parsed.error.value_or("invalid arguments") << '\n'
        << "Run 'failures --help' for usage.\n";
    return 2;
  }
  const Options& options = *parsed.options;
  if (auto refusal = refused(options)) {
    err << "failures: " << *refusal << '\n';
    return 2;
  }
  FailureRunOrError prepared = prepare_failure_run(options);
  if (const auto* error = std::get_if<std::string>(&prepared)) {
    err << "failures: " << *error << '\n';
    return 2;
  }
  const FailureRun& r = std::get<FailureRun>(prepared);
  if (options.routing.links_out) {
    if (auto error =
            topology::write_link_plan_file(r.run.plan, r.run.working, *options.routing.links_out)) {
      err << "failures: --links-out " << *error << '\n';
      return 2;
    }
  }
  std::optional<std::ofstream> network;
  if (options.network_out) {
    network.emplace(*options.network_out, std::ios::binary | std::ios::trunc);
    if (!network->is_open()) {
      err << "failures: cannot open " << options.network_out->string() << " for writing\n";
      return 2;
    }
  }
  std::ostream* network_stream = network ? &*network : nullptr;
  FailureOutcome outcome;
  std::ostream* summary = &out;
  if (options.routing.out == "-") {
    outcome = write_failure_routes(r, out, network_stream, hooks);
    summary = &err;
  } else {
    std::ofstream file(options.routing.out, std::ios::binary | std::ios::trunc);
    if (!file.is_open()) {
      err << "failures: cannot open " << options.routing.out << " for writing\n";
      return 2;
    }
    outcome = write_failure_routes(r, file, network_stream, hooks);
    file.close();
    if (!file && !outcome.error) {
      outcome.error = "writing the routes CSV failed";
    }
  }
  if (outcome.error) {
    err << "failures: " << *outcome.error << " (" << options.routing.out << ")\n";
    return 2;
  }
  const FailureStatistics& stats = outcome.statistics;
  const auto write_file = [&](const std::optional<std::filesystem::path>& path,
                              const auto& writer) {
    if (!path) {
      return true;
    }
    std::ofstream file(*path, std::ios::binary | std::ios::trunc);
    if (!file.is_open() || !writer(file)) {
      err << "failures: cannot write " << path->string() << '\n';
      return false;
    }
    return true;
  };
  if (!write_file(
          options.routing.drops_out,
          [&](std::ostream& s) { return routing_app::write_drops(r.run, stats.routing, s); }) ||
      !write_file(options.events_out, [&](std::ostream& s) { return write_events(r, stats, s); }) ||
      !write_file(options.facts_out, [&](std::ostream& s) { return write_facts(r, s); }) ||
      !write_file(options.floods_out, [&](std::ostream& s) { return write_floods(r, s); }) ||
      !write_file(options.measures_out,
                  [&](std::ostream& s) { return write_measures(r, stats, s); })) {
    return 2;
  }
  write_failure_summary(r, stats, *summary);
  if (!stats.passed()) {
    err << "failures: checks failed: " << stats.routing.routes.total() + stats.other_routes.total()
        << " route-check violations, " << stats.failures.total() << " failure-check violations, "
        << stats.force.total() << " force-check violations, " << stats.routing.physical.total()
        << " physical-check violations\n";
    return 1;
  }
  return 0;
}

}  // namespace satsim::failures_app
