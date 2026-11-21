#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <map>
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

namespace detail {

template <typename T, bool IsEnum = std::is_enum_v<T>>
struct IsSignedScalarImpl : std::is_signed<T> {};

template <typename T>
struct IsSignedScalarImpl<T, true> : std::is_signed<std::underlying_type_t<T>> {};

template <typename T>
inline constexpr bool IsSignedScalarV = IsSignedScalarImpl<T>::value;

struct DeferredDeleteNode {
  void* ptr;
  void (*deleter)(void*) noexcept;
};

inline void destroy_with_trampoline(void* p, void (*del)(void*) noexcept) noexcept {
  if (p == nullptr || del == nullptr) return;
  thread_local int depth = 0;
  thread_local std::vector<DeferredDeleteNode> queue;
  if (depth >= 16) {
    try {
      queue.push_back({p, del});
    } catch (...) {
      del(p);
    }
    return;
  }
  ++depth;
  del(p);
  if (depth == 1 && !queue.empty()) {
    while (!queue.empty()) {
      const DeferredDeleteNode next = queue.back();
      queue.pop_back();
      next.deleter(next.ptr);
    }
    std::vector<DeferredDeleteNode>().swap(queue);
  }
  --depth;
}

}  // namespace detail

// Deep-copying, value-comparable heap wrapper for recursive singular submessages.
// Supports incomplete T at member-declaration, copy, move, and destruction time via
// erased operations, and bounds recursion depth during destruction via trampoline.
template <typename T>
class Box {
 public:
  constexpr Box() noexcept = default;
  constexpr Box(std::nullptr_t) noexcept {}  // NOLINT(google-explicit-constructor)
  constexpr Box(std::nullopt_t) noexcept {}  // NOLINT(google-explicit-constructor)

  Box(const T& v)  // NOLINT(google-explicit-constructor)
      : ptr_(new T(v)), deleter_(&delete_impl), cloner_(&clone_impl) {}
  Box(T&& v)  // NOLINT(google-explicit-constructor)
      : ptr_(new T(std::move(v))), deleter_(&delete_impl), cloner_(&clone_impl) {}

  template <typename U>
    requires std::is_same_v<U, T>
  Box(const std::optional<U>& opt)  // NOLINT(google-explicit-constructor)
      : ptr_(opt ? new T(*opt) : nullptr),
        deleter_(opt ? &delete_impl : nullptr),
        cloner_(opt ? &clone_impl : nullptr) {}

  template <typename U>
    requires std::is_same_v<U, T>
  Box(std::optional<U>&& opt)  // NOLINT(google-explicit-constructor)
      : ptr_(opt ? new T(std::move(*opt)) : nullptr),
        deleter_(opt ? &delete_impl : nullptr),
        cloner_(opt ? &clone_impl : nullptr) {}

  Box(const Box& other)
      : ptr_(other.ptr_ ? other.cloner_(other.ptr_) : nullptr),
        deleter_(other.ptr_ ? other.deleter_ : nullptr),
        cloner_(other.ptr_ ? other.cloner_ : nullptr) {}

  Box(Box&& other) noexcept
      : ptr_(std::exchange(other.ptr_, nullptr)),
        deleter_(std::exchange(other.deleter_, nullptr)),
        cloner_(std::exchange(other.cloner_, nullptr)) {}

  ~Box() noexcept { reset(); }

  Box& operator=(const Box& other) {
    if (this != &other) {
      if (!other.ptr_) {
        reset();
      } else {
        T* copy = other.cloner_(other.ptr_);
        reset();
        ptr_ = copy;
        deleter_ = other.deleter_;
        cloner_ = other.cloner_;
      }
    }
    return *this;
  }

  Box& operator=(Box&& other) noexcept {
    if (this != &other) {
      reset();
      ptr_ = std::exchange(other.ptr_, nullptr);
      deleter_ = std::exchange(other.deleter_, nullptr);
      cloner_ = std::exchange(other.cloner_, nullptr);
    }
    return *this;
  }

  Box& operator=(std::nullopt_t) noexcept {
    reset();
    return *this;
  }

  Box& operator=(const T& v) {
    if (ptr_) {
      *ptr_ = v;
    } else {
      ptr_ = new T(v);
      deleter_ = &delete_impl;
      cloner_ = &clone_impl;
    }
    return *this;
  }

