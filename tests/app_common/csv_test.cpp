#include "common/csv.hpp"

#include <gtest/gtest.h>

#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include "common/statistics.hpp"
#include "satsim/time/civil_time.hpp"
#include "satsim/time/utc_time.hpp"

namespace {

using satsim::app::append_csv_field;
using satsim::app::append_double;
using satsim::app::format_utc_iso;
using satsim::app::fraction_digits_for;

satsim::time::UtcTime utc(std::string_view iso) {
  const std::optional<satsim::time::CivilTime> civil = satsim::time::parse_iso_civil_time(iso);
  const std::optional<satsim::time::UtcTime> t =
      civil ? satsim::time::UtcTime::from_civil(*civil) : std::nullopt;
  if (!t) {
    ADD_FAILURE() << "bad test time " << iso;
    std::abort();
  }
  return *t;
}

std::string as_double_text(double value) {
  std::string out;
  append_double(out, value);
  return out;
}

std::string as_field(std::string_view text) {
  std::string out;
  append_csv_field(out, text);
  return out;
}

TEST(AppCsvTest, DoublesRoundTripExactly) {
  EXPECT_EQ(as_double_text(6928.137), "6928.137");
  EXPECT_EQ(as_double_text(-0.5), "-0.5");
  EXPECT_EQ(as_double_text(1e-7), "1e-07");
  for (const double value :
       {0.1, 1.0 / 3.0, -6378.137000000001, 7.585123456789012, std::nextafter(1.0, 2.0),
        std::numeric_limits<double>::denorm_min(), std::numeric_limits<double>::max(),
        -std::numeric_limits<double>::min()}) {
    const std::string text = as_double_text(value);
    double back = 0.0;
    const auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), back);
    ASSERT_EQ(ec, std::errc{}) << text;
    EXPECT_EQ(end, text.data() + text.size()) << text;
    EXPECT_EQ(back, value) << text;
  }
}

TEST(AppCsvTest, FieldsAreQuotedOnlyWhenNeeded) {
  EXPECT_EQ(as_field("STARLINK-1008"), "STARLINK-1008");
  EXPECT_EQ(as_field(""), "");
  EXPECT_EQ(as_field("A B"), "A B");
  EXPECT_EQ(as_field("A,B"), "\"A,B\"");
  EXPECT_EQ(as_field("say \"hi\""), "\"say \"\"hi\"\"\"");
  EXPECT_EQ(as_field("\""), "\"\"\"\"");
  EXPECT_EQ(as_field("line\nbreak"), "\"line\nbreak\"");
  EXPECT_EQ(as_field("cr\r"), "\"cr\r\"");
}

TEST(AppCsvTest, FractionDigitsAreTheFewestThatAreExact) {
  EXPECT_EQ(fraction_digits_for(utc("2026-11-18T21:00:00"), 60'000'000'000), 0);
  EXPECT_EQ(fraction_digits_for(utc("2026-11-18T21:00:00"), 500'000'000), 1);
  EXPECT_EQ(fraction_digits_for(utc("2026-11-18T21:00:00"), 250'000'000), 2);
  EXPECT_EQ(fraction_digits_for(utc("2026-11-18T21:00:00.125"), 1'000'000'000), 3);
  EXPECT_EQ(fraction_digits_for(utc("2026-11-18T21:00:00"), 1), 9);
  EXPECT_EQ(fraction_digits_for(utc("2026-11-18T21:00:00.000000001"), 60'000'000'000), 9);
}

TEST(AppCsvTest, FormatsIsoTimes) {
  EXPECT_EQ(format_utc_iso(utc("2026-11-18T21:05:07"), 0), "2026-11-18T21:05:07");
  EXPECT_EQ(format_utc_iso(utc("0001-01-01T00:00:00"), 0), "0001-01-01T00:00:00");
  EXPECT_EQ(format_utc_iso(utc("2026-11-18T21:05:07.25"), 2), "2026-11-18T21:05:07.25");
  EXPECT_EQ(format_utc_iso(utc("2026-11-18T21:05:07.25"), 3), "2026-11-18T21:05:07.250");
  EXPECT_EQ(format_utc_iso(utc("2026-11-18T23:59:59.999999999"), 9),
            "2026-11-18T23:59:59.999999999");
  EXPECT_EQ(format_utc_iso(utc("2026-11-18T21:05:07.000000001"), 9),
            "2026-11-18T21:05:07.000000001");
}

// Hand-computed: definition 7 interpolates at rank p/100 * (n - 1).
TEST(AppStatisticsTest, PercentilesInterpolateBetweenOrderStatistics) {
  const std::vector<double> sorted = {10.0, 20.0, 30.0, 40.0};
  EXPECT_DOUBLE_EQ(satsim::app::percentile_of_sorted(sorted, 0.0), 10.0);
  EXPECT_DOUBLE_EQ(satsim::app::percentile_of_sorted(sorted, 50.0), 25.0);
  EXPECT_DOUBLE_EQ(satsim::app::percentile_of_sorted(sorted, 5.0), 11.5);   // rank 0.15
  EXPECT_DOUBLE_EQ(satsim::app::percentile_of_sorted(sorted, 95.0), 38.5);  // rank 2.85
  EXPECT_DOUBLE_EQ(satsim::app::percentile_of_sorted(sorted, 100.0), 40.0);
  EXPECT_DOUBLE_EQ(satsim::app::percentile_of_sorted({7.0}, 95.0), 7.0);

  const auto q = satsim::app::quantiles({40.0, 10.0, 30.0, 20.0, 50.0});
  if (!q) {
    FAIL() << "no quantiles";
  }
  EXPECT_EQ(q->count, 5U);
  EXPECT_DOUBLE_EQ(q->min, 10.0);
  EXPECT_DOUBLE_EQ(q->p5, 12.0);
  EXPECT_DOUBLE_EQ(q->median, 30.0);
  EXPECT_DOUBLE_EQ(q->p95, 48.0);
  EXPECT_DOUBLE_EQ(q->max, 50.0);
  EXPECT_FALSE(satsim::app::quantiles({}).has_value());
}

TEST(AppStatisticsTest, BandsAreHalfOpen) {
  const auto bands = satsim::app::band_counts({0.0, 49.999, 50.0, 549.0, 551.0, -1.0}, 50.0);
  EXPECT_EQ(bands,
            (std::map<std::int64_t, std::size_t>{{-1, 1}, {0, 2}, {1, 1}, {10, 1}, {11, 1}}));
}

}  // namespace
