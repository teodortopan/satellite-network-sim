#include "satsim/ground/address_block.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <utility>

namespace satsim::ground {

namespace {

constexpr int kIpv4Bits = 32;
constexpr int kIpv6Bits = 128;
constexpr std::size_t kMaxQuotedLength = 50;  // Keeps messages short on junk input.

std::string quote(std::string_view text) {
  if (text.size() > kMaxQuotedLength) {
    return "\"" + std::string(text.substr(0, kMaxQuotedLength)) + "...\"";
  }
  return "\"" + std::string(text) + "\"";
}

bool is_digit(char c) { return c >= '0' && c <= '9'; }

// Value of a hex digit, or -1.
int hex_value(char c) {
  if (c >= '0' && c <= '9') {
    return c - '0';
  }
  if (c >= 'a' && c <= 'f') {
    return c - 'a' + 10;
  }
  if (c >= 'A' && c <= 'F') {
    return c - 'A' + 10;
  }
  return -1;
}

// A decimal number without sign or leading zeros (a lone "0" is fine), at most max.
std::optional<int> parse_decimal(std::string_view text, int max) {
  if (text.empty() || text.size() > 3 || (text.size() > 1 && text.front() == '0')) {
    return std::nullopt;
  }
  int value = 0;
  for (const char c : text) {
    if (!is_digit(c)) {
      return std::nullopt;
    }
    value = value * 10 + (c - '0');
  }
  if (value > max) {
    return std::nullopt;
  }
  return value;
}

// Dotted-decimal IPv4 into out[0..3]. Returns an error message, or empty on success.
std::string parse_ipv4(std::string_view text, std::uint8_t* out) {
  constexpr int kOctets = 4;
  constexpr int kMaxOctet = 255;
  for (int i = 0; i < kOctets; ++i) {
    const std::size_t dot = text.find('.');
    const bool last = i == kOctets - 1;
    if (last != (dot == std::string_view::npos)) {
      return "IPv4 address " + quote(text) + " does not have four octets";
    }
    const std::string_view octet = last ? text : text.substr(0, dot);
    const std::optional<int> value = parse_decimal(octet, kMaxOctet);
    if (!value) {
      return "octet " + quote(octet) + " is not a decimal number 0-255 without leading zeros";
    }
    out[i] = static_cast<std::uint8_t>(*value);
    if (!last) {
      text.remove_prefix(dot + 1);
    }
  }
  return {};
}

// Colon-separated hex groups (no "::" inside), appended to groups. When allow_ipv4 is set the
// last piece may be a dotted IPv4 address, which counts as two groups.
std::string parse_groups(std::string_view text, bool allow_ipv4,
                         std::vector<std::uint16_t>& groups) {
  if (text.empty()) {
    return {};
  }
  while (true) {
    const std::size_t colon = text.find(':');
    const std::string_view piece = colon == std::string_view::npos ? text : text.substr(0, colon);
    const bool last = colon == std::string_view::npos;
    if (piece.empty()) {
      return "empty group (a lone ':' at the start or end, or ':::')";
    }
    if (last && allow_ipv4 && piece.find('.') != std::string_view::npos) {
      std::array<std::uint8_t, 4> v4{};
      std::string error = parse_ipv4(piece, v4.data());
      if (!error.empty()) {
        return "embedded " + error;
      }
      groups.push_back(static_cast<std::uint16_t>((v4[0] << 8U) | v4[1]));
      groups.push_back(static_cast<std::uint16_t>((v4[2] << 8U) | v4[3]));
      return {};
    }
    if (piece.size() > 4) {
      return "group " + quote(piece) + " has more than 4 hex digits";
    }
    unsigned value = 0;
    for (const char c : piece) {
      const int digit = hex_value(c);
      if (digit < 0) {
        return "group " + quote(piece) + " is not hexadecimal";
      }
      value = (value << 4U) | static_cast<unsigned>(digit);
    }
    groups.push_back(static_cast<std::uint16_t>(value));
    if (last) {
      return {};
    }
    text.remove_prefix(colon + 1);
  }
}

// RFC 4291 IPv6 text into out. Returns an error message, or empty on success.
std::string parse_ipv6(std::string_view text, AddressBlock::Bytes& out) {
  constexpr std::size_t kGroups = 8;
  if (text.find('%') != std::string_view::npos) {
    return "IPv6 zone index is not allowed";
  }
  const std::size_t gap = text.find("::");
  std::vector<std::uint16_t> head;
  std::vector<std::uint16_t> tail;
  if (gap == std::string_view::npos) {
    std::string error = parse_groups(text, true, head);
    if (!error.empty()) {
      return error;
    }
    if (head.size() != kGroups) {
      return "IPv6 address " + quote(text) + " has " + std::to_string(head.size()) +
             " groups, expected 8 (or fewer with '::')";
    }
  } else {
    const std::string_view after = text.substr(gap + 2);
    if (after.find("::") != std::string_view::npos) {
      return "IPv6 address " + quote(text) + " has more than one '::'";
    }
    std::string error = parse_groups(text.substr(0, gap), false, head);
    if (error.empty()) {
      error = parse_groups(after, true, tail);
    }
    if (!error.empty()) {
      return error;
    }
    // "::" stands for at least one zero group (RFC 4291 section 2.2).
    if (head.size() + tail.size() > kGroups - 1) {
      return "IPv6 address " + quote(text) + " has too many groups for '::'";
    }
  }
  std::vector<std::uint16_t> groups = head;
  groups.resize(kGroups - tail.size(), 0);
  groups.insert(groups.end(), tail.begin(), tail.end());
  for (std::size_t i = 0; i < kGroups; ++i) {
    out.at(2 * i) = static_cast<std::uint8_t>(groups[i] >> 8U);
    out.at((2 * i) + 1) = static_cast<std::uint8_t>(groups[i] & 0xFFU);
  }
  return {};
}

// Keeps the first length bits of bytes and zeroes the rest.
AddressBlock::Bytes mask(const AddressBlock::Bytes& bytes, int length) {
  AddressBlock::Bytes result{};
  const auto whole = static_cast<std::size_t>(length / 8);
  const int rest = length % 8;
  for (std::size_t i = 0; i < whole; ++i) {
    result.at(i) = bytes.at(i);
  }
  if (rest != 0) {
    const auto keep = static_cast<std::uint8_t>(0xFFU << static_cast<unsigned>(8 - rest));
    result.at(whole) = static_cast<std::uint8_t>(bytes.at(whole) & keep);
  }
  return result;
}

std::size_t family_index(AddressFamily family) { return family == AddressFamily::kIpv4 ? 0 : 1; }

}  // namespace

int AddressBlock::max_prefix_length() const noexcept {
  return family_ == AddressFamily::kIpv4 ? kIpv4Bits : kIpv6Bits;
}

bool AddressBlock::contains(const AddressBlock& other) const noexcept {
  return family_ == other.family_ && other.prefix_length_ >= prefix_length_ &&
         mask(other.bytes_, prefix_length_) == bytes_;
}

AddressBlock AddressBlock::truncated(int length) const noexcept {
  return {family_, mask(bytes_, length), length};
}

std::string AddressBlock::to_string() const {
  std::string text;
  if (family_ == AddressFamily::kIpv4) {
    for (std::size_t i = 0; i < 4; ++i) {
      if (i > 0) {
        text += '.';
      }
      text += std::to_string(bytes_.at(i));
    }
  } else {
    constexpr std::size_t kGroups = 8;
    std::array<unsigned, kGroups> groups{};
    for (std::size_t i = 0; i < kGroups; ++i) {
      groups.at(i) = (static_cast<unsigned>(bytes_.at(2 * i)) << 8U) | bytes_.at((2 * i) + 1);
    }
    // Longest run of zero groups, the first if tied; only runs of two or more are shortened.
    std::size_t best_start = kGroups;
    std::size_t best_length = 1;
    for (std::size_t i = 0; i < kGroups;) {
      if (groups.at(i) != 0) {
        ++i;
        continue;
      }
      std::size_t j = i;
      while (j < kGroups && groups.at(j) == 0) {
        ++j;
      }
      if (j - i > best_length) {
        best_start = i;
        best_length = j - i;
      }
      i = j;
    }
    constexpr std::string_view kHex = "0123456789abcdef";
    for (std::size_t i = 0; i < kGroups; ++i) {
      if (i == best_start) {
        text += "::";
        i += best_length - 1;
        continue;
      }
      if (!text.empty() && text.back() != ':') {
        text += ':';
      }
      std::string group;
      for (unsigned value = groups.at(i); value != 0 || group.empty(); value >>= 4U) {
        group.insert(group.begin(), kHex[value & 0xFU]);
      }
      text += group;
    }
  }
  return text + "/" + std::to_string(prefix_length_);
}

AddressBlockParse parse_address_block(std::string_view text) {
  AddressBlockParse result;
  const std::size_t slash = text.find('/');
  if (slash == std::string_view::npos) {
    result.error = quote(text) + " has no '/' prefix length";
    return result;
  }
  const std::string_view address = text.substr(0, slash);
  const std::string_view length_text = text.substr(slash + 1);
  const bool ipv6 = address.find(':') != std::string_view::npos;
  const AddressFamily family = ipv6 ? AddressFamily::kIpv6 : AddressFamily::kIpv4;
  const int max_length = ipv6 ? kIpv6Bits : kIpv4Bits;

  AddressBlock::Bytes bytes{};
  std::string error = ipv6 ? parse_ipv6(address, bytes) : parse_ipv4(address, bytes.data());
  if (!error.empty()) {
    result.error = quote(text) + ": " + error;
    return result;
  }
  const std::optional<int> length = parse_decimal(length_text, max_length);
  if (!length) {
    result.error = quote(text) + ": prefix length " + quote(length_text) +
                   " is not a decimal number 0-" + std::to_string(max_length) +
                   " without leading zeros";
    return result;
  }
  const AddressBlock::Bytes network = mask(bytes, *length);
  if (network != bytes) {
    result.error = quote(text) + ": address has bits set after the prefix (the block would be " +
                   AddressBlock(family, network, *length).to_string() + ")";
    return result;
  }
  result.block = AddressBlock(family, network, *length);
  return result;
}

AddressBlockIndex::AddressBlockIndex(const std::vector<AddressBlock>& blocks) {
  // Family, then decreasing prefix length, to the entries of that level.
  std::array<std::map<int, Level, std::greater<>>, 2> by_length;
  for (std::size_t i = 0; i < blocks.size(); ++i) {
    const AddressBlock& block = blocks[i];
    Level& level = by_length.at(family_index(block.family()))[block.prefix_length()];
    level.prefix_length = block.prefix_length();
    level.entries.emplace_back(block.bytes(), i);
  }
  for (std::size_t f = 0; f < by_length.size(); ++f) {
    for (auto& [length, level] : by_length.at(f)) {
      std::sort(level.entries.begin(), level.entries.end());
      levels_.at(f).push_back(std::move(level));
    }
  }
}

std::optional<std::size_t> AddressBlockIndex::longest_containing(const AddressBlock& query) const {
  for (const Level& level : levels_.at(family_index(query.family()))) {
    if (level.prefix_length > query.prefix_length()) {
      continue;
    }
    const AddressBlock::Bytes key = query.truncated(level.prefix_length).bytes();
    const auto it = std::lower_bound(
        level.entries.begin(), level.entries.end(), key,
        [](const auto& entry, const AddressBlock::Bytes& k) { return entry.first < k; });
    if (it != level.entries.end() && it->first == key) {
      return it->second;
    }
  }
  return std::nullopt;
}

}  // namespace satsim::ground
