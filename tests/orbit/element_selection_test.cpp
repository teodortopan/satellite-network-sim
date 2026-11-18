#include "satsim/orbit/element_selection.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <vector>

#include "satsim/orbit/element_set.hpp"
#include "satsim/time/civil_time.hpp"

namespace {

using satsim::orbit::ElementSelection;
using satsim::orbit::ElementSet;
using satsim::orbit::ElementSource;
using satsim::orbit::select_element_sets;
using satsim::orbit::SourcedElementSet;
using satsim::time::CivilTime;

// A synthetic element set. The name records where it came from, so tests can tell which of
// several sets with one catalogue number was chosen.
ElementSet set(std::int64_t catalog_number, const std::string& name,
               const CivilTime& epoch = CivilTime{2026, 11, 17, 0, 0, 0.0}) {
  ElementSet e;
  e.norad_cat_id = catalog_number;
  e.object_name = name;
  e.epoch = epoch;
  e.mean_motion_rev_per_day = 15.0;
  return e;
}

std::vector<std::string> names(const std::vector<SourcedElementSet>& sets) {
  std::vector<std::string> out;
  out.reserve(sets.size());
  for (const SourcedElementSet& s : sets) {
    out.push_back(s.elements.object_name);
  }
  return out;
}

TEST(ElementSelectionTest, EmptyInputs) {
  const ElementSelection selection = select_element_sets({}, {});
  EXPECT_TRUE(selection.selected.empty());
  EXPECT_TRUE(selection.superseded.empty());
}

// Supplemental where it exists, otherwise standard (the fallback path real data does not
// exercise today, since every standard satellite also has a supplemental set); output sorted
// by catalogue number whatever the input order.
TEST(ElementSelectionTest, SupplementalPreferredStandardAsFallback) {
  const std::vector<ElementSet> standard = {set(300, "std-300"), set(5, "std-5"),
                                            set(100, "std-100")};
  const std::vector<ElementSet> supplemental = {set(799501782, "sup-799501782"),
                                                set(100, "sup-100")};
  const ElementSelection selection = select_element_sets(standard, supplemental);

  EXPECT_EQ(names(selection.selected),
            (std::vector<std::string>{"std-5", "sup-100", "std-300", "sup-799501782"}));
  ASSERT_EQ(selection.selected.size(), 4U);
  EXPECT_EQ(selection.selected[0].source, ElementSource::kStandard);
  EXPECT_EQ(selection.selected[1].source, ElementSource::kSupplemental);
  EXPECT_EQ(selection.selected[2].source, ElementSource::kStandard);
  EXPECT_EQ(selection.selected[3].source, ElementSource::kSupplemental);
  EXPECT_EQ(selection.selected[1].elements, supplemental[1]);
  EXPECT_TRUE(selection.superseded.empty());  // Standard losing to supplemental is normal.
}

TEST(ElementSelectionTest, OnlyOneInput) {
  const std::vector<ElementSet> sets = {set(2, "b"), set(1, "a")};
  const ElementSelection only_standard = select_element_sets(sets, {});
  EXPECT_EQ(names(only_standard.selected), (std::vector<std::string>{"a", "b"}));
  EXPECT_EQ(only_standard.selected[0].source, ElementSource::kStandard);

  const ElementSelection only_supplemental = select_element_sets({}, sets);
  EXPECT_EQ(names(only_supplemental.selected), (std::vector<std::string>{"a", "b"}));
  EXPECT_EQ(only_supplemental.selected[0].source, ElementSource::kSupplemental);
}

// The supplemental set wins even when the standard set is newer.
TEST(ElementSelectionTest, SupplementalWinsWhateverTheEpochs) {
  const std::vector<ElementSet> standard = {set(7, "std", CivilTime{2026, 11, 18, 0, 0, 0.0})};
  const std::vector<ElementSet> supplemental = {set(7, "sup", CivilTime{2026, 11, 1, 0, 0, 0.0})};
  EXPECT_EQ(names(select_element_sets(standard, supplemental).selected),
            (std::vector<std::string>{"sup"}));
}

// A catalogue number twice in one input keeps the latest epoch; the others are reported, not
// dropped silently. Each calendar field decides on its own (a later second within an earlier
// minute does not win).
TEST(ElementSelectionTest, DuplicatesKeepTheLatestEpochAndAreReported) {
  const std::vector<ElementSet> standard = {
      set(9, "std-9-old", CivilTime{2026, 11, 17, 10, 0, 59.5}),
      set(9, "std-9-new", CivilTime{2026, 11, 17, 10, 1, 0.0}),
      set(4, "std-4"),
      set(9, "std-9-older", CivilTime{2025, 12, 31, 23, 59, 59.9}),
  };
  const std::vector<ElementSet> supplemental = {
      set(8, "sup-8-new", CivilTime{2027, 1, 1, 0, 0, 0.0}),
      set(8, "sup-8-old", CivilTime{2026, 12, 31, 0, 0, 0.0}),
  };
  const ElementSelection selection = select_element_sets(standard, supplemental);
  EXPECT_EQ(names(selection.selected),
            (std::vector<std::string>{"std-4", "sup-8-new", "std-9-new"}));
  // Input order, standard first.
  EXPECT_EQ(names(selection.superseded),
            (std::vector<std::string>{"std-9-old", "std-9-older", "sup-8-old"}));
  ASSERT_EQ(selection.superseded.size(), 3U);
  EXPECT_EQ(selection.superseded[0].source, ElementSource::kStandard);
  EXPECT_EQ(selection.superseded[2].source, ElementSource::kSupplemental);
}

// Same catalogue number and same epoch: the first in input order is kept, the other reported.
TEST(ElementSelectionTest, ExactDuplicateEpochKeepsTheFirst) {
  const std::vector<ElementSet> supplemental = {set(3, "first"), set(3, "second")};
  const ElementSelection selection = select_element_sets({}, supplemental);
  EXPECT_EQ(names(selection.selected), (std::vector<std::string>{"first"}));
  EXPECT_EQ(names(selection.superseded), (std::vector<std::string>{"second"}));
}

// A duplicated standard set that loses to a supplemental one is still reported as superseded.
TEST(ElementSelectionTest, DuplicateStandardIsReportedEvenWhenSupplementalWins) {
  const std::vector<ElementSet> standard = {set(6, "std-a", CivilTime{2026, 1, 1, 0, 0, 0.0}),
                                            set(6, "std-b", CivilTime{2026, 1, 2, 0, 0, 0.0})};
  const std::vector<ElementSet> supplemental = {set(6, "sup")};
  const ElementSelection selection = select_element_sets(standard, supplemental);
  EXPECT_EQ(names(selection.selected), (std::vector<std::string>{"sup"}));
  EXPECT_EQ(names(selection.superseded), (std::vector<std::string>{"std-a"}));
}

}  // namespace
