#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include "omnibuf/utf8.hpp"
#include "omnibuf/varint.hpp"

namespace omnibuf {

struct Error {
  std::string message;
};

template <typename T>
class Result {
 public:
  Result(T value) noexcept(std::is_nothrow_move_constructible_v<T>)  // NOLINT(google-explicit-constructor)
      : value_(std::move(value)) {}
  Result(Error err)  // NOLINT(google-explicit-constructor)
      : error_(std::make_unique<std::string>(std::move(err.message))) {}

  [[nodiscard]] bool ok() const noexcept { return error_ == nullptr && value_.has_value(); }
  [[nodiscard]] explicit operator bool() const noexcept { return ok(); }

  [[nodiscard]] T& value() & { return *value_; }
  [[nodiscard]] const T& value() const& { return *value_; }
  [[nodiscard]] T&& value() && { return std::move(*value_); }

  [[nodiscard]] const std::string& error() const noexcept {
    static const std::string kEmpty;
    return error_ ? *error_ : kEmpty;
  }

 private:
  std::optional<T> value_;
  std::unique_ptr<std::string> error_;
};

template <>
class Result<void> {
 public:
  Result() noexcept = default;
  Result(Error err)  // NOLINT(google-explicit-constructor)
      : error_(std::make_unique<std::string>(std::move(err.message))) {}

  static Result success() noexcept { return Result(); }

  [[nodiscard]] bool ok() const noexcept { return error_ == nullptr; }
  [[nodiscard]] explicit operator bool() const noexcept { return error_ == nullptr; }
  [[nodiscard]] const std::string& error() const noexcept {
    static const std::string kEmpty;
    return error_ ? *error_ : kEmpty;
  }

