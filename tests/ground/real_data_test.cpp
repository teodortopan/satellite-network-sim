// The ground model on the committed data in data/ground/: record counts, the home PoP of every
// study city, and the distances of the study pairs.

#include <gtest/gtest.h>

#include <cstddef>
#include <filesystem>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

#include "satsim/ground/distance.hpp"
#include "satsim/ground/home_pop.hpp"
#include "satsim/ground/loaders.hpp"

namespace {

using satsim::ground::CitiesLoadResult;
using satsim::ground::City;
using satsim::ground::CsvLoadResult;
using satsim::ground::Gateway;
using satsim::ground::GatewaySelection;
using satsim::ground::GeofeedEntry;
using satsim::ground::great_circle_distance_on_mean_sphere_km;
using satsim::ground::HomePop;
using satsim::ground::HomePopFinder;
using satsim::ground::HomePopLevel;
using satsim::ground::JsonLoadResult;
using satsim::ground::PairRole;
using satsim::ground::Pop;
using satsim::ground::PopBlock;

const std::filesystem::path kGround = "data/ground";
const std::filesystem::path kSnapshot = kGround / "2026-11-19";

struct GroundData {
  CitiesLoadResult cities = satsim::ground::load_cities(kGround / "cities.json");
  JsonLoadResult<Gateway> gateways = satsim::ground::load_gateways(kSnapshot / "gateways.json");
  JsonLoadResult<Pop> pops = satsim::ground::load_pop_codes(kSnapshot / "pop_codes.json");
  CsvLoadResult<PopBlock> pop_blocks =
      satsim::ground::load_starlink_pops(kSnapshot / "starlink_pops.csv");
  CsvLoadResult<GeofeedEntry> geofeed =
      satsim::ground::load_starlink_geofeed(kSnapshot / "starlink_geofeed.csv");
  HomePopFinder finder{geofeed.records, pop_blocks.records, pops.records};
};

// Loaded once: the geofeed is 13,103 lines and the tests only read it.
const GroundData& ground() {
  static const GroundData data;
  return data;
}

const City& city(const std::string& name) {
  for (const City& c : ground().cities.cities) {
    if (c.site.name == name) {
      return c;
    }
  }
  throw std::runtime_error("no city " + name);
}

std::map<std::string, std::size_t> blocks_by_code(const HomePop& home) {
  std::map<std::string, std::size_t> blocks;
  for (const auto& candidate : home.candidates) {
    blocks[candidate.code] = candidate.blocks;
  }
  return blocks;
}

TEST(RealGroundDataTest, EveryFileLoadsWithoutErrors) {
  const auto& cities = ground().cities;
  const auto& gateways = ground().gateways;
  const auto& pops = ground().pops;
  const auto& pop_blocks = ground().pop_blocks;
  const auto& geofeed = ground().geofeed;
  ASSERT_FALSE(cities.file_error.has_value()) << cities.file_error.value_or("");
  EXPECT_TRUE(cities.city_errors.empty());
  EXPECT_TRUE(cities.pair_errors.empty());
  EXPECT_EQ(ground().cities.cities.size(), 14U);
  EXPECT_EQ(cities.pairs.size(), 8U);

  ASSERT_FALSE(gateways.file_error.has_value()) << gateways.file_error.value_or("");
  EXPECT_TRUE(gateways.record_errors.empty());
  EXPECT_EQ(gateways.records.size(), 208U);
  EXPECT_EQ(satsim::ground::select_gateways(gateways.records).size(), 196U);
  EXPECT_EQ(
      satsim::ground::select_gateways(gateways.records, GatewaySelection::kOperationalAndPlanned)
          .size(),
      208U);

  ASSERT_FALSE(pops.file_error.has_value()) << pops.file_error.value_or("");
  EXPECT_TRUE(pops.record_errors.empty());
  EXPECT_EQ(pops.records.size(), 65U);

  ASSERT_FALSE(pop_blocks.file_error.has_value()) << pop_blocks.file_error.value_or("");
  EXPECT_TRUE(pop_blocks.line_errors.empty())
      << pop_blocks.line_errors.size() << " errors, first: "
      << (pop_blocks.line_errors.empty() ? "" : pop_blocks.line_errors[0].message);
  EXPECT_EQ(pop_blocks.records.size(), 4122U);

  ASSERT_FALSE(geofeed.file_error.has_value()) << geofeed.file_error.value_or("");
  EXPECT_TRUE(geofeed.line_errors.empty())
      << geofeed.line_errors.size()
      << " errors, first: " << (geofeed.line_errors.empty() ? "" : geofeed.line_errors[0].message);
  EXPECT_EQ(geofeed.records.size(), 13103U);
}

TEST(RealGroundDataTest, JoinCounts) {
  const auto& assignment = ground().finder.assignment();
  // staging2 (3 rows) is not a public PoP and is left out of pop_codes.json.
  EXPECT_EQ(assignment.ignored_pop_rows, 3U);
  EXPECT_EQ(assignment.ignored_codes, (std::vector<std::string>{"staging2"}));
  EXPECT_EQ(assignment.mapped_entries + assignment.unmapped_entries, 13103U);
  EXPECT_EQ(assignment.mapped_entries, 4119U);
  EXPECT_EQ(assignment.unmapped_entries, 8984U);
}

TEST(RealGroundDataTest, HomePopsOfTheStudyCities) {
  struct Expected {
    const char* city;
    const char* pop;  // Empty: no home PoP.
    HomePopLevel level;
  };
  const std::vector<Expected> expected = {
      {"New York", "lga", HomePopLevel::kCity},        {"London", "lhr", HomePopLevel::kCity},
      {"Tokyo", "nrt", HomePopLevel::kCity},           {"Singapore", "sin", HomePopLevel::kCity},
      {"Detroit", "ord", HomePopLevel::kCity},         {"Buenos Aires", "eze", HomePopLevel::kCity},
      {"Chicago", "ord", HomePopLevel::kCity},         {"Reykjavik", "lhr", HomePopLevel::kCity},
      {"San Francisco", "sjc", HomePopLevel::kRegion}, {"Frankfurt", "fra", HomePopLevel::kCountry},
      {"Oulu", "arn", HomePopLevel::kCountry},         {"Vladivostok", "", HomePopLevel::kNone},
      {"Paris", "mad", HomePopLevel::kCity},           {"Luanda", "jnb", HomePopLevel::kCity},
  };
  ASSERT_EQ(expected.size(), ground().cities.cities.size());
  for (const Expected& e : expected) {
    const HomePop home = ground().finder.find(city(e.city));
    EXPECT_EQ(home.level, e.level) << e.city;
    EXPECT_EQ(home.pop.has_value() ? home.pop->code : std::string(), e.pop) << e.city;
  }
}

TEST(RealGroundDataTest, CountsBehindTheCloseCalls) {
  // Paris: a near tie, decided by block count.
  const HomePop paris = ground().finder.find(city("Paris"));
  EXPECT_EQ(blocks_by_code(paris), (std::map<std::string, std::size_t>{
                                       {"mad", 20}, {"fra", 17}, {"mxp", 17}, {"lhr", 13}}));
  // fra and mxp tie at 17 blocks; fra is nearer.
  ASSERT_EQ(paris.candidates.size(), 4U);
  EXPECT_EQ(paris.candidates[1].code, "fra");
  EXPECT_EQ(paris.candidates[2].code, "mxp");

  // Luanda: 3 blocks each; Johannesburg is nearer than Nairobi.
  const HomePop luanda = ground().finder.find(city("Luanda"));
  EXPECT_EQ(blocks_by_code(luanda), (std::map<std::string, std::size_t>{{"jnb", 3}, {"nbo", 3}}));
  ASSERT_EQ(luanda.candidates.size(), 2U);
  EXPECT_LT(luanda.candidates[0].distance_km, luanda.candidates[1].distance_km);

  // San Francisco: region US-CA, where lax has more blocks but sjc is nearer.
  const HomePop san_francisco = ground().finder.find(city("San Francisco"));
  const auto sf_blocks = blocks_by_code(san_francisco);
  ASSERT_TRUE(sf_blocks.count("lax") == 1 && sf_blocks.count("sjc") == 1);
  EXPECT_GT(sf_blocks.at("lax"), sf_blocks.at("sjc"));

  // Oulu: no Oulu or FI-14 row maps; Finland's rows map to fra and arn.
  const HomePop oulu = ground().finder.find(city("Oulu"));
  EXPECT_EQ(blocks_by_code(oulu), (std::map<std::string, std::size_t>{{"arn", 3}, {"fra", 3}}));
}

TEST(RealGroundDataTest, StudyPairDistances) {
  const std::map<std::string, double> expected_km = {
      {"New York-London", 5570.0},      {"San Francisco-Tokyo", 8275.0},
      {"Frankfurt-Singapore", 10258.0}, {"Detroit-Buenos Aires", 8916.0},
      {"New York-Chicago", 1144.0},     {"Reykjavik-Vladivostok", 7869.0},
  };
  std::size_t study_pairs = 0;
  for (const auto& pair : ground().cities.pairs) {
    if (pair.role != PairRole::kStudy) {
      continue;
    }
    ++study_pairs;
    const std::string key = pair.from + "-" + pair.to;
    ASSERT_EQ(expected_km.count(key), 1U) << key;
    EXPECT_NEAR(great_circle_distance_on_mean_sphere_km(city(pair.from).site, city(pair.to).site),
                expected_km.at(key), 1.0)
        << key;
  }
  EXPECT_EQ(study_pairs, expected_km.size());
}

}  // namespace
