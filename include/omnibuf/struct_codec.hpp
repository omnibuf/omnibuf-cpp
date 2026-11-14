#pragma once

#include <algorithm>
#include <array>
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
#include <unordered_map>
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

template <uint32_t FieldNum,
          auto MemberPtr,
          WireOverride Override = WireOverride::Default,
          WireOverride KeyOverride = WireOverride::Default>
struct FieldDescriptor {
  static_assert(FieldNum >= 1 && FieldNum <= MAX_FIELD_NUMBER,
                "OmniBuf FieldDescriptor FieldNum must be in 1..MAX_FIELD_NUMBER");
  static constexpr uint32_t number = FieldNum;
  static constexpr auto member = MemberPtr;
  static constexpr WireOverride wire_override = Override;
  static constexpr WireOverride key_wire_override = KeyOverride;
  std::string_view name;
};

template <uint32_t FieldNum,
          auto MemberPtr,
          WireOverride Override = WireOverride::Default,
          WireOverride KeyOverride = WireOverride::Default>
constexpr auto field(std::string_view name) noexcept {
  return FieldDescriptor<FieldNum, MemberPtr, Override, KeyOverride>{name};
}

namespace detail {

template <typename Tuple, size_t... Is>
consteval bool tuple_field_numbers_valid(std::index_sequence<Is...>) {
  constexpr size_t N = sizeof...(Is);
  if constexpr (N == 0) {
    return true;
  } else {
    const uint32_t nums[N] = {std::tuple_element_t<Is, Tuple>::number...};
    for (size_t i = 0; i < N; ++i) {
      if (nums[i] < 1 || nums[i] > MAX_FIELD_NUMBER) return false;
      for (size_t j = i + 1; j < N; ++j) {
        if (nums[i] == nums[j]) return false;
      }
    }
    return true;
  }
}

template <RegisteredStruct T>
consteval bool validate_struct_fields() {
  using Tuple = std::remove_cvref_t<decltype(StructTraits<T>::fields())>;
  return tuple_field_numbers_valid<Tuple>(std::make_index_sequence<std::tuple_size_v<Tuple>>{});
}

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
struct IsBox : std::false_type {};
template <typename T>
struct IsBox<Box<T>> : std::true_type {};

template <typename T>
struct IsStdMap : std::false_type {};
template <typename K, typename V, typename C, typename A>
struct IsStdMap<std::map<K, V, C, A>> : std::true_type {};

template <typename T>
struct IsStdUnorderedMap : std::false_type {};
template <typename K, typename V, typename H, typename E, typename A>
struct IsStdUnorderedMap<std::unordered_map<K, V, H, E, A>> : std::true_type {};

template <typename T>
inline constexpr bool IsAnyMapV = IsStdMap<T>::value || IsStdUnorderedMap<T>::value;

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
inline std::shared_ptr<SchemaMeta> build_struct_schema_meta(int schema_depth,
                                                            std::set<const void*>& active,
                                                            bool* cyclic = nullptr);

template <RegisteredStruct T>
inline std::shared_ptr<SchemaMeta> build_struct_value_schema_meta(const T& msg, int schema_depth);

// Specialization helper for std::optional, std::unique_ptr, and omnibuf::Box
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
template <typename T>
struct PointeeType<Box<T>> {
  using type = T;
};

template <typename T, WireOverride Override>
consteval uint8_t container_value_wire_type() {
  if constexpr (RegisteredStruct<T>) {
    return WireSubMsg;
  } else if constexpr (IsUniquePtr<T>::value || IsBox<T>::value) {
    using Inner = typename PointeeType<T>::type;
    static_assert(RegisteredStruct<Inner>,
                  "Smart-pointer container element must point to RegisteredStruct");
    return WireSubMsg;
  } else if constexpr (IsStdVector<T>::value && !std::is_same_v<T, std::vector<uint8_t>>) {
    return WireArray;
  } else if constexpr (IsAnyMapV<T>) {
    return WireMap;
  } else {
    return scalar_wire_type<T, Override>();
  }
}

template <typename T>
inline std::shared_ptr<SchemaMeta> build_nested_type_schema_meta(int schema_depth,
                                                                 std::set<const void*>& active,
                                                                 bool* cyclic) {
  if constexpr (RegisteredStruct<T>) {
    return build_struct_schema_meta<T>(schema_depth, active, cyclic);
  } else if constexpr (IsStdOptional<T>::value || IsUniquePtr<T>::value || IsBox<T>::value) {
    using Inner = typename PointeeType<T>::type;
    return build_nested_type_schema_meta<Inner>(schema_depth, active, cyclic);
  } else if constexpr (IsStdVector<T>::value && !std::is_same_v<T, std::vector<uint8_t>>) {
    using Elem = typename T::value_type;
    return build_nested_type_schema_meta<Elem>(schema_depth, active, cyclic);
  } else if constexpr (IsAnyMapV<T>) {
    using Val = typename T::mapped_type;
    return build_nested_type_schema_meta<Val>(schema_depth, active, cyclic);
  } else {
    return nullptr;
  }
}

template <RegisteredStruct T>
inline std::shared_ptr<SchemaMeta> build_struct_schema_meta(int schema_depth,
                                                            std::set<const void*>& active,
                                                            bool* cyclic) {
  static_assert(validate_struct_fields<T>(),
                "StructTraits<T>::fields() has out-of-range or duplicate field numbers");
  if (schema_depth + 1 > MAX_CONTAINER_DEPTH) {
    return nullptr;
  }
  const void* tok = struct_type_token<T>();
  if (active.contains(tok)) {
    if (cyclic != nullptr) *cyclic = true;
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
              if (schema_depth + 3 <= MAX_CONTAINER_DEPTH) {
                if (auto c = build_nested_type_schema_meta<MemberType>(schema_depth + 2, active,
                                                                       cyclic)) {
                  meta->children[fd.number] = std::move(c);
                }
              }
            }(),
            ...);
      },
      fields);

  active.erase(tok);
  return meta;
}

