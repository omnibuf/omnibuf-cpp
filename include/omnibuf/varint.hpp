#pragma once

#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <stdexcept>
#include <vector>

#if defined(_MSC_VER)
#define OMNIBUF_NOINLINE __declspec(noinline)
#define OMNIBUF_RESTRICT __restrict
#elif defined(__GNUC__) || defined(__clang__)
#define OMNIBUF_NOINLINE [[gnu::noinline]]
#define OMNIBUF_RESTRICT __restrict__
#else
#define OMNIBUF_NOINLINE
#define OMNIBUF_RESTRICT
#endif

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
  if constexpr (std::endian::native == std::endian::little) {
    std::memcpy(dst, &v, sizeof(v));
  } else {
    dst[0] = static_cast<uint8_t>(v);
    dst[1] = static_cast<uint8_t>(v >> 8);
  }
}

inline void write_u32_le(uint8_t* dst, uint32_t v) noexcept {
  if constexpr (std::endian::native == std::endian::little) {
    std::memcpy(dst, &v, sizeof(v));
  } else {
    dst[0] = static_cast<uint8_t>(v);
    dst[1] = static_cast<uint8_t>(v >> 8);
    dst[2] = static_cast<uint8_t>(v >> 16);
    dst[3] = static_cast<uint8_t>(v >> 24);
  }
}

inline void write_u64_le(uint8_t* dst, uint64_t v) noexcept {
  if constexpr (std::endian::native == std::endian::little) {
    std::memcpy(dst, &v, sizeof(v));
  } else {
    dst[0] = static_cast<uint8_t>(v);
    dst[1] = static_cast<uint8_t>(v >> 8);
    dst[2] = static_cast<uint8_t>(v >> 16);
    dst[3] = static_cast<uint8_t>(v >> 24);
    dst[4] = static_cast<uint8_t>(v >> 32);
    dst[5] = static_cast<uint8_t>(v >> 40);
    dst[6] = static_cast<uint8_t>(v >> 48);
    dst[7] = static_cast<uint8_t>(v >> 56);
  }
}

inline void append_u32_le(std::vector<uint8_t>& dst, uint32_t v) {
  const size_t pos = dst.size();
  if (pos > SIZE_MAX - 4) [[unlikely]] {
    throw std::length_error("omnibuf: buffer size overflow");
  }
  dst.resize(pos + 4);
  write_u32_le(dst.data() + pos, v);
}

