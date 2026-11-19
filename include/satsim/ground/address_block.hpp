#pragma once

#include <array>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace satsim::ground {

enum class AddressFamily : std::uint8_t { kIpv4, kIpv6 };

struct AddressBlockParse;

/// An IPv4 or IPv6 address block in CIDR form (network address and prefix length), such as
/// 9.170.0.0/24 or 2401:e320::/38. Bits after the prefix are always zero. Built only by
/// parse_address_block, so every value is valid.
class AddressBlock {
 public:
  /// Network address, most significant byte first. IPv4 uses the first 4 bytes; the rest are 0.
  using Bytes = std::array<std::uint8_t, 16>;

  [[nodiscard]] AddressFamily family() const noexcept { return family_; }
  /// 0 to 32 (IPv4) or 0 to 128 (IPv6).
  [[nodiscard]] int prefix_length() const noexcept { return prefix_length_; }
  [[nodiscard]] const Bytes& bytes() const noexcept { return bytes_; }

  /// 32 or 128.
  [[nodiscard]] int max_prefix_length() const noexcept;

  /// True if every address of other is in this block: same family, other's prefix is at least
  /// as long, and the first prefix_length() bits agree. A block contains itself.
  [[nodiscard]] bool contains(const AddressBlock& other) const noexcept;

  /// The enclosing block of the given length. Requires 0 <= length <= prefix_length().
  [[nodiscard]] AddressBlock truncated(int length) const noexcept;

  /// Canonical text: dotted decimal for IPv4; for IPv6 the RFC 5952 form (lower-case hex,
  /// no leading zeros, the longest run of two or more zero groups as "::", the first if
  /// tied; no embedded IPv4 notation). Followed by "/" and the prefix length.
  [[nodiscard]] std::string to_string() const;

  /// Orders by family, then network address, then prefix length.
  friend auto operator<=>(const AddressBlock&, const AddressBlock&) = default;

 private:
  friend AddressBlockParse parse_address_block(std::string_view text);
  AddressBlock(AddressFamily family, const Bytes& bytes, int prefix_length) noexcept
      : family_(family), bytes_(bytes), prefix_length_(prefix_length) {}

  AddressFamily family_;
  Bytes bytes_;
  int prefix_length_;
};

/// The result of parse_address_block: a block, or a message saying what is wrong.
struct AddressBlockParse {
  std::optional<AddressBlock> block;
  std::string error;  ///< Empty when block is set.
};

/// Parses "address/length". IPv4: four decimal octets 0-255 without leading zeros, length
/// 0-32. IPv6: RFC 4291 text (hex groups of 1-4 digits, at most one "::", optionally a dotted
/// IPv4 in the last 32 bits, any letter case, no zone index), length 0-128. The length is
/// decimal without sign or leading zeros. Rejects a block whose address has bits set after
/// the prefix (e.g. 10.0.0.1/24): such a line is a typo, not a block. No surrounding spaces.
[[nodiscard]] AddressBlockParse parse_address_block(std::string_view text);

/// Finds, for a query block, the longest indexed block that contains it. Built once from a
/// list of distinct blocks; a lookup tries each indexed prefix length of the query's family,
/// longest first, with a binary search at each length.
class AddressBlockIndex {
 public:
  /// blocks must be distinct (equal blocks are rejected by the loaders). Lookups return
  /// positions in this vector.
  explicit AddressBlockIndex(const std::vector<AddressBlock>& blocks);

  /// Position (in the constructor's vector) of the longest block containing query, or
  /// nothing if no block contains it.
  [[nodiscard]] std::optional<std::size_t> longest_containing(const AddressBlock& query) const;

 private:
  struct Level {
    int prefix_length = 0;
    std::vector<std::pair<AddressBlock::Bytes, std::size_t>> entries;  // Sorted by bytes.
  };
  // Per family (index by AddressFamily), levels by decreasing prefix length.
  std::array<std::vector<Level>, 2> levels_;
};

}  // namespace satsim::ground
