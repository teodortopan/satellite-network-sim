#pragma once

#include <cstdint>
#include <span>
#include <vector>

#include "satsim/orbit/element_set.hpp"

namespace satsim::orbit {

/// Which CelesTrak product an element set came from.
enum class ElementSource : std::uint8_t {
  kStandard,      ///< Standard GP data, fitted to US Space Force radar tracking.
  kSupplemental,  ///< Supplemental GP data, fitted to the operator's own ephemerides.
};

/// An element set with the product it came from.
struct SourcedElementSet {
  ElementSet elements;
  ElementSource source = ElementSource::kStandard;

  friend bool operator==(const SourcedElementSet&, const SourcedElementSet&) = default;
};

/// The outcome of select_element_sets.
struct ElementSelection {
  /// One entry per catalogue number found in either input, in ascending norad_cat_id order.
  std::vector<SourcedElementSet> selected;
  /// Element sets passed over because the same input held another set with the same
  /// catalogue number and a later epoch (or the same epoch, earlier in the input). In input
  /// order, standard input first. Empty for well-formed CelesTrak downloads; reported rather
  /// than dropped silently.
  std::vector<SourcedElementSet> superseded;
};

/// Picks one element set per satellite: the supplemental set where one exists (it is more
/// accurate), otherwise the standard set. Within one input, a catalogue number that appears
/// more than once keeps its latest epoch, and the others go to `superseded`. Epochs are
/// compared as calendar fields (year, month, day, hour, minute, second), which orders valid
/// UTC times correctly without converting them. A supplemental set wins over a standard one
/// whatever their epochs; that is the normal case and is not reported.
[[nodiscard]] ElementSelection select_element_sets(std::span<const ElementSet> standard,
                                                   std::span<const ElementSet> supplemental);

}  // namespace satsim::orbit
