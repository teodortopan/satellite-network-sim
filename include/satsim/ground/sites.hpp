#pragma once

#include <Eigen/Core>
#include <cstdint>
#include <string>
#include <vector>

#include "satsim/ground/address_block.hpp"

namespace satsim::ground {

/// A place on the ground: on the WGS-84 ellipsoid surface (height 0).
struct GroundSite {
  std::string name;
  double latitude_rad = 0.0;   ///< Geodetic latitude, [-pi/2, pi/2].
  double longitude_rad = 0.0;  ///< East longitude, [-pi, pi].
  /// Earth-fixed (ITRF) position, km, from frames::geodetic_to_ecef at height 0.
  Eigen::Vector3d position_ecef_km = Eigen::Vector3d::Zero();
};

/// Builds a site from geodetic degrees (WGS-84, height 0). Requires a finite latitude within
/// [-90, 90] and longitude within [-180, 180]; the loaders check this before calling.
[[nodiscard]] GroundSite make_ground_site(std::string name, double latitude_deg,
                                          double longitude_deg);

/// A city of the study (data/ground/cities.json).
struct City {
  GroundSite site;           ///< site.name is the city's name, unique in the file.
  std::string country;       ///< ISO 3166-1 alpha-2, as in SpaceX's geofeed (e.g. "US").
  std::string region;        ///< ISO 3166-2, as in SpaceX's geofeed (e.g. "US-NY").
  std::string geofeed_city;  ///< The city's spelling in SpaceX's geofeed ("New York City").
};

enum class PairRole : std::uint8_t {
  kStudy,       ///< A pair the study reports on.
  kValidation,  ///< A pair with a published measurement or result to compare against.
};

/// A pair of cities from cities.json. from and to are names of loaded cities.
struct CityPair {
  std::string from;
  std::string to;
  PairRole role = PairRole::kStudy;
  std::string why;  ///< Free text; empty if the file gives none.
};

enum class GatewayStatus : std::uint8_t { kOperational, kPlanned };

/// A gateway: a ground station that links satellites to the ground network.
struct Gateway {
  GroundSite site;  ///< site.name is the community dataset's name ("Akron, OH").
  GatewayStatus status = GatewayStatus::kOperational;
};

/// Which gateways a caller uses.
enum class GatewaySelection : std::uint8_t { kOperationalOnly, kOperationalAndPlanned };

/// The gateways chosen by selection, in input order.
[[nodiscard]] std::vector<Gateway> select_gateways(
    const std::vector<Gateway>& gateways,
    GatewaySelection selection = GatewaySelection::kOperationalOnly);

/// A point of presence (pop_codes.json): where a Starlink user's traffic enters the internet.
struct Pop {
  std::string code;                  ///< SpaceX's code ("lhr"), unique in the file.
  std::vector<std::string> routers;  ///< SpaceX's router names for this code ("lndngbr1").
  std::string city;
  std::string country;
  std::string coordinates_from;  ///< Where the coordinates come from (provenance text).
  GroundSite site;               ///< site.name is the city.
};

/// One row of starlink_pops.csv: an address block and the PoP that serves it.
struct PopBlock {
  AddressBlock block;
  std::string router;
  std::string pop_code;
};

/// One row of starlink_geofeed.csv (RFC 8805): where an address block's customers are.
struct GeofeedEntry {
  AddressBlock block;
  std::string country;  ///< ISO 3166-1 alpha-2; never empty.
  std::string region;   ///< ISO 3166-2; may be empty.
  std::string city;     ///< May be empty.
};

}  // namespace satsim::ground
