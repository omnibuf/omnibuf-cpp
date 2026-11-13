#pragma once

#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <vector>

namespace omnibuf {

// Explicit 4-bit Wire Types (0..15) encoded in the low 4 bits of the field tag:
// Tag = (FieldNumber << 4) | WireType
inline constexpr uint8_t WireUint = 0;
inline constexpr uint8_t WireSint = 1;
inline constexpr uint8_t WireBoolFalse = 2;
inline constexpr uint8_t WireBoolTrue = 3;
inline constexpr uint8_t WireFixed32 = 4;
inline constexpr uint8_t WireFixed64 = 5;
inline constexpr uint8_t WireSfixed32 = 6;
inline constexpr uint8_t WireSfixed64 = 7;
inline constexpr uint8_t WireFloat32 = 8;
inline constexpr uint8_t WireFloat64 = 9;
inline constexpr uint8_t WireString = 10;
inline constexpr uint8_t WireBytes = 11;
inline constexpr uint8_t WireSubMsg = 12;
inline constexpr uint8_t WireArray = 13;
inline constexpr uint8_t WireMap = 14;
inline constexpr uint8_t WireExtended = 15;

// Maximum allowed container/message nesting depth across decoders and validators.
inline constexpr int MAX_CONTAINER_DEPTH = 32;
inline constexpr int kMaxNestingDepth = MAX_CONTAINER_DEPTH;

// Maximum 29-bit Protobuf field number.
inline constexpr uint32_t MAX_FIELD_NUMBER = 536870911u;

[[nodiscard]] constexpr uint64_t encode_zigzag(int64_t n) noexcept {
  return (static_cast<uint64_t>(n) << 1) ^
         static_cast<uint64_t>(n < 0 ? ~uint64_t{0} : uint64_t{0});
}

[[nodiscard]] constexpr int64_t decode_zigzag(uint64_t u) noexcept {
  return static_cast<int64_t>((u >> 1) ^ (uint64_t{0} - (u & 1u)));
}

[[nodiscard]] inline uint16_t read_u16_le(const uint8_t* p) noexcept {
  if constexpr (std::endian::native == std::endian::little) {
    uint16_t v = 0;
    std::memcpy(&v, p, sizeof(v));
    return v;
  } else {
    return static_cast<uint16_t>(p[0]) | (static_cast<uint16_t>(p[1]) << 8);
  }
}

[[nodiscard]] inline uint32_t read_u32_le(const uint8_t* p) noexcept {
  if constexpr (std::endian::native == std::endian::little) {
    uint32_t v = 0;
    std::memcpy(&v, p, sizeof(v));
    return v;
  } else {
    return static_cast<uint32_t>(p[0]) |
           (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) |
           (static_cast<uint32_t>(p[3]) << 24);
  }
}

[[nodiscard]] inline uint64_t read_u64_le(const uint8_t* p) noexcept {
  if constexpr (std::endian::native == std::endian::little) {
    uint64_t v = 0;
    std::memcpy(&v, p, sizeof(v));
    return v;
  } else {
    return static_cast<uint64_t>(p[0]) |
           (static_cast<uint64_t>(p[1]) << 8) |
           (static_cast<uint64_t>(p[2]) << 16) |
           (static_cast<uint64_t>(p[3]) << 24) |
           (static_cast<uint64_t>(p[4]) << 32) |
           (static_cast<uint64_t>(p[5]) << 40) |
           (static_cast<uint64_t>(p[6]) << 48) |
           (static_cast<uint64_t>(p[7]) << 56);
  }
}

inline void write_u16_le(uint8_t* dst, uint16_t v) noexcept {
  dst[0] = static_cast<uint8_t>(v);
  dst[1] = static_cast<uint8_t>(v >> 8);
}

inline void write_u32_le(uint8_t* dst, uint32_t v) noexcept {
  dst[0] = static_cast<uint8_t>(v);
  dst[1] = static_cast<uint8_t>(v >> 8);
  dst[2] = static_cast<uint8_t>(v >> 16);
  dst[3] = static_cast<uint8_t>(v >> 24);
}

inline void write_u64_le(uint8_t* dst, uint64_t v) noexcept {
  dst[0] = static_cast<uint8_t>(v);
  dst[1] = static_cast<uint8_t>(v >> 8);
  dst[2] = static_cast<uint8_t>(v >> 16);
  dst[3] = static_cast<uint8_t>(v >> 24);
  dst[4] = static_cast<uint8_t>(v >> 32);
  dst[5] = static_cast<uint8_t>(v >> 40);
  dst[6] = static_cast<uint8_t>(v >> 48);
  dst[7] = static_cast<uint8_t>(v >> 56);
}

inline void append_u32_le(std::vector<uint8_t>& dst, uint32_t v) {
  const size_t pos = dst.size();
  dst.resize(pos + 4);
  write_u32_le(dst.data() + pos, v);
}

inline void append_u64_le(std::vector<uint8_t>& dst, uint64_t v) {
  const size_t pos = dst.size();
  dst.resize(pos + 8);
  write_u64_le(dst.data() + pos, v);
}

[[nodiscard]] constexpr uint32_t float32_to_bits(float f) noexcept {
  return std::bit_cast<uint32_t>(f);
}

[[nodiscard]] constexpr float bits_to_float32(uint32_t b) noexcept {
  return std::bit_cast<float>(b);
}

[[nodiscard]] constexpr uint64_t float64_to_bits(double d) noexcept {
  return std::bit_cast<uint64_t>(d);
}

[[nodiscard]] constexpr double bits_to_float64(uint64_t b) noexcept {
  return std::bit_cast<double>(b);
}

struct VarintResult {
  uint64_t value = 0;
  size_t bytes_read = 0;
};

// Decodes a Prefix-Varint using std::countr_zero on the leading byte:
// - tz == 0 (b0 & 1): 1-byte varint (7-bit payload)
// - tz == 1 (b0 & 2): 2-byte varint (14-bit payload)
// - tz == 2 (b0 & 4): 4-byte varint (29-bit payload)
// - tz == 8 (b0 == 0): 9-byte varint (64-bit payload)
// - tz in 3..7 ((b0 & 7) == 0 && b0 != 0): reserved invalid prefix (bytes_read = 0)
[[nodiscard]] inline VarintResult decode_varint(std::span<const uint8_t> b) noexcept {
  if (b.empty()) {
    return {};
  }
  const uint8_t b0 = b[0];
  const int tz = std::countr_zero(b0);
  switch (tz) {
    case 0:
      return {static_cast<uint64_t>(b0 >> 1), 1};
    case 1:
      if (b.size() >= 2) {
        return {static_cast<uint64_t>(read_u16_le(b.data()) >> 2), 2};
      }
      return {};
    case 2:
      if (b.size() >= 4) {
        return {static_cast<uint64_t>(read_u32_le(b.data()) >> 3), 4};
      }
      return {};
    case 8:
      if (b.size() >= 9) {
        return {read_u64_le(b.data() + 1), 9};
      }
      return {};
    default:
      return {};
  }
}

[[nodiscard]] constexpr size_t varint_size(uint64_t v) noexcept {
  if (v <= 127u) return 1;
  if (v <= 16383u) return 2;
  if (v <= 536870911u) return 4;
  return 9;
}

inline void append_varint_multi(std::vector<uint8_t>& dst, uint64_t v) {
  if (v <= 16383u) {
    const uint16_t u = static_cast<uint16_t>((v << 2) | 2u);
    dst.push_back(static_cast<uint8_t>(u));
    dst.push_back(static_cast<uint8_t>(u >> 8));
    return;
  }
  if (v <= 536870911u) {
    const uint32_t u = static_cast<uint32_t>((v << 3) | 4u);
    append_u32_le(dst, u);
    return;
  }
  dst.push_back(0x00);
  append_u64_le(dst, v);
}

inline void append_varint(std::vector<uint8_t>& dst, uint64_t v) {
  if (v <= 127u) {
    dst.push_back(static_cast<uint8_t>((v << 1) | 1u));
    return;
  }
  append_varint_multi(dst, v);
}

inline void append_tag(std::vector<uint8_t>& dst, uint32_t field_num, uint8_t wire_type) {
  const uint64_t tag = (static_cast<uint64_t>(field_num) << 4) | static_cast<uint64_t>(wire_type & 0x0Fu);
  append_varint(dst, tag);
}

// Backpatches a reserved 2-byte Prefix-Varint length slot at dst[len_pos..len_pos+2].
// Expands in-place to 4 bytes or 9 bytes when payload exceeds 16,383 bytes.
inline void backpatch_container_len(std::vector<uint8_t>& dst, size_t len_pos) {
  const uint64_t sub_len = static_cast<uint64_t>(dst.size() - (len_pos + 2));
  if (sub_len <= 16383u) {
    const uint16_t u = static_cast<uint16_t>((sub_len << 2) | 2u);
    write_u16_le(dst.data() + len_pos, u);
    return;
  }
  if (sub_len <= 536870911u) {
    const size_t old_size = dst.size();
    dst.resize(old_size + 2);
    std::memmove(dst.data() + len_pos + 4, dst.data() + len_pos + 2, old_size - (len_pos + 2));
    write_u32_le(dst.data() + len_pos, static_cast<uint32_t>((sub_len << 3) | 4u));
    return;
  }
  const size_t old_size = dst.size();
  dst.resize(old_size + 7);
  std::memmove(dst.data() + len_pos + 9, dst.data() + len_pos + 2, old_size - (len_pos + 2));
  dst[len_pos] = 0x00;
  write_u64_le(dst.data() + len_pos + 1, sub_len);
}

// Backpatches a reserved 1-byte Prefix-Varint slot at dst[pos], shifting payload right if value > 127.
inline void patch_reserved_varint(std::vector<uint8_t>& dst, size_t pos, uint64_t value) {
  if (value <= 127u) {
    dst[pos] = static_cast<uint8_t>((value << 1) | 1u);
    return;
  }
  const size_t width = varint_size(value);
  const size_t old_size = dst.size();
  dst.resize(old_size + (width - 1));
  std::memmove(dst.data() + pos + width, dst.data() + pos + 1, old_size - (pos + 1));
  if (width == 2) {
    write_u16_le(dst.data() + pos, static_cast<uint16_t>((value << 2) | 2u));
  } else if (width == 4) {
    write_u32_le(dst.data() + pos, static_cast<uint32_t>((value << 3) | 4u));
  } else {
    dst[pos] = 0x00;
    write_u64_le(dst.data() + pos + 1, value);
  }
}

}  // namespace omnibuf