template <typename FieldT>
inline std::shared_ptr<SchemaMeta> build_field_value_schema_meta(const FieldT& val,
                                                                 int schema_depth) {
  if constexpr (RegisteredStruct<FieldT>) {
    return build_struct_value_schema_meta(val, schema_depth);
  } else if constexpr (IsStdOptional<FieldT>::value || IsUniquePtr<FieldT>::value ||
                       IsBox<FieldT>::value) {
    if (!val) return nullptr;
    return build_field_value_schema_meta(*val, schema_depth);
  } else if constexpr (IsStdVector<FieldT>::value &&
                       !std::is_same_v<FieldT, std::vector<uint8_t>>) {
    std::shared_ptr<SchemaMeta> merged;
    for (const auto& elem : val) {
      merged = merge_schema_meta(merged, build_field_value_schema_meta(elem, schema_depth));
    }
    return merged;
  } else if constexpr (IsAnyMapV<FieldT>) {
    std::shared_ptr<SchemaMeta> merged;
    for (const auto& [_, v] : val) {
      merged = merge_schema_meta(merged, build_field_value_schema_meta(v, schema_depth));
    }
    return merged;
  } else {
    return nullptr;
  }
}

template <RegisteredStruct T>
inline std::shared_ptr<SchemaMeta> build_struct_value_schema_meta(const T& msg, int schema_depth) {
  if (schema_depth + 1 > MAX_CONTAINER_DEPTH) {
    return nullptr;
  }
  auto meta = std::make_shared<SchemaMeta>();
  const std::string_view full = StructTraits<T>::type_name;
  const size_t dot = full.rfind('.');
  meta->type_name = std::string(dot == std::string_view::npos ? full : full.substr(dot + 1));

  const auto fields = StructTraits<T>::fields();
  std::apply(
      [&](const auto&... fd) {
        (
            [&] {
              meta->fields[fd.number] = std::string(fd.name);
              if (schema_depth + 3 <= MAX_CONTAINER_DEPTH) {
                if (auto c = build_field_value_schema_meta(msg.*(fd.member), schema_depth + 2)) {
                  meta->children[fd.number] = std::move(c);
                }
              }
            }(),
            ...);
      },
      fields);
  return meta;
}

template <RegisteredStruct T>
[[nodiscard]] inline const char* encode_struct_body(FastWriter& w,
                                                    const T& msg,
                                                    MarshalOptions opts);

template <typename ValT, WireOverride Override>
[[nodiscard]] inline const char* encode_scalar_payload(FastWriter& w, const ValT& val) {
  constexpr uint8_t wt = scalar_wire_type<ValT, Override>();
  if constexpr (wt == WireBoolFalse) {
    w.write_byte(val ? uint8_t{1} : uint8_t{0});
  } else if constexpr (wt == WireUint) {
    w.write_varint(static_cast<uint64_t>(val));
  } else if constexpr (wt == WireSint) {
    w.write_varint(encode_zigzag(static_cast<int64_t>(val)));
  } else if constexpr (wt == WireFixed32 || wt == WireSfixed32) {
    w.write_u32_raw(static_cast<uint32_t>(val));
  } else if constexpr (wt == WireFixed64 || wt == WireSfixed64) {
    w.write_u64_raw(static_cast<uint64_t>(val));
  } else if constexpr (wt == WireFloat32) {
    w.write_u32_raw(float32_to_bits(val));
  } else if constexpr (wt == WireFloat64) {
    w.write_u64_raw(float64_to_bits(val));
  } else if constexpr (wt == WireString) {
    if (!is_valid_utf8(val)) [[unlikely]] {
      return "omnibuf: string field contains invalid UTF-8";
    }
    w.write_varint(val.size());
    w.write_bytes(val.data(), val.size());
  } else if constexpr (wt == WireBytes) {
    w.write_varint(val.size());
    w.write_bytes(val.data(), val.size());
  }
  return nullptr;
}

template <WireOverride Override, WireOverride KeyOverride, typename ContainerT>
[[nodiscard]] inline const char* encode_vector_payload(FastWriter& w,
                                                       const ContainerT& value,
                                                       MarshalOptions opts);

template <WireOverride Override, WireOverride KeyOverride, typename MapT>
[[nodiscard]] inline const char* encode_map_payload(FastWriter& w,
                                                    const MapT& value,
                                                    MarshalOptions opts);

