#pragma once

#include <algorithm>
#include <array>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <map>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <vector>

#include "omnibuf/utf8.hpp"
#include "omnibuf/varint.hpp"

namespace omnibuf {

namespace detail {

template <typename T>
concept IntOrUint = std::integral<T> && !std::same_as<T, bool>;

}  // namespace detail

inline void encode_string_arr(std::vector<uint8_t>& dst, std::span<const std::string> items) {
  for (const auto& s : items) {
    if (!is_valid_utf8(s)) [[unlikely]] {
      throw std::invalid_argument("omnibuf: invalid UTF-8 in WireString");
    }
  }
  const size_t len_pos = dst.size();
  dst.push_back(0);
  dst.push_back(0);
  dst.push_back(WireString);
  append_varint(dst, items.size());
  for (const auto& s : items) {
    append_varint(dst, s.size());
    dst.insert(dst.end(), s.begin(), s.end());
  }
  backpatch_container_len(dst, len_pos);
}

inline void encode_bytes_arr(std::vector<uint8_t>& dst,
                             std::span<const std::vector<uint8_t>> items) {
  const size_t len_pos = dst.size();
  dst.push_back(0);
  dst.push_back(0);
  dst.push_back(WireBytes);
  append_varint(dst, items.size());
  for (const auto& b : items) {
    append_varint(dst, b.size());
    dst.insert(dst.end(), b.begin(), b.end());
  }
  backpatch_container_len(dst, len_pos);
}

inline void encode_bool_arr(std::vector<uint8_t>& dst, const std::vector<bool>& items) {
  const size_t len_pos = dst.size();
  dst.push_back(0);
  dst.push_back(0);
  dst.push_back(WireBoolFalse);
  append_varint(dst, items.size());
  for (bool v : items) {
    dst.push_back(v ? uint8_t{1} : uint8_t{0});
  }
  backpatch_container_len(dst, len_pos);
}

template <detail::IntOrUint T>
inline void encode_uint_arr(std::vector<uint8_t>& dst, std::span<const T> items) {
  const size_t len_pos = dst.size();
  dst.push_back(0);
  dst.push_back(0);
  dst.push_back(WireUint);
  append_varint(dst, items.size());
  for (T v : items) {
    append_varint(dst, static_cast<uint64_t>(v));
  }
  backpatch_container_len(dst, len_pos);
}

template <detail::IntOrUint T>
inline void encode_sint_arr(std::vector<uint8_t>& dst, std::span<const T> items) {
  const size_t len_pos = dst.size();
  dst.push_back(0);
  dst.push_back(0);
  dst.push_back(WireSint);
  append_varint(dst, items.size());
  for (T v : items) {
    append_varint(dst, encode_zigzag(static_cast<int64_t>(v)));
  }
  backpatch_container_len(dst, len_pos);
}

template <detail::IntOrUint T>
inline void encode_fixed32_arr(std::vector<uint8_t>& dst,
                               std::span<const T> items,
                               uint8_t elem_wt = WireFixed32) {
  if (items.size() > SIZE_MAX / 4) [[unlikely]] {
    throw std::length_error("omnibuf: array byte size overflow");
  }
  const size_t len_pos = dst.size();
  dst.push_back(0);
  dst.push_back(0);
  dst.push_back(elem_wt);
  append_varint(dst, items.size());
  if constexpr (std::endian::native == std::endian::little && sizeof(T) == 4) {
    if (!items.empty()) {
      const size_t pos = dst.size();
      dst.resize(pos + items.size() * 4);
      std::memcpy(dst.data() + pos, items.data(), items.size() * 4);
    }
  } else {
    for (T v : items) {
      append_u32_le(dst, static_cast<uint32_t>(v));
    }
  }
  backpatch_container_len(dst, len_pos);
}

template <detail::IntOrUint T>
inline void encode_fixed64_arr(std::vector<uint8_t>& dst,
                               std::span<const T> items,
                               uint8_t elem_wt = WireFixed64) {
  if (items.size() > SIZE_MAX / 8) [[unlikely]] {
    throw std::length_error("omnibuf: array byte size overflow");
  }
  const size_t len_pos = dst.size();
  dst.push_back(0);
  dst.push_back(0);
  dst.push_back(elem_wt);
  append_varint(dst, items.size());
  if constexpr (std::endian::native == std::endian::little && sizeof(T) == 8) {
    if (!items.empty()) {
      const size_t pos = dst.size();
      dst.resize(pos + items.size() * 8);
      std::memcpy(dst.data() + pos, items.data(), items.size() * 8);
    }
  } else {
    for (T v : items) {
      append_u64_le(dst, static_cast<uint64_t>(v));
    }
  }
  backpatch_container_len(dst, len_pos);
}

inline void encode_float32_arr(std::vector<uint8_t>& dst, std::span<const float> items) {
  if (items.size() > SIZE_MAX / 4) [[unlikely]] {
    throw std::length_error("omnibuf: array byte size overflow");
  }
  const size_t len_pos = dst.size();
  dst.push_back(0);
  dst.push_back(0);
  dst.push_back(WireFloat32);
  append_varint(dst, items.size());
  if constexpr (std::endian::native == std::endian::little && sizeof(float) == 4) {
    if (!items.empty()) {
      const size_t pos = dst.size();
      dst.resize(pos + items.size() * 4);
      std::memcpy(dst.data() + pos, items.data(), items.size() * 4);
    }
  } else {
    for (float v : items) {
      append_u32_le(dst, float32_to_bits(v));
    }
  }
  backpatch_container_len(dst, len_pos);
}

inline void encode_float64_arr(std::vector<uint8_t>& dst, std::span<const double> items) {
  if (items.size() > SIZE_MAX / 8) [[unlikely]] {
    throw std::length_error("omnibuf: array byte size overflow");
  }
  const size_t len_pos = dst.size();
  dst.push_back(0);
  dst.push_back(0);
  dst.push_back(WireFloat64);
  append_varint(dst, items.size());
  if constexpr (std::endian::native == std::endian::little && sizeof(double) == 8) {
    if (!items.empty()) {
      const size_t pos = dst.size();
      dst.resize(pos + items.size() * 8);
      std::memcpy(dst.data() + pos, items.data(), items.size() * 8);
    }
  } else {
    for (double v : items) {
      append_u64_le(dst, float64_to_bits(v));
    }
  }
  backpatch_container_len(dst, len_pos);
}

// std::map<std::string, uint64_t> is already ordered lexicographically.
inline void encode_map_string_uint(std::vector<uint8_t>& dst,
                                   const std::map<std::string, uint64_t>& m) {
  for (const auto& [k, _] : m) {
    if (!is_valid_utf8(k)) [[unlikely]] {
      throw std::invalid_argument("omnibuf: invalid UTF-8 in WireString");
    }
  }
  const size_t len_pos = dst.size();
  dst.push_back(0);
  dst.push_back(0);
  dst.push_back(WireString);
  dst.push_back(WireUint);
  append_varint(dst, m.size());
  for (const auto& [k, v] : m) {
    append_varint(dst, k.size());
    dst.insert(dst.end(), k.begin(), k.end());
    append_varint(dst, v);
  }
  backpatch_container_len(dst, len_pos);
}

inline void encode_map_string_uint(std::vector<uint8_t>& dst,
                                   const std::unordered_map<std::string, uint64_t>& m,
                                   bool deterministic = true) {
  for (const auto& [k, _] : m) {
    if (!is_valid_utf8(k)) [[unlikely]] {
      throw std::invalid_argument("omnibuf: invalid UTF-8 in WireString");
    }
  }
  const size_t len_pos = dst.size();
  dst.push_back(0);
  dst.push_back(0);
  dst.push_back(WireString);
  dst.push_back(WireUint);
  append_varint(dst, m.size());
  if (deterministic && m.size() > 1) {
    std::vector<std::string_view> keys;
    keys.reserve(m.size());
    for (const auto& [k, _] : m) {
      keys.emplace_back(k);
    }
    std::sort(keys.begin(), keys.end());
    for (std::string_view k : keys) {
      append_varint(dst, k.size());
      dst.insert(dst.end(), k.begin(), k.end());
      append_varint(dst, m.find(std::string(k))->second);
    }
  } else {
    for (const auto& [k, v] : m) {
      append_varint(dst, k.size());
      dst.insert(dst.end(), k.begin(), k.end());
      append_varint(dst, v);
    }
  }
  backpatch_container_len(dst, len_pos);
}

// FastWriter overloads for register-resident generated and struct encoding.
[[nodiscard]] inline const char* try_encode_string_arr(FastWriter& w,
                                                       std::span<const std::string> items) {
  size_t body_hint = 1 + varint_size(items.size());
  for (const auto& s : items) {
    body_hint += varint_size(s.size()) + s.size();
  }
  const bool use_4b = body_hint > 16383u;
  const size_t len_pos = w.reserve_len_hint(body_hint);
  w.write_byte(WireString);
  w.write_varint(items.size());
  for (const auto& s : items) {
    if (!is_valid_utf8(s)) [[unlikely]] {
      return "omnibuf: invalid UTF-8 in WireString";
    }
    w.write_varint(s.size());
    w.write_bytes(s.data(), s.size());
  }
  w.backpatch_len_hint(len_pos, use_4b);
  return nullptr;
}

inline void encode_string_arr(FastWriter& w, std::span<const std::string> items) {
  if (const char* err = try_encode_string_arr(w, items)) [[unlikely]] {
    throw std::invalid_argument(err);
  }
}

inline void encode_bytes_arr(FastWriter& w, std::span<const std::vector<uint8_t>> items) {
  size_t body_hint = 1 + varint_size(items.size());
  for (const auto& b : items) {
    body_hint += varint_size(b.size()) + b.size();
  }
  const bool use_4b = body_hint > 16383u;
  const size_t len_pos = w.reserve_len_hint(body_hint);
  w.write_byte(WireBytes);
  w.write_varint(items.size());
  for (const auto& b : items) {
    w.write_varint(b.size());
    w.write_bytes(b.data(), b.size());
  }
  w.backpatch_len_hint(len_pos, use_4b);
}

inline void encode_bool_arr(FastWriter& w, const std::vector<bool>& items) {
  const size_t body_hint = 1 + varint_size(items.size()) + items.size();
  const bool use_4b = body_hint > 16383u;
  const size_t len_pos = w.reserve_len_hint(body_hint);
  w.write_byte(WireBoolFalse);
  w.write_varint(items.size());
  w.ensure(items.size());
  for (bool v : items) {
    w.write_byte(v ? uint8_t{1} : uint8_t{0});
  }
  w.backpatch_len_hint(len_pos, use_4b);
}

template <detail::IntOrUint T>
inline void encode_uint_arr(FastWriter& w, std::span<const T> items) {
  const size_t body_hint = 1 + varint_size(items.size()) + items.size();
  const bool use_4b = body_hint > 16383u;
  const size_t len_pos = w.reserve_len_hint(body_hint);
  w.write_byte(WireUint);
  w.write_varint(items.size());
  for (T v : items) {
    w.write_varint(static_cast<uint64_t>(v));
  }
  w.backpatch_len_hint(len_pos, use_4b);
}

template <detail::IntOrUint T>
inline void encode_sint_arr(FastWriter& w, std::span<const T> items) {
  const size_t body_hint = 1 + varint_size(items.size()) + items.size();
  const bool use_4b = body_hint > 16383u;
  const size_t len_pos = w.reserve_len_hint(body_hint);
  w.write_byte(WireSint);
  w.write_varint(items.size());
  for (T v : items) {
    w.write_varint(encode_zigzag(static_cast<int64_t>(v)));
  }
  w.backpatch_len_hint(len_pos, use_4b);
}

template <detail::IntOrUint T>
inline void encode_fixed32_arr(FastWriter& w,
                               std::span<const T> items,
                               uint8_t elem_wt = WireFixed32) {
  if (items.size() > SIZE_MAX / 4) [[unlikely]] {
    throw std::length_error("omnibuf: array byte size overflow");
  }
  const size_t byte_len = items.size() * 4;
  const size_t body_hint = 1 + varint_size(items.size()) + byte_len;
  const bool use_4b = body_hint > 16383u;
  const size_t len_pos = w.reserve_len_hint(body_hint);
  w.write_byte(elem_wt);
  w.write_varint(items.size());
  if constexpr (std::endian::native == std::endian::little && sizeof(T) == 4) {
    w.write_bytes(items.data(), byte_len);
  } else {
    w.ensure(byte_len);
    for (T v : items) {
      w.write_u32_raw(static_cast<uint32_t>(v));
    }
  }
  w.backpatch_len_hint(len_pos, use_4b);
}

template <detail::IntOrUint T>
inline void encode_fixed64_arr(FastWriter& w,
                               std::span<const T> items,
                               uint8_t elem_wt = WireFixed64) {
  if (items.size() > SIZE_MAX / 8) [[unlikely]] {
    throw std::length_error("omnibuf: array byte size overflow");
  }
  const size_t byte_len = items.size() * 8;
  const size_t body_hint = 1 + varint_size(items.size()) + byte_len;
  const bool use_4b = body_hint > 16383u;
  const size_t len_pos = w.reserve_len_hint(body_hint);
  w.write_byte(elem_wt);
  w.write_varint(items.size());
  if constexpr (std::endian::native == std::endian::little && sizeof(T) == 8) {
    w.write_bytes(items.data(), byte_len);
  } else {
    w.ensure(byte_len);
    for (T v : items) {
      w.write_u64_raw(static_cast<uint64_t>(v));
    }
  }
  w.backpatch_len_hint(len_pos, use_4b);
}

inline void encode_float32_arr(FastWriter& w, std::span<const float> items) {
  if (items.size() > SIZE_MAX / 4) [[unlikely]] {
    throw std::length_error("omnibuf: array byte size overflow");
  }
  const size_t byte_len = items.size() * 4;
  const size_t body_hint = 1 + varint_size(items.size()) + byte_len;
  const bool use_4b = body_hint > 16383u;
  const size_t len_pos = w.reserve_len_hint(body_hint);
  w.write_byte(WireFloat32);
  w.write_varint(items.size());
  if constexpr (std::endian::native == std::endian::little && sizeof(float) == 4) {
    w.write_bytes(items.data(), byte_len);
  } else {
    w.ensure(byte_len);
    for (float v : items) {
      w.write_u32_raw(float32_to_bits(v));
    }
  }
  w.backpatch_len_hint(len_pos, use_4b);
}

inline void encode_float64_arr(FastWriter& w, std::span<const double> items) {
  if (items.size() > SIZE_MAX / 8) [[unlikely]] {
    throw std::length_error("omnibuf: array byte size overflow");
  }
  const size_t byte_len = items.size() * 8;
  const size_t body_hint = 1 + varint_size(items.size()) + byte_len;
  const bool use_4b = body_hint > 16383u;
  const size_t len_pos = w.reserve_len_hint(body_hint);
  w.write_byte(WireFloat64);
  w.write_varint(items.size());
  if constexpr (std::endian::native == std::endian::little && sizeof(double) == 8) {
    w.write_bytes(items.data(), byte_len);
  } else {
    w.ensure(byte_len);
    for (double v : items) {
      w.write_u64_raw(float64_to_bits(v));
    }
  }
  w.backpatch_len_hint(len_pos, use_4b);
}

template <uint8_t WT, typename T>
[[nodiscard]] inline const char* try_encode_container_scalar(FastWriter& w, const T& val) {
  if constexpr (WT == WireUint) {
    w.write_varint(static_cast<uint64_t>(val));
    return nullptr;
  } else if constexpr (WT == WireSint) {
    w.write_varint(encode_zigzag(static_cast<int64_t>(val)));
    return nullptr;
  } else if constexpr (WT == WireBoolFalse) {
    w.write_byte(val ? uint8_t{1} : uint8_t{0});
    return nullptr;
  } else if constexpr (WT == WireFixed32 || WT == WireSfixed32) {
    w.write_u32_raw(static_cast<uint32_t>(val));
    return nullptr;
  } else if constexpr (WT == WireFloat32) {
    w.write_u32_raw(float32_to_bits(val));
    return nullptr;
  } else if constexpr (WT == WireFixed64 || WT == WireSfixed64) {
    w.write_u64_raw(static_cast<uint64_t>(val));
    return nullptr;
  } else if constexpr (WT == WireFloat64) {
    w.write_u64_raw(float64_to_bits(val));
    return nullptr;
  } else if constexpr (WT == WireString) {
    if (!is_valid_utf8(val)) [[unlikely]] {
      return "omnibuf: invalid UTF-8 in WireString";
    }
    w.write_varint(val.size());
    w.write_bytes(val.data(), val.size());
    return nullptr;
  } else if constexpr (WT == WireBytes) {
    w.write_varint(val.size());
    w.write_bytes(val.data(), val.size());
    return nullptr;
  } else {
    return "omnibuf: unsupported scalar container wire type";
  }
}

template <uint8_t KeyWT, uint8_t ValWT, typename MapT>
[[nodiscard]] inline const char* try_encode_scalar_map(FastWriter& w, const MapT& m) {
  size_t body_hint = 2 + varint_size(m.size());
  for (const auto& [k, v] : m) {
    (void)k;
    (void)v;
    if constexpr (KeyWT == WireString || KeyWT == WireBytes) {
      body_hint += varint_size(k.size()) + k.size();
    } else if constexpr (KeyWT == WireFixed32 || KeyWT == WireSfixed32) {
      body_hint += 4;
    } else if constexpr (KeyWT == WireFixed64 || KeyWT == WireSfixed64) {
      body_hint += 8;
    } else {
      body_hint += 1;
    }
    if constexpr (ValWT == WireString || ValWT == WireBytes) {
      body_hint += varint_size(v.size()) + v.size();
    } else if constexpr (ValWT == WireFixed32 || ValWT == WireSfixed32 || ValWT == WireFloat32) {
      body_hint += 4;
    } else if constexpr (ValWT == WireFixed64 || ValWT == WireSfixed64 || ValWT == WireFloat64) {
      body_hint += 8;
    } else {
      body_hint += 1;
    }
  }
  const bool use_4b = body_hint > 16383u;
  const size_t len_pos = w.reserve_len_hint(body_hint);
  w.write_byte(KeyWT);
  w.write_byte(ValWT);
  w.write_varint(m.size());
  for (const auto& [k, v] : m) {
    if (const char* err = try_encode_container_scalar<KeyWT>(w, k)) [[unlikely]] {
      return err;
    }
    if (const char* err = try_encode_container_scalar<ValWT>(w, v)) [[unlikely]] {
      return err;
    }
  }
  w.backpatch_len_hint(len_pos, use_4b);
  return nullptr;
}

template <uint8_t KeyWT, typename MapT, typename SubEncodeFn>
[[nodiscard]] inline const char* try_encode_submsg_map(FastWriter& w,
                                                       const MapT& m,
                                                       SubEncodeFn&& encode_sub) {
  size_t body_hint = 2 + varint_size(m.size());
  for (const auto& [k, v] : m) {
    (void)k;
    (void)v;
    if constexpr (KeyWT == WireString || KeyWT == WireBytes) {
      body_hint += varint_size(k.size()) + k.size();
    } else if constexpr (KeyWT == WireFixed32 || KeyWT == WireSfixed32) {
      body_hint += 4;
    } else if constexpr (KeyWT == WireFixed64 || KeyWT == WireSfixed64) {
      body_hint += 8;
    } else {
      body_hint += 1;
    }
    if constexpr (requires { { v.min_wire_size(1) } -> std::convertible_to<size_t>; }) {
      const size_t sh = v.min_wire_size(1);
      body_hint += (sh > 16383u ? 4u : 2u) + sh;
    } else {
      body_hint += 2;
    }
  }
  const bool map_4b = body_hint > 16383u;
  const size_t len_pos = w.reserve_len_hint(body_hint);
  w.write_byte(KeyWT);
  w.write_byte(WireSubMsg);
  w.write_varint(m.size());
  for (const auto& [k, v] : m) {
    if (const char* err = try_encode_container_scalar<KeyWT>(w, k)) [[unlikely]] {
      return err;
    }
    size_t sh = 0;
    if constexpr (requires { { v.min_wire_size(1) } -> std::convertible_to<size_t>; }) {
      if (map_4b) {
        sh = v.min_wire_size(1);
      }
    }
    const bool sub_4b = sh > 16383u;
    const size_t sub_len_pos = w.reserve_len_hint(sh);
    if (const char* err = encode_sub(w, v)) [[unlikely]] {
      return err;
    }
    w.backpatch_len_hint(sub_len_pos, sub_4b);
  }
  w.backpatch_len_hint(len_pos, map_4b);
  return nullptr;
}

template <uint8_t KeyWT, uint8_t ValWT, typename MapT>
inline void encode_scalar_map(FastWriter& w, const MapT& m) {
  if (const char* err = try_encode_scalar_map<KeyWT, ValWT>(w, m)) [[unlikely]] {
    throw std::invalid_argument(err);
  }
}

inline void encode_map_string_uint(FastWriter& w, const std::map<std::string, uint64_t>& m) {
  encode_scalar_map<WireString, WireUint>(w, m);
}

// Single-pass forward streaming Encoder with inline stack fast-path and dynamic overflow growth.
class Encoder {
 public:
  static constexpr size_t kInlineStackSize = MAX_CONTAINER_DEPTH * 2;

