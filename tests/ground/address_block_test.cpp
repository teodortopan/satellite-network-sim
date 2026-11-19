#include "satsim/ground/address_block.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <initializer_list>
#include <optional>
#include <ostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace satsim::ground {

// Readable GoogleTest failure messages.
void PrintTo(const AddressBlock& block, std::ostream* out) { *out << block.to_string(); }

}  // namespace satsim::ground

namespace {

using satsim::ground::AddressBlock;
using satsim::ground::AddressBlockIndex;
using satsim::ground::AddressFamily;
using satsim::ground::parse_address_block;

AddressBlock block(const std::string& text) {
  const auto parsed = parse_address_block(text);
  EXPECT_TRUE(parsed.block.has_value()) << text << ": " << parsed.error;
  if (!parsed.block) {
    throw std::invalid_argument(text + ": " + parsed.error);
  }
  return *parsed.block;
}

std::string error_of(const std::string& text) {
  const auto parsed = parse_address_block(text);
  EXPECT_FALSE(parsed.block.has_value()) << text;
  EXPECT_FALSE(parsed.error.empty()) << text;
  return parsed.error;
}

AddressBlock::Bytes bytes(std::initializer_list<std::uint8_t> leading) {
  AddressBlock::Bytes result{};
  std::size_t i = 0;
  for (const std::uint8_t b : leading) {
    result.at(i++) = b;
  }
  return result;
}

TEST(AddressBlockTest, ParsesIpv4) {
  const AddressBlock b = block("9.170.0.0/24");
  EXPECT_EQ(b.family(), AddressFamily::kIpv4);
  EXPECT_EQ(b.prefix_length(), 24);
  EXPECT_EQ(b.max_prefix_length(), 32);
  EXPECT_EQ(b.bytes(), bytes({9, 170, 0, 0}));
  EXPECT_EQ(b.to_string(), "9.170.0.0/24");
}

TEST(AddressBlockTest, ParsesIpv4PrefixLengthsZeroAndMaximum) {
  const AddressBlock all = block("0.0.0.0/0");
  EXPECT_EQ(all.prefix_length(), 0);
  EXPECT_EQ(all.bytes(), bytes({}));
  EXPECT_EQ(all.to_string(), "0.0.0.0/0");

  const AddressBlock host = block("255.255.255.255/32");
  EXPECT_EQ(host.prefix_length(), 32);
  EXPECT_EQ(host.bytes(), bytes({255, 255, 255, 255}));
  EXPECT_EQ(host.to_string(), "255.255.255.255/32");
}

TEST(AddressBlockTest, ParsesIpv6) {
  const AddressBlock b = block("2401:e320::/38");
  EXPECT_EQ(b.family(), AddressFamily::kIpv6);
  EXPECT_EQ(b.prefix_length(), 38);
  EXPECT_EQ(b.max_prefix_length(), 128);
  EXPECT_EQ(b.bytes(), bytes({0x24, 0x01, 0xe3, 0x20}));
  EXPECT_EQ(b.to_string(), "2401:e320::/38");

  // 0x0400 in the third group is bit 37, the last bit inside a /38.
  EXPECT_EQ(block("2401:e320:400::/38").bytes(), bytes({0x24, 0x01, 0xe3, 0x20, 0x04, 0x00}));
}

TEST(AddressBlockTest, ParsesIpv6PrefixLengthsZeroAndMaximum) {
  const AddressBlock all = block("::/0");
  EXPECT_EQ(all.prefix_length(), 0);
  EXPECT_EQ(all.bytes(), bytes({}));
  EXPECT_EQ(all.to_string(), "::/0");

  const AddressBlock host = block("ffff:ffff:ffff:ffff:ffff:ffff:ffff:ffff/128");
  EXPECT_EQ(host.prefix_length(), 128);
  AddressBlock::Bytes ones{};
  ones.fill(0xff);
  EXPECT_EQ(host.bytes(), ones);
  EXPECT_EQ(host.to_string(), "ffff:ffff:ffff:ffff:ffff:ffff:ffff:ffff/128");

  const AddressBlock loopback = block("::1/128");
  AddressBlock::Bytes one{};
  one.back() = 1;
  EXPECT_EQ(loopback.bytes(), one);
  EXPECT_EQ(loopback.to_string(), "::1/128");
}

TEST(AddressBlockTest, Ipv6TextFormsGiveTheSameBlock) {
  const AddressBlock compressed = block("2001:db8::/32");
  EXPECT_EQ(block("2001:0db8:0000:0000:0000:0000:0000:0000/32"), compressed);
  EXPECT_EQ(block("2001:DB8::/32"), compressed);
  EXPECT_EQ(block("2001:db8:0:0:0::/32"), compressed);
  EXPECT_EQ(block("1::/16").to_string(), "1::/16");
}

TEST(AddressBlockTest, ParsesIpv6WithEmbeddedIpv4) {
  const AddressBlock b = block("::ffff:192.0.2.128/128");
  EXPECT_EQ(b.bytes(), bytes({0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff, 192, 0, 2, 128}));
  EXPECT_EQ(b.to_string(), "::ffff:c000:280/128");
  EXPECT_EQ(block("1:2:3:4:5:6:1.2.3.4/128").to_string(), "1:2:3:4:5:6:102:304/128");
}

TEST(AddressBlockTest, Ipv6TextIsCanonical) {
  // RFC 5952: the longest zero run is shortened, the first of equal runs, never a single group.
  EXPECT_EQ(block("2001:db8:0:0:1:0:0:1/128").to_string(), "2001:db8::1:0:0:1/128");
  EXPECT_EQ(block("2001:db8:0:0:0:1:0:0/128").to_string(), "2001:db8::1:0:0/128");
  EXPECT_EQ(block("2001:db8:0:1:1:1:1:1/128").to_string(), "2001:db8:0:1:1:1:1:1/128");
  EXPECT_EQ(block("0:0:1:0:0:0:1:0/128").to_string(), "0:0:1::1:0/128");
  EXPECT_EQ(block("1:0:0:0:0:0:0:0/16").to_string(), "1::/16");
}

TEST(AddressBlockTest, RejectsMalformedIpv4) {
  for (const char* text : {"",
                           "/24",
                           "10.0.0.0",
                           "10.0.0/24",
                           "10.0.0.0.0/24",
                           "256.0.0.0/8",
                           "01.0.0.0/8",
                           "1000.0.0.0/8",
                           "10..0.0/16",
                           "a.b.c.d/8",
                           "10.0.0.0/33",
                           "10.0.0.0/-1",
                           "10.0.0.0/+8",
                           "10.0.0.0/08",
                           "10.0.0.0/",
                           "10.0.0.0/2 4",
                           "10.0.0.0/24 ",
                           " 10.0.0.0/24",
                           "10.0.0.0/1000",
                           "-1.0.0.0/8"}) {
    error_of(text);
  }
  EXPECT_NE(error_of("10.0.0.0/33").find("prefix length"), std::string::npos);
  EXPECT_NE(error_of("256.0.0.0/8").find("octet"), std::string::npos);
}

TEST(AddressBlockTest, RejectsMalformedIpv6) {
  for (const char* text :
       {"1:2:3:4:5:6:7/128", "1:2:3:4:5:6:7:8:9/128", "1::2::3/128", ":::/0", "1:::2/128",
        "1:2:3:4:5:6:7::8/128", "12345::/16", "g::/16", "::/129", "fe80::1%eth0/128", "1::/",
        "1:/16", ":1::/16", "1::2:/128", "::1.2.3/128", "::256.0.0.0/128", "1.2.3.4::/128",
        "::1.2.3.4:5/128", "1:2:3:4:5:6:7:1.2.3.4/128"}) {
    error_of(text);
  }
  EXPECT_NE(error_of("1::2::3/128").find("more than one"), std::string::npos);
  EXPECT_NE(error_of("fe80::1%eth0/128").find("zone"), std::string::npos);
}

TEST(AddressBlockTest, RejectsHostBitsAndNamesTheIntendedBlock) {
  EXPECT_NE(error_of("10.0.0.1/24").find("10.0.0.0/24"), std::string::npos);
  EXPECT_NE(error_of("2401:e320::1/38").find("2401:e320::/38"), std::string::npos);
  EXPECT_NE(error_of("0.0.0.1/0").find("0.0.0.0/0"), std::string::npos);
  EXPECT_NE(error_of("128.0.0.0/0").find("0.0.0.0/0"), std::string::npos);
  EXPECT_NE(error_of("::1/127").find("::/127"), std::string::npos);
  // One bit past a non-byte boundary.
  EXPECT_NE(error_of("11.0.0.0/7").find("10.0.0.0/7"), std::string::npos);
  EXPECT_NE(error_of("10.3.0.0/15").find("10.2.0.0/15"), std::string::npos);
  EXPECT_TRUE(parse_address_block("10.0.0.0/7").block.has_value());
  EXPECT_TRUE(parse_address_block("10.2.0.0/16").block.has_value());
}

TEST(AddressBlockTest, ContainsChecksFamilyLengthAndPrefixBits) {
  const AddressBlock wide = block("10.1.0.0/16");
  EXPECT_TRUE(wide.contains(wide));
  EXPECT_TRUE(wide.contains(block("10.1.5.0/24")));
  EXPECT_TRUE(wide.contains(block("10.1.255.255/32")));
  EXPECT_FALSE(wide.contains(block("10.2.0.0/24")));
  EXPECT_FALSE(wide.contains(block("10.0.0.0/8")));  // Larger.
  EXPECT_FALSE(block("10.1.5.0/24").contains(wide));

  EXPECT_TRUE(block("0.0.0.0/0").contains(block("255.255.255.255/32")));
  EXPECT_FALSE(block("0.0.0.0/0").contains(block("::/128")));
  EXPECT_TRUE(block("::/0").contains(block("2401:e320::/38")));
  EXPECT_FALSE(block("::/0").contains(block("0.0.0.0/32")));

  // Non-byte boundary: a /38 holds the /40s at +0x000 to +0x300 of its third group.
  const AddressBlock v6 = block("2401:e320::/38");
  EXPECT_TRUE(v6.contains(block("2401:e320:300::/40")));
  EXPECT_FALSE(v6.contains(block("2401:e320:400::/40")));
}

TEST(AddressBlockTest, TruncatedKeepsTheLeadingBits) {
  EXPECT_EQ(block("10.1.5.128/25").truncated(16), block("10.1.0.0/16"));
  EXPECT_EQ(block("10.1.5.128/25").truncated(25), block("10.1.5.128/25"));
  EXPECT_EQ(block("10.1.5.128/25").truncated(0), block("0.0.0.0/0"));
  EXPECT_EQ(block("2401:e320:7ff::/48").truncated(38), block("2401:e320:400::/38"));
}

TEST(AddressBlockTest, OrdersByFamilyThenAddressThenLength) {
  EXPECT_LT(block("255.0.0.0/8"), block("::/0"));
  EXPECT_LT(block("10.0.0.0/8"), block("10.0.0.0/16"));
  EXPECT_LT(block("10.0.0.0/16"), block("10.1.0.0/16"));
  EXPECT_NE(block("10.0.0.0/8"), block("10.0.0.0/16"));
}

TEST(AddressBlockIndexTest, FindsTheLongestContainingBlock) {
  const std::vector<AddressBlock> blocks = {block("10.0.0.0/8"),  block("10.1.0.0/16"),
                                            block("10.1.5.0/24"), block("2401:e320::/38"),
                                            block("::/0"),        block("10.1.5.0/25")};
  const AddressBlockIndex index(blocks);
  EXPECT_EQ(index.longest_containing(block("10.1.5.0/26")), std::optional<std::size_t>(5));
  EXPECT_EQ(index.longest_containing(block("10.1.5.128/25")), std::optional<std::size_t>(2));
  EXPECT_EQ(index.longest_containing(block("10.1.5.0/24")), std::optional<std::size_t>(2));
  EXPECT_EQ(index.longest_containing(block("10.1.6.0/24")), std::optional<std::size_t>(1));
  EXPECT_EQ(index.longest_containing(block("10.200.0.0/16")), std::optional<std::size_t>(0));
  EXPECT_EQ(index.longest_containing(block("10.0.0.0/8")), std::optional<std::size_t>(0));
  EXPECT_EQ(index.longest_containing(block("10.0.0.0/7")), std::nullopt);  // Larger than all.
  EXPECT_EQ(index.longest_containing(block("11.0.0.0/24")), std::nullopt);
  EXPECT_EQ(index.longest_containing(block("2401:e320:300::/40")), std::optional<std::size_t>(3));
  EXPECT_EQ(index.longest_containing(block("2401:e320:400::/40")), std::optional<std::size_t>(4));
  EXPECT_EQ(index.longest_containing(block("::/0")), std::optional<std::size_t>(4));
}

TEST(AddressBlockIndexTest, EmptyIndexFindsNothing) {
  const AddressBlockIndex index({});
  EXPECT_EQ(index.longest_containing(block("10.0.0.0/8")), std::nullopt);
  EXPECT_EQ(index.longest_containing(block("::/0")), std::nullopt);
}

}  // namespace