template <WireOverride Override, WireOverride KeyOverride, typename ValT>
[[nodiscard]] inline const char* encode_container_elem(FastWriter& w,
                                                       const ValT& val,
                                                       MarshalOptions opts) {
  if constexpr (RegisteredStruct<ValT>) {
    const size_t len_pos = w.reserve_len_2b();
    const char* err = encode_struct_body(w, val, opts);
    if (err != nullptr) [[unlikely]] return err;
    w.backpatch_len(len_pos);
    return nullptr;
  } else if constexpr (IsUniquePtr<ValT>::value || IsBox<ValT>::value) {
    using Inner = typename PointeeType<ValT>::type;
    static_assert(RegisteredStruct<Inner>, "Smart-pointer element must be RegisteredStruct");
    const size_t len_pos = w.reserve_len_2b();
    if (val) {
      const char* err = encode_struct_body(w, *val, opts);
      if (err != nullptr) [[unlikely]] return err;
    }
    w.backpatch_len(len_pos);
    return nullptr;
  } else if constexpr (IsStdVector<ValT>::value && !std::is_same_v<ValT, std::vector<uint8_t>>) {
    return encode_vector_payload<Override, KeyOverride>(w, val, opts);
  } else if constexpr (IsAnyMapV<ValT>) {
    return encode_map_payload<Override, KeyOverride>(w, val, opts);
  } else {
    return encode_scalar_payload<ValT, Override>(w, val);
  }
}

template <WireOverride Override, WireOverride KeyOverride, typename ContainerT>
[[nodiscard]] inline const char* encode_vector_payload(FastWriter& w,
                                                       const ContainerT& value,
                                                       MarshalOptions opts) {
  using Elem = typename ContainerT::value_type;
  constexpr uint8_t elem_wt = container_value_wire_type<Elem, Override>();
  const size_t len_pos = w.reserve_len_2b();
  w.write_byte(elem_wt);
  w.write_varint(value.size());
  if constexpr (std::is_same_v<Elem, bool>) {
    w.ensure(value.size());
    for (bool b : value) {
      w.write_byte(b ? uint8_t{1} : uint8_t{0});
    }
  } else {
    for (const auto& elem : value) {
      const char* err = encode_container_elem<Override, KeyOverride>(w, elem, opts);
      if (err != nullptr) [[unlikely]] return err;
    }
  }
  w.backpatch_len(len_pos);
  return nullptr;
}

template <WireOverride Override, WireOverride KeyOverride, typename MapT>
[[nodiscard]] inline const char* encode_map_payload(FastWriter& w,
                                                    const MapT& value,
                                                    MarshalOptions opts) {
  using K = typename MapT::key_type;
  using V = typename MapT::mapped_type;
  constexpr uint8_t key_wt = scalar_wire_type<K, KeyOverride>();
  static_assert(is_valid_map_key_wire_type(key_wt), "Unsupported OmniBuf WireMap key type");
  constexpr uint8_t val_wt = container_value_wire_type<V, Override>();

  const size_t len_pos = w.reserve_len_2b();
  w.write_byte(key_wt);
  w.write_byte(val_wt);
  w.write_varint(value.size());

  if constexpr (IsStdUnorderedMap<MapT>::value) {
    if (opts.deterministic && value.size() > 1) {
      using EntryPtr = const typename MapT::value_type*;
      std::vector<EntryPtr> sorted;
      sorted.reserve(value.size());
      for (const auto& kv : value) {
        sorted.push_back(&kv);
      }
      std::sort(sorted.begin(), sorted.end(),
                [](EntryPtr a, EntryPtr b) { return a->first < b->first; });
      for (EntryPtr kv : sorted) {
        const char* k_err = encode_scalar_payload<K, KeyOverride>(w, kv->first);
        if (k_err != nullptr) [[unlikely]] return k_err;
        const char* v_err = encode_container_elem<Override, KeyOverride>(w, kv->second, opts);
        if (v_err != nullptr) [[unlikely]] return v_err;
      }
      w.backpatch_len(len_pos);
      return nullptr;
    }
  }

  for (const auto& [k, v] : value) {
    const char* k_err = encode_scalar_payload<K, KeyOverride>(w, k);
    if (k_err != nullptr) [[unlikely]] return k_err;
    const char* v_err = encode_container_elem<Override, KeyOverride>(w, v, opts);
    if (v_err != nullptr) [[unlikely]] return v_err;
  }
  w.backpatch_len(len_pos);
  return nullptr;
}

template <uint32_t FieldNum,
          WireOverride Override,
          WireOverride KeyOverride = WireOverride::Default,
          typename FieldT>