  explicit Encoder(size_t initial_capacity = 256) { buf_.reserve(initial_capacity); }

  void reset() noexcept {
    buf_.clear();
    depth_ = 0;
  }

  [[nodiscard]] size_t depth() const noexcept { return depth_; }
  [[nodiscard]] std::span<const uint8_t> bytes() const noexcept { return buf_; }
  [[nodiscard]] const std::vector<uint8_t>& vector() const noexcept { return buf_; }
  [[nodiscard]] std::vector<uint8_t> finish() && {
    if (depth_ != 0) [[unlikely]] {
      throw std::logic_error("omnibuf: Encoder::finish called with unclosed container");
    }
    return std::move(buf_);
  }
  [[nodiscard]] std::vector<uint8_t> finish_copy() const {
    if (depth_ != 0) [[unlikely]] {
      throw std::logic_error("omnibuf: Encoder::finish_copy called with unclosed container");
    }
    return buf_;
  }

  void write_raw(std::span<const uint8_t> raw) {
    buf_.insert(buf_.end(), raw.begin(), raw.end());
  }

  void write_byte(uint8_t b) { buf_.push_back(b); }

  void write_raw_varint(uint64_t v) { append_varint(buf_, v); }

  void write_tag(uint32_t field_num, uint8_t wire_type) {
    append_tag(buf_, field_num, wire_type);
  }

