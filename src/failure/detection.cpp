#include "satsim/failure/detection.hpp"

#include <cstddef>
#include <vector>

#include "satsim/failure/schedule.hpp"
#include "satsim/topology/link_plan.hpp"

namespace satsim::failure {

std::vector<Detector> possible_detectors(const Failure& failure, const topology::LinkPlan& plan) {
  switch (failure.kind) {
    case ElementKind::kLaserLink: {
      const topology::PlannedLink& link = plan.links.at(failure.element);
      return {Detector{.satellite = link.satellite_a, .link = failure.element},
              Detector{.satellite = link.satellite_b, .link = failure.element}};
    }
    case ElementKind::kSatellite: {
      std::vector<Detector> detectors;
      for (const std::size_t l : links_of(plan, failure.element)) {
        const topology::PlannedLink& link = plan.links[l];
        const std::size_t other =
            link.satellite_a == failure.element ? link.satellite_b : link.satellite_a;
        detectors.push_back(Detector{.satellite = other, .link = l});
      }
      return detectors;
    }
    case ElementKind::kGateway:
      return {};
  }
  return {};
}

std::vector<Detector> working_detectors(const Failure& failure, const FailureSchedule& schedule,
                                        const topology::LinkPlan& plan, double t_s) {
  std::vector<Detector> working;
  for (const Detector& d : possible_detectors(failure, plan)) {
    if (!satellite_down(schedule, d.satellite, t_s)) {
      working.push_back(d);
    }
  }
  return working;
}

}  // namespace satsim::failure