[[nodiscard]] inline const char* encode_struct_field(FastWriter& w,
                                                     const FieldT& value,
                                                     MarshalOptions opts) {
  if constexpr (RegisteredStruct<FieldT>) {
    w.write_tag(FieldNum, WireSubMsg);
    const size_t len_pos = w.reserve_len_2b();
    const char* err = encode_struct_body(w, value, opts);
    if (err != nullptr) [[unlikely]] return err;
    w.backpatch_len(len_pos);
    return nullptr;
  } else if constexpr (IsStdOptional<FieldT>::value || IsUniquePtr<FieldT>::value ||
                       IsBox<FieldT>::value) {
    if (!value) {
      return nullptr;
    }
    using Inner = typename PointeeType<FieldT>::type;
    if constexpr (RegisteredStruct<Inner>) {
      w.write_tag(FieldNum, WireSubMsg);
      const size_t len_pos = w.reserve_len_2b();
      const char* err = encode_struct_body(w, *value, opts);
      if (err != nullptr) [[unlikely]] return err;
      w.backpatch_len(len_pos);
      return nullptr;
    } else {
      return encode_struct_field<FieldNum, Override, KeyOverride>(w, *value, opts);
    }
  } else if constexpr (IsStdVector<FieldT>::value &&
                       !std::is_same_v<FieldT, std::vector<uint8_t>>) {
    w.write_tag(FieldNum, WireArray);
    return encode_vector_payload<Override, KeyOverride>(w, value, opts);
  } else if constexpr (IsAnyMapV<FieldT>) {
    w.write_tag(FieldNum, WireMap);
    return encode_map_payload<Override, KeyOverride>(w, value, opts);
  } else if constexpr (std::is_same_v<FieldT, bool>) {
    w.write_tag(FieldNum, value ? WireBoolTrue : WireBoolFalse);
    return nullptr;
  } else {
    constexpr uint8_t wt = scalar_wire_type<FieldT, Override>();
    w.write_tag(FieldNum, wt);
    return encode_scalar_payload<FieldT, Override>(w, value);
  }
}

template <RegisteredStruct T>
[[nodiscard]] inline const char* encode_struct_body(FastWriter& w,
                                                    const T& msg,
                                                    MarshalOptions opts) {
  static_assert(validate_struct_fields<T>(),
                "StructTraits<T>::fields() has out-of-range or duplicate field numbers");
  const auto fields = StructTraits<T>::fields();
  const char* err = nullptr;
  std::apply(
      [&](const auto&... fd) {
        (
            [&] {
              if (err != nullptr) return;
              using FD = std::remove_cvref_t<decltype(fd)>;
              err = encode_struct_field<FD::number, FD::wire_override, FD::key_wire_override>(
                  w, msg.*(FD::member), opts);
            }(),
            ...);
      },
      fields);
  return err;
}

template <RegisteredStruct T>
[[nodiscard]] inline const char* decode_struct_body(std::span<const uint8_t> data,
                                                    T& msg,
                                                    int depth,
                                                    bool merge,
                                                    DecodeState& state,
                                                    bool is_fresh = false);

template <typename FieldT>
inline void clear_struct_member(FieldT& field_ref) {
  if constexpr (RegisteredStruct<FieldT>) {
    const auto fields = StructTraits<FieldT>::fields();
    std::apply([&](const auto&... fd) { (clear_struct_member(field_ref.*(fd.member)), ...); },
               fields);
  } else if constexpr (IsStdOptional<FieldT>::value || IsUniquePtr<FieldT>::value ||
                       IsBox<FieldT>::value) {
    field_ref.reset();
  } else if constexpr (IsStdVector<FieldT>::value || IsAnyMapV<FieldT> ||
                       std::is_same_v<FieldT, std::string>) {
    field_ref.clear();
  } else {
    field_ref = FieldT{};
  }
}

template <typename ValT, WireOverride Override>
[[nodiscard]] inline PosStep decode_scalar_payload(std::span<const uint8_t> data,
                                                   size_t pos,
                                                   ValT& out) {
  constexpr uint8_t wt = scalar_wire_type<ValT, Override>();
  if constexpr (wt == WireBoolFalse) {
    if (pos >= data.size() || data[pos] > 1u) [[unlikely]] {
      return {0, "omnibuf: invalid bool byte"};
    }
    out = (data[pos] == 1u);
    return {pos + 1, nullptr};
  } else if constexpr (wt == WireUint) {
    if (pos < data.size()) [[likely]] {
      const uint8_t b0 = data[pos];
      if ((b0 & 1u) != 0) [[likely]] {
        out = static_cast<ValT>(b0 >> 1);
        return {pos + 1, nullptr};
      }
    }
    const auto [v, vn] = decode_varint(data.subspan(pos));
    if (vn == 0) [[unlikely]] return {0, "omnibuf: truncated WireUint"};
    out = static_cast<ValT>(v);
    return {pos + vn, nullptr};
  } else if constexpr (wt == WireSint) {
    if (pos < data.size()) [[likely]] {
      const uint8_t b0 = data[pos];
      if ((b0 & 1u) != 0) [[likely]] {
        out = static_cast<ValT>(decode_zigzag(static_cast<uint64_t>(b0 >> 1)));
        return {pos + 1, nullptr};
      }
    }
    const auto [v, vn] = decode_varint(data.subspan(pos));
    if (vn == 0) [[unlikely]] return {0, "omnibuf: truncated WireSint"};
    out = static_cast<ValT>(decode_zigzag(v));
    return {pos + vn, nullptr};
  } else if constexpr (wt == WireFixed32 || wt == WireSfixed32) {
    if (data.size() - pos < 4) [[unlikely]] return {0, "omnibuf: truncated 32-bit fixed"};
    const uint32_t bits = read_u32_le(data.data() + pos);
    if constexpr (std::is_signed_v<ValT>) {
      out = static_cast<ValT>(static_cast<int32_t>(bits));
    } else {
      out = static_cast<ValT>(bits);
    }
    return {pos + 4, nullptr};
  } else if constexpr (wt == WireFixed64 || wt == WireSfixed64) {
    if (data.size() - pos < 8) [[unlikely]] return {0, "omnibuf: truncated 64-bit fixed"};
    const uint64_t bits = read_u64_le(data.data() + pos);
    if constexpr (std::is_signed_v<ValT>) {
      out = static_cast<ValT>(static_cast<int64_t>(bits));
    } else {
      out = static_cast<ValT>(bits);
    }
    return {pos + 8, nullptr};
  } else if constexpr (wt == WireFloat32) {
    if (data.size() - pos < 4) [[unlikely]] return {0, "omnibuf: truncated float32"};
    out = bits_to_float32(read_u32_le(data.data() + pos));
    return {pos + 4, nullptr};
  } else if constexpr (wt == WireFloat64) {
    if (data.size() - pos < 8) [[unlikely]] return {0, "omnibuf: truncated float64"};
    out = bits_to_float64(read_u64_le(data.data() + pos));
    return {pos + 8, nullptr};
  } else if constexpr (wt == WireString) {
    const auto s_res = read_str_slice_fast(data, pos);
    if (!s_res) [[unlikely]] return {0, s_res.err};
    out.assign(reinterpret_cast<const char*>(s_res.ptr), s_res.len);
    return {s_res.next_pos, nullptr};
  } else if constexpr (wt == WireBytes) {
    const auto b_res = read_len_slice_fast(data, pos, 0);
    if (!b_res) [[unlikely]] return {0, b_res.err};
    out.assign(b_res.ptr, b_res.ptr + b_res.len);
    return {b_res.next_pos, nullptr};
  }
}

