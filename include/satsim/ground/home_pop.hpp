#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "satsim/ground/sites.hpp"

namespace satsim::ground {

/// Which level of the home-PoP rule decided.
enum class HomePopLevel : std::uint8_t {
  kCity,     ///< Geofeed rows of the city itself map to PoPs.
  kRegion,   ///< None of the city's rows map; rows of its region do.
  kCountry,  ///< Neither do; rows of its country do.
  kNone,     ///< No row of the country maps to a PoP: no Starlink service there.
};

/// A PoP considered at the level that decided.
struct PopCandidate {
  std::string code;
  std::size_t blocks = 0;    ///< Geofeed blocks at this level that map to this PoP.
  double distance_km = 0.0;  ///< From the city, great circle on the mean sphere.
};

/// A city's home PoP and the evidence behind it.
struct HomePop {
  HomePopLevel level = HomePopLevel::kNone;
  std::optional<Pop> pop;  ///< The winner; empty only for kNone.
  /// Every PoP found at the deciding level, in decision order: the winner first. Empty for
  /// kNone.
  std::vector<PopCandidate> candidates;
};

/// How SpaceX's two files were joined: which PoP serves each geofeed block.
struct PopAssignment {
  /// Per geofeed entry (same order as given): index into pops() of its PoP, or nothing.
  std::vector<std::optional<std::size_t>> pop_of_entry;
  std::size_t mapped_entries = 0;
  /// Geofeed blocks that are neither equal to nor inside any (non-ignored) PoP block.
  std::size_t unmapped_entries = 0;
  /// Rows of starlink_pops.csv whose PoP code is not in pop_codes.json (such as "staging2").
  /// They are left out before matching, as if absent from the file.
  std::size_t ignored_pop_rows = 0;
  std::vector<std::string> ignored_codes;  ///< Their distinct codes, sorted.
};

/// Finds a city's home PoP: the PoP its Starlink traffic is tunnelled to.
///
/// A geofeed block maps to the PoP of the longest block of starlink_pops.csv that equals or
/// contains it. Then, for a city:
/// 1. City level: among geofeed rows with the city's country and geofeed_city, the PoP mapped
///    by the most blocks.
/// 2. Region level, if no such row maps to a PoP: among the PoPs mapped by rows with the
///    city's country and region, the one nearest the city.
/// 3. Country level, if none: the same over all rows of the city's country.
/// 4. Otherwise no home PoP.
/// Ties (and the order of candidates) go to fewer kilometres, then to the PoP code in
/// alphabetical order. Distances are great circles on the mean sphere between the city's and
/// the PoP's coordinates.
class HomePopFinder {
 public:
  /// pop_codes gives the PoPs (codes are unique, as load_pop_codes guarantees). Copies what it
  /// needs; the arguments need not outlive the finder.
  HomePopFinder(const std::vector<GeofeedEntry>& geofeed, const std::vector<PopBlock>& pop_blocks,
                const std::vector<Pop>& pop_codes);

  [[nodiscard]] const std::vector<Pop>& pops() const noexcept { return pops_; }
  [[nodiscard]] const PopAssignment& assignment() const noexcept { return assignment_; }

  [[nodiscard]] HomePop find(const City& city) const;

 private:
  struct Row {
    std::string country;
    std::string region;
    std::string city;
    std::size_t pop = 0;  // Index into pops_.
  };

  std::vector<Pop> pops_;
  PopAssignment assignment_;
  std::vector<Row> mapped_rows_;  // Geofeed rows that map to a PoP.
};

}  // namespace satsim::ground
