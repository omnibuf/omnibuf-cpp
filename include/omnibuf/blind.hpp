#pragma once

#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <variant>
#include <vector>

#include "omnibuf/decoder.hpp"
#include "omnibuf/field0.hpp"
#include "omnibuf/utf8.hpp"
#include "omnibuf/varint.hpp"

namespace omnibuf {

// Recursive schema-less value tree preserving exact 64-bit integer signs/widths
// and distinct 32-bit vs 64-bit IEEE-754 floats for shortest round-trip JSON output.
struct BlindBytes {
  std::vector<uint8_t> data;
};

struct BlindValue;
using BlindArray = std::vector<BlindValue>;
using BlindObject = std::map<std::string, BlindValue>;

struct BlindValue {
  using Variant = std::variant<std::monostate,
                               bool,
                               uint64_t,
                               int64_t,
                               float,
                               double,
                               std::string,
                               BlindBytes,
                               BlindArray,
                               BlindObject>;
  Variant v;

  BlindValue() = default;
  template <typename T>
    requires(!std::same_as<std::remove_cvref_t<T>, BlindValue>)
  BlindValue(T&& val) : v(std::forward<T>(val)) {}  // NOLINT(google-explicit-constructor)
};

[[nodiscard]] inline std::string base64_encode(std::span<const uint8_t> bytes) {
  static constexpr char kTable[] =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  if (bytes.empty()) return {};
  std::string out;
  out.reserve(((bytes.size() + 2) / 3) * 4);
  size_t i = 0;
  for (; i + 2 < bytes.size(); i += 3) {
    const uint32_t n = (static_cast<uint32_t>(bytes[i]) << 16) |
                       (static_cast<uint32_t>(bytes[i + 1]) << 8) |
                       static_cast<uint32_t>(bytes[i + 2]);
    out.push_back(kTable[(n >> 18) & 63u]);
    out.push_back(kTable[(n >> 12) & 63u]);
    out.push_back(kTable[(n >> 6) & 63u]);
    out.push_back(kTable[n & 63u]);
  }
  const size_t rem = bytes.size() - i;
  if (rem == 1) {
    const uint32_t n = static_cast<uint32_t>(bytes[i]) << 16;
    out.push_back(kTable[(n >> 18) & 63u]);
    out.push_back(kTable[(n >> 12) & 63u]);
    out.push_back('=');
    out.push_back('=');
  } else if (rem == 2) {
    const uint32_t n =
        (static_cast<uint32_t>(bytes[i]) << 16) | (static_cast<uint32_t>(bytes[i + 1]) << 8);
    out.push_back(kTable[(n >> 18) & 63u]);
    out.push_back(kTable[(n >> 12) & 63u]);
    out.push_back(kTable[(n >> 6) & 63u]);
    out.push_back('=');
  }
  return out;
}

[[nodiscard]] inline Result<std::vector<uint8_t>> base64_decode(std::string_view b64) {
  auto decode_char = [](char c) noexcept -> int {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
  };

  std::string clean;
  clean.reserve(b64.size());
  for (char c : b64) {
    if (c != ' ' && c != '\n' && c != '\r' && c != '\t') clean.push_back(c);
  }
  if (clean.empty()) return std::vector<uint8_t>{};
  if (clean.size() % 4 != 0) {
    return Error{"omnibuf: invalid base64 length"};
  }
  size_t pad = 0;
  if (clean.ends_with("==")) {
    pad = 2;
  } else if (clean.ends_with("=")) {
    pad = 1;
  }
  const size_t byte_len = (clean.size() / 4) * 3 - pad;
  std::vector<uint8_t> out;
  out.reserve(byte_len);

  for (size_t i = 0; i < clean.size(); i += 4) {
    const bool is_last = (i + 4 == clean.size());
    const bool pad2 = is_last && clean[i + 2] == '=';
    const bool pad3 = is_last && clean[i + 3] == '=';
    const int c0 = decode_char(clean[i]);
    const int c1 = decode_char(clean[i + 1]);
    const int c2 = pad2 ? 0 : decode_char(clean[i + 2]);
    const int c3 = pad3 ? 0 : decode_char(clean[i + 3]);
    if (c0 < 0 || c1 < 0 || c2 < 0 || c3 < 0 || (pad2 && !pad3)) {
      return Error{"omnibuf: invalid base64 character"};
    }
    const uint32_t n = (static_cast<uint32_t>(c0) << 18) | (static_cast<uint32_t>(c1) << 12) |
                       (static_cast<uint32_t>(c2) << 6) | static_cast<uint32_t>(c3);
    if (out.size() < byte_len) out.push_back(static_cast<uint8_t>((n >> 16) & 0xFFu));
    if (out.size() < byte_len) out.push_back(static_cast<uint8_t>((n >> 8) & 0xFFu));
    if (out.size() < byte_len) out.push_back(static_cast<uint8_t>(n & 0xFFu));
  }
  return out;
}

namespace detail {

struct BlindSchema {
  std::shared_ptr<SchemaMeta> local;
  std::shared_ptr<const BlindSchema> parent;