[[nodiscard]] inline PosStep validate_value_step(std::span<const uint8_t> data,
                                                 size_t pos,
                                                 uint8_t wt,
                                                 int depth,
                                                 DecodeState& state) {
  auto res = validate_value(data, pos, wt, depth, state);
  if (!res) return {0, "omnibuf: invalid skipped field payload"};
  return {res.value(), nullptr};
}

template <WireOverride Override, WireOverride KeyOverride, typename ContainerT>
[[nodiscard]] inline const char* decode_vector_body(std::span<const uint8_t> body,
                                                    ContainerT& field_ref,
                                                    int depth,
                                                    DecodeState& state);

template <WireOverride Override, WireOverride KeyOverride, typename MapT>
[[nodiscard]] inline const char* decode_map_body_into(std::span<const uint8_t> body,
                                                      MapT& field_ref,
                                                      int depth,
                                                      DecodeState& state);

template <WireOverride Override, WireOverride KeyOverride, typename ValT>
[[nodiscard]] inline PosStep decode_container_elem(std::span<const uint8_t> body,
                                                   size_t p,
                                                   ValT& out,
                                                   int depth,
                                                   DecodeState& state) {
  if constexpr (RegisteredStruct<ValT>) {
    const auto elem_slice = read_len_slice_fast(body, p, 0);
    if (!elem_slice) [[unlikely]] return {0, elem_slice.err};
    const char* err = decode_struct_body(elem_slice.span(), out, depth + 1, false, state, true);
    if (err != nullptr) [[unlikely]] return {0, err};
    return {elem_slice.next_pos, nullptr};
  } else if constexpr (IsUniquePtr<ValT>::value) {
    using Inner = typename PointeeType<ValT>::type;
    const auto elem_slice = read_len_slice_fast(body, p, 0);
    if (!elem_slice) [[unlikely]] return {0, elem_slice.err};
    out = std::make_unique<Inner>();
    const char* err = decode_struct_body(elem_slice.span(), *out, depth + 1, false, state, true);
    if (err != nullptr) [[unlikely]] return {0, err};
    return {elem_slice.next_pos, nullptr};
  } else if constexpr (IsBox<ValT>::value) {
    const auto elem_slice = read_len_slice_fast(body, p, 0);
    if (!elem_slice) [[unlikely]] return {0, elem_slice.err};
    auto& inner = out.emplace();
    const char* err = decode_struct_body(elem_slice.span(), inner, depth + 1, false, state, true);
    if (err != nullptr) [[unlikely]] return {0, err};
    return {elem_slice.next_pos, nullptr};
  } else if constexpr (IsStdVector<ValT>::value && !std::is_same_v<ValT, std::vector<uint8_t>>) {
    if (depth + 1 > state.opts.max_depth) [[unlikely]] {
      return {0, "omnibuf: nested array depth exceeds max_depth"};
    }
    const auto arr_slice = read_len_slice_fast(body, p, 2);
    if (!arr_slice) [[unlikely]] return {0, arr_slice.err};
    const char* err =
        decode_vector_body<Override, KeyOverride>(arr_slice.span(), out, depth + 1, state);
    if (err != nullptr) [[unlikely]] return {0, err};
    return {arr_slice.next_pos, nullptr};
  } else if constexpr (IsAnyMapV<ValT>) {
    if (depth + 1 > state.opts.max_depth) [[unlikely]] {
      return {0, "omnibuf: nested map depth exceeds max_depth"};
    }
    const auto map_slice = read_len_slice_fast(body, p, 3);
    if (!map_slice) [[unlikely]] return {0, map_slice.err};
    const char* err =
        decode_map_body_into<Override, KeyOverride>(map_slice.span(), out, depth + 1, state);
    if (err != nullptr) [[unlikely]] return {0, err};
    return {map_slice.next_pos, nullptr};
  } else {
    return decode_scalar_payload<ValT, Override>(body, p, out);
  }
}