  Box& operator=(T&& v) {
    if (ptr_) {
      *ptr_ = std::move(v);
    } else {
      ptr_ = new T(std::move(v));
      deleter_ = &delete_impl;
      cloner_ = &clone_impl;
    }
    return *this;
  }

  void reset() noexcept {
    if (ptr_ != nullptr) {
      void* p = std::exchange(ptr_, nullptr);
      auto del = std::exchange(deleter_, nullptr);
      cloner_ = nullptr;
      detail::destroy_with_trampoline(p, del);
    }
  }

  template <typename... Args>
  T& emplace(Args&&... args) {
    if (ptr_) {
      if constexpr (sizeof...(Args) == 0) {
        *ptr_ = T{};
      } else {
        *ptr_ = T(std::forward<Args>(args)...);
      }
    } else {
      if constexpr (sizeof...(Args) == 0) {
        ptr_ = new T{};
      } else {
        ptr_ = new T(std::forward<Args>(args)...);
      }
      deleter_ = &delete_impl;
      cloner_ = &clone_impl;
    }
    return *ptr_;
  }

  [[nodiscard]] constexpr bool has_value() const noexcept { return ptr_ != nullptr; }
  [[nodiscard]] constexpr explicit operator bool() const noexcept { return ptr_ != nullptr; }

  [[nodiscard]] T* get() noexcept { return ptr_; }
  [[nodiscard]] const T* get() const noexcept { return ptr_; }

  [[nodiscard]] T& value() {
    if (!ptr_) throw std::bad_optional_access();
    return *ptr_;
  }

  [[nodiscard]] const T& value() const {
    if (!ptr_) throw std::bad_optional_access();
    return *ptr_;
  }

  [[nodiscard]] T& operator*() noexcept { return *ptr_; }
  [[nodiscard]] const T& operator*() const noexcept { return *ptr_; }
  [[nodiscard]] T* operator->() noexcept { return ptr_; }
  [[nodiscard]] const T* operator->() const noexcept { return ptr_; }

  friend bool operator==(const Box& a, const Box& b) {
    if (a.ptr_ == b.ptr_) return true;
    if (!a.ptr_ || !b.ptr_) return false;
    return *a.ptr_ == *b.ptr_;
  }

  friend bool operator!=(const Box& a, const Box& b) { return !(a == b); }

 private:
  static void delete_impl(void* p) noexcept { delete static_cast<T*>(p); }
  static T* clone_impl(const void* p) {
    if constexpr (std::is_copy_constructible_v<T>) {
      return new T(*static_cast<const T*>(p));
    } else {
      throw std::logic_error("omnibuf::Box<T> copy requires copy-constructible T");
    }
  }

  T* ptr_ = nullptr;
  void (*deleter_)(void*) noexcept = nullptr;
  T* (*cloner_)(const void*) = nullptr;
};

inline constexpr size_t kMaxInitialReserve = 4096;

struct DecodeOptions {
  int max_depth = MAX_CONTAINER_DEPTH;
  size_t max_message_bytes = 64u * 1024u * 1024u;
  size_t max_total_elements = 16u * 1024u * 1024u;
};

struct DecodeState {
  DecodeOptions opts{};
  size_t total_elements = 0;