 private:
  std::unique_ptr<std::string> error_;
};

struct ArrayHeader {
  uint8_t elem_wt = 0;
  size_t count = 0;
  size_t pos = 0;
  bool ok = false;
};

// Parses [ElementType: 1B][Count: Prefix-Varint] and validates physical byte capacity
// before any allocation. Rejects trailing bytes on empty or fixed-width arrays.
[[nodiscard]] inline ArrayHeader parse_array_header(std::span<const uint8_t> body) noexcept {
  if (body.size() < 2) {
    return {};
  }
  const uint8_t elem = body[0];
  if (elem > WireExtended || elem == WireBoolTrue) {
    return {};
  }
  const auto [c, n] = decode_varint(body.subspan(1));
  if (n == 0) {
    return {};
  }
  const size_t pos = 1 + n;
  const size_t remaining = body.size() - pos;
  size_t width = 0;
  switch (elem) {
    case WireBoolFalse:
      width = 1;
      break;
    case WireFixed32:
    case WireFloat32:
    case WireSfixed32:
      width = 4;
      break;
    case WireFixed64:
    case WireFloat64:
    case WireSfixed64:
      width = 8;
      break;
    default:
      break;
  }
  if (width > 0) {
    if (remaining % width != 0 || c != static_cast<uint64_t>(remaining / width)) {
      return {};
    }
  } else {
    if ((c == 0 && remaining != 0) || c > static_cast<uint64_t>(remaining)) {
      return {};
    }
  }
  return {elem, static_cast<size_t>(c), pos, true};
}

[[nodiscard]] constexpr bool is_valid_map_key_wire_type(uint8_t wt) noexcept {
  switch (wt) {
    case WireUint:
    case WireSint:
    case WireBoolFalse:
    case WireFixed32:
    case WireFixed64:
    case WireSfixed32:
    case WireSfixed64:
    case WireString:
      return true;
    default:
      return false;
  }
}

[[nodiscard]] constexpr bool is_valid_map_val_wire_type(uint8_t wt) noexcept {
  return wt <= WireExtended && wt != WireBoolTrue;
}

struct MapHeader {
  uint8_t key_wt = 0;
  uint8_t val_wt = 0;
  size_t count = 0;
  size_t pos = 0;
  bool ok = false;
};

// Parses [KeyType: 1B][ValueType: 1B][Count: Prefix-Varint] and enforces bounds
// and empty-map trailing-byte rejection.
[[nodiscard]] inline MapHeader parse_map_header(std::span<const uint8_t> body) noexcept {
  if (body.size() < 3) {
    return {};
  }
  const uint8_t key_wt = body[0];
  const uint8_t val_wt = body[1];
  if (!is_valid_map_key_wire_type(key_wt) || !is_valid_map_val_wire_type(val_wt)) {
    return {};
  }
  const auto [c, n] = decode_varint(body.subspan(2));
  if (n == 0) {
    return {};
  }
  const size_t pos = 2 + n;
  const size_t remaining = body.size() - pos;
  if ((c == 0 && remaining != 0) || c > static_cast<uint64_t>(remaining / 2)) {
    return {};
  }
  return {key_wt, val_wt, static_cast<size_t>(c), pos, true};
}

struct SkipResult {
  size_t next_pos = 0;
  bool ok = false;
};

// O(1) bounds-checked value skip without inner element iteration.
[[nodiscard]] inline SkipResult fast_skip_value(std::span<const uint8_t> packet,
                                                size_t pos,
                                                uint8_t wt) noexcept {
  const size_t n = packet.size();
  switch (wt) {
    case WireUint:
    case WireSint: {
      if (pos >= n) return {};
      const uint8_t vb = packet[pos];
      if ((vb & 1u) != 0) return {pos + 1, true};
      if ((vb & 2u) != 0) return {pos + 2, n - pos >= 2};
      if ((vb & 4u) != 0) return {pos + 4, n - pos >= 4};
      if (vb == 0) return {pos + 9, n - pos >= 9};
      return {};
    }
    case WireBoolFalse:
    case WireBoolTrue:
      return {pos, pos <= n};
    case WireString:
    case WireBytes:
    case WireSubMsg:
    case WireArray:
    case WireMap:
    case WireExtended: {
      if (pos >= n) return {};
      const auto [b_len, bn] = decode_varint(packet.subspan(pos));
      if (bn == 0 || b_len > static_cast<uint64_t>(n - (pos + bn))) {
        return {};
      }
      return {pos + bn + static_cast<size_t>(b_len), true};
    }
    case WireFixed32:
    case WireFloat32:
    case WireSfixed32:
      if (pos > n || n - pos < 4) return {};
      return {pos + 4, true};
    case WireFixed64:
    case WireFloat64:
    case WireSfixed64:
      if (pos > n || n - pos < 8) return {};
      return {pos + 8, true};
    default:
      return {};
  }
}

// Advances pos past a single field payload with container header validation (including
// WireExtended ByteLen >= 1 for the 1-byte subtype ID) without allocating on failure.
[[nodiscard]] inline SkipResult skip_value_checked(std::span<const uint8_t> b,
                                                   size_t pos,
                                                   uint8_t wt) noexcept {
  if (pos > b.size()) {
    return {};
  }
  if (wt == WireArray || wt == WireMap || wt == WireExtended) {
    if (pos >= b.size()) {
      return {};
    }
    const auto [length, ln] = decode_varint(b.subspan(pos));
    if (ln == 0 || length < 1 || length > static_cast<uint64_t>(b.size() - pos - ln)) {
      return {};
    }
    const size_t end = pos + ln + static_cast<size_t>(length);
    const auto body = b.subspan(pos + ln, static_cast<size_t>(length));
    if (wt == WireArray && !parse_array_header(body).ok) {
      return {};
    }
    if (wt == WireMap && !parse_map_header(body).ok) {
      return {};
    }
    return {end, true};
  }
  return fast_skip_value(b, pos, wt);
}

[[nodiscard]] inline Result<size_t> skip_value(std::span<const uint8_t> b,
                                               size_t pos,
                                               uint8_t wt) {
  const auto res = skip_value_checked(b, pos, wt);
  if (!res.ok) {
    return Error{"omnibuf: truncated or invalid field payload"};
  }
  return res.next_pos;
}

struct SliceResult {
  std::span<const uint8_t> slice;
  size_t next_pos = 0;
};

[[nodiscard]] inline Result<SliceResult> read_len_slice(std::span<const uint8_t> data,
                                                        size_t pos,
                                                        uint64_t min_len = 0) {
  if (pos >= data.size()) {
    return Error{"omnibuf: truncated length varint"};
  }
  const auto [b_len, bn] = decode_varint(data.subspan(pos));
  if (bn == 0) {
    return Error{"omnibuf: invalid or truncated length varint"};
  }
  pos += bn;
  if (b_len < min_len || b_len > static_cast<uint64_t>(data.size() - pos)) {
    return Error{"omnibuf: length exceeds packet bounds"};
  }
  const size_t len_sz = static_cast<size_t>(b_len);
  return SliceResult{data.subspan(pos, len_sz), pos + len_sz};
}

[[nodiscard]] inline Result<SliceResult> read_str_slice(std::span<const uint8_t> data, size_t pos) {
  auto res = read_len_slice(data, pos, 0);
  if (!res) return Error{res.error()};
  if (!is_valid_utf8(res.value().slice)) {
    return Error{"omnibuf: invalid UTF-8 in WireString"};
  }
  return res;
}

[[nodiscard]] inline size_t grow_capacity(size_t current, size_t required) noexcept {
  if (current <= (SIZE_MAX / 4) && current * 2 > required) {
    return current * 2;
  }
  return required;
}

// Forward declarations for mutual recursion in full validation (defined in field0.hpp).
[[nodiscard]] inline Result<void> validate_message_bytes(std::span<const uint8_t> data, int depth);
[[nodiscard]] inline Result<size_t> validate_field0(std::span<const uint8_t> data,
                                                    size_t pos,
                                                    uint8_t wt,
                                                    int depth);

[[nodiscard]] inline Result<size_t> validate_value(std::span<const uint8_t> b,
                                                   size_t pos,
                                                   uint8_t wt,
                                                   int depth);

[[nodiscard]] inline Result<size_t> validate_container_entry(std::span<const uint8_t> body,
                                                             size_t p,
                                                             uint8_t wt,
                                                             int depth) {
  if (wt == WireBoolFalse) {
    if (p >= body.size() || body[p] > 1u) {
      return Error{"omnibuf: invalid bool padding bits in container"};
    }
    return p + 1;
  }
  return validate_value(body, p, wt, depth);
}

[[nodiscard]] inline Result<void> validate_array_body(std::span<const uint8_t> body,
                                                      size_t p,
                                                      uint8_t elem_wt,
                                                      size_t count,
                                                      int depth) {
  switch (elem_wt) {
    case WireBoolFalse:
      if (body.size() - p != count) {
        return Error{"omnibuf: invalid bool array byte length"};
      }
      for (size_t i = 0; i < count; ++i) {
        if (body[p + i] > 1u) {
          return Error{"omnibuf: invalid bool byte in array"};
        }
      }
      return Result<void>::success();
    case WireFixed32:
    case WireSfixed32:
    case WireFloat32:
      if (body.size() - p != count * 4) {
        return Error{"omnibuf: invalid 32-bit array byte length"};
      }
      return Result<void>::success();
    case WireFixed64:
    case WireSfixed64:
    case WireFloat64:
      if (body.size() - p != count * 8) {
        return Error{"omnibuf: invalid 64-bit array byte length"};
      }
      return Result<void>::success();
    default:
      break;
  }
  for (size_t i = 0; i < count; ++i) {
    auto next_res = validate_container_entry(body, p, elem_wt, depth);
    if (!next_res) return Error{next_res.error()};
    p = next_res.value();
  }
  if (p != body.size()) {
    return Error{"omnibuf: trailing bytes in WireArray"};
  }
  return Result<void>::success();
}

[[nodiscard]] inline Result<size_t> validate_value(std::span<const uint8_t> b,
                                                   size_t pos,
                                                   uint8_t wt,
                                                   int depth) {
  if (depth > MAX_CONTAINER_DEPTH) {
    return Error{"omnibuf: nesting depth exceeds MAX_CONTAINER_DEPTH"};
  }
  if (pos > b.size()) {
    return Error{"omnibuf: position out of bounds"};
  }
  if (wt == WireString) {
    auto s_res = read_str_slice(b, pos);
    if (!s_res) return Error{s_res.error()};
    return s_res.value().next_pos;
  }
  if (wt == WireSubMsg) {
    auto sub_res = read_len_slice(b, pos, 0);
    if (!sub_res) return Error{sub_res.error()};
    auto v_res = validate_message_bytes(sub_res.value().slice, depth + 1);
    if (!v_res) return Error{v_res.error()};
    return sub_res.value().next_pos;
  }
  if (wt == WireArray || wt == WireMap || wt == WireExtended) {
    if (pos >= b.size()) {
      return Error{"omnibuf: truncated container length"};
    }
    const auto [length, ln] = decode_varint(b.subspan(pos));
    if (ln == 0 || length < 1 || length > static_cast<uint64_t>(b.size() - pos - ln)) {
      return Error{"omnibuf: invalid container length"};
    }
    const size_t end = pos + ln + static_cast<size_t>(length);
    const auto body = b.subspan(pos + ln, static_cast<size_t>(length));
    if (wt == WireArray) {
      if (depth + 1 > MAX_CONTAINER_DEPTH) {
        return Error{"omnibuf: array nesting depth exceeds MAX_CONTAINER_DEPTH"};
      }
      const auto hdr = parse_array_header(body);
      if (!hdr.ok) {
        return Error{"omnibuf: invalid WireArray header"};
      }
      auto a_res = validate_array_body(body, hdr.pos, hdr.elem_wt, hdr.count, depth + 1);
      if (!a_res) return Error{a_res.error()};
    } else if (wt == WireMap) {
      if (depth + 1 > MAX_CONTAINER_DEPTH) {
        return Error{"omnibuf: map nesting depth exceeds MAX_CONTAINER_DEPTH"};
      }
      const auto hdr = parse_map_header(body);
      if (!hdr.ok) {
        return Error{"omnibuf: invalid WireMap header"};
      }
      size_t p = hdr.pos;
      for (size_t i = 0; i < hdr.count; ++i) {
        auto k_res = validate_container_entry(body, p, hdr.key_wt, depth + 1);
        if (!k_res) return Error{k_res.error()};
        auto v_res = validate_container_entry(body, k_res.value(), hdr.val_wt, depth + 1);
        if (!v_res) return Error{v_res.error()};
        p = v_res.value();
      }
      if (p != body.size()) {
        return Error{"omnibuf: trailing bytes in WireMap"};
      }
    }
    // WireExtended: length >= 1 guarantees the 1B ExtType header (0x00..0xFF) is present and skippable.
    return end;
  }

  const auto skip = fast_skip_value(b, pos, wt);
  if (!skip.ok) {
    return Error{"omnibuf: truncated or invalid scalar payload"};
  }
  return skip.next_pos;
}

// Array body decoders for generated and reflection-driven codecs.
[[nodiscard]] inline bool append_string_arr_body(std::vector<std::string>& dst,
                                                 std::span<const uint8_t> body) {
  const auto hdr = parse_array_header(body);
  if (!hdr.ok || hdr.elem_wt != WireString) return false;
  size_t p = hdr.pos;
  dst.reserve(grow_capacity(dst.capacity(), dst.size() + hdr.count));
  for (size_t i = 0; i < hdr.count; ++i) {
    auto s_res = read_str_slice(body, p);
    if (!s_res) return false;
    const auto sp = s_res.value().slice;
    dst.emplace_back(reinterpret_cast<const char*>(sp.data()), sp.size());
    p = s_res.value().next_pos;
  }
  return p == body.size();
}

[[nodiscard]] inline bool append_bytes_arr_body(std::vector<std::vector<uint8_t>>& dst,
                                                std::span<const uint8_t> body) {
  const auto hdr = parse_array_header(body);
  if (!hdr.ok || hdr.elem_wt != WireBytes) return false;
  size_t p = hdr.pos;
  dst.reserve(grow_capacity(dst.capacity(), dst.size() + hdr.count));
  for (size_t i = 0; i < hdr.count; ++i) {
    auto b_res = read_len_slice(body, p, 0);
    if (!b_res) return false;
    const auto sp = b_res.value().slice;
    dst.emplace_back(sp.begin(), sp.end());
    p = b_res.value().next_pos;
  }
  return p == body.size();
}

[[nodiscard]] inline bool append_bool_arr_body(std::vector<bool>& dst,
                                               std::span<const uint8_t> body) {
  const auto hdr = parse_array_header(body);
  if (!hdr.ok || hdr.elem_wt != WireBoolFalse) return false;
  dst.reserve(dst.size() + hdr.count);
  for (size_t i = 0; i < hdr.count; ++i) {
    const uint8_t v = body[hdr.pos + i];
    if (v > 1u) return false;
    dst.push_back(v == 1u);
  }
  return true;
}

template <typename T>
[[nodiscard]] inline bool append_uint_arr_body(std::vector<T>& dst, std::span<const uint8_t> body) {
  const auto hdr = parse_array_header(body);
  if (!hdr.ok || hdr.elem_wt != WireUint) return false;
  size_t p = hdr.pos;
  dst.reserve(grow_capacity(dst.capacity(), dst.size() + hdr.count));
  for (size_t i = 0; i < hdr.count; ++i) {
    const auto [v, vn] = decode_varint(body.subspan(p));
    if (vn == 0) return false;
    p += vn;
    dst.push_back(static_cast<T>(v));
  }
  return p == body.size();
}

template <typename T>
[[nodiscard]] inline bool append_sint_arr_body(std::vector<T>& dst, std::span<const uint8_t> body) {
  const auto hdr = parse_array_header(body);
  if (!hdr.ok || hdr.elem_wt != WireSint) return false;
  size_t p = hdr.pos;
  dst.reserve(grow_capacity(dst.capacity(), dst.size() + hdr.count));
  for (size_t i = 0; i < hdr.count; ++i) {
    const auto [v, vn] = decode_varint(body.subspan(p));
    if (vn == 0) return false;
    p += vn;
    dst.push_back(static_cast<T>(decode_zigzag(v)));
  }
  return p == body.size();
}

template <typename T>
[[nodiscard]] inline bool append_fixed32_arr_body(std::vector<T>& dst,
                                                  std::span<const uint8_t> body,
                                                  uint8_t expected_wt) {
  const auto hdr = parse_array_header(body);
  if (!hdr.ok || hdr.elem_wt != expected_wt) return false;
  size_t p = hdr.pos;
  dst.reserve(grow_capacity(dst.capacity(), dst.size() + hdr.count));
  for (size_t i = 0; i < hdr.count; ++i) {
    const uint32_t bits = read_u32_le(body.data() + p);
    p += 4;
    if constexpr (std::is_same_v<T, float>) {
      dst.push_back(bits_to_float32(bits));
    } else if constexpr (std::is_signed_v<T>) {
      dst.push_back(static_cast<T>(static_cast<int32_t>(bits)));
    } else {
      dst.push_back(static_cast<T>(bits));
    }
  }
  return true;
}

template <typename T>
[[nodiscard]] inline bool append_fixed64_arr_body(std::vector<T>& dst,
                                                  std::span<const uint8_t> body,
                                                  uint8_t expected_wt) {
  const auto hdr = parse_array_header(body);
  if (!hdr.ok || hdr.elem_wt != expected_wt) return false;
  size_t p = hdr.pos;
  dst.reserve(grow_capacity(dst.capacity(), dst.size() + hdr.count));
  for (size_t i = 0; i < hdr.count; ++i) {
    const uint64_t bits = read_u64_le(body.data() + p);
    p += 8;
    if constexpr (std::is_same_v<T, double>) {
      dst.push_back(bits_to_float64(bits));
    } else if constexpr (std::is_signed_v<T>) {
      dst.push_back(static_cast<T>(static_cast<int64_t>(bits)));
    } else {
      dst.push_back(static_cast<T>(bits));
    }
  }
  return true;
}

[[nodiscard]] inline bool decode_map_string_uint_body(std::map<std::string, uint64_t>& dst,
                                                      std::span<const uint8_t> body) {
  const auto hdr = parse_map_header(body);
  if (!hdr.ok || hdr.key_wt != WireString || hdr.val_wt != WireUint) return false;
  size_t p = hdr.pos;
  for (size_t i = 0; i < hdr.count; ++i) {
    auto k_res = read_str_slice(body, p);
    if (!k_res) return false;
    p = k_res.value().next_pos;
    const auto [v, vn] = decode_varint(body.subspan(p));
    if (vn == 0) return false;
    p += vn;
    const auto ksp = k_res.value().slice;
    dst[std::string(reinterpret_cast<const char*>(ksp.data()), ksp.size())] = v;
  }
  return p == body.size();
}

}  // namespace omnibuf