template <WireOverride Override, WireOverride KeyOverride, typename ContainerT>
[[nodiscard]] inline const char* decode_vector_body(std::span<const uint8_t> body,
                                                    ContainerT& field_ref,
                                                    int depth,
                                                    DecodeState& state) {
  using Elem = typename ContainerT::value_type;
  if constexpr (std::is_same_v<Elem, std::string>) {
    if (!append_string_arr_body(field_ref, body, &state)) [[unlikely]] {
      return "omnibuf: invalid string array";
    }
    return nullptr;
  } else if constexpr (std::is_same_v<Elem, std::vector<uint8_t>>) {
    if (!append_bytes_arr_body(field_ref, body, &state)) [[unlikely]] {
      return "omnibuf: invalid bytes array";
    }
    return nullptr;
  } else if constexpr (std::is_same_v<Elem, bool>) {
    if (!append_bool_arr_body(field_ref, body, &state)) [[unlikely]] {
      return "omnibuf: invalid bool array";
    }
    return nullptr;
  } else {
    constexpr uint8_t exp_elem_wt = container_value_wire_type<Elem, Override>();
    if constexpr (exp_elem_wt == WireUint && std::integral<Elem>) {
      if (!append_uint_arr_body(field_ref, body, &state)) [[unlikely]] {
        return "omnibuf: invalid uint array";
      }
      return nullptr;
    } else if constexpr (exp_elem_wt == WireSint && std::integral<Elem>) {
      if (!append_sint_arr_body(field_ref, body, &state)) [[unlikely]] {
        return "omnibuf: invalid sint array";
      }
      return nullptr;
    } else {
      const auto hdr = parse_array_header(body);
      if (!hdr.ok || hdr.elem_wt != exp_elem_wt) [[unlikely]] {
        return "omnibuf: WireArray element type mismatch";
      }
      if (!state.charge_elements(hdr.count)) [[unlikely]] {
        return "omnibuf: WireArray exceeds max_total_elements";
      }
      field_ref.reserve(bounded_reserve_target(field_ref.capacity(), field_ref.size(), hdr.count));
      size_t p = hdr.pos;
      for (size_t i = 0; i < hdr.count; ++i) {
        if constexpr (RegisteredStruct<Elem>) {
          const auto elem_slice = read_len_slice_fast(body, p, 0);
          if (!elem_slice) [[unlikely]] return elem_slice.err;
          Elem& item = field_ref.emplace_back();
          const char* err =
              decode_struct_body(elem_slice.span(), item, depth + 1, false, state, true);
          if (err != nullptr) [[unlikely]] return err;
          p = elem_slice.next_pos;
        } else {
          Elem item{};
          const auto item_res =
              decode_container_elem<Override, KeyOverride>(body, p, item, depth, state);
          if (!item_res) [[unlikely]] return item_res.err;
          field_ref.push_back(std::move(item));
          p = item_res.pos;
        }
      }
      if (p != body.size()) [[unlikely]] return "omnibuf: WireArray trailing bytes";
      return nullptr;
    }
  }
}

template <WireOverride Override, WireOverride KeyOverride, typename MapT>
[[nodiscard]] inline const char* decode_map_body_into(std::span<const uint8_t> body,
                                                      MapT& field_ref,
                                                      int depth,
                                                      DecodeState& state) {
  const auto hdr = parse_map_header(body);
  if (!hdr.ok) [[unlikely]] return "omnibuf: invalid WireMap header";

  using K = typename MapT::key_type;
  using V = typename MapT::mapped_type;
  constexpr uint8_t exp_key_wt = scalar_wire_type<K, KeyOverride>();
  constexpr uint8_t exp_val_wt = container_value_wire_type<V, Override>();
  if (hdr.key_wt != exp_key_wt || hdr.val_wt != exp_val_wt) [[unlikely]] {
    return "omnibuf: WireMap key/value wire type mismatch";
  }
  if (!state.charge_elements(hdr.count)) [[unlikely]] {
    return "omnibuf: WireMap exceeds max_total_elements";
  }
  if constexpr (IsStdUnorderedMap<MapT>::value) {
    field_ref.reserve(bounded_reserve_target(field_ref.bucket_count(), field_ref.size(), hdr.count));
  }
  size_t p = hdr.pos;
  for (size_t i = 0; i < hdr.count; ++i) {
    K key{};
    const auto k_res = decode_scalar_payload<K, KeyOverride>(body, p, key);
    if (!k_res) [[unlikely]] return k_res.err;
    p = k_res.pos;
    V val{};
    const auto v_res = decode_container_elem<Override, KeyOverride>(body, p, val, depth, state);
    if (!v_res) [[unlikely]] return v_res.err;
    field_ref[std::move(key)] = std::move(val);
    p = v_res.pos;
  }
  if (p != body.size()) [[unlikely]] return "omnibuf: WireMap trailing bytes";
  return nullptr;
}

template <WireOverride Override,
          WireOverride KeyOverride = WireOverride::Default,
          typename FieldT>
