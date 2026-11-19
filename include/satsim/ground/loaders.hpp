#pragma once

#include <cstddef>
#include <filesystem>
#include <iosfwd>
#include <optional>
#include <string>
#include <vector>

#include "satsim/ground/sites.hpp"

// Loaders for the ground data in data/ground/ (see its README). Every loader works the same
// way: a problem with the input as a whole (unreadable, not JSON, wrong top-level shape) is a
// file_error and nothing is loaded; a bad record is skipped and reported with its position and
// every problem found in it, and the good records are kept in file order. Nothing is
// defaulted. Loaders never throw. The path overloads prefix file_error with the path.

namespace satsim::ground {

/// Why one record of a JSON array was rejected.
struct RecordError {
  std::size_t index = 0;  ///< Position in its JSON array, from 0.
  std::string name;       ///< The record's name or code if it could be read, else empty.
  std::string message;    ///< Every problem found, joined by "; ".
};

/// Why one line of a CSV file was rejected.
struct LineError {
  std::size_t line = 0;  ///< Line number, from 1.
  std::string message;   ///< Every problem found, joined by "; ".
};

template <typename T>
struct JsonLoadResult {
  std::vector<T> records;
  std::vector<RecordError> record_errors;
  std::optional<std::string> file_error;  ///< When set, the vectors are empty.
};

template <typename T>
struct CsvLoadResult {
  std::vector<T> records;
  std::vector<LineError> line_errors;
  std::optional<std::string> file_error;  ///< When set, the vectors are empty.
};

/// cities.json: an object with arrays "cities" and "pairs" (other keys are ignored).
struct CitiesLoadResult {
  std::vector<City> cities;
  std::vector<CityPair> pairs;
  std::vector<RecordError> city_errors;   ///< index is the position in "cities".
  std::vector<RecordError> pair_errors;   ///< index is the position in "pairs".
  std::optional<std::string> file_error;  ///< When set, the vectors are empty.
};

/// A city needs non-empty strings name, country, region and geofeed_city, and numbers lat in
/// [-90, 90] and lon in [-180, 180] (degrees). Every city whose name appears more than once is
/// rejected (no copy is kept, since which one is meant is unknown). A pair needs strings from
/// and to naming two different loaded cities, and role "study" or "validation"; why is an
/// optional string.
[[nodiscard]] CitiesLoadResult load_cities(std::istream& in) noexcept;
[[nodiscard]] CitiesLoadResult load_cities(const std::filesystem::path& path) noexcept;

/// gateways.json: an array of {name, lat, lon, status}. name is a non-empty string, lat and lon
/// as for cities, status "operational" or "planned". All gateways are loaded; choose with
/// select_gateways.
[[nodiscard]] JsonLoadResult<Gateway> load_gateways(std::istream& in) noexcept;
[[nodiscard]] JsonLoadResult<Gateway> load_gateways(const std::filesystem::path& path) noexcept;

/// pop_codes.json: an array of {code, routers, city, country, lat, lon, coordinates_from}.
/// code, city, country and coordinates_from are non-empty strings, routers a non-empty array
/// of non-empty strings, lat and lon as for cities. Every PoP whose code appears more than
/// once is rejected.
[[nodiscard]] JsonLoadResult<Pop> load_pop_codes(std::istream& in) noexcept;
[[nodiscard]] JsonLoadResult<Pop> load_pop_codes(const std::filesystem::path& path) noexcept;

/// starlink_pops.csv: no header, lines "prefix,router,pop_code" (exactly 3 fields). prefix
/// must parse with parse_address_block; router and pop_code must be non-empty. Every line
/// whose block appears on another line is rejected. Quoted fields are not supported (a '"'
/// rejects the line). A '\r' before the '\n' is accepted; an empty line is an error, except
/// after the last newline.
[[nodiscard]] CsvLoadResult<PopBlock> load_starlink_pops(std::istream& in) noexcept;
[[nodiscard]] CsvLoadResult<PopBlock> load_starlink_pops(
    const std::filesystem::path& path) noexcept;

/// starlink_geofeed.csv (RFC 8805): no header, lines "prefix,country,region,city" with an
/// optional fifth field (postal code, deprecated by RFC 8805, read and ignored). country must
/// be two upper-case ASCII letters; region and city may be empty. Duplicate blocks, quotes and
/// line endings as for load_starlink_pops.
[[nodiscard]] CsvLoadResult<GeofeedEntry> load_starlink_geofeed(std::istream& in) noexcept;
[[nodiscard]] CsvLoadResult<GeofeedEntry> load_starlink_geofeed(
    const std::filesystem::path& path) noexcept;

}  // namespace satsim::ground