  void write_uint(uint32_t field_num, uint64_t v) {
    write_tag(field_num, WireUint);
    append_varint(buf_, v);
  }

  void write_sint(uint32_t field_num, int64_t v) {
    write_tag(field_num, WireSint);
    append_varint(buf_, encode_zigzag(v));
  }

  void write_bool(uint32_t field_num, bool v) {
    write_tag(field_num, v ? WireBoolTrue : WireBoolFalse);
  }

  void write_fixed32(uint32_t field_num, uint32_t v) {
    write_tag(field_num, WireFixed32);
    append_u32_le(buf_, v);
  }

  void write_sfixed32(uint32_t field_num, int32_t v) {
    write_tag(field_num, WireSfixed32);
    append_u32_le(buf_, static_cast<uint32_t>(v));
  }

  void write_float32(uint32_t field_num, float v) {
    write_tag(field_num, WireFloat32);
    append_u32_le(buf_, float32_to_bits(v));
  }

  void write_fixed64(uint32_t field_num, uint64_t v) {
    write_tag(field_num, WireFixed64);
    append_u64_le(buf_, v);
  }

  void write_sfixed64(uint32_t field_num, int64_t v) {
    write_tag(field_num, WireSfixed64);
    append_u64_le(buf_, static_cast<uint64_t>(v));
  }

