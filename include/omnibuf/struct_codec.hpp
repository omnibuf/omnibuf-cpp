#pragma once

#include <algorithm>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

#include "omnibuf/decoder.hpp"
#include "omnibuf/encoder.hpp"
#include "omnibuf/field0.hpp"
#include "omnibuf/utf8.hpp"
#include "omnibuf/varint.hpp"

namespace omnibuf {

struct MarshalOptions {
  bool deterministic = true;
  bool self_describing = false;
};

enum class WireOverride : uint8_t {
  Default = 0,
  Uint = 1,
  Sint = 2,
  Fixed32 = 3,
  Fixed64 = 4,
  Sfixed32 = 5,
  Sfixed64 = 6,
};

// Specialize StructTraits<T> (or use OMNIBUF_STRUCT) to register native C++20 structs.
template <typename T>
struct StructTraits {
  static constexpr bool is_registered = false;
};

template <typename T>
concept RegisteredStruct = requires {
  { StructTraits<T>::type_name } -> std::convertible_to<std::string_view>;
  { StructTraits<T>::fields() };
};

template <uint32_t FieldNum, auto MemberPtr, WireOverride Override = WireOverride::Default>
struct FieldDescriptor {
  static constexpr uint32_t number = FieldNum;
  static constexpr auto member = MemberPtr;
  static constexpr WireOverride wire_override = Override;
  std::string_view name;
};

template <uint32_t FieldNum, auto MemberPtr, WireOverride Override = WireOverride::Default>
constexpr auto field(std::string_view name) noexcept {
  return FieldDescriptor<FieldNum, MemberPtr, Override>{name};
}

namespace detail {

template <typename T>
struct IsStdVector : std::false_type {};
template <typename T, typename A>
struct IsStdVector<std::vector<T, A>> : std::true_type {};

template <typename T>
struct IsStdOptional : std::false_type {};
template <typename T>
struct IsStdOptional<std::optional<T>> : std::true_type {};

template <typename T>
struct IsUniquePtr : std::false_type {};
template <typename T, typename D>
struct IsUniquePtr<std::unique_ptr<T, D>> : std::true_type {};

template <typename T>
struct IsStdMap : std::false_type {};
template <typename K, typename V, typename C, typename A>
struct IsStdMap<std::map<K, V, C, A>> : std::true_type {};

template <typename T>
const void* struct_type_token() noexcept {
  static const int token = 0;
  return &token;
}

template <typename T, WireOverride Override>
consteval uint8_t scalar_wire_type() {
  if constexpr (Override == WireOverride::Uint) {
    return WireUint;
  } else if constexpr (Override == WireOverride::Sint) {
    return WireSint;
  } else if constexpr (Override == WireOverride::Fixed32) {
    return std::is_signed_v<T> ? WireSfixed32 : WireFixed32;
  } else if constexpr (Override == WireOverride::Fixed64) {
    return std::is_signed_v<T> ? WireSfixed64 : WireFixed64;
  } else if constexpr (Override == WireOverride::Sfixed32) {
    return WireSfixed32;
  } else if constexpr (Override == WireOverride::Sfixed64) {
    return WireSfixed64;
  } else if constexpr (std::is_same_v<T, bool>) {
    return WireBoolFalse;
  } else if constexpr (std::is_same_v<T, float>) {
    return WireFloat32;
  } else if constexpr (std::is_same_v<T, double>) {
    return WireFloat64;
  } else if constexpr (std::is_same_v<T, std::string>) {
    return WireString;
  } else if constexpr (std::is_same_v<T, std::vector<uint8_t>>) {
    return WireBytes;
  } else if constexpr (std::integral<T> && std::is_unsigned_v<T>) {
    return WireUint;
  } else if constexpr (std::integral<T> && std::is_signed_v<T>) {
    return WireSint;
  } else if constexpr (std::is_enum_v<T>) {
    return std::is_unsigned_v<std::underlying_type_t<T>> ? WireUint : WireSint;
  } else {
    static_assert(sizeof(T) == 0, "Unsupported scalar type for OmniBuf struct codec");
    return 0;
  }
}

template <RegisteredStruct T>
inline std::shared_ptr<SchemaMeta> build_struct_schema_meta(int depth,
                                                            std::set<const void*>& active);

// Specialization helper for std::optional and std::unique_ptr
template <typename T>
struct PointeeType {
  using type = void;
};
template <typename T>
struct PointeeType<std::optional<T>> {
  using type = T;
};
template <typename T, typename D>
struct PointeeType<std::unique_ptr<T, D>> {
  using type = T;
};

template <RegisteredStruct T>
inline std::shared_ptr<SchemaMeta> build_struct_schema_meta(int depth,
                                                            std::set<const void*>& active) {
  const void* tok = struct_type_token<T>();
  if (depth > MAX_CONTAINER_DEPTH || active.contains(tok)) {
    return nullptr;
  }
  active.insert(tok);
  auto meta = std::make_shared<SchemaMeta>();
  const std::string_view full = StructTraits<T>::type_name;
  const size_t dot = full.rfind('.');
  meta->type_name = std::string(dot == std::string_view::npos ? full : full.substr(dot + 1));

  const auto fields = StructTraits<T>::fields();
  std::apply(
      [&](const auto&... fd) {
        (
            [&] {
              using MemberType =
                  std::remove_cvref_t<decltype(std::declval<T>().*(fd.member))>;
              meta->fields[fd.number] = std::string(fd.name);
              if constexpr (RegisteredStruct<MemberType>) {
                if (auto c = build_struct_schema_meta<MemberType>(depth + 1, active)) {
                  meta->children[fd.number] = std::move(c);
                }
              } else if constexpr (IsStdOptional<MemberType>::value ||
                                   IsUniquePtr<MemberType>::value) {
                using Inner = typename PointeeType<MemberType>::type;
                if constexpr (RegisteredStruct<Inner>) {
                  if (auto c = build_struct_schema_meta<Inner>(depth + 1, active)) {
                    meta->children[fd.number] = std::move(c);
                  }
                }
              } else if constexpr (IsStdVector<MemberType>::value &&
                                   !std::is_same_v<MemberType, std::vector<uint8_t>>) {
                using Elem = typename MemberType::value_type;
                if constexpr (RegisteredStruct<Elem>) {
                  if (auto c = build_struct_schema_meta<Elem>(depth + 2, active)) {
                    meta->children[fd.number] = std::move(c);
                  }
                } else if constexpr (IsUniquePtr<Elem>::value) {
                  using Inner = typename PointeeType<Elem>::type;
                  if constexpr (RegisteredStruct<Inner>) {
                    if (auto c = build_struct_schema_meta<Inner>(depth + 2, active)) {
                      meta->children[fd.number] = std::move(c);
                    }
                  }
                }
              } else if constexpr (IsStdMap<MemberType>::value) {
                using Val = typename MemberType::mapped_type;
                if constexpr (RegisteredStruct<Val>) {
                  if (auto c = build_struct_schema_meta<Val>(depth + 2, active)) {
                    meta->children[fd.number] = std::move(c);
                  }
                }
              }
            }(),
            ...);
      },
      fields);

  active.erase(tok);
  return meta;
}

template <RegisteredStruct T>
[[nodiscard]] inline Result<void> encode_struct_body(std::vector<uint8_t>& dst,
                                                     const T& msg,
                                                     MarshalOptions opts);

template <typename ValT, WireOverride Override>
[[nodiscard]] inline Result<void> encode_scalar_payload(std::vector<uint8_t>& dst,
                                                        const ValT& val) {
  constexpr uint8_t wt = scalar_wire_type<ValT, Override>();
  if constexpr (wt == WireBoolFalse) {
    dst.push_back(val ? uint8_t{1} : uint8_t{0});
  } else if constexpr (wt == WireUint) {
    append_varint(dst, static_cast<uint64_t>(val));
  } else if constexpr (wt == WireSint) {
    append_varint(dst, encode_zigzag(static_cast<int64_t>(val)));
  } else if constexpr (wt == WireFixed32 || wt == WireSfixed32) {
    append_u32_le(dst, static_cast<uint32_t>(val));
  } else if constexpr (wt == WireFixed64 || wt == WireSfixed64) {
    append_u64_le(dst, static_cast<uint64_t>(val));
  } else if constexpr (wt == WireFloat32) {
    append_u32_le(dst, float32_to_bits(val));
  } else if constexpr (wt == WireFloat64) {
    append_u64_le(dst, float64_to_bits(val));
  } else if constexpr (wt == WireString) {
    if (!is_valid_utf8(val)) {
      return Error{"omnibuf: string field contains invalid UTF-8"};
    }
    append_varint(dst, val.size());
    dst.insert(dst.end(), val.begin(), val.end());
  } else if constexpr (wt == WireBytes) {
    append_varint(dst, val.size());
    dst.insert(dst.end(), val.begin(), val.end());
  }
  return Result<void>::success();
}

template <uint32_t FieldNum, WireOverride Override, typename FieldT>
[[nodiscard]] inline Result<void> encode_struct_field(std::vector<uint8_t>& dst,
                                                      const FieldT& value,
                                                      MarshalOptions opts) {
  if constexpr (RegisteredStruct<FieldT>) {
    append_tag(dst, FieldNum, WireSubMsg);
    const size_t len_pos = dst.size();
    dst.push_back(0);
    dst.push_back(0);
    auto res = encode_struct_body(dst, value, opts);
    if (!res) return res;
    backpatch_container_len(dst, len_pos);
    return Result<void>::success();
  } else if constexpr (IsStdOptional<FieldT>::value || IsUniquePtr<FieldT>::value) {
    if (!value) {
      return Result<void>::success();
    }
    using Inner = typename PointeeType<FieldT>::type;
    if constexpr (RegisteredStruct<Inner>) {
      append_tag(dst, FieldNum, WireSubMsg);
      const size_t len_pos = dst.size();
      dst.push_back(0);
      dst.push_back(0);
      auto res = encode_struct_body(dst, *value, opts);
      if (!res) return res;
      backpatch_container_len(dst, len_pos);
      return Result<void>::success();
    } else {
      return encode_struct_field<FieldNum, Override>(dst, *value, opts);
    }
  } else if constexpr (IsStdVector<FieldT>::value &&
                       !std::is_same_v<FieldT, std::vector<uint8_t>>) {
    using Elem = typename FieldT::value_type;
    append_tag(dst, FieldNum, WireArray);
    const size_t len_pos = dst.size();
    dst.push_back(0);
    dst.push_back(0);
    if constexpr (RegisteredStruct<Elem>) {
      dst.push_back(WireSubMsg);
      append_varint(dst, value.size());
      for (const auto& elem : value) {
        const size_t e_len_pos = dst.size();
        dst.push_back(0);
        dst.push_back(0);
        auto res = encode_struct_body(dst, elem, opts);
        if (!res) return res;
        backpatch_container_len(dst, e_len_pos);
      }
    } else if constexpr (IsUniquePtr<Elem>::value) {
      using Inner = typename PointeeType<Elem>::type;
      static_assert(RegisteredStruct<Inner>, "unique_ptr vector element must be RegisteredStruct");
      dst.push_back(WireSubMsg);
      append_varint(dst, value.size());
      for (const auto& elem_ptr : value) {
        const size_t e_len_pos = dst.size();
        dst.push_back(0);
        dst.push_back(0);
        if (elem_ptr) {
          auto res = encode_struct_body(dst, *elem_ptr, opts);
          if (!res) return res;
        }
        backpatch_container_len(dst, e_len_pos);
      }
    } else {
      constexpr uint8_t elem_wt = scalar_wire_type<Elem, Override>();
      dst.push_back(elem_wt);
      append_varint(dst, value.size());
      if constexpr (std::is_same_v<Elem, bool>) {
        for (bool b : value) {
          dst.push_back(b ? uint8_t{1} : uint8_t{0});
        }
      } else {
        for (const auto& elem : value) {
          auto res = encode_scalar_payload<Elem, Override>(dst, elem);
          if (!res) return res;
        }
      }
    }
    backpatch_container_len(dst, len_pos);
    return Result<void>::success();
  } else if constexpr (IsStdMap<FieldT>::value) {
    using K = typename FieldT::key_type;
    using V = typename FieldT::mapped_type;
    constexpr uint8_t key_wt = scalar_wire_type<K, WireOverride::Default>();
    constexpr uint8_t val_wt =
        RegisteredStruct<V> ? WireSubMsg : scalar_wire_type<V, Override>();
    append_tag(dst, FieldNum, WireMap);
    const size_t len_pos = dst.size();
    dst.push_back(0);
    dst.push_back(0);
    dst.push_back(key_wt);
    dst.push_back(val_wt);
    append_varint(dst, value.size());
    // std::map iterates keys in ascending deterministic order automatically.
    for (const auto& [k, v] : value) {
      auto k_res = encode_scalar_payload<K, WireOverride::Default>(dst, k);
      if (!k_res) return k_res;
      if constexpr (RegisteredStruct<V>) {
        const size_t v_len_pos = dst.size();
        dst.push_back(0);
        dst.push_back(0);
        auto v_res = encode_struct_body(dst, v, opts);
        if (!v_res) return v_res;
        backpatch_container_len(dst, v_len_pos);
      } else {
        auto v_res = encode_scalar_payload<V, Override>(dst, v);
        if (!v_res) return v_res;
      }
    }
    backpatch_container_len(dst, len_pos);
    return Result<void>::success();
  } else if constexpr (std::is_same_v<FieldT, bool>) {
    append_tag(dst, FieldNum, value ? WireBoolTrue : WireBoolFalse);
    return Result<void>::success();
  } else {
    constexpr uint8_t wt = scalar_wire_type<FieldT, Override>();
    append_tag(dst, FieldNum, wt);
    return encode_scalar_payload<FieldT, Override>(dst, value);
  }
}

template <RegisteredStruct T>
[[nodiscard]] inline Result<void> encode_struct_body(std::vector<uint8_t>& dst,
                                                     const T& msg,
                                                     MarshalOptions opts) {
  const auto fields = StructTraits<T>::fields();
  Result<void> status = Result<void>::success();
  std::apply(
      [&](const auto&... fd) {
        (
            [&] {
              if (!status) return;
              using FD = std::remove_cvref_t<decltype(fd)>;
              status = encode_struct_field<FD::number, FD::wire_override>(dst, msg.*(FD::member), opts);
            }(),
            ...);
      },
      fields);
  return status;
}

template <RegisteredStruct T>
[[nodiscard]] inline Result<void> decode_struct_body(std::span<const uint8_t> data,
                                                     T& msg,
                                                     int depth,
                                                     bool merge,
                                                     bool is_fresh = false);

template <typename ValT, WireOverride Override>
[[nodiscard]] inline Result<size_t> decode_scalar_payload(std::span<const uint8_t> data,
                                                          size_t pos,
                                                          ValT& out) {
  constexpr uint8_t wt = scalar_wire_type<ValT, Override>();
  if constexpr (wt == WireBoolFalse) {
    if (pos >= data.size() || data[pos] > 1u) {
      return Error{"omnibuf: invalid bool byte"};
    }
    out = (data[pos] == 1u);
    return pos + 1;
  } else if constexpr (wt == WireUint) {
    const auto [v, vn] = decode_varint(data.subspan(pos));
    if (vn == 0) return Error{"omnibuf: truncated WireUint"};
    out = static_cast<ValT>(v);
    return pos + vn;
  } else if constexpr (wt == WireSint) {
    const auto [v, vn] = decode_varint(data.subspan(pos));
    if (vn == 0) return Error{"omnibuf: truncated WireSint"};
    out = static_cast<ValT>(decode_zigzag(v));
    return pos + vn;
  } else if constexpr (wt == WireFixed32 || wt == WireSfixed32) {
    if (data.size() - pos < 4) return Error{"omnibuf: truncated 32-bit fixed"};
    const uint32_t bits = read_u32_le(data.data() + pos);
    if constexpr (std::is_signed_v<ValT>) {
      out = static_cast<ValT>(static_cast<int32_t>(bits));
    } else {
      out = static_cast<ValT>(bits);
    }
    return pos + 4;
  } else if constexpr (wt == WireFixed64 || wt == WireSfixed64) {
    if (data.size() - pos < 8) return Error{"omnibuf: truncated 64-bit fixed"};
    const uint64_t bits = read_u64_le(data.data() + pos);
    if constexpr (std::is_signed_v<ValT>) {
      out = static_cast<ValT>(static_cast<int64_t>(bits));
    } else {
      out = static_cast<ValT>(bits);
    }
    return pos + 8;
  } else if constexpr (wt == WireFloat32) {
    if (data.size() - pos < 4) return Error{"omnibuf: truncated float32"};
    out = bits_to_float32(read_u32_le(data.data() + pos));
    return pos + 4;
  } else if constexpr (wt == WireFloat64) {
    if (data.size() - pos < 8) return Error{"omnibuf: truncated float64"};
    out = bits_to_float64(read_u64_le(data.data() + pos));
    return pos + 8;
  } else if constexpr (wt == WireString) {
    auto s_res = read_str_slice(data, pos);
    if (!s_res) return Error{s_res.error()};
    const auto sp = s_res.value().slice;
    out.assign(reinterpret_cast<const char*>(sp.data()), sp.size());
    return s_res.value().next_pos;
  } else if constexpr (wt == WireBytes) {
    auto b_res = read_len_slice(data, pos, 0);
    if (!b_res) return Error{b_res.error()};
    const auto sp = b_res.value().slice;
    out.assign(sp.begin(), sp.end());
    return b_res.value().next_pos;
  }
}

template <WireOverride Override, typename FieldT>
[[nodiscard]] inline Result<size_t> decode_matched_struct_field(std::span<const uint8_t> data,
                                                                size_t pos,
                                                                uint8_t wt,
                                                                FieldT& field_ref,
                                                                int depth,
                                                                bool already_seen) {
  if constexpr (RegisteredStruct<FieldT>) {
    if (wt != WireSubMsg) {
      return validate_value(data, pos, wt, depth);
    }
    auto sub_res = read_len_slice(data, pos, 0);
    if (!sub_res) return Error{sub_res.error()};
    auto dec_res = decode_struct_body(sub_res.value().slice, field_ref, depth + 1, already_seen);
    if (!dec_res) return Error{dec_res.error()};
    return sub_res.value().next_pos;
  } else if constexpr (IsStdOptional<FieldT>::value) {
    using Inner = typename FieldT::value_type;
    if constexpr (RegisteredStruct<Inner>) {
      if (wt != WireSubMsg) {
        return validate_value(data, pos, wt, depth);
      }
      auto sub_res = read_len_slice(data, pos, 0);
      if (!sub_res) return Error{sub_res.error()};
      bool fresh = false;
      if (!field_ref.has_value() || !already_seen) {
        field_ref.emplace();
        fresh = true;
      }
      auto dec_res = decode_struct_body(sub_res.value().slice, *field_ref, depth + 1, already_seen, fresh);
      if (!dec_res) return Error{dec_res.error()};
      return sub_res.value().next_pos;
    } else {
      Inner tmp{};
      auto res = decode_matched_struct_field<Override>(data, pos, wt, tmp, depth, already_seen);
      if (!res) return res;
      field_ref = std::move(tmp);
      return res;
    }
  } else if constexpr (IsUniquePtr<FieldT>::value) {
    using Inner = typename PointeeType<FieldT>::type;
    static_assert(RegisteredStruct<Inner>, "unique_ptr field must point to RegisteredStruct");
    if (wt != WireSubMsg) {
      return validate_value(data, pos, wt, depth);
    }
    auto sub_res = read_len_slice(data, pos, 0);
    if (!sub_res) return Error{sub_res.error()};
    bool fresh = false;
    if (!field_ref || !already_seen) {
      field_ref = std::make_unique<Inner>();
      fresh = true;
    }
    auto dec_res = decode_struct_body(sub_res.value().slice, *field_ref, depth + 1, already_seen, fresh);
    if (!dec_res) return Error{dec_res.error()};
    return sub_res.value().next_pos;
  } else if constexpr (IsStdVector<FieldT>::value &&
                       !std::is_same_v<FieldT, std::vector<uint8_t>>) {
    if (wt != WireArray) {
      return validate_value(data, pos, wt, depth);
    }
    if (depth + 1 > MAX_CONTAINER_DEPTH) {
      return Error{"omnibuf: array nesting depth exceeds MAX_CONTAINER_DEPTH"};
    }
    auto body_res = read_len_slice(data, pos, 2);
    if (!body_res) return Error{body_res.error()};
    const auto body = body_res.value().slice;
    const auto hdr = parse_array_header(body);
    if (!hdr.ok) return Error{"omnibuf: invalid WireArray header"};

    if (!already_seen) {
      field_ref.clear();
    }

    using Elem = typename FieldT::value_type;
    if constexpr (RegisteredStruct<Elem>) {
      if (hdr.elem_wt != WireSubMsg) return Error{"omnibuf: WireArray element type mismatch"};
      field_ref.reserve(grow_capacity(field_ref.capacity(), field_ref.size() + hdr.count));
      size_t p = hdr.pos;
      for (size_t i = 0; i < hdr.count; ++i) {
        auto elem_slice = read_len_slice(body, p, 0);
        if (!elem_slice) return Error{elem_slice.error()};
        Elem item{};
        auto dec_res = decode_struct_body(elem_slice.value().slice, item, depth + 2, false, true);
        if (!dec_res) return Error{dec_res.error()};
        field_ref.push_back(std::move(item));
        p = elem_slice.value().next_pos;
      }
      if (p != body.size()) return Error{"omnibuf: WireArray(submsg) trailing bytes"};
    } else if constexpr (IsUniquePtr<Elem>::value) {
      using Inner = typename PointeeType<Elem>::type;
      if (hdr.elem_wt != WireSubMsg) return Error{"omnibuf: WireArray element type mismatch"};
      field_ref.reserve(grow_capacity(field_ref.capacity(), field_ref.size() + hdr.count));
      size_t p = hdr.pos;
      for (size_t i = 0; i < hdr.count; ++i) {
        auto elem_slice = read_len_slice(body, p, 0);
        if (!elem_slice) return Error{elem_slice.error()};
        auto item = std::make_unique<Inner>();
        auto dec_res = decode_struct_body(elem_slice.value().slice, *item, depth + 2, false, true);
        if (!dec_res) return Error{dec_res.error()};
        field_ref.push_back(std::move(item));
        p = elem_slice.value().next_pos;
      }
      if (p != body.size()) return Error{"omnibuf: WireArray(submsg) trailing bytes"};
    } else {
      constexpr uint8_t exp_elem_wt = scalar_wire_type<Elem, Override>();
      if (hdr.elem_wt != exp_elem_wt) return Error{"omnibuf: WireArray element type mismatch"};
      if constexpr (std::is_same_v<Elem, bool>) {
        if (!append_bool_arr_body(field_ref, body)) return Error{"omnibuf: invalid bool array"};
      } else {
        field_ref.reserve(grow_capacity(field_ref.capacity(), field_ref.size() + hdr.count));
        size_t p = hdr.pos;
        for (size_t i = 0; i < hdr.count; ++i) {
          Elem item{};
          auto item_res = decode_scalar_payload<Elem, Override>(body, p, item);
          if (!item_res) return Error{item_res.error()};
          field_ref.push_back(std::move(item));
          p = item_res.value();
        }
        if (p != body.size()) return Error{"omnibuf: WireArray trailing bytes"};
      }
    }
    return body_res.value().next_pos;
  } else if constexpr (IsStdMap<FieldT>::value) {
    if (wt != WireMap) {
      return validate_value(data, pos, wt, depth);
    }
    if (depth + 1 > MAX_CONTAINER_DEPTH) {
      return Error{"omnibuf: map nesting depth exceeds MAX_CONTAINER_DEPTH"};
    }
    auto body_res = read_len_slice(data, pos, 3);
    if (!body_res) return Error{body_res.error()};
    const auto body = body_res.value().slice;
    const auto hdr = parse_map_header(body);
    if (!hdr.ok) return Error{"omnibuf: invalid WireMap header"};

    using K = typename FieldT::key_type;
    using V = typename FieldT::mapped_type;
    constexpr uint8_t exp_key_wt = scalar_wire_type<K, WireOverride::Default>();
    constexpr uint8_t exp_val_wt =
        RegisteredStruct<V> ? WireSubMsg : scalar_wire_type<V, Override>();
    if (hdr.key_wt != exp_key_wt || hdr.val_wt != exp_val_wt) {
      return Error{"omnibuf: WireMap key/value wire type mismatch"};
    }
    if (!already_seen) {
      field_ref.clear();
    }
    size_t p = hdr.pos;
    for (size_t i = 0; i < hdr.count; ++i) {
      K key{};
      auto k_res = decode_scalar_payload<K, WireOverride::Default>(body, p, key);
      if (!k_res) return Error{k_res.error()};
      p = k_res.value();
      if constexpr (RegisteredStruct<V>) {
        auto v_slice = read_len_slice(body, p, 0);
        if (!v_slice) return Error{v_slice.error()};
        V val{};
        auto dec_res = decode_struct_body(v_slice.value().slice, val, depth + 2, false);
        if (!dec_res) return Error{dec_res.error()};
        field_ref[std::move(key)] = std::move(val);
        p = v_slice.value().next_pos;
      } else {
        V val{};
        auto v_res = decode_scalar_payload<V, Override>(body, p, val);
        if (!v_res) return Error{v_res.error()};
        field_ref[std::move(key)] = std::move(val);
        p = v_res.value();
      }
    }
    if (p != body.size()) return Error{"omnibuf: WireMap trailing bytes"};
    return body_res.value().next_pos;
  } else if constexpr (std::is_same_v<FieldT, bool>) {
    if (wt == WireBoolTrue) {
      field_ref = true;
      return pos;
    }
    if (wt == WireBoolFalse) {
      field_ref = false;
      return pos;
    }
    return validate_value(data, pos, wt, depth);
  } else {
    constexpr uint8_t exp_wt = scalar_wire_type<FieldT, Override>();
    if (wt != exp_wt) {
      return validate_value(data, pos, wt, depth);
    }
    return decode_scalar_payload<FieldT, Override>(data, pos, field_ref);
  }
}

template <RegisteredStruct T>
[[nodiscard]] inline Result<void> decode_struct_body(std::span<const uint8_t> data,
                                                     T& msg,
                                                     int depth,
                                                     bool merge,
                                                     bool is_fresh) {
  if (depth > MAX_CONTAINER_DEPTH) {
    return Error{"omnibuf: message nesting depth exceeds MAX_CONTAINER_DEPTH"};
  }
  if (!merge && !is_fresh) {
    msg = T{};
  }
  const auto fields = StructTraits<T>::fields();
  constexpr size_t num_fields = std::tuple_size_v<std::remove_cvref_t<decltype(fields)>>;
  std::array<bool, (num_fields > 0 ? num_fields : 1)> seen{};
  if (merge) {
    seen.fill(true);
  }

  size_t pos = 0;
  const size_t n = data.size();
  while (pos < n) {
    uint32_t fnum = 0;
    uint8_t wt = 0;
    const uint8_t b0 = data[pos];
    if ((b0 & 1u) != 0) {
      fnum = static_cast<uint32_t>(b0 >> 5);
      wt = static_cast<uint8_t>((b0 >> 1) & 0x0Fu);
      ++pos;
    } else if ((b0 & 2u) != 0 && pos + 2 <= n) {
      const uint16_t u = static_cast<uint16_t>(read_u16_le(data.data() + pos) >> 2);
      fnum = static_cast<uint32_t>(u >> 4);
      wt = static_cast<uint8_t>(u & 0x0Fu);
      pos += 2;
    } else {
      const auto [tag, tn] = decode_varint(data.subspan(pos));
      if (tn == 0 || (tag >> 4) > MAX_FIELD_NUMBER) {
        return Error{"omnibuf: invalid or truncated tag"};
      }
      pos += tn;
      fnum = static_cast<uint32_t>(tag >> 4);
      wt = static_cast<uint8_t>(tag & 0x0Fu);
    }

    if (fnum == 0) {
      auto f0_res = validate_field0(data, pos, wt, depth);
      if (!f0_res) return Error{f0_res.error()};
      pos = f0_res.value();
      continue;
    }

    Result<size_t> step_res = pos;
    const bool matched = [&]<size_t... Is>(std::index_sequence<Is...>) {
      return (
          [&]() -> bool {
            const auto& fd = std::get<Is>(fields);
            using FD = std::remove_cvref_t<decltype(fd)>;
            if (FD::number != fnum) {
              return false;
            }
            const bool was_seen = seen[Is];
            step_res = decode_matched_struct_field<FD::wire_override>(
                data, pos, wt, msg.*(FD::member), depth, was_seen);
            seen[Is] = true;
            return true;
          }() ||
          ...);
    }(std::make_index_sequence<num_fields>{});

    if (!matched) {
      step_res = validate_value(data, pos, wt, depth);
    }
    if (!step_res) {
      return Error{step_res.error()};
    }
    pos = step_res.value();
  }
  return Result<void>::success();
}

}  // namespace detail

template <RegisteredStruct T>
[[nodiscard]] inline SchemaMeta struct_schema_meta() {
  std::set<const void*> active;
  auto ptr = detail::build_struct_schema_meta<T>(0, active);
  return ptr ? *ptr : SchemaMeta{};
}

template <RegisteredStruct T>
[[nodiscard]] inline Result<std::vector<uint8_t>> marshal(const T& msg,
                                                          MarshalOptions opts = {}) {
  std::vector<uint8_t> dst;
  dst.reserve(512);
  if (opts.self_describing) {
    std::set<const void*> active;
    if (auto meta = detail::build_struct_schema_meta<T>(0, active)) {
      append_field0_schema_meta(dst, *meta);
    }
  }
  MarshalOptions body_opts = opts;
  body_opts.self_describing = false;
  auto res = detail::encode_struct_body(dst, msg, body_opts);
  if (!res) return Error{res.error()};
  return dst;
}

template <RegisteredStruct T>
[[nodiscard]] inline Result<void> unmarshal(std::span<const uint8_t> data, T& msg) {
  return detail::decode_struct_body(data, msg, 0, false);
}

}  // namespace omnibuf
