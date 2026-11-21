#include "satsim/failure/failure_file.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "fixture.hpp"
#include "satsim/constellation/classification.hpp"
#include "satsim/failure/schedule.hpp"
#include "satsim/time/civil_time.hpp"
#include "satsim/time/utc_time.hpp"
#include "satsim/topology/link_plan.hpp"
#include "support/synthetic.hpp"
#include "topology/ideal.hpp"

namespace {

namespace fs = std::filesystem;
using satsim::constellation::testing::utc;
using satsim::failure::ElementKind;
using satsim::failure::Failure;
using satsim::failure::FailureSchedule;
using satsim::failure::FailureScheduleOrError;
using satsim::failure::testing::gateway_names;
using satsim::failure::testing::kStepNs;
using satsim::failure::testing::run_start;
using satsim::failure::testing::small;
using satsim::failure::testing::small_plan;

constexpr std::string_view kHeader =
    "kind,norad_cat_id_a,norad_cat_id_b,gateway,start_utc,length_s\n";

FailureScheduleOrError read(std::string_view text, std::int64_t step_ns = kStepNs,
                            const satsim::time::UtcTime& start = run_start()) {
  std::istringstream in{std::string(text)};
  return satsim::failure::read_failure_schedule(in, small().classification, small_plan(),
                                                gateway_names(), start, step_ns);
}

std::string error_of(const FailureScheduleOrError& r) {
  if (const auto* e = std::get_if<std::string>(&r)) {
    return *e;
  }
  ADD_FAILURE() << "accepted";
  return "";
}

FailureSchedule schedule_of(const FailureScheduleOrError& r) {
  if (const auto* e = std::get_if<std::string>(&r)) {
    ADD_FAILURE() << *e;
    return {};
  }
  return std::get<FailureSchedule>(r);
}

// The message for one failure line after the header.
std::string error_of_line(std::string_view line) {
  return error_of(read(std::string(kHeader) + std::string(line) + "\n"));
}

// Every kind, in a file with comments anywhere and "\r\n" line ends. Indices: satellite 100001
// is 1, 100002 is 2; the link 100004-100000 is link 2 (written the other way round);
// "New York, NY" is gateway 1 and `The "Q" site` gateway 2.
TEST(FailureFileTest, ReadsEveryKind) {
  const std::string text =
      "# failures\r\n" +
      std::string("kind,norad_cat_id_a,norad_cat_id_b,gateway,start_utc,length_s") +
      "\r\n"
      "satellite,100001,,,2026-11-18T21:15:10,\r\n"
      "# a comment between failures\r\n"
      "laser_link,100004,100000,,2026-11-18T21:15:00,10.5\r\n"
      "gateway,,,\"New York, NY\",2026-11-18T21:16:00,60\r\n"
      "gateway,,,\"The \"\"Q\"\" site\",2026-11-18T20:15:00,\r\n"
      "satellite,100002,,,2026-11-18T21:14:59.25,0.5\r\n"
      "gateway,,,Hawthorne,2026-11-18T21:15:01,0.001\r\n";
  const FailureSchedule s = schedule_of(read(text));
  const std::vector<Failure> expected = {
      {.kind = ElementKind::kSatellite,
       .element = 1,
       .start_utc = utc({2026, 11, 18, 21, 15, 10.0}),
       .start_s = 10.0,
       .length_s = std::nullopt},
      {.kind = ElementKind::kLaserLink,
       .element = 2,
       .start_utc = run_start(),
       .start_s = 0.0,
       .length_s = 10.5},
      {.kind = ElementKind::kGateway,
       .element = 1,
       .start_utc = utc({2026, 11, 18, 21, 16, 0.0}),
       .start_s = 60.0,
       .length_s = 60.0},
      {.kind = ElementKind::kGateway,
       .element = 2,
       .start_utc = utc({2026, 11, 18, 20, 15, 0.0}),
       .start_s = -3600.0,
       .length_s = std::nullopt},
      {.kind = ElementKind::kSatellite,
       .element = 2,
       .start_utc = utc({2026, 11, 18, 21, 14, 59.25}),
       .start_s = -0.75,
       .length_s = 0.5},
      {.kind = ElementKind::kGateway,
       .element = 0,
       .start_utc = utc({2026, 11, 18, 21, 15, 1.0}),
       .start_s = 1.0,
       .length_s = 0.001},
  };
  EXPECT_EQ(s.failures, expected);
}

TEST(FailureFileTest, AHeaderAloneIsAnEmptySchedule) {
  EXPECT_EQ(schedule_of(read(kHeader)), FailureSchedule{});
  EXPECT_EQ(schedule_of(read("# none\n" + std::string(kHeader) + "# still none\n")),
            FailureSchedule{});
}

TEST(FailureFileTest, RefusesEmptyFilesAndBadHeaders) {
  EXPECT_EQ(error_of(read("")), "file is empty");
  EXPECT_EQ(error_of(read("# only a comment\n")), "no header");
  EXPECT_EQ(error_of(read("kind,norad_cat_id_a,norad_cat_id_b,gateway,start_utc\n")),
            "line 1: the header is \"kind,norad_cat_id_a,norad_cat_id_b,gatew...\", expected "
            "kind,norad_cat_id_a,norad_cat_id_b,gateway,start_utc,length_s");
  EXPECT_EQ(error_of(read("satellite,100001,,,2026-11-18T21:15:10,\n")),
            "line 1: the header is \"satellite,100001,,,2026-11-18T21:15:10,\", expected "
            "kind,norad_cat_id_a,norad_cat_id_b,gateway,start_utc,length_s");
  EXPECT_EQ(error_of(read(std::string(kHeader) + "\nsatellite,100001,,,2026-11-18T21:15:10,\n")),
            "line 2: empty line");
  EXPECT_EQ(error_of(read(kHeader, 0)), "the run's step must be positive");
}

TEST(FailureFileTest, RefusesMalformedLines) {
  EXPECT_EQ(error_of_line("satellite,100001,,,2026-11-18T21:15:10"),
            "line 2: has 5 fields, expected 6");
  EXPECT_EQ(error_of_line("satellite,100001,,,2026-11-18T21:15:10,,"),
            "line 2: has 7 fields, expected 6");
  EXPECT_EQ(error_of_line("gateway,,,New York, NY,2026-11-18T21:15:10,"),
            "line 2: has 7 fields, expected 6");
  EXPECT_EQ(error_of_line("gateway,,,\"New York, NY,2026-11-18T21:15:10,"),
            "line 2: a quoted field is not closed");
  EXPECT_EQ(error_of_line("gateway,,,\"New York\" NY,2026-11-18T21:15:10,"),
            "line 2: the quoted field \"New York\" is followed by more text");
  EXPECT_EQ(error_of_line("gateway,,,The \"Q\" site,2026-11-18T21:15:10,"),
            "line 2: \"The \"Q\" site\" holds a double quote but is not quoted");
  EXPECT_EQ(error_of_line("\"satellite\",100001,,,2026-11-18T21:15:10,"),
            "line 2: the kind field is quoted; only the gateway field may be");
  EXPECT_EQ(error_of_line("satellite,\"100001\",,,2026-11-18T21:15:10,"),
            "line 2: the norad_cat_id_a field is quoted; only the gateway field may be");
  EXPECT_EQ(error_of_line("satellite,100001,,,\"2026-11-18T21:15:10\","),
            "line 2: the start_utc field is quoted; only the gateway field may be");
  EXPECT_EQ(error_of_line("plane,100001,,,2026-11-18T21:15:10,"),
            "line 2: \"plane\" is not a kind (satellite, laser_link or gateway)");
  EXPECT_EQ(error_of_line("satellite,abc,,,2026-11-18T21:15:10,"),
            "line 2: \"abc\" is not a catalogue number");
  EXPECT_EQ(error_of_line("laser_link,100000,-1,,2026-11-18T21:15:10,"),
            "line 2: \"-1\" is not a catalogue number");
  // The line number counts comments.
  EXPECT_EQ(error_of(read(std::string(kHeader) + "# c\nsatellite,100001,,,2026-11-18T21:15:10,\n" +
                          "satellite, 100002,,,2026-11-18T21:15:10,\n")),
            "line 4: \" 100002\" is not a catalogue number");
}

TEST(FailureFileTest, RefusesFieldsThatDoNotFitTheKind) {
  EXPECT_EQ(error_of_line("satellite,,,,2026-11-18T21:15:10,"),
            "line 2: kind satellite needs norad_cat_id_a");
  EXPECT_EQ(error_of_line("satellite,100001,100002,,2026-11-18T21:15:10,"),
            "line 2: kind satellite must leave norad_cat_id_b empty");
  EXPECT_EQ(error_of_line("satellite,100001,,Hawthorne,2026-11-18T21:15:10,"),
            "line 2: kind satellite must leave gateway empty");
  EXPECT_EQ(error_of_line("satellite,100001,,\"\",2026-11-18T21:15:10,"),
            "line 2: kind satellite must leave gateway empty");
  EXPECT_EQ(error_of_line("laser_link,100001,,,2026-11-18T21:15:10,"),
            "line 2: kind laser_link needs norad_cat_id_b");
  EXPECT_EQ(error_of_line("laser_link,,100001,,2026-11-18T21:15:10,"),
            "line 2: kind laser_link needs norad_cat_id_a");
  EXPECT_EQ(error_of_line("laser_link,100000,100001,Hawthorne,2026-11-18T21:15:10,"),
            "line 2: kind laser_link must leave gateway empty");
  EXPECT_EQ(error_of_line("gateway,,,,2026-11-18T21:15:10,"), "line 2: kind gateway needs gateway");
  EXPECT_EQ(error_of_line("gateway,100000,,Hawthorne,2026-11-18T21:15:10,"),
            "line 2: kind gateway must leave norad_cat_id_a empty");
  EXPECT_EQ(error_of_line("gateway,,100000,Hawthorne,2026-11-18T21:15:10,"),
            "line 2: kind gateway must leave norad_cat_id_b empty");
}

TEST(FailureFileTest, RefusesElementsThatAreNotInTheRun) {
  EXPECT_EQ(error_of_line("satellite,999999,,,2026-11-18T21:15:10,"),
            "line 2: satellite 999999 is not in the classification");
  EXPECT_EQ(error_of_line("satellite,100003,,,2026-11-18T21:15:10,"),
            "line 2: satellite 100003 is in no plane (excluded: no_lasers)");
  EXPECT_EQ(error_of_line("laser_link,100000,999999,,2026-11-18T21:15:10,"),
            "line 2: satellite 999999 is not in the classification");
  EXPECT_EQ(error_of_line("laser_link,100003,100000,,2026-11-18T21:15:10,"),
            "line 2: satellite 100003 is in no plane (excluded: no_lasers)");
  EXPECT_EQ(error_of_line("laser_link,100001,100001,,2026-11-18T21:15:10,"),
            "line 2: links satellite 100001 to itself");
  // Two plane members of one plane, but no link of the plan.
  EXPECT_EQ(error_of_line("laser_link,100000,100002,,2026-11-18T21:15:10,"),
            "line 2: satellites 100000 and 100002 have no link in the laser plan");
  EXPECT_EQ(error_of_line("gateway,,,Paris,2026-11-18T21:15:10,"),
            "line 2: no gateway is named \"Paris\"");
  // Names match exactly: no trimming, no case folding.
  EXPECT_EQ(error_of_line("gateway,,,hawthorne,2026-11-18T21:15:10,"),
            "line 2: no gateway is named \"hawthorne\"");
  EXPECT_EQ(error_of_line("gateway,,,\"New York,NY\",2026-11-18T21:15:10,"),
            "line 2: no gateway is named \"New York,NY\"");
  EXPECT_EQ(error_of_line("gateway,,,Twin,2026-11-18T21:15:10,"),
            "line 2: more than one gateway is named \"Twin\"");
  // A quoted name without a comma or quote is the same name.
  EXPECT_EQ(
      schedule_of(read(std::string(kHeader) + "gateway,,,\"Hawthorne\",2026-11-18T21:15:10,\n"))
          .failures.at(0)
          .element,
      0U);
}

TEST(FailureFileTest, RefusesBadStarts) {
  EXPECT_EQ(error_of_line("satellite,100001,,,,"),
            "line 2: \"\" is not a UTC time (YYYY-MM-DDTHH:MM:SS with an optional fraction)");
  EXPECT_EQ(error_of_line("satellite,100001,,,2026-11-18 21:15:10,"),
            "line 2: \"2026-11-18 21:15:10\" is not a UTC time (YYYY-MM-DDTHH:MM:SS with an "
            "optional fraction)");
  EXPECT_EQ(error_of_line("satellite,100001,,,2026-11-18T21:15:10Z,"),
            "line 2: \"2026-11-18T21:15:10Z\" is not a UTC time (YYYY-MM-DDTHH:MM:SS with an "
            "optional fraction)");
  EXPECT_EQ(error_of_line("satellite,100001,,,2026-02-30T21:15:10,"),
            "line 2: \"2026-02-30T21:15:10\" is not a UTC time (YYYY-MM-DDTHH:MM:SS with an "
            "optional fraction)");
  EXPECT_EQ(error_of_line("satellite,100001,,,2026-11-18T21:15:10.5,"),
            "line 2: start_utc 2026-11-18T21:15:10.5 is neither a step of the run "
            "(2026-11-18T21:15:00 + k x 1 s) nor before its start");
  // One nanosecond after the start is not a step; one before is old news.
  EXPECT_EQ(error_of_line("satellite,100001,,,2026-11-18T21:15:00.000000001,"),
            "line 2: start_utc 2026-11-18T21:15:00.000000001 is neither a step of the run "
            "(2026-11-18T21:15:00 + k x 1 s) nor before its start");
  EXPECT_EQ(error_of_line("satellite,100001,,,2066-11-18T21:15:00,"),
            "line 2: start_utc 2066-11-18T21:15:00 is more than 1e18 ns from the run's start");
  EXPECT_EQ(error_of_line("satellite,100001,,,1986-11-18T21:15:00,"),
            "line 2: start_utc 1986-11-18T21:15:00 is more than 1e18 ns from the run's start");
}

// A start is a step (run start + k x step, k >= 0) or before the run's start; the instant is
// k x step_s for a step, the exact offset for an older one.
TEST(FailureFileTest, MatchesStartsToSteps) {
  const auto start_s = [](std::string_view time, std::int64_t step_ns,
                          const satsim::time::UtcTime& start) {
    const FailureSchedule s = schedule_of(read(
        std::string(kHeader) + "satellite,100001,,," + std::string(time) + ",\n", step_ns, start));
    return s.failures.empty() ? -1e300 : s.failures[0].start_s;
  };
  EXPECT_EQ(start_s("2026-11-18T21:15:00", kStepNs, run_start()), 0.0);
  EXPECT_EQ(start_s("2026-11-18T21:14:59.999999999", kStepNs, run_start()), -1e-9);
  // 1,000,000 s after the start: 11 days, 13:46:40 later.
  EXPECT_EQ(start_s("2026-11-30T11:01:40", kStepNs, run_start()), 1e6);
  // A step of 15 s from a start with a fraction of a second.
  const satsim::time::UtcTime fractional = utc({2026, 11, 18, 21, 15, 0.125});
  constexpr std::int64_t k15 = 15 * kStepNs;
  EXPECT_EQ(start_s("2026-11-18T21:15:30.125", k15, fractional), 30.0);
  EXPECT_EQ(start_s("2026-11-18T21:15:00.125", k15, fractional), 0.0);
  EXPECT_EQ(start_s("2026-11-18T21:15:00", k15, fractional), -0.125);
  EXPECT_EQ(start_s("2026-11-18T21:14:50.125", k15, fractional), -10.0);
  EXPECT_EQ(error_of(read(std::string(kHeader) + "satellite,100001,,,2026-11-18T21:15:20.125,\n",
                          k15, fractional)),
            "line 2: start_utc 2026-11-18T21:15:20.125 is neither a step of the run "
            "(2026-11-18T21:15:00.125 + k x 15 s) nor before its start");
  // A step of 0.25 s: step 7 is at 7 x 0.25 s.
  constexpr std::int64_t kQuarter = kStepNs / 4;
  EXPECT_EQ(start_s("2026-11-18T21:15:01.75", kQuarter, run_start()), 1.75);
  EXPECT_EQ(error_of(read(std::string(kHeader) + "satellite,100001,,,2026-11-18T21:15:01.7,\n",
                          kQuarter)),
            "line 2: start_utc 2026-11-18T21:15:01.7 is neither a step of the run "
            "(2026-11-18T21:15:00 + k x 0.25 s) nor before its start");
}

TEST(FailureFileTest, RefusesBadNegativeAndZeroLengths) {
  EXPECT_EQ(error_of_line("satellite,100001,,,2026-11-18T21:15:10,ten"),
            "line 2: length_s \"ten\" is not a decimal number");
  EXPECT_EQ(error_of_line("satellite,100001,,,2026-11-18T21:15:10,1e3"),
            "line 2: length_s \"1e3\" is not a decimal number");
  EXPECT_EQ(error_of_line("satellite,100001,,,2026-11-18T21:15:10,1.2.3"),
            "line 2: length_s \"1.2.3\" is not a decimal number");
  EXPECT_EQ(error_of_line("satellite,100001,,,2026-11-18T21:15:10, 5"),
            "line 2: length_s \" 5\" is not a decimal number");
  EXPECT_EQ(error_of_line("satellite,100001,,,2026-11-18T21:15:10,-"),
            "line 2: length_s \"-\" is not a decimal number");
  EXPECT_EQ(error_of_line("satellite,100001,,,2026-11-18T21:15:10,-5"),
            "line 2: length_s -5 is negative");
  EXPECT_EQ(error_of_line("satellite,100001,,,2026-11-18T21:15:10,0"),
            "line 2: length_s 0 is zero: a failure that ends as it starts does nothing");
  EXPECT_EQ(error_of_line("satellite,100001,,,2026-11-18T21:15:10,0.000"),
            "line 2: length_s 0.000 is zero: a failure that ends as it starts does nothing");
  EXPECT_EQ(error_of_line("satellite,100001,,,2026-11-18T21:15:10,-0"),
            "line 2: length_s -0 is zero: a failure that ends as it starts does nothing");
  // 1e-20 s does not move an instant of 1e6 s.
  EXPECT_EQ(error_of_line("satellite,100001,,,2026-11-30T11:01:40,0.00000000000000000001"),
            "line 2: length_s 0.00000000000000000001 is too short to end after the start");
  EXPECT_EQ(schedule_of(read(std::string(kHeader) + "satellite,100001,,,2026-11-18T21:15:10,.5\n"))
                .failures.at(0)
                .length_s,
            std::optional<double>(0.5));
}

// The same element twice: refused when the intervals [start, end) meet (a permanent failure
// never ends), accepted when one ends at or before the other starts. A link is the same link
// whichever way its ends are written.
TEST(FailureFileTest, RefusesTheSameElementFailingTwiceAtOverlappingTimes) {
  const std::string h(kHeader);
  EXPECT_EQ(error_of(read(h + "satellite,100001,,,2026-11-18T21:15:10,\n"
                              "satellite,100001,,,2026-11-18T21:15:20,\n")),
            "line 3: satellite 100001 already fails at an overlapping time on line 2");
  EXPECT_EQ(error_of(read(h + "satellite,100001,,,2026-11-18T21:15:20,\n"
                              "satellite,100001,,,2026-11-18T21:15:10,\n")),
            "line 3: satellite 100001 already fails at an overlapping time on line 2");
  EXPECT_EQ(error_of(read(h + "satellite,100001,,,2026-11-18T21:15:10,10\n"
                              "satellite,100002,,,2026-11-18T21:15:10,\n"
                              "satellite,100001,,,2026-11-18T21:15:19,5\n")),
            "line 4: satellite 100001 already fails at an overlapping time on line 2");
  EXPECT_EQ(error_of(read(h + "satellite,100001,,,2026-11-18T21:15:10,30\n"
                              "satellite,100001,,,2026-11-18T21:15:39,\n")),
            "line 3: satellite 100001 already fails at an overlapping time on line 2");
  EXPECT_EQ(error_of(read(h + "laser_link,100000,100004,,2026-11-18T21:15:10,\n"
                              "laser_link,100004,100000,,2026-11-18T21:15:11,1\n")),
            "line 3: laser link 100000-100004 already fails at an overlapping time on line 2");
  EXPECT_EQ(error_of(read(h + "gateway,,,\"New York, NY\",2026-11-18T20:15:10,\n"
                              "gateway,,,\"New York, NY\",2026-11-18T21:15:11,1\n")),
            "line 3: gateway \"New York, NY\" already fails at an overlapping time on line 2");

  // Touching, in either order, and a later permanent one after a transient one: accepted.
  EXPECT_EQ(schedule_of(read(h + "satellite,100001,,,2026-11-18T21:15:10,10\n"
                                 "satellite,100001,,,2026-11-18T21:15:20,10\n"
                                 "satellite,100001,,,2026-11-18T21:15:00,10\n"
                                 "satellite,100001,,,2026-11-18T21:15:30,\n"))
                .failures.size(),
            4U);
  // Different elements of one kind, or the same index in different kinds, never clash: link 1
  // and satellite 1, gateway 1 and satellite 1.
  EXPECT_EQ(schedule_of(read(h + "satellite,100001,,,2026-11-18T21:15:10,\n"
                                 "laser_link,100001,100002,,2026-11-18T21:15:10,\n"
                                 "gateway,,,\"New York, NY\",2026-11-18T21:15:10,\n"
                                 "laser_link,100000,100001,,2026-11-18T21:15:10,\n"))
                .failures.size(),
            4U);
}

// The schedule of ReadsEveryKind, written: one comment, the header, the failures with the
// link's ends as the plan has them and names quoted only where needed. Read back, it is the
// same schedule.
TEST(FailureFileTest, WritesTheFormatAndReadsItBack) {
  const FailureSchedule s{
      .failures = {satsim::failure::testing::failure_at(ElementKind::kSatellite, 1, 10.0),
                   satsim::failure::testing::failure_at(ElementKind::kLaserLink, 2, 0.0, 10.5),
                   satsim::failure::testing::failure_at(ElementKind::kGateway, 1, 60.0, 60.0),
                   satsim::failure::testing::failure_at(ElementKind::kGateway, 2, -3600.0),
                   satsim::failure::testing::failure_at(ElementKind::kSatellite, 2, -0.75, 0.5),
                   satsim::failure::testing::failure_at(ElementKind::kGateway, 0, 1.0, 0.001),
                   Failure{.kind = ElementKind::kLaserLink,
                           .element = 5,
                           .start_utc = utc({2026, 11, 18, 21, 14, 59.123456789}),
                           .start_s = -0.876543211,
                           .length_s = 1.0 / 3.0}}};
  std::ostringstream out;
  ASSERT_TRUE(satsim::failure::write_failure_schedule(out, s, small().classification, small_plan(),
                                                      gateway_names()));
  EXPECT_EQ(out.str(),
            "# satsim failure schedule (docs/phase-4-failures.md 4.2): 7 failures\n"
            "kind,norad_cat_id_a,norad_cat_id_b,gateway,start_utc,length_s\n"
            "satellite,100001,,,2026-11-18T21:15:10,\n"
            "laser_link,100000,100004,,2026-11-18T21:15:00,10.5\n"
            "gateway,,,\"New York, NY\",2026-11-18T21:16:00,60\n"
            "gateway,,,\"The \"\"Q\"\" site\",2026-11-18T20:15:00,\n"
            "satellite,100002,,,2026-11-18T21:14:59.25,0.5\n"
            "gateway,,,Hawthorne,2026-11-18T21:15:01,0.001\n"
            "laser_link,200000,200001,,2026-11-18T21:14:59.123456789,0.3333333333333333\n");
  const FailureSchedule back = schedule_of(read(out.str()));
  // The old start's instant is its exact offset, which the hand-written -0.876543211 is.
  EXPECT_EQ(back, s);
}

// Every satellite of plane 1 and every laser link of a plan planned on a compact shell of
// Hypatia's kind (12 planes of 10), at steps of 15 s from a start with a fraction, some
// transient, some old: written then read, the same schedule.
TEST(FailureFileTest, RoundTripOnAPlannedPlan) {
  using satsim::topology::testing::WalkerShell;
  const std::array<WalkerShell, 1> shells = {WalkerShell{.planes = 12,
                                                         .per_plane = 10,
                                                         .inclination_deg = 53.0,
                                                         .height_km = 550.0,
                                                         .phasing = 5,
                                                         .epoch = {2026, 11, 18, 21, 15, 0.0}}};
  const satsim::topology::testing::Ideal ideal = satsim::topology::testing::ideal_shells(shells);
  const satsim::topology::LinkPlan plan = satsim::topology::plan_laser_links(
      ideal.classification, ideal.positions_ecef_km, satsim::topology::LinkPlanSettings{});
  ASSERT_GT(plan.links.size(), 120U);  // 12 rings of 10, and cross-plane links.
  const satsim::time::UtcTime start = utc({2026, 11, 18, 21, 15, 0.125});
  constexpr std::int64_t k15 = 15 * kStepNs;
  FailureSchedule s;
  for (const std::size_t satellite : ideal.classification.planes[0][1].members) {
    const auto k = static_cast<double>(satellite);
    s.failures.push_back(Failure{.kind = ElementKind::kSatellite,
                                 .element = satellite,
                                 .start_utc = start.plus_seconds(k * 15.0),
                                 .start_s = k * 15.0,
                                 .length_s = std::nullopt});
  }
  for (std::size_t l = 0; l < plan.links.size(); ++l) {
    const bool old = l % 3 == 0;
    const double start_s = old ? -0.5 * static_cast<double>(l + 1) : 15.0 * static_cast<double>(l);
    s.failures.push_back(
        Failure{.kind = ElementKind::kLaserLink,
                .element = l,
                .start_utc = start.plus_seconds(start_s),
                .start_s = start_s,
                .length_s = l % 2 == 0 ? std::optional<double>(0.1 * static_cast<double>(l + 1))
                                       : std::nullopt});
  }
  std::ostringstream out;
  ASSERT_TRUE(satsim::failure::write_failure_schedule(out, s, ideal.classification, plan, {}));
  std::istringstream in(out.str());
  const FailureSchedule back = schedule_of(
      satsim::failure::read_failure_schedule(in, ideal.classification, plan, {}, start, k15));
  EXPECT_EQ(back, s);
}

TEST(FailureFileTest, WriteThenReadAFile) {
  const FailureSchedule s{
      .failures = {satsim::failure::testing::failure_at(ElementKind::kGateway, 1, 5.0, 2.0)}};
  const fs::path path = fs::temp_directory_path() / "satsim_failure_file_test.csv";
  ASSERT_EQ(satsim::failure::write_failure_schedule_file(path, s, small().classification,
                                                         small_plan(), gateway_names()),
            std::nullopt);
  EXPECT_EQ(schedule_of(satsim::failure::read_failure_schedule_file(
                path, small().classification, small_plan(), gateway_names(), run_start(), kStepNs)),
            s);
  fs::remove(path);
  EXPECT_EQ(error_of(satsim::failure::read_failure_schedule_file(
                path, small().classification, small_plan(), gateway_names(), run_start(), kStepNs)),
            "cannot open " + path.string());
  EXPECT_EQ(error_of(satsim::failure::read_failure_schedule_file(
                fs::temp_directory_path(), small().classification, small_plan(), gateway_names(),
                run_start(), kStepNs)),
            fs::temp_directory_path().string() + " is a directory");
  EXPECT_NE(
      satsim::failure::write_failure_schedule_file(
          path / "no_dir" / "x.csv", s, small().classification, small_plan(), gateway_names()),
      std::nullopt);
}

TEST(FailureFileTest, AFileErrorStartsWithThePath) {
  const fs::path path = fs::temp_directory_path() / "satsim_failure_file_error_test.csv";
  {
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    file << kHeader << "satellite,999999,,,2026-11-18T21:15:10,\n";
  }
  EXPECT_EQ(error_of(satsim::failure::read_failure_schedule_file(
                path, small().classification, small_plan(), gateway_names(), run_start(), kStepNs)),
            path.string() + ": line 2: satellite 999999 is not in the classification");
  fs::remove(path);
}

// Names the format cannot hold are not written.
TEST(FailureFileTest, RefusesToWriteNamesTheFormatCannotHold) {
  const FailureSchedule s{
      .failures = {satsim::failure::testing::failure_at(ElementKind::kGateway, 0, 5.0)}};
  for (const std::string& bad : {std::string(""), std::string("Two\nlines"), std::string("CR\r")}) {
    const std::vector<std::string> names = {bad};
    std::ostringstream out;
    EXPECT_FALSE(satsim::failure::write_failure_schedule(out, s, small().classification,
                                                         small_plan(), names));
    EXPECT_EQ(out.str(), "");
  }
}

}  // namespace
