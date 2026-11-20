#include "satsim/topology/link_file.hpp"

#include <gtest/gtest.h>

#include <Eigen/Core>
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "ideal.hpp"
#include "satsim/constellation/classification.hpp"
#include "satsim/time/civil_time.hpp"
#include "satsim/topology/link_plan.hpp"
#include "satsim/topology/link_state.hpp"
#include "satsim/topology/physical_checks.hpp"
#include "satsim/topology/positions.hpp"
#include "support/synthetic.hpp"

namespace {

namespace fs = std::filesystem;
using satsim::constellation::Classification;
using satsim::topology::count_links_by_kind;
using satsim::topology::count_unassigned_lasers;
using satsim::topology::Laser;
using satsim::topology::LaserPattern;
using satsim::topology::LaserSide;
using satsim::topology::LinkFileSettings;
using satsim::topology::LinkKind;
using satsim::topology::LinkPlan;
using satsim::topology::LinkPlanOrError;
using satsim::topology::LinkPlanSettings;
using satsim::topology::PlannedLink;
using satsim::topology::UnassignedReason;
using satsim::topology::testing::Ideal;
using satsim::topology::testing::ideal_shells;
using satsim::topology::testing::IdealOptions;
using satsim::topology::testing::WalkerShell;

constexpr satsim::time::CivilTime kEpoch{2026, 11, 18, 21, 15, 0.0};

// A compact shell of Hypatia's kind (53 deg, 550 km), 12 planes of 10: small enough for the
// sanitizer and Valgrind runs. The round trip on Hypatia's full shell is in
// link_file_hypatia_check.cpp, outside them (GCC's ASan takes about 20 minutes on it).
const Ideal& compact_shell() {
  static const Ideal kIdeal = [] {
    const std::array<WalkerShell, 1> shells = {WalkerShell{.planes = 12,
                                                           .per_plane = 10,
                                                           .inclination_deg = 53.0,
                                                           .height_km = 550.0,
                                                           .phasing = 5,
                                                           .epoch = kEpoch}};
    return ideal_shells(shells);
  }();
  return kIdeal;
}

// A small two-shell classification for the refusals, checked by hand:
//   shell 0: 4 planes of 4 at 550 km, catalogue numbers 100000 + 4 p + s; satellite 100003
//            (plane 0, s 3) is excluded (no_lasers), so plane 0 has 3 members;
//   shell 1: 2 planes of 3 at 400 km, catalogue numbers 200000 + 3 p + s.
// Classification indices follow catalogue order: 100000 is 0, ..., 100015 is 15, 200000 is 16.
const Ideal& small() {
  static const Ideal kIdeal = [] {
    const std::array<WalkerShell, 2> shells = {
        WalkerShell{.planes = 4, .per_plane = 4, .height_km = 550.0, .epoch = kEpoch},
        WalkerShell{.planes = 2,
                    .per_plane = 3,
                    .inclination_deg = 70.0,
                    .height_km = 400.0,
                    .first_id = 200000,
                    .epoch = kEpoch}};
    IdealOptions options;
    options.exclude = [](std::size_t shell, std::size_t p, std::size_t s) {
      return shell == 0 && p == 0 && s == 3;
    };
    return ideal_shells(shells, options);
  }();
  return kIdeal;
}

LinkPlanOrError read(std::string_view text, const Classification& c,
                     const LinkFileSettings& settings = {}) {
  std::istringstream in{std::string(text)};
  return satsim::topology::read_link_plan(in, c, settings);
}

std::string error_of(const LinkPlanOrError& r) {
  if (const auto* e = std::get_if<std::string>(&r)) {
    return *e;
  }
  ADD_FAILURE() << "accepted";
  return "";
}

// The plan read (a copy: the result is usually a temporary), or an empty plan and a failure.
LinkPlan plan_of(const LinkPlanOrError& r) {
  if (const auto* e = std::get_if<std::string>(&r)) {
    ADD_FAILURE() << *e;
    return {};
  }
  return std::get<LinkPlan>(r);
}

std::size_t index_of(const Classification& c, std::int64_t id) {
  for (std::size_t i = 0; i < c.satellites.size(); ++i) {
    if (c.satellites[i].norad_cat_id == id) {
      return i;
    }
  }
  ADD_FAILURE() << "no satellite " << id;
  return 0;
}

// Every physical check at the reference time, lasers only.
std::size_t violations(const Ideal& ideal, const LinkPlan& p) {
  const auto positions = satsim::topology::testing::step_positions(
      satsim::constellation::testing::utc(kEpoch), ideal.positions_ecef_km);
  satsim::topology::LinkStateEvaluator evaluator(p, {});
  return satsim::topology::run_physical_checks(ideal.classification, p, evaluator.step(positions),
                                               positions, {}, {}, {})
      .total();
}

// Plan, write, read with the same pattern: the same plan (operator==), and the same link
// states at a step. 12 planes of 10 have 120 in-plane links (12 rings of 10) under every
// pattern; the file has two comment lines and the header before the links.
TEST(LinkFileTest, RoundTripOnACompactShellGivesTheSamePlan) {
  const Ideal& ideal = compact_shell();
  for (const LaserPattern pattern : satsim::topology::kAllLaserPatterns) {
    const LinkPlan planned = satsim::topology::plan_laser_links(
        ideal.classification, ideal.positions_ecef_km, LinkPlanSettings{.pattern = pattern});
    EXPECT_EQ(count_links_by_kind(planned)[0], 120U);
    std::ostringstream out;
    ASSERT_TRUE(satsim::topology::write_link_plan(planned, ideal.classification, out));
    const std::string text = out.str();
    EXPECT_EQ(static_cast<std::size_t>(std::count(text.begin(), text.end(), '\n')),
              planned.links.size() + 3);
    EXPECT_NE(text.find("\nnorad_cat_id_a,norad_cat_id_b,side_a,side_b\n"), std::string::npos);

    const LinkPlan read_back = plan_of(read(text, ideal.classification, {.pattern = pattern}));
    EXPECT_EQ(read_back, planned) << satsim::topology::name_of(pattern);
    EXPECT_EQ(violations(ideal, read_back), 0U);

    // The link states at the reference time, and with satellite 5 unpropagated at a later step:
    // every link touching it is then end_not_propagated.
    auto positions = satsim::topology::testing::step_positions(
        satsim::constellation::testing::utc(kEpoch), ideal.positions_ecef_km);
    satsim::topology::LinkStateEvaluator a(planned, {});
    satsim::topology::LinkStateEvaluator b(read_back, {});
    EXPECT_EQ(a.step(positions).states, b.step(positions).states);
    positions.utc = satsim::constellation::testing::utc({2026, 11, 18, 21, 16, 0.0});
    positions.position_ecef_km[5].reset();
    const auto later_a = a.step(positions);
    EXPECT_EQ(later_a.states, b.step(positions).states);
    const auto touching = std::ranges::count_if(planned.links, [](const PlannedLink& l) {
      return l.satellite_a == 5 || l.satellite_b == 5;
    });
    EXPECT_EQ(satsim::topology::count_down_by_reason(later_a)[0],
              static_cast<std::size_t>(touching));
  }
}

TEST(LinkFileTest, WriteThenReadAFile) {
  const Ideal& ideal = compact_shell();
  const LinkPlan planned = satsim::topology::plan_laser_links(
      ideal.classification, ideal.positions_ecef_km, LinkPlanSettings{});
  const fs::path path = fs::temp_directory_path() / "satsim_link_file_test.csv";
  ASSERT_EQ(satsim::topology::write_link_plan_file(planned, ideal.classification, path),
            std::nullopt);
  EXPECT_EQ(plan_of(satsim::topology::read_link_plan_file(path, ideal.classification, {})),
            planned);
  fs::remove(path);
  const std::string missing =
      error_of(satsim::topology::read_link_plan_file(path, ideal.classification, {}));
  EXPECT_EQ(missing, "cannot open " + path.string());
  EXPECT_NE(satsim::topology::write_link_plan_file(planned, ideal.classification,
                                                   path / "no_dir" / "x.csv"),
            std::nullopt);
}

TEST(LinkFileTest, RefusesUnknownAndExcludedSatellites) {
  const Classification& c = small().classification;
  EXPECT_EQ(error_of(read("norad_cat_id_a,norad_cat_id_b\n100000,999999\n", c)),
            "line 2: satellite 999999 is not in the classification");
  EXPECT_EQ(error_of(read("norad_cat_id_a,norad_cat_id_b\n100003,100000\n", c)),
            "line 2: satellite 100003 is in no plane (excluded: no_lasers)");
}

TEST(LinkFileTest, RefusesASelfLink) {
  EXPECT_EQ(
      error_of(read("norad_cat_id_a,norad_cat_id_b\n100000,100000\n", small().classification)),
      "line 2: links satellite 100000 to itself");
}

TEST(LinkFileTest, RefusesALinkTwiceInEitherOrder) {
  const Classification& c = small().classification;
  EXPECT_EQ(
      error_of(read("# links\nnorad_cat_id_a,norad_cat_id_b\n100000,100001\n100001,100000\n", c)),
      "line 4: links 100001 and 100000, already linked on line 3");
  EXPECT_EQ(error_of(read("norad_cat_id_a,norad_cat_id_b\n100000,100001\n100000,100001\n", c)),
            "line 3: links 100000 and 100001, already linked on line 2");
}

// Satellite 100000 linked to four others: the fourth link (line 5) is one too many for the
// three lasers of `three`, and allowed with a limit of 4, which the plan then records.
TEST(LinkFileTest, RefusesMoreLinksThanTheLimit) {
  const Classification& c = small().classification;
  const std::string text =
      "norad_cat_id_a,norad_cat_id_b\n100000,100001\n100000,100002\n100004,100000\n"
      "100000,100008\n";
  EXPECT_EQ(error_of(read(text, c)), "line 5: gives satellite 100000 more than 3 links");
  EXPECT_EQ(error_of(read(text, c, {.pattern = LaserPattern::kThree, .max_lasers = 3})),
            "line 5: gives satellite 100000 more than 3 links");
  const LinkPlan p = plan_of(read(text, c, {.pattern = LaserPattern::kThree, .max_lasers = 4}));
  EXPECT_EQ(p.links.size(), 4U);
  EXPECT_EQ(p.max_lasers, std::optional<std::size_t>(4));
  EXPECT_EQ(satsim::topology::max_lasers_of(p), 4U);
  EXPECT_EQ(violations(small(), p), 0U);
  // plus-grid's own budget is 4: no limit to record.
  const LinkPlan q = plan_of(read(text, c, {.pattern = LaserPattern::kPlusGrid}));
  EXPECT_EQ(q.max_lasers, std::nullopt);
  EXPECT_EQ(satsim::topology::max_lasers_of(q), 4U);
}

TEST(LinkFileTest, RefusesMalformedLines) {
  const Classification& c = small().classification;
  const std::string h = "norad_cat_id_a,norad_cat_id_b\n";
  const std::string hs = "norad_cat_id_a,norad_cat_id_b,side_a,side_b\n";
  EXPECT_EQ(error_of(read(h + "100000\n", c)), "line 2: has 1 fields, expected 2");
  EXPECT_EQ(error_of(read(h + "100000,100001,ahead,behind\n", c)),
            "line 2: has 4 fields, expected 2");
  EXPECT_EQ(error_of(read(hs + "100000,100001\n", c)), "line 2: has 2 fields, expected 4");
  EXPECT_EQ(error_of(read(h + "100000,abc\n", c)), "line 2: \"abc\" is not a catalogue number");
  EXPECT_EQ(error_of(read(h + "-1,100000\n", c)), "line 2: \"-1\" is not a catalogue number");
  EXPECT_EQ(error_of(read(h + "100000, 100001\n", c)),
            "line 2: \" 100001\" is not a catalogue number");
  EXPECT_EQ(error_of(read(hs + "100000,100001,up,behind\n", c)),
            "line 2: \"up\" is not a laser side");
  EXPECT_EQ(error_of(read(hs + "100000,100001,ahead,down\n", c)),
            "line 2: \"down\" is not a laser side");
  EXPECT_EQ(error_of(read(hs + "100000,100001,ahead,west\n", c)),
            "line 2: sides ahead and west are not ahead/behind, east/west or "
            "second_east/second_west");
  EXPECT_EQ(error_of(read(hs + "100000,100001,behind,ahead\n", c)),
            "line 2: sides behind and ahead are not ahead/behind, east/west or "
            "second_east/second_west");
  EXPECT_EQ(error_of(read(h + "100000,100001\n\n100001,100002\n", c)), "line 3: empty line");
  EXPECT_EQ(error_of(read("a,b\n100000,100001\n", c)),
            "line 1: the header is \"a,b\", expected norad_cat_id_a,norad_cat_id_b or "
            "norad_cat_id_a,norad_cat_id_b,side_a,side_b");
  EXPECT_EQ(error_of(read("100000,100001\n", c)),
            "line 1: the header is \"100000,100001\", expected norad_cat_id_a,norad_cat_id_b or "
            "norad_cat_id_a,norad_cat_id_b,side_a,side_b");
}

TEST(LinkFileTest, RefusesEmptyFiles) {
  const Classification& c = small().classification;
  EXPECT_EQ(error_of(read("", c)), "file is empty");
  EXPECT_EQ(error_of(read("# only a comment\n", c)), "no header and no links");
  EXPECT_EQ(error_of(read("norad_cat_id_a,norad_cat_id_b\n", c)), "no links");
  EXPECT_EQ(error_of(read("norad_cat_id_a,norad_cat_id_b\r\n# none\r\n", c)), "no links");
}

// A link from shell 0 to shell 1, and one between planes 0 and 2 of shell 0 (second
// neighbours; without side columns any link between two planes is labelled east/west,
// cross_plane): a file may hold any links. The cross-shell link makes the plan allow links
// between shells, so the physical checks pass.
TEST(LinkFileTest, AcceptsCrossShellLinks) {
  const Ideal& ideal = small();
  const Classification& c = ideal.classification;
  const LinkPlan p =
      plan_of(read("norad_cat_id_a,norad_cat_id_b\n100000,200000\n100001,100009\n", c));
  ASSERT_EQ(p.links.size(), 2U);
  EXPECT_TRUE(p.cross_shell_links);
  EXPECT_EQ(p.links[0], (PlannedLink{.satellite_a = 0,
                                     .satellite_b = index_of(c, 200000),
                                     .side_a = LaserSide::kEast,
                                     .side_b = LaserSide::kWest,
                                     .kind = LinkKind::kCrossPlane,
                                     .shell_index = 0}));
  EXPECT_EQ(p.links[1].kind, LinkKind::kCrossPlane);
  EXPECT_EQ(violations(ideal, p), 0U);

  // Without the cross-shell link the plan does not allow any.
  const LinkPlan q = plan_of(read("norad_cat_id_a,norad_cat_id_b\n100001,100009\n", c));
  EXPECT_FALSE(q.cross_shell_links);
  // The same link with side columns, read with sides second_east/second_west.
  const LinkPlan r = plan_of(
      read("norad_cat_id_a,norad_cat_id_b,side_a,side_b\n100001,100009,second_east,second_west\n",
           c, {.pattern = LaserPattern::kSix}));
  EXPECT_EQ(r.links[0].kind, LinkKind::kSecondCrossPlane);
  EXPECT_EQ(violations(ideal, r), 0U);
}

// The per-satellite tables of a side-less file under `three`, worked by hand:
//  - link 0, 100000-100001: same plane (shell 0, plane 0), so 100000's ahead and 100001's
//    behind;
//  - link 1, 100000-100004: planes 0 and 1, so 100000's east and 100004's west.
// 100000 (plane 0, index 0, even): ahead -> 0, behind unlinked, east -> 1. Plane 0 has 3
// members, so its behind laser is no_free_partner (not plane_too_small).
// 100004 (plane 1, index 0, even): ahead, behind, east unlinked (no_free_partner: plane 1 has 4
// members and an east neighbour), then a west laser of its own -> 1.
// Shell 1's satellites (planes of 3, 2 planes: each the other's east and west, no second):
// lasers ahead, behind, then east (even index) or west (odd), all no_free_partner.
TEST(LinkFileTest, FillsTheLasersFromTheLinks) {
  const Ideal& ideal = small();
  const Classification& c = ideal.classification;
  const LinkPlan p =
      plan_of(read("norad_cat_id_a,norad_cat_id_b\n100000,100001\n100000,100004\n", c));
  ASSERT_EQ(p.links.size(), 2U);
  EXPECT_EQ(p.links[0].kind, LinkKind::kInPlane);
  EXPECT_EQ(p.links[0].side_a, LaserSide::kAhead);
  EXPECT_EQ(p.links[0].side_b, LaserSide::kBehind);
  const auto free = [](LaserSide side, UnassignedReason reason) {
    return Laser{.side = side, .link = std::nullopt, .unassigned = reason};
  };
  const auto linked = [](LaserSide side, std::size_t link) {
    return Laser{.side = side, .link = link, .unassigned = std::nullopt};
  };
  constexpr auto kNoPartner = UnassignedReason::kNoFreePartner;
  EXPECT_EQ(p.lasers[index_of(c, 100000)],
            (std::vector<Laser>{linked(LaserSide::kAhead, 0), free(LaserSide::kBehind, kNoPartner),
                                linked(LaserSide::kEast, 1)}));
  EXPECT_EQ(
      p.lasers[index_of(c, 100004)],
      (std::vector<Laser>{free(LaserSide::kAhead, kNoPartner), free(LaserSide::kBehind, kNoPartner),
                          free(LaserSide::kEast, kNoPartner), linked(LaserSide::kWest, 1)}));
  EXPECT_TRUE(p.lasers[index_of(c, 100003)].empty());  // Excluded.
  EXPECT_EQ(
      p.lasers[index_of(c, 200001)],
      (std::vector<Laser>{free(LaserSide::kAhead, kNoPartner), free(LaserSide::kBehind, kNoPartner),
                          free(LaserSide::kWest, kNoPartner)}));
  ASSERT_EQ(p.neighbours.size(), 2U);
  EXPECT_EQ(p.neighbours[0], satsim::topology::plane_neighbours(c.planes[0]));
  EXPECT_EQ(p.neighbours[1][0].east, std::optional<std::size_t>(1));
  EXPECT_EQ(p.neighbours[1][0].second_east, std::nullopt);
  EXPECT_EQ(count_links_by_kind(p), (std::array<std::size_t, 3>{1, 1, 0}));
  // 21 plane members x 3 lasers + 1 appended = 64 lasers, 4 of them linked.
  EXPECT_EQ(count_unassigned_lasers(p), (std::array<std::size_t, 3>{0, 0, 60}));
  EXPECT_EQ(violations(ideal, p), 0U);
}

// A plane of one or two keeps the planner's plane_too_small reason, and a single plane its
// no_neighbour_plane.
TEST(LinkFileTest, UnlinkedLasersKeepThePlannersReasons) {
  const std::array<WalkerShell, 1> shells = {
      WalkerShell{.planes = 1, .per_plane = 2, .height_km = 550.0, .epoch = kEpoch}};
  const Ideal ideal = ideal_shells(shells);
  const LinkPlan planned = satsim::topology::plan_laser_links(
      ideal.classification, ideal.positions_ecef_km, LinkPlanSettings{});
  ASSERT_EQ(planned.links.size(), 1U);
  std::ostringstream out;
  ASSERT_TRUE(satsim::topology::write_link_plan(planned, ideal.classification, out));
  const LinkPlan p = plan_of(read(out.str(), ideal.classification));
  EXPECT_EQ(p, planned);
  // Satellite 0: ahead linked, behind plane_too_small, east no_neighbour_plane.
  EXPECT_EQ(count_unassigned_lasers(p), (std::array<std::size_t, 3>{2, 2, 0}));
}

}  // namespace