  void write_float64(uint32_t field_num, double v) {
    write_tag(field_num, WireFloat64);
    append_u64_le(buf_, float64_to_bits(v));
  }

  [[nodiscard]] bool try_write_string(uint32_t field_num, std::string_view s) {
    if (!is_valid_utf8(s)) [[unlikely]] {
      return false;
    }
    write_tag(field_num, WireString);
    append_varint(buf_, s.size());
    buf_.insert(buf_.end(), s.begin(), s.end());
    return true;
  }

  void write_string(uint32_t field_num, std::string_view s) {
    if (!try_write_string(field_num, s)) [[unlikely]] {
      throw std::invalid_argument("omnibuf: invalid UTF-8 in WireString");
    }
  }

  void write_bytes(uint32_t field_num, std::span<const uint8_t> b) {
    write_tag(field_num, WireBytes);
    append_varint(buf_, b.size());
    buf_.insert(buf_.end(), b.begin(), b.end());
  }

  void begin_container() {
    push_frame(buf_.size(), 0);
    buf_.push_back(0);
    buf_.push_back(0);
  }

  void end_container() {
    const auto [len_pos, _] = pop_frame();
    backpatch_container_len(buf_, len_pos);
  }

  void begin_submsg(uint32_t field_num) {
    write_tag(field_num, WireSubMsg);
    begin_container();
  }

