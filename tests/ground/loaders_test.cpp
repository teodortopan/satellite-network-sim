#include "satsim/ground/loaders.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <filesystem>
#include <sstream>
#include <string>
#include <type_traits>
#include <vector>

#include "satsim/core/angles.hpp"

namespace {

using satsim::deg_to_rad;
using satsim::ground::AddressFamily;
using satsim::ground::GatewayStatus;
using satsim::ground::LineError;
using satsim::ground::load_cities;
using satsim::ground::load_gateways;
using satsim::ground::load_pop_codes;
using satsim::ground::load_starlink_geofeed;
using satsim::ground::load_starlink_pops;
using satsim::ground::PairRole;
using satsim::ground::RecordError;

const std::filesystem::path kData = "tests/ground/data";

bool contains(const std::string& text, const std::string& part) {
  return text.find(part) != std::string::npos;
}

// The error at index (JSON) or line (CSV), or a failure if there is none.
const RecordError* record_error(const std::vector<RecordError>& errors, std::size_t index) {
  for (const RecordError& error : errors) {
    if (error.index == index) {
      return &error;
    }
  }
  ADD_FAILURE() << "no error at index " << index;
  return nullptr;
}

std::string message_at(const std::vector<RecordError>& errors, std::size_t index) {
  const RecordError* error = record_error(errors, index);
  return error == nullptr ? std::string() : error->message;
}

std::string message_at(const std::vector<LineError>& errors, std::size_t line) {
  for (const LineError& error : errors) {
    if (error.line == line) {
      return error.message;
    }
  }
  ADD_FAILURE() << "no error on line " << line;
  return {};
}

template <typename Error>
std::vector<std::size_t> positions(const std::vector<Error>& errors) {
  std::vector<std::size_t> result;
  for (const Error& error : errors) {
    if constexpr (std::is_same_v<Error, LineError>) {
      result.push_back(error.line);
    } else {
      result.push_back(error.index);
    }
  }
  return result;
}

// ---- Whole-file failures, the same for every loader.

TEST(GroundLoadersTest, MissingFileIsAFileErrorNamingThePath) {
  const auto path = kData / "no_such_file.json";
  const auto cities = load_cities(path);
  ASSERT_TRUE(cities.file_error.has_value());
  EXPECT_TRUE(contains(cities.file_error.value_or(""), "cannot open"));
  EXPECT_TRUE(contains(cities.file_error.value_or(""), path.string()));
  EXPECT_TRUE(cities.cities.empty());
  EXPECT_TRUE(load_gateways(path).file_error.has_value());
  EXPECT_TRUE(load_pop_codes(path).file_error.has_value());
  EXPECT_TRUE(load_starlink_pops(path).file_error.has_value());
  EXPECT_TRUE(load_starlink_geofeed(path).file_error.has_value());
}

TEST(GroundLoadersTest, DirectoryIsAFileError) {
  const auto result = load_starlink_pops(kData);
  ASSERT_TRUE(result.file_error.has_value());
  EXPECT_TRUE(contains(result.file_error.value_or(""), "is a directory"));
  EXPECT_TRUE(load_gateways(kData).file_error.has_value());
}

TEST(GroundLoadersTest, InvalidJsonIsAFileErrorNamingThePath) {
  const auto path = kData / "not_json.json";
  for (const auto& error : {load_cities(path).file_error, load_gateways(path).file_error,
                            load_pop_codes(path).file_error}) {
    ASSERT_TRUE(error.has_value());
    EXPECT_TRUE(contains(error.value_or(""), "not valid JSON")) << error.value_or("");
    EXPECT_TRUE(contains(error.value_or(""), path.string())) << error.value_or("");
  }
}

TEST(GroundLoadersTest, WrongTopLevelShapeIsAFileError) {
  const auto cities = load_cities(kData / "top_level_array.json");
  ASSERT_TRUE(cities.file_error.has_value());
  EXPECT_TRUE(contains(cities.file_error.value_or(""), "expected an object"));

  const auto no_pairs = load_cities(kData / "cities_no_pairs.json");
  ASSERT_TRUE(no_pairs.file_error.has_value());
  EXPECT_TRUE(contains(no_pairs.file_error.value_or(""), "\"pairs\" missing"));

  const auto not_array = load_cities(kData / "cities_not_array.json");
  ASSERT_TRUE(not_array.file_error.has_value());
  EXPECT_TRUE(
      contains(not_array.file_error.value_or(""), "\"cities\" is an object, expected an array"));

  const auto gateways = load_gateways(kData / "top_level_object.json");
  ASSERT_TRUE(gateways.file_error.has_value());
  EXPECT_TRUE(contains(gateways.file_error.value_or(""), "expected an array"));
  EXPECT_TRUE(load_pop_codes(kData / "top_level_object.json").file_error.has_value());
}

TEST(GroundLoadersTest, EmptyCsvIsAFileError) {
  const auto pops = load_starlink_pops(kData / "empty.csv");
  ASSERT_TRUE(pops.file_error.has_value());
  EXPECT_TRUE(contains(pops.file_error.value_or(""), "empty"));
  EXPECT_TRUE(load_starlink_geofeed(kData / "empty.csv").file_error.has_value());
}

TEST(GroundLoadersTest, UnreadableStreamIsAFileError) {
  std::istringstream in("[]");
  in.setstate(std::ios::failbit);
  EXPECT_TRUE(load_gateways(in).file_error.has_value());
  std::istringstream csv("1.0.0.0/8,r,c\n");
  csv.setstate(std::ios::failbit);
  EXPECT_TRUE(load_starlink_pops(csv).file_error.has_value());
}

TEST(GroundLoadersTest, EmptyJsonArraysLoadNothing) {
  std::istringstream cities(R"({"cities": [], "pairs": []})");
  const auto result = load_cities(cities);
  EXPECT_FALSE(result.file_error.has_value());
  EXPECT_TRUE(result.cities.empty());
  EXPECT_TRUE(result.pairs.empty());
  std::istringstream gateways("[]");
  const auto g = load_gateways(gateways);
  EXPECT_FALSE(g.file_error.has_value());
  EXPECT_TRUE(g.records.empty());
}

// ---- cities.json

TEST(CitiesLoaderTest, KeepsGoodCitiesAndReportsEachBadOne) {
  const auto result = load_cities(kData / "cities_mixed.json");
  ASSERT_FALSE(result.file_error.has_value()) << result.file_error.value_or("");

  ASSERT_EQ(result.cities.size(), 3U);
  EXPECT_EQ(result.cities[0].site.name, "New York");
  EXPECT_EQ(result.cities[0].country, "US");
  EXPECT_EQ(result.cities[0].region, "US-NY");
  EXPECT_EQ(result.cities[0].geofeed_city, "New York City");
  EXPECT_DOUBLE_EQ(result.cities[0].site.latitude_rad, deg_to_rad(40.7128));
  EXPECT_DOUBLE_EQ(result.cities[0].site.longitude_rad, deg_to_rad(-74.006));
  EXPECT_GT(result.cities[0].site.position_ecef_km.norm(), 6300.0);
  EXPECT_EQ(result.cities[1].site.name, "London");
  // The limits themselves are allowed.
  EXPECT_EQ(result.cities[2].site.name, "Edges");
  EXPECT_DOUBLE_EQ(result.cities[2].site.latitude_rad, deg_to_rad(-90.0));
  EXPECT_DOUBLE_EQ(result.cities[2].site.longitude_rad, deg_to_rad(180.0));

  const auto& errors = result.city_errors;
  EXPECT_EQ(positions(errors), (std::vector<std::size_t>{2, 3, 4, 5, 6, 7, 8, 9, 11}));
  // Both copies of a duplicated name are rejected, each naming the other.
  EXPECT_TRUE(contains(message_at(errors, 2), "name \"Twice\" is also at index 9"));
  EXPECT_TRUE(contains(message_at(errors, 9), "name \"Twice\" is also at index 2"));
  EXPECT_EQ(record_error(errors, 9)->name, "Twice");
  EXPECT_EQ(message_at(errors, 3), "name missing");
  EXPECT_EQ(message_at(errors, 4), "country is empty");
  EXPECT_EQ(record_error(errors, 4)->name, "Empty country");
  EXPECT_EQ(message_at(errors, 5), "lat = 90.5 is outside [-90, 90] degrees");
  EXPECT_EQ(message_at(errors, 6), "lon = 180.25 is outside [-180, 180] degrees");
  EXPECT_EQ(message_at(errors, 7), "lat is a string, expected a number");
  EXPECT_EQ(message_at(errors, 8), "record is a string, expected an object");
  // Every problem of a record is reported.
  EXPECT_EQ(message_at(errors, 11),
            "name is a number, expected a string; country missing; region is a number, "
            "expected a string; lon missing");
}

TEST(CitiesLoaderTest, KeepsGoodPairsAndReportsEachBadOne) {
  const auto result = load_cities(kData / "cities_mixed.json");
  ASSERT_FALSE(result.file_error.has_value());

  ASSERT_EQ(result.pairs.size(), 2U);
  EXPECT_EQ(result.pairs[0].from, "New York");
  EXPECT_EQ(result.pairs[0].to, "London");
  EXPECT_EQ(result.pairs[0].role, PairRole::kStudy);
  EXPECT_EQ(result.pairs[0].why, "transatlantic");
  EXPECT_EQ(result.pairs[1].from, "London");
  EXPECT_EQ(result.pairs[1].to, "Edges");
  EXPECT_EQ(result.pairs[1].role, PairRole::kValidation);
  EXPECT_EQ(result.pairs[1].why, "");

  const auto& errors = result.pair_errors;
  EXPECT_EQ(positions(errors), (std::vector<std::size_t>{2, 3, 4, 5, 6, 7}));
  EXPECT_EQ(message_at(errors, 2), "city \"Nowhere\" is not a loaded city");
  // A city rejected as a duplicate is not loaded either.
  EXPECT_EQ(message_at(errors, 3), "city \"Twice\" is not a loaded city");
  EXPECT_EQ(message_at(errors, 4), "from and to are the same city");
  EXPECT_EQ(message_at(errors, 5), "role \"guess\" is not \"study\" or \"validation\"");
  EXPECT_EQ(message_at(errors, 6), "to missing");
  EXPECT_EQ(message_at(errors, 7), "record is an array, expected an object");
}

// ---- gateways.json

TEST(GatewaysLoaderTest, KeepsGoodGatewaysAndReportsEachBadOne) {
  const auto result = load_gateways(kData / "gateways_mixed.json");
  ASSERT_FALSE(result.file_error.has_value()) << result.file_error.value_or("");

  ASSERT_EQ(result.records.size(), 3U);
  EXPECT_EQ(result.records[0].site.name, "Adelanto, CA");
  EXPECT_EQ(result.records[0].status, GatewayStatus::kOperational);
  EXPECT_DOUBLE_EQ(result.records[0].site.latitude_rad, deg_to_rad(34.6024));
  EXPECT_EQ(result.records[1].site.name, "Cape Canaveral, FL");
  EXPECT_EQ(result.records[1].status, GatewayStatus::kPlanned);
  EXPECT_EQ(result.records[2].site.name, "Akron, OH");

  const auto& errors = result.record_errors;
  EXPECT_EQ(positions(errors), (std::vector<std::size_t>{2, 3, 4, 5, 6, 7, 8}));
  EXPECT_EQ(message_at(errors, 2), "status \"retired\" is not \"operational\" or \"planned\"");
  EXPECT_EQ(record_error(errors, 2)->name, "Retired, XX");
  EXPECT_EQ(message_at(errors, 3), "name is empty");
  EXPECT_EQ(message_at(errors, 4), "status missing");
  EXPECT_EQ(message_at(errors, 5), "lat = -91 is outside [-90, 90] degrees");
  EXPECT_EQ(message_at(errors, 6), "lon = -181 is outside [-180, 180] degrees");
  EXPECT_EQ(message_at(errors, 7), "lon is null, expected a number");
  EXPECT_EQ(message_at(errors, 8), "record is a number, expected an object");
}

// ---- pop_codes.json

TEST(PopCodesLoaderTest, KeepsGoodPopsAndReportsEachBadOne) {
  const auto result = load_pop_codes(kData / "pop_codes_mixed.json");
  ASSERT_FALSE(result.file_error.has_value()) << result.file_error.value_or("");

  ASSERT_EQ(result.records.size(), 2U);
  const auto& lhr = result.records[0];
  EXPECT_EQ(lhr.code, "lhr");
  EXPECT_EQ(lhr.routers, (std::vector<std::string>{"lndngbr1"}));
  EXPECT_EQ(lhr.city, "London");
  EXPECT_EQ(lhr.country, "GB");
  EXPECT_EQ(lhr.coordinates_from, "community dataset (pop_locations.json)");
  EXPECT_EQ(lhr.site.name, "London");
  EXPECT_DOUBLE_EQ(lhr.site.latitude_rad, deg_to_rad(51.5074));
  EXPECT_EQ(result.records[1].code, "fra");
  EXPECT_EQ(result.records[1].routers, (std::vector<std::string>{"frntdeu1", "frntdeu2"}));

  const auto& errors = result.record_errors;
  EXPECT_EQ(positions(errors), (std::vector<std::size_t>{1, 2, 3, 4, 5, 6, 7, 8, 9}));
  EXPECT_EQ(message_at(errors, 1), "code \"dup\" is also at index 8");
  EXPECT_EQ(message_at(errors, 8), "code \"dup\" is also at index 1");
  EXPECT_EQ(message_at(errors, 2), "code is empty");
  EXPECT_EQ(message_at(errors, 3), "routers is an array, expected a non-empty array");
  EXPECT_EQ(message_at(errors, 4), "routers has an entry that is not a non-empty string");
  EXPECT_EQ(message_at(errors, 5), "routers has an entry that is not a non-empty string");
  EXPECT_EQ(message_at(errors, 6), "routers is a string, expected a non-empty array");
  EXPECT_EQ(message_at(errors, 7), "coordinates_from missing");
  EXPECT_EQ(record_error(errors, 7)->name, "nof");
  EXPECT_EQ(message_at(errors, 9), "lat = 100 is outside [-90, 90] degrees");
}

// ---- starlink_pops.csv

TEST(StarlinkPopsLoaderTest, KeepsGoodLinesAndReportsEachBadOne) {
  const auto result = load_starlink_pops(kData / "starlink_pops_mixed.csv");
  ASSERT_FALSE(result.file_error.has_value()) << result.file_error.value_or("");

  ASSERT_EQ(result.records.size(), 4U);
  EXPECT_EQ(result.records[0].block.to_string(), "9.170.0.0/24");
  EXPECT_EQ(result.records[0].router, "lsancax1");
  EXPECT_EQ(result.records[0].pop_code, "lax");
  // The CRLF line: the '\r' is not part of the code.
  EXPECT_EQ(result.records[1].block.to_string(), "2401:e320::/38");
  EXPECT_EQ(result.records[1].block.family(), AddressFamily::kIpv6);
  EXPECT_EQ(result.records[1].pop_code, "bth");
  EXPECT_EQ(result.records[2].block.to_string(), "9.170.8.0/24");
  EXPECT_EQ(result.records[3].block.to_string(), "2401:e320:400::/38");
  EXPECT_EQ(result.records[3].pop_code, "jkt");

  const auto& errors = result.line_errors;
  EXPECT_EQ(positions(errors), (std::vector<std::size_t>{3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 15}));
  EXPECT_EQ(message_at(errors, 3), "has 2 fields, expected 3");
  EXPECT_EQ(message_at(errors, 4), "has 4 fields, expected 3");
  EXPECT_TRUE(contains(message_at(errors, 5), "prefix \"not-a-prefix\""));
  EXPECT_TRUE(contains(message_at(errors, 6), "bits set after the prefix"));
  EXPECT_TRUE(contains(message_at(errors, 6), "9.170.3.0/24"));
  EXPECT_EQ(message_at(errors, 7), "router is empty");
  EXPECT_EQ(message_at(errors, 8), "PoP code is empty");
  EXPECT_EQ(message_at(errors, 9), "empty line");
  EXPECT_EQ(message_at(errors, 10), "quoted fields are not supported");
  EXPECT_EQ(message_at(errors, 11), "block 9.170.7.0/24 is also at line 12");
  EXPECT_EQ(message_at(errors, 12), "block 9.170.7.0/24 is also at line 11");
  // Every problem of a line is reported.
  const std::string all = message_at(errors, 15);
  EXPECT_TRUE(contains(all, "prefix \"\" has no '/'"));
  EXPECT_TRUE(contains(all, "; router is empty; PoP code is empty"));
}

// ---- starlink_geofeed.csv

TEST(StarlinkGeofeedLoaderTest, KeepsGoodLinesAndReportsEachBadOne) {
  const auto result = load_starlink_geofeed(kData / "starlink_geofeed_mixed.csv");
  ASSERT_FALSE(result.file_error.has_value()) << result.file_error.value_or("");

  ASSERT_EQ(result.records.size(), 3U);
  EXPECT_EQ(result.records[0].block.to_string(), "2605:59c7:a000:503::/64");
  EXPECT_EQ(result.records[0].country, "US");
  EXPECT_EQ(result.records[0].region, "US-HI");
  EXPECT_EQ(result.records[0].city, "");
  // Four fields, empty region, CRLF.
  EXPECT_EQ(result.records[1].block.to_string(), "9.161.66.0/24");
  EXPECT_EQ(result.records[1].region, "");
  EXPECT_EQ(result.records[1].city, "Meliadine");
  // A postal code is read and ignored; the last line has no newline.
  EXPECT_EQ(result.records[2].country, "DE");
  EXPECT_EQ(result.records[2].city, "Frankfurt");

  const auto& errors = result.line_errors;
  EXPECT_EQ(positions(errors), (std::vector<std::size_t>{1, 4, 5, 6, 7, 8, 9, 10, 11, 12}));
  EXPECT_EQ(message_at(errors, 1), "block 9.161.65.0/24 is also at line 12");
  EXPECT_EQ(message_at(errors, 12), "block 9.161.65.0/24 is also at line 1");
  EXPECT_EQ(message_at(errors, 4), "has 3 fields, expected 4 or 5");
  EXPECT_EQ(message_at(errors, 5), "has 6 fields, expected 4 or 5");
  EXPECT_EQ(message_at(errors, 6), "country \"ca\" is not two upper-case letters");
  EXPECT_EQ(message_at(errors, 7), "country \"\" is not two upper-case letters");
  EXPECT_EQ(message_at(errors, 8), "country \"CAN\" is not two upper-case letters");
  EXPECT_TRUE(contains(message_at(errors, 9), "bits set after the prefix"));
  EXPECT_EQ(message_at(errors, 10), "empty line");
  EXPECT_EQ(message_at(errors, 11), "quoted fields are not supported");
}

TEST(StarlinkGeofeedLoaderTest, ReadsFromAStream) {
  std::istringstream in("10.0.0.0/8,AR,AR-C,Buenos Aires,\n::/0,AR,,,\n");
  const auto result = load_starlink_geofeed(in);
  ASSERT_FALSE(result.file_error.has_value());
  EXPECT_TRUE(result.line_errors.empty());
  ASSERT_EQ(result.records.size(), 2U);
  EXPECT_EQ(result.records[0].city, "Buenos Aires");
  EXPECT_EQ(result.records[1].block.prefix_length(), 0);
}

}  // namespace