  [[nodiscard]] std::string field_name(uint32_t fnum) const {
    for (const BlindSchema* s = this; s != nullptr; s = s->parent.get()) {
      if (s->local) {
        auto it = s->local->fields.find(fnum);
        if (it != s->local->fields.end()) {
          return it->second;
        }
      }
    }
    return {};
  }

  [[nodiscard]] static std::shared_ptr<const BlindSchema> child_of(
      const std::shared_ptr<const BlindSchema>& s,
      uint32_t fnum) {
    if (!s) return nullptr;
    auto p_child = child_of(s->parent, fnum);
    if (s->local) {
      auto it = s->local->children.find(fnum);
      if (it != s->local->children.end() && it->second) {
        auto node = std::make_shared<BlindSchema>();
        node->local = it->second;
        node->parent = std::move(p_child);
        return node;
      }
    }
    return p_child;
  }
};

template <typename IntT>
[[nodiscard]] inline std::string int_to_dec(IntT val) {
  char buf[32];
  auto [ptr, ec] = std::to_chars(buf, buf + sizeof(buf), val);
  (void)ec;
  return std::string(buf, ptr);
}

struct BlindShape {
  uint8_t wt = 0;
  uint8_t key_wt = 0;
  bool any_map = false;
  std::shared_ptr<const BlindShape> elem;
};

[[nodiscard]] inline std::pair<std::shared_ptr<const BlindShape>, bool> unify_blind_shapes(
    const std::shared_ptr<const BlindShape>& a,
    const std::shared_ptr<const BlindShape>& b) {
  if (!a) return {b, true};
  if (!b) return {a, true};
  if (a->wt != b->wt) return {nullptr, false};
  if (a->wt == WireMap) {
    if (a->any_map) return {b, true};
    if (b->any_map) return {a, true};
    if (a->key_wt != b->key_wt) return {nullptr, false};
  }
  if (a->wt != WireArray && a->wt != WireMap) {
    return {a, true};
  }
  auto [u, ok] = unify_blind_shapes(a->elem, b->elem);
  if (!ok) return {nullptr, false};
  return {std::make_shared<BlindShape>(BlindShape{a->wt, a->key_wt, false, std::move(u)}), true};
}

struct BlindContainerShapeResult {
  std::shared_ptr<const BlindShape> shape;
  size_t next_pos = 0;
};

[[nodiscard]] inline Result<BlindContainerShapeResult> parse_blind_container_shape(
    std::span<const uint8_t> data,
    size_t pos,
    uint8_t wt,
    int depth,
    DecodeState& state) {
  if (depth + 1 > state.opts.max_depth) {
    return Error{"omnibuf: container nesting depth exceeds max_depth"};
  }
  if (wt == WireArray) {
    auto body_res = read_len_slice(data, pos, 2);
    if (!body_res) return Error{body_res.error()};
    const auto body = body_res.value().slice;
    const size_t next_pos = body_res.value().next_pos;
    const auto hdr = parse_array_header(body);
    if (!hdr.ok) {
      return Error{"omnibuf: invalid WireArray header"};
    }
    if (hdr.elem_wt != WireArray && hdr.elem_wt != WireMap) {
      auto leaf = std::make_shared<BlindShape>(BlindShape{hdr.elem_wt, 0, false, nullptr});
      auto outer = std::make_shared<BlindShape>(BlindShape{WireArray, 0, false, std::move(leaf)});
      return BlindContainerShapeResult{std::move(outer), next_pos};
    }
    if (hdr.count == 0) {
      auto inner = (hdr.elem_wt == WireArray)
                       ? std::make_shared<BlindShape>(BlindShape{WireArray, 0, false, nullptr})
                       : std::make_shared<BlindShape>(BlindShape{WireMap, 0, true, nullptr});
      auto outer = std::make_shared<BlindShape>(BlindShape{WireArray, 0, false, std::move(inner)});
      return BlindContainerShapeResult{std::move(outer), next_pos};
    }
    std::shared_ptr<const BlindShape> merged;
    size_t p = hdr.pos;
    for (size_t i = 0; i < hdr.count; ++i) {
      auto elem_res = parse_blind_container_shape(body, p, hdr.elem_wt, depth + 1, state);
      if (!elem_res) return Error{elem_res.error()};
      auto [u, compat] = unify_blind_shapes(merged, elem_res.value().shape);
      if (!compat) {
        return Error{"omnibuf: inconsistent nested container element shapes in WireArray"};
      }
      merged = std::move(u);
      p = elem_res.value().next_pos;
    }
    if (p != body.size()) {
      return Error{"omnibuf: WireArray count/length mismatch"};
    }
    auto outer = std::make_shared<BlindShape>(BlindShape{WireArray, 0, false, std::move(merged)});
    return BlindContainerShapeResult{std::move(outer), next_pos};
  }

  auto body_res = read_len_slice(data, pos, 3);
  if (!body_res) return Error{body_res.error()};
  const auto body = body_res.value().slice;
  const size_t next_pos = body_res.value().next_pos;
  const auto hdr = parse_map_header(body);
  if (!hdr.ok) {
    return Error{"omnibuf: invalid WireMap header"};
  }
  if (hdr.val_wt != WireArray && hdr.val_wt != WireMap) {
    auto leaf = std::make_shared<BlindShape>(BlindShape{hdr.val_wt, 0, false, nullptr});
    auto outer =
        std::make_shared<BlindShape>(BlindShape{WireMap, hdr.key_wt, false, std::move(leaf)});
    return BlindContainerShapeResult{std::move(outer), next_pos};
  }
  if (hdr.count == 0) {
    auto inner = (hdr.val_wt == WireArray)
                     ? std::make_shared<BlindShape>(BlindShape{WireArray, 0, false, nullptr})
                     : std::make_shared<BlindShape>(BlindShape{WireMap, 0, true, nullptr});
    auto outer =
        std::make_shared<BlindShape>(BlindShape{WireMap, hdr.key_wt, false, std::move(inner)});
    return BlindContainerShapeResult{std::move(outer), next_pos};
  }
  std::shared_ptr<const BlindShape> merged;
  size_t p = hdr.pos;
  for (size_t i = 0; i < hdr.count; ++i) {
    auto next_p = validate_container_entry(body, p, hdr.key_wt, depth + 1, state);
    if (!next_p) return Error{next_p.error()};
    auto val_res = parse_blind_container_shape(body, next_p.value(), hdr.val_wt, depth + 1, state);
    if (!val_res) return Error{val_res.error()};
    auto [u, compat] = unify_blind_shapes(merged, val_res.value().shape);
    if (!compat) {
      return Error{"omnibuf: inconsistent nested container value shapes in WireMap"};
    }
    merged = std::move(u);
    p = val_res.value().next_pos;
  }
  if (p != body.size()) {
    return Error{"omnibuf: WireMap trailing bytes"};
  }
  auto outer =
      std::make_shared<BlindShape>(BlindShape{WireMap, hdr.key_wt, false, std::move(merged)});
  return BlindContainerShapeResult{std::move(outer), next_pos};
}

[[nodiscard]] inline Result<std::shared_ptr<const BlindShape>> blind_field_shape(
    std::span<const uint8_t> data,
    size_t pos,
    uint8_t wt,
    int depth,
    DecodeState& state) {
  if (wt == WireBoolTrue) {
    wt = WireBoolFalse;
  }
  if (wt != WireArray && wt != WireMap) {
    return std::make_shared<const BlindShape>(BlindShape{wt, 0, false, nullptr});
  }
  auto res = parse_blind_container_shape(data, pos, wt, depth, state);
  if (!res) return Error{res.error()};
  return res.value().shape;
}

[[nodiscard]] inline Result<BlindObject> decode_blind_with_schema(
    std::span<const uint8_t> data,
    const std::shared_ptr<const BlindSchema>& inherited,
    int depth,
    DecodeState& state);

[[nodiscard]] inline Result<void> decode_array_body_blind(
    BlindArray& dst,
    std::span<const uint8_t> body,
    const std::shared_ptr<const BlindSchema>& child_schema,
    int depth,
    DecodeState& state);

[[nodiscard]] inline Result<void> decode_map_body_blind(
    BlindObject& dst,
    std::span<const uint8_t> body,
    const std::shared_ptr<const BlindSchema>& child_schema,
    int depth,
    DecodeState& state);

struct MapKeyResult {
  std::string key;
  size_t next_pos = 0;
};

[[nodiscard]] inline Result<MapKeyResult> decode_map_key_blind(std::span<const uint8_t> body,
                                                               size_t p,
                                                               uint8_t key_wt) {
  switch (key_wt) {
    case WireString: {
      auto s_res = read_str_slice(body, p);
      if (!s_res) return Error{s_res.error()};
      const auto sp = s_res.value().slice;
      return MapKeyResult{std::string(reinterpret_cast<const char*>(sp.data()), sp.size()),
                          s_res.value().next_pos};
    }
    case WireUint: {
      const auto [v, vn] = decode_varint(body.subspan(p));
      if (vn == 0) return Error{"omnibuf: truncated WireMap uint key"};
      return MapKeyResult{int_to_dec(v), p + vn};
    }
    case WireSint: {
      const auto [v, vn] = decode_varint(body.subspan(p));
      if (vn == 0) return Error{"omnibuf: truncated WireMap sint key"};
      return MapKeyResult{int_to_dec(decode_zigzag(v)), p + vn};
    }
    case WireFixed32:
    case WireSfixed32: {
      if (body.size() - p < 4) return Error{"omnibuf: truncated WireMap fixed32 key"};
      const uint32_t bits = read_u32_le(body.data() + p);
      if (key_wt == WireSfixed32) {
        return MapKeyResult{int_to_dec(static_cast<int32_t>(bits)), p + 4};
      }
      return MapKeyResult{int_to_dec(bits), p + 4};
    }
    case WireFixed64:
    case WireSfixed64: {
      if (body.size() - p < 8) return Error{"omnibuf: truncated WireMap fixed64 key"};
      const uint64_t bits = read_u64_le(body.data() + p);
      if (key_wt == WireSfixed64) {
        return MapKeyResult{int_to_dec(static_cast<int64_t>(bits)), p + 8};
      }
      return MapKeyResult{int_to_dec(bits), p + 8};
    }
    case WireBoolFalse: {
      if (body.size() - p < 1) return Error{"omnibuf: truncated WireMap bool key"};
      const uint8_t b = body[p];
      if (b > 1u) return Error{"omnibuf: invalid WireMap bool key byte"};
      return MapKeyResult{b == 1u ? "true" : "false", p + 1};
    }
    default:
      return Error{"omnibuf: unsupported WireMap key wire type"};
  }
}

struct MapValResult {
  BlindValue val;
  size_t next_pos = 0;
};

[[nodiscard]] inline Result<MapValResult> decode_map_val_blind(
    std::span<const uint8_t> body,
    size_t p,
    uint8_t val_wt,
    const std::shared_ptr<const BlindSchema>& child_schema,
    int depth,
    DecodeState& state) {
  switch (val_wt) {
    case WireUint: {
      const auto [v, vn] = decode_varint(body.subspan(p));
      if (vn == 0) return Error{"omnibuf: truncated WireMap uint value"};
      return MapValResult{BlindValue(v), p + vn};
    }
    case WireSint: {
      const auto [v, vn] = decode_varint(body.subspan(p));
      if (vn == 0) return Error{"omnibuf: truncated WireMap sint value"};
      return MapValResult{BlindValue(decode_zigzag(v)), p + vn};
    }
    case WireString: {
      auto s_res = read_str_slice(body, p);
      if (!s_res) return Error{s_res.error()};
      const auto sp = s_res.value().slice;
      return MapValResult{
          BlindValue(std::string(reinterpret_cast<const char*>(sp.data()), sp.size())),
          s_res.value().next_pos};
    }
    case WireBytes: {
      auto b_res = read_len_slice(body, p, 0);
      if (!b_res) return Error{b_res.error()};
      const auto sp = b_res.value().slice;
      return MapValResult{BlindValue(BlindBytes{std::vector<uint8_t>(sp.begin(), sp.end())}),
                          b_res.value().next_pos};
    }
    case WireSubMsg: {
      auto sub_res = read_len_slice(body, p, 0);
      if (!sub_res) return Error{sub_res.error()};
      auto sub_obj =
          decode_blind_with_schema(sub_res.value().slice, child_schema, depth + 1, state);
      if (!sub_obj) return Error{sub_obj.error()};
      return MapValResult{BlindValue(std::move(sub_obj.value())), sub_res.value().next_pos};
    }
    case WireArray: {
      auto arr_slice = read_len_slice(body, p, 2);
      if (!arr_slice) return Error{arr_slice.error()};
      BlindArray arr;
      auto arr_res =
          decode_array_body_blind(arr, arr_slice.value().slice, child_schema, depth + 1, state);
      if (!arr_res) return Error{arr_res.error()};
      return MapValResult{BlindValue(std::move(arr)), arr_slice.value().next_pos};
    }
    case WireMap: {
      auto map_slice = read_len_slice(body, p, 3);
      if (!map_slice) return Error{map_slice.error()};
      BlindObject mp;
      auto map_res =
          decode_map_body_blind(mp, map_slice.value().slice, child_schema, depth + 1, state);
      if (!map_res) return Error{map_res.error()};
      return MapValResult{BlindValue(std::move(mp)), map_slice.value().next_pos};
    }
    case WireFixed32:
    case WireFloat32:
    case WireSfixed32: {
      if (body.size() - p < 4) return Error{"omnibuf: truncated 32-bit container value"};
      const uint32_t bits = read_u32_le(body.data() + p);
      if (val_wt == WireFloat32) {
        return MapValResult{BlindValue(bits_to_float32(bits)), p + 4};
      }
      if (val_wt == WireSfixed32) {
        return MapValResult{BlindValue(static_cast<int64_t>(static_cast<int32_t>(bits))), p + 4};
      }
      return MapValResult{BlindValue(static_cast<uint64_t>(bits)), p + 4};
    }
    case WireFixed64:
    case WireFloat64:
    case WireSfixed64: {
      if (body.size() - p < 8) return Error{"omnibuf: truncated 64-bit container value"};
      const uint64_t bits = read_u64_le(body.data() + p);
      if (val_wt == WireFloat64) {
        return MapValResult{BlindValue(bits_to_float64(bits)), p + 8};
      }
      if (val_wt == WireSfixed64) {
        return MapValResult{BlindValue(static_cast<int64_t>(bits)), p + 8};
      }
      return MapValResult{BlindValue(bits), p + 8};
    }
    case WireBoolFalse: {
      if (body.size() - p < 1) return Error{"omnibuf: truncated bool container value"};
      const uint8_t b = body[p];
      if (b > 1u) return Error{"omnibuf: invalid bool padding bits in container value"};
      return MapValResult{BlindValue(b == 1u), p + 1};
    }
    case WireExtended:
      return Error{"omnibuf: unrecognized WireExtended subtype"};
    default:
      return Error{"omnibuf: unsupported container value wire type"};
  }
}

[[nodiscard]] inline Result<void> decode_array_body_blind(
    BlindArray& dst,
    std::span<const uint8_t> body,
    const std::shared_ptr<const BlindSchema>& child_schema,
    int depth,
    DecodeState& state) {
  if (depth > state.opts.max_depth) {
    return Error{"omnibuf: array nesting depth exceeds max_depth"};
  }
  const auto hdr = parse_array_header(body);
  if (!hdr.ok) {
    return Error{"omnibuf: invalid WireArray header"};
  }
  if (!state.charge_elements(hdr.count)) {
    return Error{"omnibuf: WireArray exceeds max_total_elements"};
  }
  size_t p = hdr.pos;
  dst.reserve(bounded_reserve_target(dst.capacity(), dst.size(), hdr.count));

  if (hdr.elem_wt == WireExtended) {
    if (hdr.count > 0) {
      return Error{"omnibuf: unrecognized WireExtended subtype in WireArray"};
    }
    if (p != body.size()) {
      return Error{"omnibuf: WireArray(WireExtended) trailing bytes"};
    }
    return Result<void>::success();
  }

  for (size_t i = 0; i < hdr.count; ++i) {
    auto elem_res = decode_map_val_blind(body, p, hdr.elem_wt, child_schema, depth, state);
    if (!elem_res) return Error{elem_res.error()};
    dst.push_back(std::move(elem_res.value().val));
    p = elem_res.value().next_pos;
  }
  if (p != body.size()) {
    return Error{"omnibuf: WireArray count/length mismatch"};
  }
  return Result<void>::success();
}

[[nodiscard]] inline Result<void> decode_map_body_blind(
    BlindObject& dst,
    std::span<const uint8_t> body,
    const std::shared_ptr<const BlindSchema>& child_schema,
    int depth,
    DecodeState& state) {
  if (depth > state.opts.max_depth) {
    return Error{"omnibuf: map nesting depth exceeds max_depth"};
  }
  const auto hdr = parse_map_header(body);
  if (!hdr.ok) {
    return Error{"omnibuf: invalid WireMap header"};
  }
  if (!state.charge_elements(hdr.count)) {
    return Error{"omnibuf: WireMap exceeds max_total_elements"};
  }
  size_t p = hdr.pos;
  for (size_t i = 0; i < hdr.count; ++i) {
    auto k_res = decode_map_key_blind(body, p, hdr.key_wt);
    if (!k_res) return Error{k_res.error()};
    auto v_res = decode_map_val_blind(
        body, k_res.value().next_pos, hdr.val_wt, child_schema, depth, state);
    if (!v_res) return Error{v_res.error()};
    dst[std::move(k_res.value().key)] = std::move(v_res.value().val);
    p = v_res.value().next_pos;
  }
  if (p != body.size()) {
    return Error{"omnibuf: WireMap trailing bytes"};
  }
  return Result<void>::success();
}

[[nodiscard]] inline Result<BlindObject> decode_blind_with_schema(
    std::span<const uint8_t> data,
    const std::shared_ptr<const BlindSchema>& inherited,
    int depth,
    DecodeState& state) {
  if (depth > state.opts.max_depth) {
    return Error{"omnibuf: message nesting depth exceeds max_depth"};
  }
  if (data.size() > state.opts.max_message_bytes) {
    return Error{"omnibuf: message exceeds max_message_bytes"};
  }
  BlindObject out;
  if (data.empty()) {
    return out;
  }

  // Pass 1: Pre-scan all Field #0 SchemaMeta blocks before resolving field names.
  std::shared_ptr<SchemaMeta> local_schema;
  const size_t n = data.size();
  for (size_t scan = 0; scan < n;) {
    const auto [tag, tn] = decode_varint(data.subspan(scan));
    if (tn == 0 || (tag >> 4) > MAX_FIELD_NUMBER) {
      return Error{"omnibuf: invalid or truncated tag"};
    }
    scan += tn;
    const uint32_t fnum = static_cast<uint32_t>(tag >> 4);
    const uint8_t wt = static_cast<uint8_t>(tag & 0x0Fu);
    if (fnum == 0) {
      if (wt != WireSubMsg) {
        return Error{"omnibuf: Field #0 must have WireSubMsg wire type (12)"};
      }
      auto f0_body = read_len_slice(data, scan, 0);
      if (!f0_body) return Error{f0_body.error()};
      auto meta = decode_schema_meta(f0_body.value().slice, depth + 1, state);
      if (!meta) return Error{meta.error()};
      local_schema = merge_schema_meta(local_schema, meta.value());
      scan = f0_body.value().next_pos;
    } else {
      auto skip_res = skip_value(data, scan, wt);
      if (!skip_res) return Error{skip_res.error()};
      scan = skip_res.value();
    }
  }

  std::shared_ptr<const BlindSchema> active_schema = inherited;
  if (local_schema) {
    auto node = std::make_shared<BlindSchema>();
    node->local = std::move(local_schema);
    node->parent = inherited;
    active_schema = std::move(node);
  }

  struct SubMsgSlices {
    std::span<const uint8_t> first{};
    std::vector<uint8_t> merged{};
    size_t count = 0;
  };
  std::map<uint32_t, SubMsgSlices> sub_packets;
  std::unordered_map<uint32_t, std::shared_ptr<const BlindShape>> shapes;
  std::unordered_map<std::string, uint32_t> owners;

  size_t pos = 0;
  while (pos < n) {
    const auto [tag, tn] = decode_varint(data.subspan(pos));
    if (tn == 0) {
      return Error{"omnibuf: truncated tag"};
    }
    pos += tn;
    const uint32_t field_num = static_cast<uint32_t>(tag >> 4);
    const uint8_t wt = static_cast<uint8_t>(tag & 0x0Fu);

    if (field_num == 0) {
      auto f0_body = read_len_slice(data, pos, 0);
      if (!f0_body) return Error{f0_body.error()};
      pos = f0_body.value().next_pos;
      continue;
    }

    std::string name = active_schema ? active_schema->field_name(field_num) : std::string{};
    if (name.empty()) {
      name = "field_" + int_to_dec(field_num);
    }

    if (auto it = owners.find(name); it != owners.end() && it->second != field_num) {
      return Error{"omnibuf: distinct field numbers collide on resolved field name"};
    }
    owners[name] = field_num;

    auto shape_res = blind_field_shape(data, pos, wt, depth, state);
    if (!shape_res) return Error{shape_res.error()};
    auto shape_ptr = std::move(shape_res.value());
    if (auto it = shapes.find(field_num); it != shapes.end()) {
      auto [u, compat] = unify_blind_shapes(it->second, shape_ptr);
      if (!compat) {
        return Error{"omnibuf: field changes wire shape within one message"};
      }
      shape_ptr = std::move(u);
    }
    shapes[field_num] = std::move(shape_ptr);

    const auto child_schema = BlindSchema::child_of(active_schema, field_num);

    switch (wt) {
      case WireUint: {
        const auto [v, vn] = decode_varint(data.subspan(pos));
        if (vn == 0) return Error{"omnibuf: truncated WireUint"};
        pos += vn;
        out[name] = BlindValue(v);
        break;
      }
      case WireSint: {
        const auto [v, vn] = decode_varint(data.subspan(pos));
        if (vn == 0) return Error{"omnibuf: truncated WireSint"};
        pos += vn;
        out[name] = BlindValue(decode_zigzag(v));
        break;
      }
      case WireBoolFalse:
        out[name] = BlindValue(false);
        break;
      case WireBoolTrue:
        out[name] = BlindValue(true);
        break;
      case WireString: {
        auto s_res = read_str_slice(data, pos);
        if (!s_res) return Error{s_res.error()};
        const auto sp = s_res.value().slice;
        out[name] = BlindValue(std::string(reinterpret_cast<const char*>(sp.data()), sp.size()));
        pos = s_res.value().next_pos;
        break;
      }
      case WireBytes: {
        auto b_res = read_len_slice(data, pos, 0);
        if (!b_res) return Error{b_res.error()};
        const auto sp = b_res.value().slice;
        out[name] = BlindValue(BlindBytes{std::vector<uint8_t>(sp.begin(), sp.end())});
        pos = b_res.value().next_pos;
        break;
      }
      case WireSubMsg: {
        auto sub_res = read_len_slice(data, pos, 0);
        if (!sub_res) return Error{sub_res.error()};
        const auto sp = sub_res.value().slice;
        if (sp.size() > state.opts.max_message_bytes) {
          return Error{"omnibuf: submessage exceeds max_message_bytes"};
        }
        auto& entry = sub_packets[field_num];
        if (entry.count == 0) {
          entry.first = sp;
          entry.count = 1;
        } else {
          // Validate individual slices before concatenation so a truncated tag/value
          // at the end of an earlier slice cannot fuse with the next slice, without
          // charging state.total_elements twice (the merged bytes are decoded right below).
          DecodeState seg_state{state.opts, 0};
          if (entry.count == 1) {
            auto vr0 = validate_message_bytes(entry.first, depth + 1, seg_state);
            if (!vr0) return Error{vr0.error()};
            entry.merged.reserve(entry.first.size() + sp.size());
            entry.merged.insert(entry.merged.end(), entry.first.begin(), entry.first.end());
          }
          DecodeState sp_state{state.opts, 0};
          auto vr1 = validate_message_bytes(sp, depth + 1, sp_state);
          if (!vr1) return Error{vr1.error()};
          if (sp.size() > state.opts.max_message_bytes - entry.merged.size()) {
            return Error{"omnibuf: merged submessage exceeds max_message_bytes"};
          }
          entry.merged.insert(entry.merged.end(), sp.begin(), sp.end());
          ++entry.count;
        }
        pos = sub_res.value().next_pos;
        break;
      }
      case WireArray: {
        auto arr_slice = read_len_slice(data, pos, 2);
        if (!arr_slice) return Error{arr_slice.error()};
        auto it = out.find(name);
        if (it == out.end() || !std::holds_alternative<BlindArray>(it->second.v)) {
          BlindArray arr;
          auto a_res =
              decode_array_body_blind(arr, arr_slice.value().slice, child_schema, depth + 1, state);
          if (!a_res) return Error{a_res.error()};
          out[name] = BlindValue(std::move(arr));
        } else {
          auto& arr = std::get<BlindArray>(it->second.v);
          auto a_res =
              decode_array_body_blind(arr, arr_slice.value().slice, child_schema, depth + 1, state);
          if (!a_res) return Error{a_res.error()};
        }
        pos = arr_slice.value().next_pos;
        break;
      }
      case WireMap: {
        auto map_slice = read_len_slice(data, pos, 3);
        if (!map_slice) return Error{map_slice.error()};
        auto it = out.find(name);
        if (it == out.end() || !std::holds_alternative<BlindObject>(it->second.v)) {
          BlindObject mp;
          auto m_res =
              decode_map_body_blind(mp, map_slice.value().slice, child_schema, depth + 1, state);
          if (!m_res) return Error{m_res.error()};
          out[name] = BlindValue(std::move(mp));
        } else {
          auto& mp = std::get<BlindObject>(it->second.v);
          auto m_res =
              decode_map_body_blind(mp, map_slice.value().slice, child_schema, depth + 1, state);
          if (!m_res) return Error{m_res.error()};
        }
        pos = map_slice.value().next_pos;
        break;
      }
      case WireExtended:
        return Error{"omnibuf: unrecognized WireExtended subtype"};
      case WireFixed32:
      case WireFloat32:
      case WireSfixed32: {
        if (n - pos < 4) return Error{"omnibuf: truncated 32-bit field"};
        const uint32_t bits = read_u32_le(data.data() + pos);
        if (wt == WireFloat32) {
          out[name] = BlindValue(bits_to_float32(bits));
        } else if (wt == WireSfixed32) {
          out[name] = BlindValue(static_cast<int64_t>(static_cast<int32_t>(bits)));
        } else {
          out[name] = BlindValue(static_cast<uint64_t>(bits));
        }
        pos += 4;
        break;
      }
      case WireFixed64:
      case WireFloat64:
      case WireSfixed64: {
        if (n - pos < 8) return Error{"omnibuf: truncated 64-bit field"};
        const uint64_t bits = read_u64_le(data.data() + pos);
        if (wt == WireFloat64) {
          out[name] = BlindValue(bits_to_float64(bits));
        } else if (wt == WireSfixed64) {
          out[name] = BlindValue(static_cast<int64_t>(bits));
        } else {
          out[name] = BlindValue(bits);
        }
        pos += 8;
        break;
      }
      default:
        return Error{"omnibuf: unsupported wire type"};
    }
  }

  for (const auto& [field_num, entry] : sub_packets) {
    std::string name = active_schema ? active_schema->field_name(field_num) : std::string{};
    if (name.empty()) {
      name = "field_" + int_to_dec(field_num);
    }
    const auto child_schema = BlindSchema::child_of(active_schema, field_num);
    const std::span<const uint8_t> sub_span =
        (entry.count == 1) ? entry.first : std::span<const uint8_t>(entry.merged);
    auto sub_obj = decode_blind_with_schema(sub_span, child_schema, depth + 1, state);
    if (!sub_obj) return Error{sub_obj.error()};
    out[name] = BlindValue(std::move(sub_obj.value()));
  }

  return out;
}

inline void append_json_escaped_string(std::string& out, std::string_view s) {
  static constexpr char kHex[] = "0123456789abcdef";
  out.push_back('"');
  for (unsigned char c : s) {
    switch (c) {
      case '"':
        out += "\\\"";
        break;
      case '\\':
        out += "\\\\";
        break;
      case '\b':
        out += "\\b";
        break;
      case '\f':
        out += "\\f";
        break;
      case '\n':
        out += "\\n";
        break;
      case '\r':
        out += "\\r";
        break;
      case '\t':
        out += "\\t";
        break;
      default:
        if (c < 0x20u) {
          out += "\\u00";
          out.push_back(kHex[(c >> 4) & 0x0Fu]);
          out.push_back(kHex[c & 0x0Fu]);
        } else {
          out.push_back(static_cast<char>(c));
        }
        break;
    }
  }
  out.push_back('"');
}

inline void append_blind_value_json(std::string& out,
                                    const BlindValue& val,
                                    bool& has_non_finite,
                                    bool& depth_exceeded,
                                    int depth = 0) {
  if (depth_exceeded) return;
  std::visit(
      [&](const auto& x) {
        using T = std::decay_t<decltype(x)>;
        if constexpr (std::is_same_v<T, std::monostate>) {
          out += "null";
        } else if constexpr (std::is_same_v<T, bool>) {
          out += x ? "true" : "false";
        } else if constexpr (std::is_same_v<T, uint64_t> || std::is_same_v<T, int64_t>) {
          char buf[32];
          auto [ptr, ec] = std::to_chars(buf, buf + sizeof(buf), x);
          (void)ec;
          out.append(buf, ptr);
        } else if constexpr (std::is_same_v<T, float> || std::is_same_v<T, double>) {
          if (!std::isfinite(x)) {
            has_non_finite = true;
            out += "null";
            return;
          }
          if (x == 0 && std::signbit(x)) {
            out += "-0";
            return;
          }
          char buf[64];
          auto [ptr, ec] = std::to_chars(buf, buf + sizeof(buf), x);
          (void)ec;
          out.append(buf, ptr);
        } else if constexpr (std::is_same_v<T, std::string>) {
          append_json_escaped_string(out, x);
        } else if constexpr (std::is_same_v<T, BlindBytes>) {
          append_json_escaped_string(out, base64_encode(x.data));
        } else if constexpr (std::is_same_v<T, BlindArray>) {
          if (depth > MAX_CONTAINER_DEPTH) {
            depth_exceeded = true;
            return;
          }
          out.push_back('[');
          for (size_t i = 0; i < x.size(); ++i) {
            if (i > 0) out.push_back(',');
            append_blind_value_json(out, x[i], has_non_finite, depth_exceeded, depth + 1);
            if (depth_exceeded) return;
          }
          out.push_back(']');
        } else if constexpr (std::is_same_v<T, BlindObject>) {
          if (depth > MAX_CONTAINER_DEPTH) {
            depth_exceeded = true;
            return;
          }
          out.push_back('{');
          bool first = true;
          for (const auto& [k, v] : x) {
            if (!first) out.push_back(',');
            first = false;
            append_json_escaped_string(out, k);
            out.push_back(':');
            append_blind_value_json(out, v, has_non_finite, depth_exceeded, depth + 1);
            if (depth_exceeded) return;
          }
          out.push_back('}');
        }
      },
      val.v);
}

inline void append_blind_value_json(std::string& out,
                                    const BlindValue& val,
                                    bool& has_non_finite) {
  bool depth_exceeded = false;
  append_blind_value_json(out, val, has_non_finite, depth_exceeded, 0);
}

}  // namespace detail

// Decodes any OmniBuf packet without a compile-time schema.
[[nodiscard]] inline Result<BlindObject> decode_blind(std::span<const uint8_t> packet,
                                                      DecodeOptions opts = {}) {
  DecodeState state{opts, 0};
  return detail::decode_blind_with_schema(packet, nullptr, 0, state);
}

// Serializes a BlindObject to canonical RFC 8259 JSON with sorted keys, exact 64-bit integers,
// shortest round-trip IEEE-754 float/double representation, and Base64 for bytes.
// Rejects non-finite IEEE-754 floats (NaN or Inf), which have no valid RFC 8259 representation.
[[nodiscard]] inline Result<std::string> stringify_blind_json(const BlindObject& obj) {
  std::string out;
  out.reserve(256);
  bool has_non_finite = false;
  bool depth_exceeded = false;
  detail::append_blind_value_json(out, BlindValue(obj), has_non_finite, depth_exceeded, 0);
  if (depth_exceeded) {
    return Error{"omnibuf: BlindValue nesting depth exceeds MAX_CONTAINER_DEPTH"};
  }
  if (has_non_finite) {
    return Error{"omnibuf: non-finite float cannot be serialized to RFC 8259 JSON"};
  }
  return out;
}

[[nodiscard]] inline Result<std::string> decode_blind_json(std::span<const uint8_t> packet,
                                                           DecodeOptions opts = {}) {
  auto res = decode_blind(packet, opts);
  if (!res) return Error{res.error()};
  return stringify_blind_json(res.value());
}

}  // namespace omnibuf
