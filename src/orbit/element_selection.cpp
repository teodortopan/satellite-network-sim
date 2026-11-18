#include "satsim/orbit/element_selection.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <map>
#include <span>
#include <tuple>
#include <utility>
#include <vector>

#include "satsim/time/civil_time.hpp"

namespace satsim::orbit {

namespace {

bool is_earlier(const time::CivilTime& a, const time::CivilTime& b) noexcept {
  return std::tie(a.year, a.month, a.day, a.hour, a.minute, a.second) <
         std::tie(b.year, b.month, b.day, b.hour, b.minute, b.second);
}

// Index of the set kept for each catalogue number in `sets`: the latest epoch, the earliest in
// input order on a tie. Every other index is appended to superseded_out, in input order.
std::map<std::int64_t, std::size_t> latest_per_catalog_number(
    std::span<const ElementSet> sets, std::vector<std::size_t>& superseded_out) {
  std::map<std::int64_t, std::size_t> kept;
  std::vector<std::size_t> superseded;
  for (std::size_t i = 0; i < sets.size(); ++i) {
    const auto [it, inserted] = kept.try_emplace(sets[i].norad_cat_id, i);
    if (inserted) {
      continue;
    }
    if (is_earlier(sets[it->second].epoch, sets[i].epoch)) {
      superseded.push_back(it->second);
      it->second = i;
    } else {
      superseded.push_back(i);
    }
  }
  std::sort(superseded.begin(), superseded.end());
  superseded_out = std::move(superseded);
  return kept;
}

}  // namespace

ElementSelection select_element_sets(std::span<const ElementSet> standard,
                                     std::span<const ElementSet> supplemental) {
  ElementSelection selection;
  std::vector<std::size_t> superseded_standard;
  std::vector<std::size_t> superseded_supplemental;
  const auto kept_standard = latest_per_catalog_number(standard, superseded_standard);
  const auto kept_supplemental = latest_per_catalog_number(supplemental, superseded_supplemental);

  // Ordered by catalogue number; supplemental entries overwrite standard ones.
  std::map<std::int64_t, SourcedElementSet> chosen;
  for (const auto& [catalog_number, index] : kept_standard) {
    chosen.insert_or_assign(catalog_number,
                            SourcedElementSet{standard[index], ElementSource::kStandard});
  }
  for (const auto& [catalog_number, index] : kept_supplemental) {
    chosen.insert_or_assign(catalog_number,
                            SourcedElementSet{supplemental[index], ElementSource::kSupplemental});
  }

  selection.selected.reserve(chosen.size());
  for (auto& entry : chosen) {
    selection.selected.push_back(std::move(entry.second));
  }
  selection.superseded.reserve(superseded_standard.size() + superseded_supplemental.size());
  for (const std::size_t index : superseded_standard) {
    selection.superseded.push_back({standard[index], ElementSource::kStandard});
  }
  for (const std::size_t index : superseded_supplemental) {
    selection.superseded.push_back({supplemental[index], ElementSource::kSupplemental});
  }
  return selection;
}

}  // namespace satsim::orbit
