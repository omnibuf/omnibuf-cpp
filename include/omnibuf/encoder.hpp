#pragma once

#include <algorithm>
#include <array>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <map>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "omnibuf/varint.hpp"

namespace omnibuf {

namespace detail {

template <typename T>
concept IntOrUint = std::integral<T> && !std::same_as<T, bool>;

}  // namespace detail

inline void encode_string_arr(std::vector<uint8_t>& dst, std::span<const std::string> items) {
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
  const size_t len_pos = dst.size();
  dst.push_back(0);
  dst.push_back(0);
  dst.push_back(elem_wt);
  append_varint(dst, items.size());
  for (T v : items) {
    append_u32_le(dst, static_cast<uint32_t>(v));
  }
  backpatch_container_len(dst, len_pos);
}

template <detail::IntOrUint T>
inline void encode_fixed64_arr(std::vector<uint8_t>& dst,
                               std::span<const T> items,
                               uint8_t elem_wt = WireFixed64) {
  const size_t len_pos = dst.size();
  dst.push_back(0);
  dst.push_back(0);
  dst.push_back(elem_wt);
  append_varint(dst, items.size());
  for (T v : items) {
    append_u64_le(dst, static_cast<uint64_t>(v));
  }
  backpatch_container_len(dst, len_pos);
}

inline void encode_float32_arr(std::vector<uint8_t>& dst, std::span<const float> items) {
  const size_t len_pos = dst.size();
  dst.push_back(0);
  dst.push_back(0);
  dst.push_back(WireFloat32);
  append_varint(dst, items.size());
  for (float v : items) {
    append_u32_le(dst, float32_to_bits(v));
  }
  backpatch_container_len(dst, len_pos);
}

inline void encode_float64_arr(std::vector<uint8_t>& dst, std::span<const double> items) {
  const size_t len_pos = dst.size();
  dst.push_back(0);
  dst.push_back(0);
  dst.push_back(WireFloat64);
  append_varint(dst, items.size());
  for (double v : items) {
    append_u64_le(dst, float64_to_bits(v));
  }
  backpatch_container_len(dst, len_pos);
}

// std::map<std::string, uint64_t> is already ordered lexicographically.
inline void encode_map_string_uint(std::vector<uint8_t>& dst,
                                   const std::map<std::string, uint64_t>& m) {
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

// Single-pass forward streaming Encoder with inline stack fast-path and dynamic overflow growth.
class Encoder {
 public:
  static constexpr size_t kInlineStackSize = MAX_CONTAINER_DEPTH * 2;

  explicit Encoder(size_t initial_capacity = 256) { buf_.reserve(initial_capacity); }

  void reset() noexcept {
    buf_.clear();
    depth_ = 0;
  }

  [[nodiscard]] std::span<const uint8_t> bytes() const noexcept { return buf_; }
  [[nodiscard]] const std::vector<uint8_t>& vector() const noexcept { return buf_; }
  [[nodiscard]] std::vector<uint8_t> finish() && noexcept {
    depth_ = 0;
    return std::move(buf_);
  }
  [[nodiscard]] std::vector<uint8_t> finish_copy() const { return buf_; }

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

  void write_string(uint32_t field_num, std::string_view s) {
    write_tag(field_num, WireString);
    append_varint(buf_, s.size());
    buf_.insert(buf_.end(), s.begin(), s.end());
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
    const size_t parent = depth_ - 1;
    if (parent < kInlineStackSize) {
      ++count_stack_[parent];
    } else {
      ++count_overflow_[parent - kInlineStackSize];
    }
  }

  Frame pop_frame() {
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