[[nodiscard]] inline PosStep decode_matched_struct_field(std::span<const uint8_t> data,
                                                         size_t pos,
                                                         uint8_t wt,
                                                         FieldT& field_ref,
                                                         int depth,
                                                         bool already_seen,
                                                         bool& mark_seen,
                                                         DecodeState& state) {
  if constexpr (RegisteredStruct<FieldT>) {
    if (wt != WireSubMsg) [[unlikely]] {
      return validate_value_step(data, pos, wt, depth, state);
    }
    const auto sub_res = read_len_slice_fast(data, pos, 0);
    if (!sub_res) [[unlikely]] return {0, sub_res.err};
    mark_seen = true;
    const char* err = decode_struct_body(sub_res.span(), field_ref, depth + 1, already_seen, state);
    if (err != nullptr) [[unlikely]] return {0, err};
    return {sub_res.next_pos, nullptr};
  } else if constexpr (IsStdOptional<FieldT>::value) {
    using Inner = typename FieldT::value_type;
    if constexpr (RegisteredStruct<Inner>) {
      if (wt != WireSubMsg) [[unlikely]] {
        return validate_value_step(data, pos, wt, depth, state);
      }
      const auto sub_res = read_len_slice_fast(data, pos, 0);
      if (!sub_res) [[unlikely]] return {0, sub_res.err};
      mark_seen = true;
      bool fresh = false;
      if (!field_ref.has_value() || !already_seen) {
        field_ref.emplace();
        fresh = true;
      }
      const char* err =
          decode_struct_body(sub_res.span(), *field_ref, depth + 1, already_seen, state, fresh);
      if (err != nullptr) [[unlikely]] return {0, err};
      return {sub_res.next_pos, nullptr};
    } else {
      Inner tmp{};
      bool inner_seen = false;
      const auto res = decode_matched_struct_field<Override, KeyOverride>(
          data, pos, wt, tmp, depth, already_seen, inner_seen, state);
      if (!res) [[unlikely]] return res;
      if (inner_seen) {
        mark_seen = true;
        field_ref = std::move(tmp);
      }
      return res;
    }
  } else if constexpr (IsUniquePtr<FieldT>::value || IsBox<FieldT>::value) {
    using Inner = typename PointeeType<FieldT>::type;
    static_assert(RegisteredStruct<Inner>,
                  "unique_ptr / Box field must point to RegisteredStruct");
    if (wt != WireSubMsg) [[unlikely]] {
      return validate_value_step(data, pos, wt, depth, state);
    }
    const auto sub_res = read_len_slice_fast(data, pos, 0);
    if (!sub_res) [[unlikely]] return {0, sub_res.err};
    mark_seen = true;
    bool fresh = false;
    if (!field_ref || !already_seen) {
      if constexpr (IsUniquePtr<FieldT>::value) {
        field_ref = std::make_unique<Inner>();
      } else {
        field_ref.emplace();
      }
      fresh = true;
    }
    const char* err =
        decode_struct_body(sub_res.span(), *field_ref, depth + 1, already_seen, state, fresh);
    if (err != nullptr) [[unlikely]] return {0, err};
    return {sub_res.next_pos, nullptr};
  } else if constexpr (IsStdVector<FieldT>::value &&
                       !std::is_same_v<FieldT, std::vector<uint8_t>>) {
    if (wt != WireArray) [[unlikely]] {
      return validate_value_step(data, pos, wt, depth, state);
    }
    if (depth + 1 > state.opts.max_depth) [[unlikely]] {
      return {0, "omnibuf: array nesting depth exceeds max_depth"};
    }
    const auto body_res = read_len_slice_fast(data, pos, 2);
    if (!body_res) [[unlikely]] return {0, body_res.err};
    mark_seen = true;
    if (!already_seen) {
      field_ref.clear();
    }
    const char* err =
        decode_vector_body<Override, KeyOverride>(body_res.span(), field_ref, depth + 1, state);
    if (err != nullptr) [[unlikely]] return {0, err};
    return {body_res.next_pos, nullptr};
  } else if constexpr (IsAnyMapV<FieldT>) {
    if (wt != WireMap) [[unlikely]] {
      return validate_value_step(data, pos, wt, depth, state);
    }
    if (depth + 1 > state.opts.max_depth) [[unlikely]] {
      return {0, "omnibuf: map nesting depth exceeds max_depth"};
    }
    const auto body_res = read_len_slice_fast(data, pos, 3);
    if (!body_res) [[unlikely]] return {0, body_res.err};
    mark_seen = true;
    if (!already_seen) {
      field_ref.clear();
    }
    const char* err =
        decode_map_body_into<Override, KeyOverride>(body_res.span(), field_ref, depth + 1, state);
    if (err != nullptr) [[unlikely]] return {0, err};
    return {body_res.next_pos, nullptr};
  } else if constexpr (std::is_same_v<FieldT, bool>) {
    if (wt == WireBoolTrue) {
      mark_seen = true;
      field_ref = true;
      return {pos, nullptr};
    }
    if (wt == WireBoolFalse) {
      mark_seen = true;
      field_ref = false;
      return {pos, nullptr};
    }
    return validate_value_step(data, pos, wt, depth, state);
  } else {
    constexpr uint8_t exp_wt = scalar_wire_type<FieldT, Override>();
    if (wt != exp_wt) [[unlikely]] {
      return validate_value_step(data, pos, wt, depth, state);
    }
    mark_seen = true;
    return decode_scalar_payload<FieldT, Override>(data, pos, field_ref);
  }
}