  void end_submsg() { end_container(); }

  void begin_submsg_arr(uint32_t field_num) {
    write_tag(field_num, WireArray);
    push_frame(buf_.size(), 0);
    buf_.push_back(0);
    buf_.push_back(0);
    buf_.push_back(WireSubMsg);
    buf_.push_back(0);  // 1B reserved count slot
  }

  void begin_submsg_elem() {
    increment_parent_count();
    begin_container();
  }

  void end_submsg_elem() { end_container(); }

  void end_submsg_arr() {
    const auto [len_pos, count] = pop_frame();
    patch_reserved_varint(buf_, len_pos + 3, count);
    backpatch_container_len(buf_, len_pos);
  }

  void write_string_arr(uint32_t field_num, std::span<const std::string> items) {
    write_tag(field_num, WireArray);
    encode_string_arr(buf_, items);
  }

  template <detail::IntOrUint T>
  void write_uint_arr(uint32_t field_num, std::span<const T> items) {
    write_tag(field_num, WireArray);
    encode_uint_arr(buf_, items);
  }

  template <detail::IntOrUint T>
  void write_sint_arr(uint32_t field_num, std::span<const T> items) {
    write_tag(field_num, WireArray);
    encode_sint_arr(buf_, items);
  }

  void write_map_string_uint(uint32_t field_num, const std::map<std::string, uint64_t>& m) {
    write_tag(field_num, WireMap);
    encode_map_string_uint(buf_, m);
  }

