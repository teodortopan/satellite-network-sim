#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <string_view>

#include "satsim/time/utc_time.hpp"

namespace satsim::propagate {

/// The CSV columns, in order. Every name carries its frame and unit:
///  - utc: the time, YYYY-MM-DDTHH:MM:SS[.f...] (format_utc_iso), UTC;
///  - norad_cat_id, object_name (CSV-quoted when needed), element_source ("supplemental" or
///    "standard"), element_age_s (utc minus the element set's epoch);
///  - teme_*: SGP4's output frame, position in km and velocity in km/s;
///  - ecef_*: Earth-fixed ITRF position in km (GMST at UT1, then polar motion);
///  - geodetic_*: WGS-84 latitude and east longitude in degrees, height above the ellipsoid
///    in km;
///  - j2000_*: J2000 (FK5, IAU 1976/1980) position in km and velocity in km/s.
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

/// Appends the shortest decimal text that reads back as exactly `value` (std::to_chars, so
/// independent of the C locale), e.g. "6928.137" or "1e-07".
void append_double(std::string& out, double value);

/// Appends an integer in decimal.
void append_integer(std::string& out, std::int64_t value);

/// Appends `text` as one CSV field (RFC 4180): unchanged unless it holds a comma, a double
/// quote, a carriage return or a line feed, in which case it is enclosed in double quotes
/// and every double quote inside is doubled.
void append_csv_field(std::string& out, std::string_view text);

/// The number of fraction digits (0 to 9) that writes every time start + i * step exactly:
/// the fewest digits for which both start's fraction of a second and step_ns are whole
/// multiples of the last digit's unit.
[[nodiscard]] int fraction_digits_for(const time::UtcTime& start, std::int64_t step_ns) noexcept;

/// `utc` as YYYY-MM-DDTHH:MM:SS with `fraction_digits` (0 to 9) digits after a '.' (none for
/// 0). Digits beyond `fraction_digits` are dropped, so callers pick enough of them
/// (fraction_digits_for). The form parse_iso_civil_time reads.
[[nodiscard]] std::string format_utc_iso(const time::UtcTime& utc, int fraction_digits);

}  // namespace satsim::propagate
