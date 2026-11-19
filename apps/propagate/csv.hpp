#pragma once

#include <array>
#include <string_view>

namespace satsim::propagate {

/// The CSV columns, in order. Every name carries its frame and unit:
///  - utc: the time, YYYY-MM-DDTHH:MM:SS[.f...] (app::format_utc_iso), UTC;
///  - norad_cat_id, object_name (CSV-quoted when needed), element_source ("supplemental" or
///    "standard"), element_age_s (utc minus the element set's epoch);
///  - teme_*: SGP4's output frame, position in km and velocity in km/s;
///  - ecef_*: Earth-fixed ITRF position in km (GMST at UT1, then polar motion);
///  - geodetic_*: WGS-84 latitude and east longitude in degrees, height above the ellipsoid
///    in km;
///  - j2000_*: J2000 (FK5, IAU 1976/1980) position in km and velocity in km/s.
/// The number and text helpers are in common/csv.hpp, shared with the other programs.
inline constexpr std::array<std::string_view, 23> kCsvColumns = {
    "utc",
    "norad_cat_id",
    "object_name",
    "element_source",
    "element_age_s",
    "teme_x_km",
    "teme_y_km",
    "teme_z_km",
    "teme_vx_km_s",
    "teme_vy_km_s",
    "teme_vz_km_s",
    "ecef_x_km",
    "ecef_y_km",
    "ecef_z_km",
    "geodetic_latitude_deg",
    "geodetic_longitude_deg",
    "geodetic_height_km",
    "j2000_x_km",
    "j2000_y_km",
    "j2000_z_km",
    "j2000_vx_km_s",
    "j2000_vy_km_s",
    "j2000_vz_km_s",
};

}  // namespace satsim::propagate
