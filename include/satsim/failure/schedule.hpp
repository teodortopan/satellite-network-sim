#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>
#include <vector>

#include "satsim/time/utc_time.hpp"
#include "satsim/topology/link_plan.hpp"

namespace satsim::failure {

// The failure schedule of a run (docs/phase-4-failures.md 4.2) and what it says is down at an
// instant (2, 4.3).
//
// Time, for the whole of Phase 4. An instant is a double: seconds from the run's start. Step k
// of a run with step step_ns (integer nanoseconds) is at k * step_s, where step_s =
// step_ns / 1e9 as a double (the product computed in double). A failure instant is either a
// step (so exactly k * step_s) or before the run's start (negative: "old news", known by every
// actor from the first step). A transient failure ends at start_s + length_s, which need not be
// a step.
//
// Elements. A satellite is an index into Classification::satellites, a laser link an index
// into LinkPlan::links, a gateway an index into the run's gateway list (the gateway names
// handed to read_failure_schedule, in order).

/// What can fail (spec 4.2). A whole plane or a region is a list of satellites.
enum class ElementKind : std::uint8_t {
  kSatellite,  ///< Its lasers and every ground link to it.
  kLaserLink,  ///< That link.
  kGateway,    ///< Every ground link to it, and its fibre.
};

inline constexpr std::array<ElementKind, 3> kAllElementKinds = {
    ElementKind::kSatellite, ElementKind::kLaserLink, ElementKind::kGateway};

/// "satellite", "laser_link", "gateway": the kind column of the failure file.
[[nodiscard]] std::string_view name_of(ElementKind kind) noexcept;

/// The kind named by name_of, or nullopt.
[[nodiscard]] std::optional<ElementKind> parse_element_kind(std::string_view name) noexcept;

/// One failure.
struct Failure {
  ElementKind kind = ElementKind::kSatellite;
  /// By kind: an index into Classification::satellites (a plane member), into LinkPlan::links,
  /// or into the run's gateway list.
  std::size_t element = 0;
  /// The failure instant as written in the file, exact to the nanosecond.
  time::UtcTime start_utc;
  /// The failure instant in seconds from the run's start (the convention above): k * step_s
  /// for a step k >= 0, else start_utc's offset from the run's start, negative.
  double start_s = 0.0;
  /// How long the element stays down, in seconds, > 0; nullopt for a permanent failure.
  std::optional<double> length_s = std::nullopt;

  friend bool operator==(const Failure&, const Failure&) = default;
};

/// The failures of a run, in file order.
struct FailureSchedule {
  std::vector<Failure> failures;

  friend bool operator==(const FailureSchedule&, const FailureSchedule&) = default;
};

/// When `failure` ends (start_s + length_s), or nullopt if it is permanent.
[[nodiscard]] std::optional<double> end_s(const Failure& failure) noexcept;

/// Whether `failure` is in force at `t_s`: start_s <= t_s, and it is permanent or
/// t_s < start_s + length_s. At its end instant the element works again.
[[nodiscard]] bool in_force(const Failure& failure, double t_s) noexcept;

/// Whether a satellite failure of `satellite` is in force at `t_s`.
[[nodiscard]] bool satellite_down(const FailureSchedule& schedule, std::size_t satellite,
                                  double t_s) noexcept;

/// Whether a gateway failure of `gateway` is in force at `t_s`.
[[nodiscard]] bool gateway_down(const FailureSchedule& schedule, std::size_t gateway,
                                double t_s) noexcept;

/// Whether planned link `link` of `plan` is failed at `t_s`: a laser-link failure of it is in
/// force, or either of its end satellites is down (a failed satellite's lasers are dead).
/// Whether the link has line of sight is link_state.hpp's business, not this one's.
[[nodiscard]] bool link_failed(const FailureSchedule& schedule, const topology::LinkPlan& plan,
                               std::size_t link, double t_s);

/// Everything down at one instant, by index: what satellite_down, link_failed and gateway_down
/// give for every satellite (satellite_count of them), every planned link and every gateway
/// (gateway_count), in one pass over the schedule. Precondition: every failure's element is
/// below its count.
struct DownAt {
  std::vector<bool> satellites;
  std::vector<bool> links;
  std::vector<bool> gateways;

  friend bool operator==(const DownAt&, const DownAt&) = default;
};

[[nodiscard]] DownAt down_at(const FailureSchedule& schedule, const topology::LinkPlan& plan,
                             std::size_t satellite_count, std::size_t gateway_count, double t_s);

/// One failure beginning or ending at an instant.
struct Change {
  std::size_t failure = 0;  ///< Index into FailureSchedule::failures.
  bool begins = true;       ///< True at its start_s, false at its end.

  friend bool operator==(const Change&, const Change&) = default;
};

/// An instant at which what is down changes, and what changes there.
struct Transition {
  double t_s = 0.0;
  /// In failure order. A failure appears at most once per instant (its length is > 0 and its
  /// end differs from its start, read_failure_schedule checks it).
  std::vector<Change> changes;

  friend bool operator==(const Transition&, const Transition&) = default;
};

/// Every instant at which something begins or ends, old failures' (negative) starts included,
/// in increasing t_s, each instant once (equal doubles are one instant).
[[nodiscard]] std::vector<Transition> transitions(const FailureSchedule& schedule);

/// The planned links of `plan` with `satellite` at either end, in increasing link index.
[[nodiscard]] std::vector<std::size_t> links_of(const topology::LinkPlan& plan,
                                                std::size_t satellite);

/// The planned links `failure` takes down while it is in force: a laser link's own index, a
/// satellite's links_of, none for a gateway (its ground links are not in the plan).
[[nodiscard]] std::vector<std::size_t> links_taken_down(const Failure& failure,
                                                        const topology::LinkPlan& plan);

}  // namespace satsim::failure
