#include "world.hpp"

#include <optional>
#include <vector>

#include "failure/fixture.hpp"
#include "failures/timeline.hpp"
#include "inputs/inputs.hpp"
#include "satsim/failure/schedule.hpp"

namespace satsim::failures_app::testing {

World::World(app::EndpointModel model) {
  geometry.base.position_ecef_km.assign(22, std::nullopt);
  geometry.base.city_visible = {{seen(1, 500.0), seen(5, 550.0)}, {seen(16, 600.0)}};
  geometry.base.gateway_visible = {{seen(1, 600.0), seen(2, 700.0)},
                                   {seen(5, 800.0), seen(16, 900.0)}};
  geometry.set_length(0, 1, 1000.0);
  geometry.set_length(1, 2, 1500.0);
  geometry.set_length(0, 4, 2000.0);
  geometry.set_length(4, 5, 1000.0);
  geometry.set_length(1, 5, 1000.0);
  geometry.set_length(16, 17, 1000.0);
  settings = TimelineSettings{.times = {},
                              .model = model,
                              .hop_delay_s = kHopDelayS,
                              .step_s = 1.0,
                              .step_count = 100,
                              .gateway_count = 2,
                              .atmosphere_km = 80.0};
}

Timeline World::timeline(const std::vector<failure::Failure>& failures) {
  return build_timeline(failure::testing::small().classification, failure::testing::small_plan(),
                        failure::FailureSchedule{.failures = failures}, settings, geometry);
}

}  // namespace satsim::failures_app::testing
