#include "satsim/failure/schedule.hpp"

#include <algorithm>
#include <cstddef>
#include <map>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

#include "satsim/topology/link_plan.hpp"

namespace satsim::failure {

std::string_view name_of(ElementKind kind) noexcept {
  switch (kind) {
    case ElementKind::kSatellite:
      return "satellite";
    case ElementKind::kLaserLink:
      return "laser_link";
    case ElementKind::kGateway:
      return "gateway";
  }
  return "?";
}

std::optional<ElementKind> parse_element_kind(std::string_view name) noexcept {
  for (const ElementKind kind : kAllElementKinds) {
    if (name == name_of(kind)) {
      return kind;
    }
  }
  return std::nullopt;
}

std::optional<double> end_s(const Failure& failure) noexcept {
  if (!failure.length_s) {
    return std::nullopt;
  }
  return failure.start_s + *failure.length_s;
}

bool in_force(const Failure& failure, double t_s) noexcept {
  if (t_s < failure.start_s) {
    return false;
  }
  const std::optional<double> end = end_s(failure);
  return !end || t_s < *end;
}

namespace {

bool element_down(const FailureSchedule& schedule, ElementKind kind, std::size_t element,
                  double t_s) noexcept {
  return std::ranges::any_of(schedule.failures, [&](const Failure& f) {
    return f.kind == kind && f.element == element && in_force(f, t_s);
  });
}

}  // namespace

bool satellite_down(const FailureSchedule& schedule, std::size_t satellite, double t_s) noexcept {
  return element_down(schedule, ElementKind::kSatellite, satellite, t_s);
}

bool gateway_down(const FailureSchedule& schedule, std::size_t gateway, double t_s) noexcept {
  return element_down(schedule, ElementKind::kGateway, gateway, t_s);
}

bool link_failed(const FailureSchedule& schedule, const topology::LinkPlan& plan, std::size_t link,
                 double t_s) {
  const topology::PlannedLink& planned = plan.links.at(link);
  return element_down(schedule, ElementKind::kLaserLink, link, t_s) ||
         satellite_down(schedule, planned.satellite_a, t_s) ||
         satellite_down(schedule, planned.satellite_b, t_s);
}

DownAt down_at(const FailureSchedule& schedule, const topology::LinkPlan& plan,
               std::size_t satellite_count, std::size_t gateway_count, double t_s) {
  DownAt down{.satellites = std::vector<bool>(satellite_count, false),
              .links = std::vector<bool>(plan.links.size(), false),
              .gateways = std::vector<bool>(gateway_count, false)};
  for (const Failure& f : schedule.failures) {
    if (!in_force(f, t_s)) {
      continue;
    }
    switch (f.kind) {
      case ElementKind::kSatellite:
        down.satellites.at(f.element) = true;
        break;
      case ElementKind::kLaserLink:
        down.links.at(f.element) = true;
        break;
      case ElementKind::kGateway:
        down.gateways.at(f.element) = true;
        break;
    }
  }
  for (std::size_t l = 0; l < plan.links.size(); ++l) {
    const topology::PlannedLink& link = plan.links[l];
    if (down.satellites.at(link.satellite_a) || down.satellites.at(link.satellite_b)) {
      down.links[l] = true;
    }
  }
  return down;
}

std::vector<Transition> transitions(const FailureSchedule& schedule) {
  // Ordered by instant; within one, by failure index (each failure is added once per instant,
  // starts and ends in failure order, then sorted).
  std::map<double, std::vector<Change>> by_instant;
  for (std::size_t i = 0; i < schedule.failures.size(); ++i) {
    const Failure& f = schedule.failures[i];
    by_instant[f.start_s].push_back(Change{.failure = i, .begins = true});
    if (const std::optional<double> end = end_s(f)) {
      by_instant[*end].push_back(Change{.failure = i, .begins = false});
    }
  }
  std::vector<Transition> result;
  result.reserve(by_instant.size());
  for (auto& [t_s, changes] : by_instant) {
    std::ranges::sort(changes, [](const Change& a, const Change& b) {
      return a.failure != b.failure ? a.failure < b.failure : !a.begins && b.begins;
    });
    result.push_back(Transition{.t_s = t_s, .changes = std::move(changes)});
  }
  return result;
}

std::vector<std::size_t> links_of(const topology::LinkPlan& plan, std::size_t satellite) {
  std::vector<std::size_t> links;
  for (std::size_t l = 0; l < plan.links.size(); ++l) {
    if (plan.links[l].satellite_a == satellite || plan.links[l].satellite_b == satellite) {
      links.push_back(l);
    }
  }
  return links;
}

std::vector<std::size_t> links_taken_down(const Failure& failure, const topology::LinkPlan& plan) {
  switch (failure.kind) {
    case ElementKind::kSatellite:
      return links_of(plan, failure.element);
    case ElementKind::kLaserLink:
      return {failure.element};
    case ElementKind::kGateway:
      return {};
  }
  return {};
}

}  // namespace satsim::failure
