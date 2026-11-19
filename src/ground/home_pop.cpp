#include "satsim/ground/home_pop.hpp"

#include <algorithm>
#include <array>
#include <map>
#include <set>
#include <utility>

#include "satsim/ground/address_block.hpp"
#include "satsim/ground/distance.hpp"

namespace satsim::ground {

HomePopFinder::HomePopFinder(const std::vector<GeofeedEntry>& geofeed,
                             const std::vector<PopBlock>& pop_blocks,
                             const std::vector<Pop>& pop_codes)
    : pops_(pop_codes) {
  std::map<std::string, std::size_t> pop_by_code;
  for (std::size_t i = 0; i < pops_.size(); ++i) {
    pop_by_code.emplace(pops_[i].code, i);
  }

  // The usable rows of starlink_pops.csv: those whose code is a known PoP.
  std::vector<AddressBlock> blocks;
  std::vector<std::size_t> pop_of_block;
  std::set<std::string> ignored;
  for (const PopBlock& row : pop_blocks) {
    const auto it = pop_by_code.find(row.pop_code);
    if (it == pop_by_code.end()) {
      ++assignment_.ignored_pop_rows;
      ignored.insert(row.pop_code);
      continue;
    }
    blocks.push_back(row.block);
    pop_of_block.push_back(it->second);
  }
  assignment_.ignored_codes.assign(ignored.begin(), ignored.end());

  const AddressBlockIndex index(blocks);
  assignment_.pop_of_entry.reserve(geofeed.size());
  for (const GeofeedEntry& entry : geofeed) {
    const std::optional<std::size_t> block = index.longest_containing(entry.block);
    if (!block) {
      ++assignment_.unmapped_entries;
      assignment_.pop_of_entry.emplace_back(std::nullopt);
      continue;
    }
    const std::size_t pop = pop_of_block[*block];
    ++assignment_.mapped_entries;
    assignment_.pop_of_entry.emplace_back(pop);
    mapped_rows_.push_back(
        Row{.country = entry.country, .region = entry.region, .city = entry.city, .pop = pop});
  }
}

HomePop HomePopFinder::find(const City& city) const {
  struct Level {
    HomePopLevel level;
    const std::string* key;  // The city's own key at this level; an empty key is skipped.
    bool (*matches)(const City&, const Row&);
  };
  const std::array<Level, 3> levels = {{
      {HomePopLevel::kCity, &city.geofeed_city,
       [](const City& c, const Row& r) {
         return r.country == c.country && r.city == c.geofeed_city;
       }},
      {HomePopLevel::kRegion, &city.region,
       [](const City& c, const Row& r) { return r.country == c.country && r.region == c.region; }},
      {HomePopLevel::kCountry, &city.country,
       [](const City& c, const Row& r) { return r.country == c.country; }},
  }};

  for (const Level& level : levels) {
    if (level.key->empty()) {
      continue;
    }
    std::map<std::size_t, std::size_t> blocks_per_pop;  // Index into pops_ to count.
    for (const Row& row : mapped_rows_) {
      if (level.matches(city, row)) {
        ++blocks_per_pop[row.pop];
      }
    }
    if (blocks_per_pop.empty()) {
      continue;
    }

    HomePop result;
    result.level = level.level;
    std::vector<std::pair<PopCandidate, std::size_t>> candidates;  // With index into pops_.
    for (const auto& [pop, blocks] : blocks_per_pop) {
      PopCandidate candidate;
      candidate.code = pops_[pop].code;
      candidate.blocks = blocks;
      candidate.distance_km = great_circle_distance_on_mean_sphere_km(city.site, pops_[pop].site);
      candidates.emplace_back(std::move(candidate), pop);
    }
    // City level: most blocks first. Every level: then fewer kilometres, then the code.
    const bool by_blocks = level.level == HomePopLevel::kCity;
    std::sort(candidates.begin(), candidates.end(), [by_blocks](const auto& a, const auto& b) {
      if (by_blocks && a.first.blocks != b.first.blocks) {
        return a.first.blocks > b.first.blocks;
      }
      if (a.first.distance_km != b.first.distance_km) {
        return a.first.distance_km < b.first.distance_km;
      }
      return a.first.code < b.first.code;
    });
    result.pop = pops_[candidates.front().second];
    for (auto& entry : candidates) {
      result.candidates.push_back(std::move(entry.first));
    }
    return result;
  }
  return HomePop{};
}

}  // namespace satsim::ground