 private:
  struct Frame {
    size_t len_pos = 0;
    size_t count = 0;
  };

  void push_frame(size_t len_pos, size_t count) {
    if (depth_ < kInlineStackSize) {
      len_stack_[depth_] = len_pos;
      count_stack_[depth_] = count;
    } else {
      const size_t idx = depth_ - kInlineStackSize;
      if (idx < len_overflow_.size()) {
        len_overflow_[idx] = len_pos;
        count_overflow_[idx] = count;
      } else {
        len_overflow_.push_back(len_pos);
        count_overflow_.push_back(count);
      }
    }
    ++depth_;
  }

  void increment_parent_count() {
    if (depth_ == 0) [[unlikely]] {
      throw std::logic_error(
          "omnibuf: Encoder::begin_submsg_elem called without active array container");
    }
    const size_t parent = depth_ - 1;
    if (parent < kInlineStackSize) {
      ++count_stack_[parent];
    } else {
      ++count_overflow_[parent - kInlineStackSize];
    }
  }

  Frame pop_frame() {
    if (depth_ == 0) [[unlikely]] {
      throw std::logic_error("omnibuf: Encoder container stack underflow");
    }
    --depth_;
    if (depth_ < kInlineStackSize) {
      return {len_stack_[depth_], count_stack_[depth_]};
    }
    const size_t idx = depth_ - kInlineStackSize;
    return {len_overflow_[idx], count_overflow_[idx]};
  }

  std::vector<uint8_t> buf_;
  std::array<size_t, kInlineStackSize> len_stack_{};
  std::array<size_t, kInlineStackSize> count_stack_{};
  std::vector<size_t> len_overflow_;
  std::vector<size_t> count_overflow_;
  size_t depth_ = 0;
};

}  // namespace omnibuf
