#pragma once

#include <compare>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

#include "failures/control_graph.hpp"
#include "failures/geometry.hpp"
#include "failures/settings.hpp"
#include "inputs/inputs.hpp"
#include "satsim/constellation/classification.hpp"
#include "satsim/failure/schedule.hpp"
#include "satsim/topology/geometry.hpp"
#include "satsim/topology/link_plan.hpp"
#include "satsim/topology/link_state.hpp"

// What a failure schedule causes, worked out before a run steps through time (Phase 4 spec
// 4.2 to 4.4 and 4.8; docs/phase-4-plan.md, design choices 1 and 2): the run's fixed link list
// with every link's force over time, and every fact with who detects it when and when the
// planner knows.
//
// Time. An instant is seconds from the run's start as a double (failure/schedule.hpp). Step k
// is at k * step_s (computed in double). The step of an instant t is floor(t / step_s),
// clamped to >= 0 (the frozen geometry of spec 4.3). "The first step at or after t" is the
// smallest k >= 0 with k * step_s >= t. Nothing is worked out on geometry at or after
// step_count (the run's end): no deferred detection, no re-matching, no report.
//
// Order. Events are processed in increasing instant; at one instant, in this order: failure
// beginnings (in schedule order), failure ends (in schedule order), ends of acquisition (in
// link order), then one re-matching. Old failures (start_s < 0, not ended by 0) are all
// processed first, in increasing start_s then schedule order.
//
// Every instant, as formed (doubles, in this order):
//  - detection of a link that is up at the failure:   t_f + detection_s
//  - deferred detection (link down at t_f, up at step k > step(t_f)):  k * step_s + detection_s
//  - detection of a failed satellite (gateways, best-case dishes) or gateway (satellites):
//                                                      t_f + detection_s
//  - the planner knowing of a failed gateway:          (t_f + detection_s) + planner_processing_s
//  - a return (satellite or gateway up, at its end):   t_r = start_s + length_s, known at once;
//    a returned gateway known by the planner at       t_r + planner_processing_s
//  - a returned planned link carrying traffic:         t_r + acquisition_s
//  - a repair link matched at step k carrying traffic: k * step_s + acquisition_s, or for one
//    matched for old failures, (latest start_s of the old failures that freed its lasers) +
//    acquisition_s (in force from the start if <= 0)
//  - the planner knowing a fact: the smallest over its detectors of
//    (detection instant + report time) + planner_processing_s, where the report time is 0 for
//    a gateway and, for a satellite or dish, the total network::shortest_paths_from_many
//    returns for it from every working gateway (start delay 0, extra_per_edge_s =
//    hop_processing_s) on the control graph of the true network at the detection instant.

namespace satsim::failures_app {

/// What a fact is about.
enum class SubjectKind : std::uint8_t {
  kLink,       ///< index: into Timeline::links.
  kSatellite,  ///< index: into Classification::satellites.
  kGateway,    ///< index: into the run's gateway list.
};

struct Subject {
  SubjectKind kind = SubjectKind::kLink;
  std::size_t index = 0;

  friend bool operator==(const Subject&, const Subject&) = default;
  friend auto operator<=>(const Subject&, const Subject&) = default;
};

/// One actor knowing a fact by detecting it.
struct Detection {
  std::size_t node = 0;  ///< A control-graph node (ControlNodes).
  double t_s = 0.0;      ///< The instant it knows.

  friend bool operator==(const Detection&, const Detection&) = default;
};

/// One piece of news (spec 2, 4.4).
struct Fact {
  Subject subject;
  bool down = true;  ///< "down" (true) or "up again" (false).
  double t_s = 0.0;  ///< The instant it became true.
  /// News every actor knows from the first step (spec 4.2): no detectors, planner_known_s 0.
  bool old = false;
  /// In increasing node order.
  std::vector<Detection> detectors;
  /// +infinity if the planner never learns it.
  double planner_known_s = 0.0;
  /// The failure (index into FailureSchedule::failures) whose beginning or end made it;
  /// nullopt for the end of an acquisition (a link carrying traffic).
  std::optional<std::size_t> cause = std::nullopt;

  friend bool operator==(const Fact&, const Fact&) = default;
};

/// A link's force from an instant on.
struct ForceChange {
  double from_s = 0.0;  ///< -infinity for the force the link starts with.
  topology::LinkForce force = topology::LinkForce::kInForce;

  friend bool operator==(const ForceChange&, const ForceChange&) = default;
};

/// What build_timeline needs besides the classification, plan, schedule and geometry.
struct TimelineSettings {
  ReactionTimes times{};
  app::EndpointModel model = app::EndpointModel::kBestCase;
  /// Added to every radio and laser edge of the control graph (routing's hop delay).
  double hop_delay_s = 0.0;
  double step_s = 1.0;
  /// Steps of the run (>= 1): steps 0 .. step_count - 1.
  std::int64_t step_count = 1;
  std::size_t gateway_count = 0;
  /// Line of sight for re-matched links (topology::RepairInput::atmosphere_km).
  double atmosphere_km = topology::kDefaultAtmosphereKm;
};

struct Timeline {
  /// The run's fixed link list: the plan's links in order, then every repair link in the order
  /// made.
  std::vector<topology::PlannedLink> links;
  std::size_t planned_count = 0;
  /// Per link: its force over time, in increasing from_s, the first from -infinity (a planned
  /// link starts kInForce, a repair link kNotInForce). At most one change per instant.
  std::vector<std::vector<ForceChange>> force;
  std::vector<Fact> facts;
  /// The instants >= 0 at which the truth changes (a failure begins or ends, a link's force
  /// changes), increasing, each once.
  std::vector<double> changes;
  /// The control graph's nodes (satellites of the classification, cities of the geometry,
  /// the gateways of the settings).
  ControlNodes nodes{};
  /// The schedule, as given.
  failure::FailureSchedule schedule{};