  [[nodiscard]] bool charge_elements(size_t n) noexcept {
    if (n > opts.max_total_elements || total_elements > opts.max_total_elements - n) {
      return false;
    }
    total_elements += n;
    return true;
  }
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

// Trivially copyable register-returned step types for zero-allocation hot-path decoding.
struct PosStep {
  size_t pos = 0;
  const char* err = nullptr;
  [[nodiscard]] explicit constexpr operator bool() const noexcept { return err == nullptr; }
};

struct SliceStep {
  const uint8_t* ptr = nullptr;
  size_t len = 0;
  size_t next_pos = 0;
  const char* err = nullptr;
  [[nodiscard]] explicit constexpr operator bool() const noexcept { return err == nullptr; }
  [[nodiscard]] constexpr std::span<const uint8_t> span() const noexcept { return {ptr, len}; }
};

[[nodiscard]] inline SliceStep read_len_slice_fast(std::span<const uint8_t> data,
                                                   size_t pos,
                                                   uint64_t min_len = 0) noexcept {
  const size_t size = data.size();
  const uint8_t* base = data.data();
  if (pos >= size) [[unlikely]] {
    return {nullptr, 0, 0, "omnibuf: truncated length varint"};
  }
  uint64_t b_len = 0;
  const uint8_t b0 = base[pos];
  if ((b0 & 1u) != 0) [[likely]] {
    b_len = static_cast<uint64_t>(b0 >> 1);
    ++pos;
  } else if ((b0 & 2u) != 0 && pos + 2 <= size) {
    b_len = static_cast<uint64_t>(read_u16_le(base + pos) >> 2);
    pos += 2;
  } else {
    const auto [v, bn] = decode_varint(data.subspan(pos));
    if (bn == 0) {
      return {nullptr, 0, 0, "omnibuf: invalid or truncated length varint"};
    }
    b_len = v;
    pos += bn;
  }
  if (b_len < min_len || b_len > static_cast<uint64_t>(size - pos)) [[unlikely]] {
    return {nullptr, 0, 0, "omnibuf: length exceeds packet bounds"};
  }
  const size_t len_sz = static_cast<size_t>(b_len);
  return {base + pos, len_sz, pos + len_sz, nullptr};
}

[[nodiscard]] inline SliceStep read_str_slice_fast(std::span<const uint8_t> data,
                                                   size_t pos) noexcept {
  const auto res = read_len_slice_fast(data, pos, 0);
  if (!res) [[unlikely]] return res;
  if (!is_valid_utf8(res.span())) [[unlikely]] {
    return {nullptr, 0, 0, "omnibuf: invalid UTF-8 in WireString"};
  }
  return res;
}

[[nodiscard]] inline Result<SliceResult> read_len_slice(std::span<const uint8_t> data,
                                                        size_t pos,
                                                        uint64_t min_len = 0) {
  const auto res = read_len_slice_fast(data, pos, min_len);
  if (!res) return Error{res.err};
  return SliceResult{res.span(), res.next_pos};
}

[[nodiscard]] inline Result<SliceResult> read_str_slice(std::span<const uint8_t> data, size_t pos) {
  const auto res = read_str_slice_fast(data, pos);
  if (!res) return Error{res.err};
  return SliceResult{res.span(), res.next_pos};
}

[[nodiscard]] inline size_t grow_capacity(size_t current, size_t required) noexcept {
  if (current <= (SIZE_MAX / 4) && current * 2 > required) {
    return current * 2;
  }
  return required;
}

[[nodiscard]] inline size_t bounded_reserve_target(
    size_t current_cap,
    size_t current_size,
    size_t incoming_count,
    size_t max_initial_batch = kMaxInitialReserve) noexcept {
  const size_t batch = std::min(incoming_count, max_initial_batch);
  if (batch > SIZE_MAX - current_size) {
    return current_cap;
  }
  return grow_capacity(current_cap, current_size + batch);
}

// Forward declarations for mutual recursion in full validation (defined in field0.hpp).
[[nodiscard]] inline Result<void> validate_message_bytes(std::span<const uint8_t> data,
                                                         int depth,
                                                         DecodeState& state);
[[nodiscard]] inline Result<void> validate_message_bytes(std::span<const uint8_t> data, int depth);
[[nodiscard]] inline Result<size_t> validate_field0(std::span<const uint8_t> data,
                                                    size_t pos,
                                                    uint8_t wt,
                                                    int depth,
                                                    DecodeState& state);
[[nodiscard]] inline Result<size_t> validate_field0(std::span<const uint8_t> data,
                                                    size_t pos,
                                                    uint8_t wt,
                                                    int depth);

[[nodiscard]] inline Result<size_t> validate_value(std::span<const uint8_t> b,
                                                   size_t pos,
                                                   uint8_t wt,
                                                   int depth,
                                                   DecodeState& state);

[[nodiscard]] inline Result<size_t> validate_container_entry(std::span<const uint8_t> body,
                                                             size_t p,
                                                             uint8_t wt,
                                                             int depth,
                                                             DecodeState& state) {
  if (wt == WireBoolFalse) {
    if (p >= body.size() || body[p] > 1u) {
      return Error{"omnibuf: invalid bool padding bits in container"};
    }
    return p + 1;
  }
  return validate_value(body, p, wt, depth, state);
}

[[nodiscard]] inline Result<void> validate_array_body(std::span<const uint8_t> body,
                                                      size_t p,
                                                      uint8_t elem_wt,
                                                      size_t count,
                                                      int depth,
                                                      DecodeState& state) {
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
      if (count > (body.size() - p) / 4 || body.size() - p != count * 4) {
        return Error{"omnibuf: invalid 32-bit array byte length"};
      }
      return Result<void>::success();
    case WireFixed64:
    case WireSfixed64:
    case WireFloat64:
      if (count > (body.size() - p) / 8 || body.size() - p != count * 8) {
        return Error{"omnibuf: invalid 64-bit array byte length"};
      }
      return Result<void>::success();
    default:
      break;
  }
  for (size_t i = 0; i < count; ++i) {
    auto next_res = validate_container_entry(body, p, elem_wt, depth, state);
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
                                                   int depth,
                                                   DecodeState& state) {
  if (depth > state.opts.max_depth) {
    return Error{"omnibuf: nesting depth exceeds max_depth"};
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
    if (sub_res.value().slice.size() > state.opts.max_message_bytes) {
      return Error{"omnibuf: submessage exceeds max_message_bytes"};
    }
    auto v_res = validate_message_bytes(sub_res.value().slice, depth + 1, state);
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
      if (depth + 1 > state.opts.max_depth) {
        return Error{"omnibuf: array nesting depth exceeds max_depth"};
      }
      const auto hdr = parse_array_header(body);
      if (!hdr.ok) {
        return Error{"omnibuf: invalid WireArray header"};
      }
      if (!state.charge_elements(hdr.count)) {
        return Error{"omnibuf: total decoded elements exceed max_total_elements"};
      }
      auto a_res = validate_array_body(body, hdr.pos, hdr.elem_wt, hdr.count, depth + 1, state);
      if (!a_res) return Error{a_res.error()};
    } else if (wt == WireMap) {
      if (depth + 1 > state.opts.max_depth) {
        return Error{"omnibuf: map nesting depth exceeds max_depth"};
      }
      const auto hdr = parse_map_header(body);
      if (!hdr.ok) {
        return Error{"omnibuf: invalid WireMap header"};
      }
      if (!state.charge_elements(hdr.count)) {
        return Error{"omnibuf: total decoded elements exceed max_total_elements"};
      }
      size_t p = hdr.pos;
      for (size_t i = 0; i < hdr.count; ++i) {
        auto k_res = validate_container_entry(body, p, hdr.key_wt, depth + 1, state);
        if (!k_res) return Error{k_res.error()};
        auto v_res = validate_container_entry(body, k_res.value(), hdr.val_wt, depth + 1, state);
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

[[nodiscard]] inline Result<size_t> validate_value(std::span<const uint8_t> b,
                                                   size_t pos,
                                                   uint8_t wt,
                                                   int depth) {
  DecodeState state{};
  return validate_value(b, pos, wt, depth, state);
}

// Array body decoders for generated and reflection-driven codecs.
[[nodiscard]] inline bool append_string_arr_body(std::vector<std::string>& dst,
                                                 std::span<const uint8_t> body,
                                                 DecodeState* state = nullptr) {
  const auto hdr = parse_array_header(body);
  if (!hdr.ok || hdr.elem_wt != WireString) return false;
  if (state && !state->charge_elements(hdr.count)) return false;
  size_t p = hdr.pos;
  dst.reserve(bounded_reserve_target(dst.capacity(), dst.size(), hdr.count));
  for (size_t i = 0; i < hdr.count; ++i) {
    const auto s_res = read_str_slice_fast(body, p);
    if (!s_res) return false;
    dst.emplace_back(reinterpret_cast<const char*>(s_res.ptr), s_res.len);
    p = s_res.next_pos;
  }
  return p == body.size();
}

[[nodiscard]] inline bool append_bytes_arr_body(std::vector<std::vector<uint8_t>>& dst,
                                                std::span<const uint8_t> body,
                                                DecodeState* state = nullptr) {
  const auto hdr = parse_array_header(body);
  if (!hdr.ok || hdr.elem_wt != WireBytes) return false;
  if (state && !state->charge_elements(hdr.count)) return false;
  size_t p = hdr.pos;
  dst.reserve(bounded_reserve_target(dst.capacity(), dst.size(), hdr.count));
  for (size_t i = 0; i < hdr.count; ++i) {
    const auto b_res = read_len_slice_fast(body, p, 0);
    if (!b_res) return false;
    dst.emplace_back(b_res.ptr, b_res.ptr + b_res.len);
    p = b_res.next_pos;
  }
  return p == body.size();
}

[[nodiscard]] inline bool append_bool_arr_body(std::vector<bool>& dst,
                                               std::span<const uint8_t> body,
                                               DecodeState* state = nullptr) {
  const auto hdr = parse_array_header(body);
  if (!hdr.ok || hdr.elem_wt != WireBoolFalse) return false;
  if (state && !state->charge_elements(hdr.count)) return false;
  dst.reserve(dst.size() + hdr.count);
  for (size_t i = 0; i < hdr.count; ++i) {
    const uint8_t v = body[hdr.pos + i];
    if (v > 1u) return false;
    dst.push_back(v == 1u);
  }
  return true;
}

[[nodiscard]] inline bool append_string_view_arr_body(std::vector<std::string_view>& dst,
                                                      std::span<const uint8_t> body,
                                                      DecodeState* state = nullptr) {
  const auto hdr = parse_array_header(body);
  if (!hdr.ok || hdr.elem_wt != WireString) return false;
  if (state && !state->charge_elements(hdr.count)) return false;
  size_t p = hdr.pos;
  dst.reserve(bounded_reserve_target(dst.capacity(), dst.size(), hdr.count));
  for (size_t i = 0; i < hdr.count; ++i) {
    const auto s_res = read_str_slice_fast(body, p);
    if (!s_res) return false;
    dst.emplace_back(reinterpret_cast<const char*>(s_res.ptr), s_res.len);
    p = s_res.next_pos;
  }
  return p == body.size();
}

[[nodiscard]] inline bool append_bytes_view_arr_body(std::vector<std::span<const uint8_t>>& dst,
                                                     std::span<const uint8_t> body,
                                                     DecodeState* state = nullptr) {
  const auto hdr = parse_array_header(body);
  if (!hdr.ok || hdr.elem_wt != WireBytes) return false;
  if (state && !state->charge_elements(hdr.count)) return false;
  size_t p = hdr.pos;
  dst.reserve(bounded_reserve_target(dst.capacity(), dst.size(), hdr.count));
  for (size_t i = 0; i < hdr.count; ++i) {
    const auto b_res = read_len_slice_fast(body, p, 0);
    if (!b_res) return false;
    dst.emplace_back(b_res.ptr, b_res.len);
    p = b_res.next_pos;
  }
  return p == body.size();
}

template <typename T>
[[nodiscard]] inline bool append_uint_arr_body(std::vector<T>& dst,
                                               std::span<const uint8_t> body,
                                               DecodeState* state = nullptr) {
  const auto hdr = parse_array_header(body);
  if (!hdr.ok || hdr.elem_wt != WireUint) return false;
  if (state && !state->charge_elements(hdr.count)) return false;
  size_t p = hdr.pos;
  dst.reserve(bounded_reserve_target(dst.capacity(), dst.size(), hdr.count));
  for (size_t i = 0; i < hdr.count; ++i) {
    const auto [v, vn] = decode_varint(body.subspan(p));
    if (vn == 0) return false;
    p += vn;
    if constexpr (std::is_enum_v<T>) {
      dst.push_back(static_cast<T>(static_cast<std::underlying_type_t<T>>(v)));
    } else {
      dst.push_back(static_cast<T>(v));
    }
  }
  return p == body.size();
}

template <typename T>
[[nodiscard]] inline bool append_sint_arr_body(std::vector<T>& dst,
                                               std::span<const uint8_t> body,
                                               DecodeState* state = nullptr) {
  const auto hdr = parse_array_header(body);
  if (!hdr.ok || hdr.elem_wt != WireSint) return false;
  if (state && !state->charge_elements(hdr.count)) return false;
  size_t p = hdr.pos;
  dst.reserve(bounded_reserve_target(dst.capacity(), dst.size(), hdr.count));
  for (size_t i = 0; i < hdr.count; ++i) {
    const auto [v, vn] = decode_varint(body.subspan(p));
    if (vn == 0) return false;
    p += vn;
    if constexpr (std::is_enum_v<T>) {
      dst.push_back(static_cast<T>(static_cast<std::underlying_type_t<T>>(decode_zigzag(v))));
    } else {
      dst.push_back(static_cast<T>(decode_zigzag(v)));
    }
  }
  return p == body.size();
}

template <typename T>
[[nodiscard]] inline bool append_fixed32_arr_body(std::vector<T>& dst,
                                                  std::span<const uint8_t> body,
                                                  uint8_t expected_wt,
                                                  DecodeState* state = nullptr) {
  const auto hdr = parse_array_header(body);
  if (!hdr.ok || hdr.elem_wt != expected_wt) return false;
  if (state && !state->charge_elements(hdr.count)) return false;
  if constexpr (std::endian::native == std::endian::little && sizeof(T) == 4 &&
                std::is_trivially_copyable_v<T>) {
    if (hdr.count > 0) {
      const size_t old_sz = dst.size();
      dst.resize(old_sz + hdr.count);
      std::memcpy(dst.data() + old_sz, body.data() + hdr.pos, hdr.count * 4);
    }
    return true;
  } else {
    size_t p = hdr.pos;
    dst.reserve(grow_capacity(dst.capacity(), dst.size() + hdr.count));
    for (size_t i = 0; i < hdr.count; ++i) {
      const uint32_t bits = read_u32_le(body.data() + p);
      p += 4;
      if constexpr (std::is_same_v<T, float>) {
        dst.push_back(bits_to_float32(bits));
      } else if constexpr (detail::IsSignedScalarV<T>) {
        dst.push_back(static_cast<T>(static_cast<int32_t>(bits)));
      } else {
        dst.push_back(static_cast<T>(bits));
      }
    }
    return true;
  }
}

template <typename T>
[[nodiscard]] inline bool append_fixed64_arr_body(std::vector<T>& dst,
                                                  std::span<const uint8_t> body,
                                                  uint8_t expected_wt,
                                                  DecodeState* state = nullptr) {
  const auto hdr = parse_array_header(body);
  if (!hdr.ok || hdr.elem_wt != expected_wt) return false;
  if (state && !state->charge_elements(hdr.count)) return false;
  if constexpr (std::endian::native == std::endian::little && sizeof(T) == 8 &&
                std::is_trivially_copyable_v<T>) {
    if (hdr.count > 0) {
      const size_t old_sz = dst.size();
      dst.resize(old_sz + hdr.count);
      std::memcpy(dst.data() + old_sz, body.data() + hdr.pos, hdr.count * 8);
    }
    return true;
  } else {
    size_t p = hdr.pos;
    dst.reserve(grow_capacity(dst.capacity(), dst.size() + hdr.count));
    for (size_t i = 0; i < hdr.count; ++i) {
      const uint64_t bits = read_u64_le(body.data() + p);
      p += 8;
      if constexpr (std::is_same_v<T, double>) {
        dst.push_back(bits_to_float64(bits));
      } else if constexpr (detail::IsSignedScalarV<T>) {
        dst.push_back(static_cast<T>(static_cast<int64_t>(bits)));
      } else {
        dst.push_back(static_cast<T>(bits));
      }
    }
    return true;
  }
}

template <uint8_t WT, typename T>
[[nodiscard]] inline bool decode_container_scalar(std::span<const uint8_t> body,
                                                  size_t& p,
                                                  T& out) {
  if constexpr (WT == WireUint) {
    const auto [v, vn] = decode_varint(body.subspan(p));
    if (vn == 0) return false;
    p += vn;
    out = static_cast<T>(v);
    return true;
  } else if constexpr (WT == WireSint) {
    const auto [v, vn] = decode_varint(body.subspan(p));
    if (vn == 0) return false;
    p += vn;
    if constexpr (std::is_enum_v<T>) {
      out = static_cast<T>(static_cast<std::underlying_type_t<T>>(decode_zigzag(v)));
    } else {
      out = static_cast<T>(decode_zigzag(v));
    }
    return true;
  } else if constexpr (WT == WireBoolFalse) {
    if (p >= body.size() || body[p] > 1u) return false;
    out = (body[p++] == 1u);
    return true;
  } else if constexpr (WT == WireFixed32 || WT == WireSfixed32 || WT == WireFloat32) {
    if (p > body.size() || body.size() - p < 4) return false;
    const uint32_t bits = read_u32_le(body.data() + p);
    p += 4;
    if constexpr (std::is_same_v<T, float>) {
      out = bits_to_float32(bits);
    } else if constexpr (detail::IsSignedScalarV<T>) {
      out = static_cast<T>(static_cast<int32_t>(bits));
    } else {
      out = static_cast<T>(bits);
    }
    return true;
  } else if constexpr (WT == WireFixed64 || WT == WireSfixed64 || WT == WireFloat64) {
    if (p > body.size() || body.size() - p < 8) return false;
    const uint64_t bits = read_u64_le(body.data() + p);
    p += 8;
    if constexpr (std::is_same_v<T, double>) {
      out = bits_to_float64(bits);
    } else if constexpr (detail::IsSignedScalarV<T>) {
      out = static_cast<T>(static_cast<int64_t>(bits));
    } else {
      out = static_cast<T>(bits);
    }
    return true;
  } else if constexpr (WT == WireString) {
    const auto s_res = read_str_slice_fast(body, p);
    if (!s_res) return false;
    if constexpr (std::is_same_v<T, std::string_view>) {
      out = std::string_view(reinterpret_cast<const char*>(s_res.ptr), s_res.len);
    } else {
      out.assign(reinterpret_cast<const char*>(s_res.ptr), s_res.len);
    }
    p = s_res.next_pos;
    return true;
  } else if constexpr (WT == WireBytes) {
    const auto b_res = read_len_slice_fast(body, p, 0);
    if (!b_res) return false;
    if constexpr (std::is_same_v<T, std::span<const uint8_t>>) {
      out = std::span<const uint8_t>(b_res.ptr, b_res.len);
    } else {
      out.assign(b_res.ptr, b_res.ptr + b_res.len);
    }
    p = b_res.next_pos;
    return true;
  } else {
    return false;
  }
}

template <uint8_t KeyWT, uint8_t ValWT, typename MapT>
[[nodiscard]] inline bool decode_scalar_map_body(MapT& dst,
                                                 std::span<const uint8_t> body,
                                                 DecodeState* state = nullptr) {
  const auto hdr = parse_map_header(body);
  if (!hdr.ok || hdr.key_wt != KeyWT || hdr.val_wt != ValWT) return false;
  if (state && !state->charge_elements(hdr.count)) return false;
  size_t p = hdr.pos;
  using K = typename MapT::key_type;
  using V = typename MapT::mapped_type;
  for (size_t i = 0; i < hdr.count; ++i) {
    K k{};
    V v{};
    if (!decode_container_scalar<KeyWT, K>(body, p, k)) return false;
    if (!decode_container_scalar<ValWT, V>(body, p, v)) return false;
    dst[std::move(k)] = std::move(v);
  }
  return p == body.size();
}

template <uint8_t KeyWT, typename MapT, typename SubDecodeFn>
[[nodiscard]] inline bool decode_submsg_map_body(MapT& dst,
                                                 std::span<const uint8_t> body,
                                                 SubDecodeFn&& decode_sub,
                                                 DecodeState* state = nullptr) {
  const auto hdr = parse_map_header(body);
  if (!hdr.ok || hdr.key_wt != KeyWT || hdr.val_wt != WireSubMsg) return false;
  if (state && !state->charge_elements(hdr.count)) return false;
  size_t p = hdr.pos;
  using K = typename MapT::key_type;
  using V = typename MapT::mapped_type;
  for (size_t i = 0; i < hdr.count; ++i) {
    K k{};
    if (!decode_container_scalar<KeyWT, K>(body, p, k)) return false;
    const auto sub_res = read_len_slice_fast(body, p, 0);
    if (!sub_res) return false;
    p = sub_res.next_pos;
    V v{};
    if (!decode_sub(sub_res.span(), v)) return false;
    dst[std::move(k)] = std::move(v);
  }
  return p == body.size();
}

[[nodiscard]] inline bool decode_map_string_uint_body(std::map<std::string, uint64_t>& dst,
                                                      std::span<const uint8_t> body,
                                                      DecodeState* state = nullptr) {
  return decode_scalar_map_body<WireString, WireUint>(dst, body, state);
}

}  // namespace omnibuf
