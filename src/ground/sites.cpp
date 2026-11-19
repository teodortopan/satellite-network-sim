#include "satsim/ground/sites.hpp"

#include <utility>

#include "satsim/core/angles.hpp"
#include "satsim/frames/geodetic.hpp"

namespace satsim::ground {

GroundSite make_ground_site(std::string name, double latitude_deg, double longitude_deg) {
  GroundSite site;
  site.name = std::move(name);
  site.latitude_rad = deg_to_rad(latitude_deg);
  site.longitude_rad = deg_to_rad(longitude_deg);
  frames::Geodetic geodetic;
  geodetic.latitude_rad = site.latitude_rad;
  geodetic.longitude_rad = site.longitude_rad;
  geodetic.height_km = 0.0;
  site.position_ecef_km = frames::geodetic_to_ecef(geodetic);
  return site;
}

std::vector<Gateway> select_gateways(const std::vector<Gateway>& gateways,
                                     GatewaySelection selection) {
  std::vector<Gateway> chosen;
  for (const Gateway& gateway : gateways) {
    if (selection == GatewaySelection::kOperationalAndPlanned ||
        gateway.status == GatewayStatus::kOperational) {
      chosen.push_back(gateway);
    }
  }
  return chosen;
}

}  // namespace satsim::ground