  /// Every link's force at `t`: the last change with from_s <= t.
  [[nodiscard]] std::vector<topology::LinkForce> force_at(double t) const;
  /// Per satellite / gateway: non-zero when a failure of it is in force at t
  /// (failure::in_force).
  [[nodiscard]] std::vector<char> satellite_down_at(double t) const;
  [[nodiscard]] std::vector<char> gateway_down_at(double t) const;
};

/// The step of an instant: floor(t / step_s) clamped to >= 0.
[[nodiscard]] std::int64_t step_of(double t_s, double step_s) noexcept;

/// The first step at or after an instant: the smallest k >= 0 with k * step_s >= t_s.
[[nodiscard]] std::int64_t first_step_at_or_after(double t_s, double step_s) noexcept;

/// The pre-pass. `plan` is the original laser plan of `classification`; every failure of
/// `schedule` names an element below its count (read_failure_schedule checks it).
///
/// (a) A failure begins at t_f. Its element goes down (a satellite or gateway already down by
///     another failure in force changes nothing and makes no fact). Every link that is in force
///     or acquiring and is taken down (a laser link failure's own link; every link, planned or
///     repair, touching a failed satellite) becomes kFailed from t_f. Facts, in this order:
///     - "satellite s down": detected by every gateway not down at t_f that sees s at step(t_f)
///       and, best case, every city that does, at t_f + detection_s;
///     - "gateway g down": detected by every satellite not down at t_f in g's visible list at
///       step(t_f), at t_f + detection_s; the planner knows at
///       (t_f + detection_s) + planner_processing_s;
///     - "link down" for every link taken down here, in link order: if its geometric state at
///       step(t_f) is up, each end satellite not down at t_f detects at t_f + detection_s;
///       else at the first later step k < step_count at which it is up, if the failure is
///       still in force at k * step_s, each end not down then detects at
///       k * step_s + detection_s; otherwise nobody detects it.
/// (b) A failure ends at t_r (a satellite or gateway still down by another failure changes
///     nothing and makes no fact). Facts "satellite s up" (gateways not down at t_r that see it
///     at step(t_r), best case cities too, at t_r) and "gateway g up" (satellites not down
///     at t_r it sees at step(t_r), at t_r; planner t_r + planner_processing_s). Then for each
///     planned link the failure took down (failure::links_taken_down), in link order, that is
///     kFailed and has no failure left (no laser-link failure of it in force, both ends up):
///     every repair link in force or acquiring that uses one of its two lasers (same satellite
///     and side) is dropped (kNotInForce from t_r, fact "link down" known at t_r by both its
///     ends); then the planned link is kAcquiring from t_r and kInForce from
///     t_r + acquisition_s, when the fact "link up" is made, known then by both ends, unless
///     it was failed again meanwhile.
/// (c) Re-matching, only with times.repoint. A laser (satellite, side) is free at an instant
///     when its satellite is not down and its current link is kFailed or kNotInForce, or it
///     never had one (Laser::unassigned == kNoFreePartner; other unassigned lasers never take
///     part). A freed laser is eligible from an instant: its satellite's detection instant of
///     the lost link (never, if it does not detect it), the drop instant for a dropped repair
///     link, the return instant for a laser of a returned satellite that is still free after
///     its return, and always for one that never had a partner. At each step k < step_count
///     at which a laser that is free there has k as the first step at or after its eligible
///     instant, topology::match_free_lasers is called once, after the events of that instant,
///     with step k's positions, the satellites down, the free lasers that are eligible by
///     k * step_s, and as linked every link in force, acquiring or failed. Each link it makes
///     is appended to links, kAcquiring from k * step_s and kInForce from
///     k * step_s + acquisition_s, with a fact "link up" then (as in (b)).
/// (d) Old failures (start_s < 0): one with an end <= 0 is ignored. The others' down facts are
///     old (no detectors, planner_known_s 0), their force changes are at start_s. With
///     times.repoint, the lasers they free are eligible at 0 and matched in one call at step 0;
///     a link of that call whose lasers were freed by old failures starts acquiring at the
///     latest of those failures' start_s (a laser freed at step 0 or never partnered counts as
///     0 and as -infinity): in force from that + acquisition_s, from the start with an old
///     "link up" fact at that instant if it is <= 0. An old failure that ends during the run
///     ends as in (b).
/// (e) planner_known_s as in the header comment; +infinity when no detector reports (a
///     detector whose detection instant is at or after the run's end does not report). Old
///     facts: 0. Gateway facts: (a) and (b).
[[nodiscard]] Timeline build_timeline(const constellation::Classification& classification,
                                      const topology::LinkPlan& plan,
                                      const failure::FailureSchedule& schedule,
                                      const TimelineSettings& settings, Geometry& geometry);

}  // namespace satsim::failures_app