inline void append_u64_le(std::vector<uint8_t>& dst, uint64_t v) {
  const size_t pos = dst.size();
  if (pos > SIZE_MAX - 8) [[unlikely]] {
    throw std::length_error("omnibuf: buffer size overflow");
  }
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

// Fast pointer-based Prefix-Varint reader avoiding std::span::subspan construction
// and prioritizing 1-byte and 2-byte payloads before 4-byte and 9-byte payloads.
[[nodiscard]] inline bool read_varint_fast(const uint8_t* base,
                                           size_t n,
                                           size_t& pos,
                                           uint64_t& out) noexcept {
  if (pos >= n) [[unlikely]] return false;
  const uint8_t b0 = base[pos];
  if ((b0 & 1u) != 0) [[likely]] {
    out = static_cast<uint64_t>(b0 >> 1);
    ++pos;
    return true;
  }
  if ((b0 & 2u) != 0) [[likely]] {
    if (n - pos < 2) [[unlikely]] return false;
    out = static_cast<uint64_t>(read_u16_le(base + pos) >> 2);
    pos += 2;
    return true;
  }
  if ((b0 & 4u) != 0) {
    if (n - pos < 4) [[unlikely]] return false;
    out = static_cast<uint64_t>(read_u32_le(base + pos) >> 3);
    pos += 4;
    return true;
  }
  if (b0 == 0) {
    if (n - pos < 9) [[unlikely]] return false;
    out = read_u64_le(base + pos + 1);
    pos += 9;
    return true;
  }
  return false;
}

// Writes a Prefix-Varint directly into a pre-ensured buffer (requires >= 9 bytes capacity)
// and returns the advanced cursor pointer.
[[nodiscard]] inline uint8_t* write_varint_raw(uint8_t* dst, uint64_t v) noexcept {
  if (v <= 127u) [[likely]] {
    *dst = static_cast<uint8_t>((v << 1) | 1u);
    return dst + 1;
  }
  if (v <= 16383u) [[likely]] {
    write_u16_le(dst, static_cast<uint16_t>((v << 2) | 2u));
    return dst + 2;
  }
  if (v <= 536870911u) {
    write_u32_le(dst, static_cast<uint32_t>((v << 3) | 4u));
    return dst + 4;
  }
  dst[0] = 0x00;
  write_u64_le(dst + 1, v);
  return dst + 9;
}

template <uint64_t TagVal>
inline uint8_t* write_const_varint_raw(uint8_t* dst) noexcept {
  if constexpr (TagVal <= 127u) {
    *dst = static_cast<uint8_t>((TagVal << 1) | 1u);
    return dst + 1;
  } else if constexpr (TagVal <= 16383u) {
    write_u16_le(dst, static_cast<uint16_t>((TagVal << 2) | 2u));
    return dst + 2;
  } else if constexpr (TagVal <= 536870911u) {
    write_u32_le(dst, static_cast<uint32_t>((TagVal << 3) | 4u));
    return dst + 4;
  } else {
    dst[0] = 0x00;
    write_u64_le(dst + 1, TagVal);
    return dst + 9;
  }
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
  if (len_pos > dst.size() || dst.size() - len_pos < 2) [[unlikely]] {
    throw std::logic_error("omnibuf: invalid container length slot");
  }
  const uint64_t sub_len = static_cast<uint64_t>(dst.size() - (len_pos + 2));
  if (sub_len <= 16383u) {
    const uint16_t u = static_cast<uint16_t>((sub_len << 2) | 2u);
    write_u16_le(dst.data() + len_pos, u);
    return;
  }
  if (sub_len <= 536870911u) {
    const size_t old_size = dst.size();
    if (old_size > SIZE_MAX - 2) [[unlikely]] {
      throw std::length_error("omnibuf: buffer size overflow");
    }
    dst.resize(old_size + 2);
    std::memmove(dst.data() + len_pos + 4, dst.data() + len_pos + 2, old_size - (len_pos + 2));
    write_u32_le(dst.data() + len_pos, static_cast<uint32_t>((sub_len << 3) | 4u));
    return;
  }
  const size_t old_size = dst.size();
  if (old_size > SIZE_MAX - 7) [[unlikely]] {
    throw std::length_error("omnibuf: buffer size overflow");
  }
  dst.resize(old_size + 7);
  std::memmove(dst.data() + len_pos + 9, dst.data() + len_pos + 2, old_size - (len_pos + 2));
  dst[len_pos] = 0x00;
  write_u64_le(dst.data() + len_pos + 1, sub_len);
}

// Backpatches a reserved 1-byte Prefix-Varint slot at dst[pos], shifting payload right if value > 127.
inline void patch_reserved_varint(std::vector<uint8_t>& dst, size_t pos, uint64_t value) {
  if (pos >= dst.size()) [[unlikely]] {
    throw std::logic_error("omnibuf: invalid reserved varint slot");
  }
  if (value <= 127u) {
    dst[pos] = static_cast<uint8_t>((value << 1) | 1u);
    return;
  }
  const size_t width = varint_size(value);
  const size_t old_size = dst.size();
  if (old_size > SIZE_MAX - (width - 1)) [[unlikely]] {
    throw std::length_error("omnibuf: buffer size overflow");
  }
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

// Cursor writer over std::vector<uint8_t> with RAII exception rollback.
// Stages writes in a 1 KiB stack buffer and appends to vec via reserve + insert
// so std::vector::resize never zero-fills output memory.
class FastWriter {
 public:
  static constexpr size_t kInlineCap = 1024;

  explicit FastWriter(std::vector<uint8_t>& vec, size_t initial_headroom = 0)
      : vec_(&vec),
        data_(inline_buf_),
        initial_size_(vec.size()),
        flushed_(vec.size()),
        pos_(0),
        limit_(kInlineCap) {
    if (initial_headroom > kInlineCap) {
      if (initial_headroom > SIZE_MAX - initial_size_) [[unlikely]] {
        throw std::length_error("omnibuf: buffer size overflow");
      }
      const size_t target = initial_size_ + initial_headroom;
      if (target > vec.capacity()) {
        vec.reserve(target);
      }
    }
  }

  FastWriter(const FastWriter&) = delete;
  FastWriter& operator=(const FastWriter&) = delete;

  ~FastWriter() noexcept {
    if (!finished_ && vec_ != nullptr && vec_->size() != initial_size_) {
      vec_->resize(initial_size_);
    }
  }

  [[nodiscard]] size_t size() const noexcept { return flushed_ + pos_; }

  [[nodiscard]] uint8_t* raw_cur() noexcept { return data_ + pos_; }

  void set_raw_cur(uint8_t* p) noexcept { pos_ = static_cast<size_t>(p - data_); }

  void finish() {
    if (!finished_) {
      if (!direct_mode_) {
        if (pos_ > 0) {
          vec_->insert(vec_->end(), inline_buf_, inline_buf_ + pos_);
          flushed_ = vec_->size();
          pos_ = 0;
        }
      } else {
        vec_->resize(pos_);
      }
      finished_ = true;
    }
  }

  void rollback() noexcept {
    if (!finished_ && vec_ != nullptr) {
      if (vec_->size() != initial_size_) {
        vec_->resize(initial_size_);
      }
      direct_mode_ = false;
      data_ = inline_buf_;
      flushed_ = initial_size_;
      pos_ = 0;
      limit_ = kInlineCap;
      finished_ = true;
    }
  }

  void reserve_capacity(size_t extra) {
    if (direct_mode_) {
      ensure(extra);
      return;
    }
    const size_t cur = flushed_ + pos_;
    if (extra > SIZE_MAX - cur) [[unlikely]] {
      throw std::length_error("omnibuf: buffer size overflow");
    }
    const size_t target = cur + extra;
    const size_t cap = vec_->capacity();
    if (target > cap) {
      const size_t doubled = (cap <= SIZE_MAX / 2) ? (cap * 2) : target;
      vec_->reserve(doubled > target ? doubled : target);
    }
  }

  void ensure(size_t extra) {
    if (extra > limit_ - pos_) [[unlikely]] {
      flush_or_grow(extra);
    }
  }

  void write_byte(uint8_t b) {
    ensure(1);
    data_[pos_++] = b;
  }

  void write_u16_raw(uint16_t v) {
    ensure(2);
    write_u16_le(data_ + pos_, v);
    pos_ += 2;
  }

  void write_u32_raw(uint32_t v) {
    ensure(4);
    write_u32_le(data_ + pos_, v);
    pos_ += 4;
  }

  void write_u64_raw(uint64_t v) {
    ensure(8);
    write_u64_le(data_ + pos_, v);
    pos_ += 8;
  }

  void write_bytes(const void* src, size_t len) {
    if (len == 0) return;
    if (len > limit_ - pos_) [[unlikely]] {
      write_bytes_slow(src, len);
      return;
    }
    if (!direct_mode_) [[likely]] {
      std::memcpy(data_ + pos_, src, len);
    } else {
      std::memmove(data_ + pos_, src, len);
    }
    pos_ += len;
  }

  void write_bytes_noalias(const void* src, size_t len) {
    if (len == 0) return;
    if (len > limit_ - pos_) [[unlikely]] {
      write_bytes_noalias_slow(src, len);
      return;
    }
    std::memcpy(data_ + pos_, src, len);
    pos_ += len;
  }

  void write_varint(uint64_t v) {
    if (v <= 127u) [[likely]] {
      ensure(1);
      data_[pos_++] = static_cast<uint8_t>((v << 1) | 1u);
      return;
    }
    write_varint_multi(v);
  }

  void write_tag(uint32_t field_num, uint8_t wire_type) {
    const uint64_t tag =
        (static_cast<uint64_t>(field_num) << 4) | static_cast<uint64_t>(wire_type & 0x0Fu);
    write_varint(tag);
  }

  template <uint32_t FieldNum, uint8_t WireType>
  void write_tag_const() {
    constexpr uint64_t kTag =
        (static_cast<uint64_t>(FieldNum) << 4) | static_cast<uint64_t>(WireType & 0x0Fu);
    constexpr size_t kTagLen = varint_size(kTag);
    ensure(kTagLen);
    write_const_varint_raw<kTag>(data_ + pos_);
    pos_ += kTagLen;
  }

  template <uint32_t FieldNum>
  void write_tagged_bool(bool v) {
    constexpr uint64_t kTagFalse =
        (static_cast<uint64_t>(FieldNum) << 4) | static_cast<uint64_t>(WireBoolFalse);
    constexpr uint64_t kTagTrue =
        (static_cast<uint64_t>(FieldNum) << 4) | static_cast<uint64_t>(WireBoolTrue);
    constexpr size_t kTagLen = varint_size(kTagTrue);
    ensure(kTagLen);
    if constexpr (kTagLen == 1) {
      data_[pos_++] = static_cast<uint8_t>(((v ? kTagTrue : kTagFalse) << 1) | 1u);
    } else if constexpr (kTagLen == 2) {
      write_u16_le(data_ + pos_, static_cast<uint16_t>(((v ? kTagTrue : kTagFalse) << 2) | 2u));
      pos_ += 2;
    } else {
      if (v) {
        write_const_varint_raw<kTagTrue>(data_ + pos_);
      } else {
        write_const_varint_raw<kTagFalse>(data_ + pos_);
      }
      pos_ += kTagLen;
    }
  }

  template <uint32_t FieldNum, uint8_t WireType>
  void write_tagged_varint(uint64_t v) {
    constexpr uint64_t kTag =
        (static_cast<uint64_t>(FieldNum) << 4) | static_cast<uint64_t>(WireType & 0x0Fu);
    constexpr size_t kTagLen = varint_size(kTag);
    ensure(kTagLen + 9);
    uint8_t* p = write_const_varint_raw<kTag>(data_ + pos_);
    if (v <= 127u) {
      *p = static_cast<uint8_t>((v << 1) | 1u);
      pos_ += kTagLen + 1;
    } else if (v <= 16383u) {
      write_u16_le(p, static_cast<uint16_t>((v << 2) | 2u));
      pos_ += kTagLen + 2;
    } else if (v <= 536870911u) {
      write_u32_le(p, static_cast<uint32_t>((v << 3) | 4u));
      pos_ += kTagLen + 4;
    } else {
      *p = 0x00;
      write_u64_le(p + 1, v);
      pos_ += kTagLen + 9;
    }
  }

  template <uint32_t FieldNum, uint8_t WireType>
  void write_tagged_u32(uint32_t v) {
    constexpr uint64_t kTag =
        (static_cast<uint64_t>(FieldNum) << 4) | static_cast<uint64_t>(WireType & 0x0Fu);
    constexpr size_t kTagLen = varint_size(kTag);
    ensure(kTagLen + 4);
    uint8_t* p = write_const_varint_raw<kTag>(data_ + pos_);
    write_u32_le(p, v);
    pos_ += kTagLen + 4;
  }

  template <uint32_t FieldNum, uint8_t WireType>
  void write_tagged_u64(uint64_t v) {
    constexpr uint64_t kTag =
        (static_cast<uint64_t>(FieldNum) << 4) | static_cast<uint64_t>(WireType & 0x0Fu);
    constexpr size_t kTagLen = varint_size(kTag);
    ensure(kTagLen + 8);
    uint8_t* p = write_const_varint_raw<kTag>(data_ + pos_);
    write_u64_le(p, v);
    pos_ += kTagLen + 8;
  }

  template <uint32_t FieldNum, uint8_t WireType>
  void write_tagged_bytes_noalias(const void* src, size_t len) {
    constexpr uint64_t kTag =
        (static_cast<uint64_t>(FieldNum) << 4) | static_cast<uint64_t>(WireType & 0x0Fu);
    constexpr size_t kTagLen = varint_size(kTag);
    if (len <= 127u && limit_ - pos_ >= kTagLen + 1 + len) [[likely]] {
      uint8_t* p = write_const_varint_raw<kTag>(data_ + pos_);
      *p++ = static_cast<uint8_t>((len << 1) | 1u);
      if (len > 0) {
        std::memcpy(p, src, len);
      }
      pos_ += kTagLen + 1 + len;
      return;
    }
    write_tag_const<FieldNum, WireType>();
    write_varint(len);
    write_bytes_noalias(src, len);
  }

  template <uint32_t FieldNum, uint8_t WireType>
  void write_tagged_bytes(const void* src, size_t len) {
    constexpr uint64_t kTag =
        (static_cast<uint64_t>(FieldNum) << 4) | static_cast<uint64_t>(WireType & 0x0Fu);
    constexpr size_t kTagLen = varint_size(kTag);
    if (!direct_mode_ && len <= 127u && limit_ - pos_ >= kTagLen + 1 + len) [[likely]] {
      uint8_t* p = write_const_varint_raw<kTag>(data_ + pos_);
      *p++ = static_cast<uint8_t>((len << 1) | 1u);
      if (len > 0) {
        std::memcpy(p, src, len);
      }
      pos_ += kTagLen + 1 + len;
      return;
    }
    write_tag_const<FieldNum, WireType>();
    write_varint(len);
    write_bytes(src, len);
  }

  void write_len_bytes_noalias(const void* src, size_t len) {
    if (len <= 127u && limit_ - pos_ >= 1 + len) [[likely]] {
      uint8_t* p = data_ + pos_;
      *p++ = static_cast<uint8_t>((len << 1) | 1u);
      if (len > 0) {
        std::memcpy(p, src, len);
      }
      pos_ += 1 + len;
      return;
    }
    write_varint(len);
    write_bytes_noalias(src, len);
  }

  void write_container_len_header(size_t body_len) {
    if (body_len <= 16383u) [[likely]] {
      write_u16_raw(static_cast<uint16_t>((body_len << 2) | 2u));
    } else if (body_len <= 536870911u) [[likely]] {
      write_u32_raw(static_cast<uint32_t>((body_len << 3) | 4u));
    } else {
      ensure(9);
      data_[pos_] = 0x00;
      write_u64_le(data_ + pos_ + 1, static_cast<uint64_t>(body_len));
      pos_ += 9;
    }
  }

  [[nodiscard]] size_t reserve_len_2b() {
    ensure(2);
    const size_t p = flushed_ + pos_;
    pos_ += 2;
    return p;
  }

  [[nodiscard]] size_t reserve_len_4b() {
    ensure(4);
    const size_t p = flushed_ + pos_;
    pos_ += 4;
    return p;
  }

  [[nodiscard]] size_t reserve_len_hint(size_t body_hint) {
    if (body_hint > 16383u) [[unlikely]] {
      const size_t pad = (body_hint <= SIZE_MAX - 512u) ? (body_hint + 512u) : body_hint;
      reserve_capacity(pad);
      return reserve_len_4b();
    }
    return reserve_len_2b();
  }

  void backpatch_len(size_t len_pos) {
    const size_t total = flushed_ + pos_;
    if (len_pos > total || total - len_pos < 2) [[unlikely]] {
      throw std::logic_error("omnibuf: invalid container length slot");
    }
    const uint64_t sub_len = static_cast<uint64_t>(total - (len_pos + 2));
    if (sub_len <= 16383u) [[likely]] {
      uint8_t* slot = (len_pos >= flushed_) ? (data_ + (len_pos - flushed_))
                                            : (vec_->data() + len_pos);
      write_u16_le(slot, static_cast<uint16_t>((sub_len << 2) | 2u));
      return;
    }
    backpatch_len_slow(len_pos, sub_len);
  }

  void backpatch_len_4b(size_t len_pos) {
    const size_t total = flushed_ + pos_;
    if (len_pos > total || total - len_pos < 4) [[unlikely]] {
      throw std::logic_error("omnibuf: invalid 4-byte container length slot");
    }
    const uint64_t sub_len = static_cast<uint64_t>(total - (len_pos + 4));
    if (sub_len > 16383u && sub_len <= 536870911u) [[likely]] {
      uint8_t* slot = (len_pos >= flushed_) ? (data_ + (len_pos - flushed_))
                                            : (vec_->data() + len_pos);
      write_u32_le(slot, static_cast<uint32_t>((sub_len << 3) | 4u));
      return;
    }
    backpatch_len_4b_slow(len_pos, sub_len);
  }

  void backpatch_len_hint(size_t len_pos, bool reserved_4b) {
    if (!reserved_4b) [[likely]] {
      backpatch_len(len_pos);
      return;
    }
    backpatch_len_4b(len_pos);
  }

 private:
  void flush_staging() {
    if (pos_ > 0) {
      vec_->insert(vec_->end(), inline_buf_, inline_buf_ + pos_);
      flushed_ = vec_->size();
      pos_ = 0;
    }
  }

  OMNIBUF_NOINLINE void grow_direct(size_t extra) {
    if (extra > SIZE_MAX - pos_) [[unlikely]] {
      throw std::length_error("omnibuf: buffer size overflow");
    }
    const size_t needed = pos_ + extra;
    const size_t padded = (needed <= SIZE_MAX - 256) ? (needed + 256) : needed;
    const size_t doubled = (limit_ <= (SIZE_MAX / 2)) ? (limit_ * 2) : padded;
    const size_t next = (doubled > padded) ? doubled : padded;
    vec_->resize(next);
    data_ = vec_->data();
    limit_ = next;
  }

  OMNIBUF_NOINLINE void flush_or_grow(size_t extra) {
    if (direct_mode_) {
      grow_direct(extra);
      return;
    }
    flush_staging();
    if (extra <= kInlineCap) {
      return;
    }
    // Caller requested a contiguous writable span > kInlineCap via ensure()/raw_cur().
    direct_mode_ = true;
    pos_ = flushed_;
    flushed_ = 0;
    limit_ = vec_->size();
    grow_direct(extra);
  }

  OMNIBUF_NOINLINE void write_bytes_noalias_slow(const void* src, size_t len) {
    if (direct_mode_) {
      grow_direct(len);
      std::memcpy(data_ + pos_, src, len);
      pos_ += len;
      return;
    }
    flush_staging();
    if (len <= kInlineCap) {
      std::memcpy(inline_buf_, src, len);
      pos_ = len;
      return;
    }
    const auto* p = static_cast<const uint8_t*>(src);
    vec_->insert(vec_->end(), p, p + len);
    flushed_ = vec_->size();
  }

  OMNIBUF_NOINLINE void write_bytes_slow(const void* src, size_t len) {
    if (direct_mode_) {
      const auto source = reinterpret_cast<uintptr_t>(src);
      const auto start = reinterpret_cast<uintptr_t>(data_);
      if (source >= start && source - start < limit_) {
        const size_t offset = source - start;
        if (len > limit_ - offset) {
          throw std::out_of_range("omnibuf: aliased source exceeds buffer");
        }
        grow_direct(len);
        std::memmove(data_ + pos_, data_ + offset, len);
        pos_ += len;
        return;
      }
      grow_direct(len);
      std::memcpy(data_ + pos_, src, len);
      pos_ += len;
      return;
    }
    // Check if src aliases the backing vector before flushing/inserting.
    const auto source = reinterpret_cast<uintptr_t>(src);
    const auto vec_start = reinterpret_cast<uintptr_t>(vec_->data());
    const size_t vec_sz = vec_->size();
    if (vec_sz > 0 && source >= vec_start && source - vec_start < vec_sz) {
      const size_t offset = source - vec_start;
      if (len > vec_sz - offset) {
        throw std::out_of_range("omnibuf: aliased source exceeds buffer");
      }
      flush_staging();
      const size_t dst_off = vec_->size();
      if (len > SIZE_MAX - dst_off) [[unlikely]] {
        throw std::length_error("omnibuf: buffer size overflow");
      }
      vec_->resize(dst_off + len);
      std::memmove(vec_->data() + dst_off, vec_->data() + offset, len);
      flushed_ = vec_->size();
      return;
    }
    write_bytes_noalias_slow(src, len);
  }

  inline void write_varint_multi(uint64_t v) {
    if (v <= 16383u) {
      ensure(2);
      write_u16_le(data_ + pos_, static_cast<uint16_t>((v << 2) | 2u));
      pos_ += 2;
      return;
    }
    if (v <= 536870911u) {
      ensure(4);
      write_u32_le(data_ + pos_, static_cast<uint32_t>((v << 3) | 4u));
      pos_ += 4;
      return;
    }
    ensure(9);
    data_[pos_] = 0x00;
    write_u64_le(data_ + pos_ + 1, v);
    pos_ += 9;
  }

  OMNIBUF_NOINLINE void backpatch_len_slow(size_t len_pos, uint64_t sub_len) {
    const size_t extra = (sub_len <= 536870911u) ? 2 : 7;
    if (!direct_mode_) {
      flush_staging();
      const size_t old_size = vec_->size();
      if (extra > SIZE_MAX - old_size) [[unlikely]] {
        throw std::length_error("omnibuf: buffer size overflow");
      }
      vec_->resize(old_size + extra);
      std::memmove(vec_->data() + len_pos + 2 + extra, vec_->data() + len_pos + 2,
                   old_size - (len_pos + 2));
      if (extra == 2) {
        write_u32_le(vec_->data() + len_pos, static_cast<uint32_t>((sub_len << 3) | 4u));
      } else {
        (*vec_)[len_pos] = 0x00;
        write_u64_le(vec_->data() + len_pos + 1, sub_len);
      }
      flushed_ = vec_->size();
      return;
    }
    ensure(extra);
    std::memmove(data_ + len_pos + 2 + extra, data_ + len_pos + 2, pos_ - (len_pos + 2));
    pos_ += extra;
    if (extra == 2) {
      write_u32_le(data_ + len_pos, static_cast<uint32_t>((sub_len << 3) | 4u));
    } else {
      data_[len_pos] = 0x00;
      write_u64_le(data_ + len_pos + 1, sub_len);
    }
  }

  OMNIBUF_NOINLINE void backpatch_len_4b_slow(size_t len_pos, uint64_t sub_len) {
    if (!direct_mode_) {
      flush_staging();
      if (sub_len <= 16383u) {
        write_u16_le(vec_->data() + len_pos, static_cast<uint16_t>((sub_len << 2) | 2u));
        std::memmove(vec_->data() + len_pos + 2, vec_->data() + len_pos + 4,
                     static_cast<size_t>(sub_len));
        vec_->resize(vec_->size() - 2);
        flushed_ = vec_->size();
        return;
      }
      const size_t old_size = vec_->size();
      if (5 > SIZE_MAX - old_size) [[unlikely]] {
        throw std::length_error("omnibuf: buffer size overflow");
      }
      vec_->resize(old_size + 5);
      std::memmove(vec_->data() + len_pos + 9, vec_->data() + len_pos + 4,
                   old_size - (len_pos + 4));
      (*vec_)[len_pos] = 0x00;
      write_u64_le(vec_->data() + len_pos + 1, sub_len);
      flushed_ = vec_->size();
      return;
    }
    if (sub_len <= 16383u) {
      write_u16_le(data_ + len_pos, static_cast<uint16_t>((sub_len << 2) | 2u));
      std::memmove(data_ + len_pos + 2, data_ + len_pos + 4, static_cast<size_t>(sub_len));
      pos_ -= 2;
      return;
    }
    ensure(5);
    std::memmove(data_ + len_pos + 9, data_ + len_pos + 4, pos_ - (len_pos + 4));
    pos_ += 5;
    data_[len_pos] = 0x00;
    write_u64_le(data_ + len_pos + 1, sub_len);
  }

  std::vector<uint8_t>* vec_ = nullptr;
  uint8_t* data_ = nullptr;
  size_t initial_size_ = 0;
  size_t flushed_ = 0;
  size_t pos_ = 0;
  size_t limit_ = 0;
  bool direct_mode_ = false;
  bool finished_ = false;
  alignas(8) uint8_t inline_buf_[kInlineCap];
};

}  // namespace omnibuf
