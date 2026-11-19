#include "satsim/ground/home_pop.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include "satsim/core/angles.hpp"
#include "satsim/ground/address_block.hpp"
#include "satsim/ground/distance.hpp"

namespace {

using satsim::deg_to_rad;
using satsim::ground::City;
using satsim::ground::GeofeedEntry;
using satsim::ground::HomePop;
using satsim::ground::HomePopFinder;
using satsim::ground::HomePopLevel;
using satsim::ground::kEarthMeanRadiusKm;
using satsim::ground::make_ground_site;
using satsim::ground::parse_address_block;
using satsim::ground::Pop;
using satsim::ground::PopBlock;

satsim::ground::AddressBlock block(const std::string& text) {
  const auto parsed = parse_address_block(text);
  if (!parsed.block) {
    throw std::invalid_argument(text + ": " + parsed.error);
  }
  return *parsed.block;
}

// Every site of this fixture is on the equator, so distances are whole degrees of arc.
double degrees_km(double degrees) { return kEarthMeanRadiusKm * deg_to_rad(degrees); }

Pop pop(const std::string& code, double lon) {
  Pop p;
  p.code = code;
  p.routers = {code + "r1"};
  p.city = code;
  p.country = "XX";
  p.coordinates_from = "test";
  p.site = make_ground_site(code, 0.0, lon);
  return p;
}

City city(const std::string& name, const std::string& country, const std::string& region,
          double lon) {
  City c;
  c.site = make_ground_site(name, 0.0, lon);
  c.country = country;
  c.region = region;
  c.geofeed_city = name;
  return c;
}

GeofeedEntry entry(const std::string& prefix, const std::string& country, const std::string& region,
                   const std::string& city_name) {
  return GeofeedEntry{
      .block = block(prefix), .country = country, .region = region, .city = city_name};
}

PopBlock pop_block(const std::string& prefix, const std::string& code) {
  return PopBlock{.block = block(prefix), .router = code + "r1", .pop_code = code};
}

// PoPs on the equator at longitudes aaa 0, bbb 10, ccc 20, ddd -10. Listed in reverse
// alphabetical order so that no result can come from the input order.
std::vector<Pop> fixture_pops() {
  return {pop("ddd", -10.0), pop("ccc", 20.0), pop("bbb", 10.0), pop("aaa", 0.0)};
}

std::vector<PopBlock> fixture_pop_blocks() {
  return {
      pop_block("10.0.0.0/16", "aaa"), pop_block("10.1.0.0/16", "bbb"),
      pop_block("10.1.5.0/24", "ccc"), pop_block("10.2.0.0/16", "ccc"),
      pop_block("10.3.0.0/16", "zzz"),  // Not in the PoP list: ignored.
      pop_block("10.4.0.0/16", "ddd"), pop_block("2001:db8::/32", "ddd"),
  };
}

std::vector<GeofeedEntry> fixture_geofeed() {
  return {
      entry("10.0.1.0/24", "XA", "XA-1", "Alpha"),    // 0: aaa
      entry("10.0.2.0/24", "XA", "XA-1", "Alpha"),    // 1: aaa
      entry("10.1.5.128/25", "XA", "XA-1", "Alpha"),  // 2: ccc (the /24 beats the /16)
      entry("10.1.6.0/24", "XA", "XA-1", "Alpha"),    // 3: bbb
      entry("192.168.0.0/24", "XA", "XA-1", "Beta"),  // 4: unmapped
      entry("10.3.1.0/24", "XA", "XA-2", "Gamma"),    // 5: unmapped (only in zzz's block)
      entry("10.4.1.0/24", "XA", "XA-3", ""),         // 6: ddd
      entry("10.1.7.0/24", "XB", "XB-1", "Tie"),      // 7: bbb
      entry("10.2.0.0/16", "XB", "XB-1", "Tie"),      // 8: ccc (equal block)
      entry("2001:db8:1::/48", "XD", "XD-1", "Six"),  // 9: ddd
      entry("2001:db8::/31", "XD", "XD-1", "Six"),    // 10: unmapped (larger than ddd's)
      entry("172.16.0.0/12", "XC", "XC-1", "Delta"),  // 11: unmapped
      entry("10.1.8.0/24", "XE", "XE-1", "Even"),     // 12: bbb
      entry("10.4.2.0/24", "XE", "XE-1", "Even"),     // 13: ddd
  };
}

// The finder over the fixture above, built once.
const HomePopFinder& fixture_finder() {
  static const HomePopFinder finder(fixture_geofeed(), fixture_pop_blocks(), fixture_pops());
  return finder;
}

std::string code_of(const HomePop& home) { return home.pop.has_value() ? home.pop->code : ""; }

// The candidates in order, with their block counts and distances in degrees of arc; the
// winner is the first.
void expect_candidates(const HomePop& home, const std::vector<std::string>& codes,
                       const std::vector<std::size_t>& blocks, const std::vector<double>& degrees) {
  ASSERT_EQ(home.candidates.size(), codes.size());
  for (std::size_t i = 0; i < codes.size(); ++i) {
    EXPECT_EQ(home.candidates[i].code, codes[i]) << i;
    EXPECT_EQ(home.candidates[i].blocks, blocks[i]) << i;
    EXPECT_NEAR(home.candidates[i].distance_km, degrees_km(degrees[i]), 1e-6) << i;
  }
  EXPECT_EQ(code_of(home), codes.front());
}

TEST(HomePopTest, AssignsEachGeofeedBlockToTheLongestContainingPopBlock) {
  const auto& assignment = fixture_finder().assignment();
  // Indices into fixture_pops(): ddd 0, ccc 1, bbb 2, aaa 3.
  const std::vector<std::optional<std::size_t>> expected = {
      3, 3, 1, 2, std::nullopt, std::nullopt, 0, 2, 1, 0, std::nullopt, std::nullopt, 2, 0};
  EXPECT_EQ(assignment.pop_of_entry, expected);
  EXPECT_EQ(assignment.mapped_entries, 10U);
  EXPECT_EQ(assignment.unmapped_entries, 4U);
  EXPECT_EQ(assignment.ignored_pop_rows, 1U);
  EXPECT_EQ(assignment.ignored_codes, (std::vector<std::string>{"zzz"}));
  ASSERT_EQ(fixture_finder().pops().size(), 4U);
  EXPECT_EQ(fixture_finder().pops()[0].code, "ddd");
}

TEST(HomePopTest, CityLevelPicksThePopWithTheMostBlocks) {
  // Alpha at longitude 1: aaa has 2 blocks; bbb (9 degrees) and ccc (19 degrees) 1 each.
  const HomePop home = fixture_finder().find(city("Alpha", "XA", "XA-1", 1.0));
  EXPECT_EQ(home.level, HomePopLevel::kCity);
  expect_candidates(home, {"aaa", "bbb", "ccc"}, {2, 1, 1}, {1.0, 9.0, 19.0});
}

TEST(HomePopTest, CityLevelTieGoesToTheNearerPop) {
  // Tie at longitude 16: bbb and ccc have 1 block each; ccc is 4 degrees away, bbb 6.
  const HomePop home = fixture_finder().find(city("Tie", "XB", "XB-1", 16.0));
  EXPECT_EQ(home.level, HomePopLevel::kCity);
  expect_candidates(home, {"ccc", "bbb"}, {1, 1}, {4.0, 6.0});
}

TEST(HomePopTest, CityLevelTieAtEqualDistanceGoesToTheFirstCode) {
  // Even at longitude 0: bbb (+10) and ddd (-10), 1 block each, exactly as far.
  const HomePop home = fixture_finder().find(city("Even", "XE", "XE-1", 0.0));
  EXPECT_EQ(home.level, HomePopLevel::kCity);
  ASSERT_EQ(home.candidates.size(), 2U);
  EXPECT_EQ(home.candidates[0].distance_km, home.candidates[1].distance_km);
  expect_candidates(home, {"bbb", "ddd"}, {1, 1}, {10.0, 10.0});
}

TEST(HomePopTest, CityLevelWorksForIpv6Blocks) {
  const HomePop home = fixture_finder().find(city("Six", "XD", "XD-1", -10.0));
  EXPECT_EQ(home.level, HomePopLevel::kCity);
  expect_candidates(home, {"ddd"}, {1}, {0.0});
}

TEST(HomePopTest, RegionLevelPicksTheNearestPopNotTheMostBlocks) {
  // Beta's only row is unmapped. Region XA-1 maps aaa (2 blocks, 18 degrees), bbb (1, 8) and
  // ccc (1, 2): the nearest wins although aaa has more blocks.
  const HomePop home = fixture_finder().find(city("Beta", "XA", "XA-1", 18.0));
  EXPECT_EQ(home.level, HomePopLevel::kRegion);
  expect_candidates(home, {"ccc", "bbb", "aaa"}, {1, 1, 2}, {2.0, 8.0, 18.0});
}

TEST(HomePopTest, CountryLevelWhenNeitherCityNorRegionMaps) {
  // Gamma's row (region XA-2) lies only in the ignored zzz block. Country XA maps aaa, bbb,
  // ccc and ddd; ddd is 1 degree away.
  const HomePop home = fixture_finder().find(city("Gamma", "XA", "XA-2", -9.0));
  EXPECT_EQ(home.level, HomePopLevel::kCountry);
  expect_candidates(home, {"ddd", "aaa", "bbb", "ccc"}, {1, 2, 1, 1}, {1.0, 9.0, 19.0, 29.0});
}

TEST(HomePopTest, EmptyCityOrRegionKeyIsSkipped) {
  // Row 6 has an empty city; a city with an empty geofeed_city must not match it.
  City nameless = city("Nameless", "XA", "XA-3", -10.0);
  nameless.geofeed_city.clear();
  const HomePop home = fixture_finder().find(nameless);
  EXPECT_EQ(home.level, HomePopLevel::kRegion);
  expect_candidates(home, {"ddd"}, {1}, {0.0});

  City no_region = city("Nowhere", "XA", "", -10.0);
  EXPECT_EQ(fixture_finder().find(no_region).level, HomePopLevel::kCountry);
}

TEST(HomePopTest, NoHomePopWhenTheCountryHasNoMappedBlock) {
  // Delta's country has only an unmapped row; XF has no rows at all.
  for (const City& c : {city("Delta", "XC", "XC-1", 5.0), city("Far", "XF", "XF-1", 5.0)}) {
    const HomePop home = fixture_finder().find(c);
    EXPECT_EQ(home.level, HomePopLevel::kNone) << c.site.name;
    EXPECT_FALSE(home.pop.has_value()) << c.site.name;
    EXPECT_TRUE(home.candidates.empty()) << c.site.name;
  }
}

TEST(HomePopIgnoredCodeTest, IgnoredRowsAreRemovedBeforeMatching) {
  // The geofeed block lies in both blocks; without the unknown /16 it falls to the /8.
  const HomePopFinder finder({entry("10.3.1.0/24", "XA", "XA-1", "Alpha")},
                             {pop_block("10.0.0.0/8", "aaa"), pop_block("10.3.0.0/16", "zzz"),
                              pop_block("10.4.0.0/16", "zzz"), pop_block("10.5.0.0/16", "yyy")},
                             {pop("aaa", 0.0)});
  EXPECT_EQ(finder.assignment().pop_of_entry,
            (std::vector<std::optional<std::size_t>>{std::optional<std::size_t>(0)}));
  EXPECT_EQ(finder.assignment().ignored_pop_rows, 3U);
  EXPECT_EQ(finder.assignment().ignored_codes, (std::vector<std::string>{"yyy", "zzz"}));
  const HomePop home = finder.find(city("Alpha", "XA", "XA-1", 0.0));
  EXPECT_EQ(home.level, HomePopLevel::kCity);
  EXPECT_EQ(code_of(home), "aaa");
}

TEST(HomePopEmptyTest, NoDataGivesNoHomePop) {
  const HomePopFinder finder({}, {}, {});
  EXPECT_EQ(finder.assignment().mapped_entries, 0U);
  EXPECT_EQ(finder.find(city("Alpha", "XA", "XA-1", 0.0)).level, HomePopLevel::kNone);
}

}  // namespace