template <RegisteredStruct T>
[[nodiscard]] inline const char* decode_struct_body(std::span<const uint8_t> data,
                                                    T& msg,
                                                    int depth,
                                                    bool merge,
                                                    DecodeState& state,
                                                    bool is_fresh) {
  static_assert(validate_struct_fields<T>(),
                "StructTraits<T>::fields() has out-of-range or duplicate field numbers");
  if (depth > state.opts.max_depth) [[unlikely]] {
    return "omnibuf: message nesting depth exceeds max_depth";
  }
  if (data.size() > state.opts.max_message_bytes) [[unlikely]] {
    return "omnibuf: message exceeds max_message_bytes";
  }
  const auto fields = StructTraits<T>::fields();
  if (!merge && !is_fresh) {
    std::apply([&](const auto&... fd) { (clear_struct_member(msg.*(fd.member)), ...); }, fields);
  }
  constexpr size_t num_fields = std::tuple_size_v<std::remove_cvref_t<decltype(fields)>>;
  constexpr size_t kSeenWords = (num_fields == 0) ? 1 : ((num_fields + 63) / 64);
  std::array<uint64_t, kSeenWords> seen_words{};
  if (merge) {
    seen_words.fill(~uint64_t{0});
  }

  size_t pos = 0;
  const size_t n = data.size();
  const uint8_t* base = data.data();
  while (pos < n) {
    uint32_t fnum = 0;
    uint8_t wt = 0;
    const uint8_t b0 = base[pos];
    if ((b0 & 1u) != 0) [[likely]] {
      fnum = static_cast<uint32_t>(b0 >> 5);
      wt = static_cast<uint8_t>((b0 >> 1) & 0x0Fu);
      ++pos;
    } else if ((b0 & 2u) != 0 && pos + 2 <= n) {
      const uint16_t u = static_cast<uint16_t>(read_u16_le(base + pos) >> 2);
      fnum = static_cast<uint32_t>(u >> 4);
      wt = static_cast<uint8_t>(u & 0x0Fu);
      pos += 2;
    } else {
      const auto [tag, tn] = decode_varint(data.subspan(pos));
      if (tn == 0 || (tag >> 4) > MAX_FIELD_NUMBER) [[unlikely]] {
        return "omnibuf: invalid or truncated tag";
      }
      pos += tn;
      fnum = static_cast<uint32_t>(tag >> 4);
      wt = static_cast<uint8_t>(tag & 0x0Fu);
    }

    if (fnum == 0) [[unlikely]] {
      auto f0_res = validate_field0(data, pos, wt, depth, state);
      if (!f0_res) return "omnibuf: invalid Field #0";
      pos = f0_res.value();
      continue;
    }

    PosStep step_res{pos, nullptr};
    const bool matched = [&]<size_t... Is>(std::index_sequence<Is...>) {
      return (
          [&]() -> bool {
            const auto& fd = std::get<Is>(fields);
            using FD = std::remove_cvref_t<decltype(fd)>;
            if (FD::number != fnum) {
              return false;
            }
            constexpr size_t word_idx = Is / 64;
            constexpr uint64_t bit = uint64_t{1} << (Is % 64);
            const bool was_seen = (seen_words[word_idx] & bit) != 0;
            bool mark_seen = false;
            step_res = decode_matched_struct_field<FD::wire_override, FD::key_wire_override>(
                data, pos, wt, msg.*(FD::member), depth, was_seen, mark_seen, state);
            if (mark_seen) {
              seen_words[word_idx] |= bit;
            }
            return true;
          }() ||
          ...);
    }(std::make_index_sequence<num_fields>{});

    if (!matched) [[unlikely]] {
      step_res = validate_value_step(data, pos, wt, depth, state);
    }
    if (!step_res) [[unlikely]] {
      return step_res.err;
    }
    pos = step_res.pos;
  }
  return nullptr;
}

}  // namespace detail

template <RegisteredStruct T>
[[nodiscard]] inline SchemaMeta struct_schema_meta() {
  std::set<const void*> active;
  auto ptr = detail::build_struct_schema_meta<T>(1, active);
  return ptr ? *ptr : SchemaMeta{};
}

template <RegisteredStruct T>
[[nodiscard]] inline Result<std::vector<uint8_t>> marshal(const T& msg,
                                                          MarshalOptions opts = {}) {
  std::vector<uint8_t> dst;
  if (opts.self_describing) {
    dst.reserve(512);
    std::set<const void*> active;
    bool cyclic = false;
    auto meta = detail::build_struct_schema_meta<T>(1, active, &cyclic);
    if (cyclic) {
      meta = detail::build_struct_value_schema_meta(msg, 1);
    }
    if (meta) {
      append_field0_schema_meta(dst, *meta);
    }
  }
  MarshalOptions body_opts = opts;
  body_opts.self_describing = false;
  FastWriter w(dst, 512);
  const char* err = detail::encode_struct_body(w, msg, body_opts);
  if (err != nullptr) [[unlikely]] return Error{err};
  w.finish();
  return dst;
}

template <RegisteredStruct T>
[[nodiscard]] inline Result<void> unmarshal(std::span<const uint8_t> data,
                                            T& msg,
                                            DecodeOptions opts = {}) {
  DecodeState state{opts, 0};
  const char* err = detail::decode_struct_body(data, msg, 0, false, state);
  if (err != nullptr) [[unlikely]] return Error{err};
  return Result<void>::success();
}

}  // namespace omnibuf
