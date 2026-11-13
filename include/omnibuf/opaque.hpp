#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "omnibuf/decoder.hpp"
#include "omnibuf/utf8.hpp"
#include "omnibuf/varint.hpp"

namespace omnibuf {

struct LocatedField {
  std::span<const uint8_t> val_slice;
  uint8_t wire_type = 0;
  bool ok = false;
};

// Scans field headers in O(1) time per field using Prefix-Varint length prefixes
// without parsing inside containers, returning the last matching occurrence ("last write wins").
[[nodiscard]] inline LocatedField locate_field_mask(std::span<const uint8_t> packet,
                                                    uint32_t target_field,
                                                    uint16_t exp_mask) noexcept {
  size_t pos = 0;
  const size_t n = packet.size();
  LocatedField result{};

  while (pos < n) {
    uint32_t fnum = 0;
    uint8_t wt = 0;
    const uint8_t b0 = packet[pos];
    if ((b0 & 1u) != 0) {
      fnum = static_cast<uint32_t>(b0 >> 5);
      wt = static_cast<uint8_t>((b0 >> 1) & 0x0Fu);
      ++pos;
    } else if ((b0 & 2u) != 0) {
      if (pos + 2 > n) return {};
      const uint16_t u = static_cast<uint16_t>(read_u16_le(packet.data() + pos) >> 2);
      fnum = static_cast<uint32_t>(u >> 4);
      wt = static_cast<uint8_t>(u & 0x0Fu);
      pos += 2;
    } else {
      const auto [tag, tn] = decode_varint(packet.subspan(pos));
      if (tn == 0 || (tag >> 4) > MAX_FIELD_NUMBER) return {};
      fnum = static_cast<uint32_t>(tag >> 4);
      wt = static_cast<uint8_t>(tag & 0x0Fu);
      pos += tn;
    }

    if (fnum == 0 && wt != WireSubMsg) {
      return {};
    }
    if (fnum == target_field && (exp_mask & (uint16_t{1} << wt)) != 0) {
      result.val_slice = packet.subspan(pos);
      result.wire_type = wt;
      result.ok = true;
    }

    const auto skip_res = skip_value_checked(packet, pos, wt);
    if (!skip_res.ok) return {};
    pos = skip_res.next_pos;
  }
  return result;
}

[[nodiscard]] inline LocatedField locate_field(std::span<const uint8_t> packet,
                                               uint32_t target_field) noexcept {
  return locate_field_mask(packet, target_field, 0xFFFFu);
}

struct NextContainerResult {
  std::span<const uint8_t> body;
  size_t next_pos = 0;
  bool matched = false;
  bool valid = false;
};

[[nodiscard]] inline NextContainerResult next_container_field(std::span<const uint8_t> packet,
                                                              size_t pos,
                                                              uint32_t target_field,
                                                              uint8_t exp_wt,
                                                              uint64_t min_len) noexcept {
  const size_t n = packet.size();
  while (pos < n) {
    uint32_t fnum = 0;
    uint8_t wt = 0;
    const uint8_t b0 = packet[pos];
    if ((b0 & 1u) != 0) {
      fnum = static_cast<uint32_t>(b0 >> 5);
      wt = static_cast<uint8_t>((b0 >> 1) & 0x0Fu);
      ++pos;
    } else if ((b0 & 2u) != 0) {
      if (pos + 2 > n) return {};
      const uint16_t u = static_cast<uint16_t>(read_u16_le(packet.data() + pos) >> 2);
      fnum = static_cast<uint32_t>(u >> 4);
      wt = static_cast<uint8_t>(u & 0x0Fu);
      pos += 2;
    } else {
      const auto [tag, tn] = decode_varint(packet.subspan(pos));
      if (tn == 0 || (tag >> 4) > MAX_FIELD_NUMBER) return {};
      fnum = static_cast<uint32_t>(tag >> 4);
      wt = static_cast<uint8_t>(tag & 0x0Fu);
      pos += tn;
    }

    if (fnum == 0 && wt != WireSubMsg) {
      return {};
    }
    if (fnum == target_field && wt == exp_wt) {
      const auto [b_len, bn] = decode_varint(packet.subspan(pos));
      if (bn == 0 || b_len < min_len || b_len > static_cast<uint64_t>(n - (pos + bn))) {
        return {};
      }
      pos += bn;
      const size_t len_sz = static_cast<size_t>(b_len);
      return {packet.subspan(pos, len_sz), pos + len_sz, true, true};
    }

    const auto skip_res = skip_value_checked(packet, pos, wt);
    if (!skip_res.ok) return {};
    pos = skip_res.next_pos;
  }
  return {{}, n, false, true};
}

// Lazy zero-decode scalar, string, bytes, submsg, array, and map readers.
[[nodiscard]] inline std::pair<uint64_t, bool> read_uint(std::span<const uint8_t> packet,
                                                         uint32_t field_num) noexcept {
  const auto loc = locate_field_mask(packet, field_num, uint16_t{1} << WireUint);
  if (!loc.ok) return {0, false};
  const auto [v, n] = decode_varint(loc.val_slice);
  return {v, n > 0};
}

[[nodiscard]] inline std::pair<int64_t, bool> read_sint(std::span<const uint8_t> packet,
                                                        uint32_t field_num) noexcept {
  const auto loc = locate_field_mask(packet, field_num, uint16_t{1} << WireSint);
  if (!loc.ok) return {0, false};
  const auto [v, n] = decode_varint(loc.val_slice);
  if (n == 0) return {0, false};
  return {decode_zigzag(v), true};
}

[[nodiscard]] inline std::pair<bool, bool> read_bool(std::span<const uint8_t> packet,
                                                     uint32_t field_num) noexcept {
  const auto loc = locate_field_mask(
      packet, field_num, (uint16_t{1} << WireBoolFalse) | (uint16_t{1} << WireBoolTrue));
  if (!loc.ok) return {false, false};
  return {loc.wire_type == WireBoolTrue, true};
}

[[nodiscard]] inline std::pair<uint32_t, bool> read_fixed32(std::span<const uint8_t> packet,
                                                            uint32_t field_num) noexcept {
  const auto loc = locate_field_mask(packet, field_num, uint16_t{1} << WireFixed32);
  if (!loc.ok || loc.val_slice.size() < 4) return {0, false};
  return {read_u32_le(loc.val_slice.data()), true};
}

[[nodiscard]] inline std::pair<int32_t, bool> read_sfixed32(std::span<const uint8_t> packet,
                                                            uint32_t field_num) noexcept {
  const auto loc = locate_field_mask(packet, field_num, uint16_t{1} << WireSfixed32);
  if (!loc.ok || loc.val_slice.size() < 4) return {0, false};
  return {static_cast<int32_t>(read_u32_le(loc.val_slice.data())), true};
}

[[nodiscard]] inline std::pair<float, bool> read_float32(std::span<const uint8_t> packet,
                                                         uint32_t field_num) noexcept {
  const auto loc = locate_field_mask(packet, field_num, uint16_t{1} << WireFloat32);
  if (!loc.ok || loc.val_slice.size() < 4) return {0.0f, false};
  return {bits_to_float32(read_u32_le(loc.val_slice.data())), true};
}

[[nodiscard]] inline std::pair<uint64_t, bool> read_fixed64(std::span<const uint8_t> packet,
                                                            uint32_t field_num) noexcept {
  const auto loc = locate_field_mask(packet, field_num, uint16_t{1} << WireFixed64);
  if (!loc.ok || loc.val_slice.size() < 8) return {0, false};
  return {read_u64_le(loc.val_slice.data()), true};
}

[[nodiscard]] inline std::pair<int64_t, bool> read_sfixed64(std::span<const uint8_t> packet,
                                                            uint32_t field_num) noexcept {
  const auto loc = locate_field_mask(packet, field_num, uint16_t{1} << WireSfixed64);
  if (!loc.ok || loc.val_slice.size() < 8) return {0, false};
  return {static_cast<int64_t>(read_u64_le(loc.val_slice.data())), true};
}

[[nodiscard]] inline std::pair<double, bool> read_float64(std::span<const uint8_t> packet,
                                                          uint32_t field_num) noexcept {
  const auto loc = locate_field_mask(packet, field_num, uint16_t{1} << WireFloat64);
  if (!loc.ok || loc.val_slice.size() < 8) return {0.0, false};
  return {bits_to_float64(read_u64_le(loc.val_slice.data())), true};
}

[[nodiscard]] inline std::pair<std::string_view, bool> read_string_view(
    std::span<const uint8_t> packet,
    uint32_t field_num) noexcept {
  const auto loc = locate_field_mask(packet, field_num, uint16_t{1} << WireString);
  if (!loc.ok || loc.val_slice.empty()) return {{}, false};
  const auto [s_len, sn] = decode_varint(loc.val_slice);
  if (sn == 0 || s_len > static_cast<uint64_t>(loc.val_slice.size() - sn)) return {{}, false};
  const auto body = loc.val_slice.subspan(sn, static_cast<size_t>(s_len));
  if (!is_valid_utf8(body)) return {{}, false};
  return {std::string_view(reinterpret_cast<const char*>(body.data()), body.size()), true};
}

[[nodiscard]] inline std::pair<std::string, bool> read_string(std::span<const uint8_t> packet,
                                                              uint32_t field_num) {
  const auto [sv, ok] = read_string_view(packet, field_num);
  if (!ok) return {{}, false};
  return {std::string(sv), true};
}

[[nodiscard]] inline std::pair<std::span<const uint8_t>, bool> read_bytes_view(
    std::span<const uint8_t> packet,
    uint32_t field_num) noexcept {
  const auto loc = locate_field_mask(packet, field_num, uint16_t{1} << WireBytes);
  if (!loc.ok || loc.val_slice.empty()) return {{}, false};
  const auto [b_len, bn] = decode_varint(loc.val_slice);
  if (bn == 0 || b_len > static_cast<uint64_t>(loc.val_slice.size() - bn)) return {{}, false};
  return {loc.val_slice.subspan(bn, static_cast<size_t>(b_len)), true};
}

[[nodiscard]] inline std::pair<std::vector<uint8_t>, bool> read_bytes(
    std::span<const uint8_t> packet,
    uint32_t field_num) {
  const auto [sp, ok] = read_bytes_view(packet, field_num);
  if (!ok) return {{}, false};
  return {std::vector<uint8_t>(sp.begin(), sp.end()), true};
}

// Reads a nested WireSubMsg payload, concatenating segments when multiple occurrences exist.
[[nodiscard]] inline std::pair<std::vector<uint8_t>, bool> read_submsg(
    std::span<const uint8_t> packet,
    uint32_t field_num) {
  std::vector<uint8_t> merged;
  bool found = false;
  size_t pos = 0;
  while (true) {
    const auto res = next_container_field(packet, pos, field_num, WireSubMsg, 0);
    if (!res.valid) return {{}, false};
    if (!res.matched) return {std::move(merged), found};
    merged.insert(merged.end(), res.body.begin(), res.body.end());
    found = true;
    pos = res.next_pos;
  }
}

[[nodiscard]] inline size_t counted_arr_len(std::span<const uint8_t> packet,
                                            uint32_t field_num,
                                            uint8_t elem_wt) noexcept {
  size_t total = 0;
  size_t pos = 0;
  while (true) {
    const auto res = next_container_field(packet, pos, field_num, WireArray, 2);
    if (!res.valid) return 0;
    if (!res.matched) return total;
    pos = res.next_pos;
    const auto hdr = parse_array_header(res.body);
    if (!hdr.ok || hdr.elem_wt != elem_wt) return 0;
    total += hdr.count;
  }
}

[[nodiscard]] inline std::pair<size_t, bool> count_submsg_arr_elems(
    std::span<const uint8_t> body) noexcept {
  const auto hdr = parse_array_header(body);
  if (!hdr.ok || hdr.elem_wt != WireSubMsg) return {0, false};
  size_t p = hdr.pos;
  for (size_t i = 0; i < hdr.count; ++i) {
    const auto [l, n] = decode_varint(body.subspan(p));
    if (n == 0 || l > static_cast<uint64_t>(body.size() - p - n)) return {0, false};
    p += n + static_cast<size_t>(l);
  }
  return {hdr.count, p == body.size()};
}

[[nodiscard]] inline size_t submsg_arr_len(std::span<const uint8_t> packet,
                                           uint32_t field_num) noexcept {
  size_t total = 0;
  size_t pos = 0;
  while (true) {
    const auto res = next_container_field(packet, pos, field_num, WireArray, 2);
    if (!res.valid) return 0;
    if (!res.matched) return total;
    pos = res.next_pos;
    const auto [c, ok] = count_submsg_arr_elems(res.body);
    if (!ok) return 0;
    total += c;
  }
}

[[nodiscard]] inline std::pair<std::span<const uint8_t>, bool> read_elem_in_submsg_arr(
    std::span<const uint8_t> body,
    size_t index) noexcept {
  const auto hdr = parse_array_header(body);
  if (!hdr.ok || hdr.elem_wt != WireSubMsg || index >= hdr.count) return {{}, false};
  size_t p = hdr.pos;
  std::span<const uint8_t> elem{};
  for (size_t i = 0; i < hdr.count; ++i) {
    const auto [l, n] = decode_varint(body.subspan(p));
    if (n == 0 || l > static_cast<uint64_t>(body.size() - p - n)) return {{}, false};
    p += n;
    const size_t len_sz = static_cast<size_t>(l);
    if (i == index) {
      elem = body.subspan(p, len_sz);
    }
    p += len_sz;
  }
  if (p != body.size()) return {{}, false};
  return {elem, true};
}

[[nodiscard]] inline std::pair<std::span<const uint8_t>, bool> read_submsg_arr_elem(
    std::span<const uint8_t> packet,
    uint32_t field_num,
    size_t index) noexcept {
  size_t rem_idx = index;
  std::span<const uint8_t> found_elem{};
  bool found = false;
  size_t pos = 0;
  while (true) {
    const auto res = next_container_field(packet, pos, field_num, WireArray, 2);
    if (!res.valid) return {{}, false};
    if (!res.matched) return {found_elem, found};
    pos = res.next_pos;
    if (!found) {
      const auto [elem, ok] = read_elem_in_submsg_arr(res.body, rem_idx);
      if (ok) {
        found_elem = elem;
        found = true;
        continue;
      }
    }
    const auto [c, ok] = count_submsg_arr_elems(res.body);
    if (!ok) return {{}, false};
    if (!found) {
      rem_idx -= c;
    }
  }
}

template <typename VecT, typename DecodeFn>
[[nodiscard]] inline bool read_arrays_into(VecT& dst,
                                           std::span<const uint8_t> packet,
                                           uint32_t field_num,
                                           DecodeFn&& decode_fn) {
  const size_t orig_size = dst.size();
  bool found = false;
  size_t pos = 0;
  while (true) {
    const auto res = next_container_field(packet, pos, field_num, WireArray, 2);
    if (!res.valid) {
      dst.resize(orig_size);
      return false;
    }
    if (!res.matched) return found;
    pos = res.next_pos;
    if (!decode_fn(dst, res.body)) {
      dst.resize(orig_size);
      return false;
    }
    found = true;
  }
}

[[nodiscard]] inline std::pair<uint64_t, bool> lookup_map_string_uint(
    std::span<const uint8_t> packet,
    uint32_t field_num,
    std::string_view target_key) noexcept {
  uint64_t last_val = 0;
  bool found = false;
  size_t pos = 0;
  while (true) {
    const auto res = next_container_field(packet, pos, field_num, WireMap, 3);
    if (!res.valid) return {0, false};
    if (!res.matched) return {last_val, found};
    pos = res.next_pos;
    const auto hdr = parse_map_header(res.body);
    if (!hdr.ok || hdr.key_wt != WireString || hdr.val_wt != WireUint) return {0, false};
    size_t p = hdr.pos;
    for (size_t i = 0; i < hdr.count; ++i) {
      auto k_res = read_str_slice(res.body, p);
      if (!k_res) return {0, false};
      p = k_res.value().next_pos;
      const auto [v, vn] = decode_varint(res.body.subspan(p));
      if (vn == 0) return {0, false};
      p += vn;
      const auto ksp = k_res.value().slice;
      const std::string_view k_sv(reinterpret_cast<const char*>(ksp.data()), ksp.size());
      if (k_sv == target_key) {
        last_val = v;
        found = true;
      }
    }
    if (p != res.body.size()) return {0, false};
  }
}

[[nodiscard]] inline std::pair<std::map<std::string, uint64_t>, bool> read_map_string_uint(
    std::span<const uint8_t> packet,
    uint32_t field_num) {
  std::map<std::string, uint64_t> out;
  bool found = false;
  size_t pos = 0;
  while (true) {
    const auto res = next_container_field(packet, pos, field_num, WireMap, 3);
    if (!res.valid) return {{}, false};
    if (!res.matched) return {std::move(out), found};
    pos = res.next_pos;
    if (!decode_map_string_uint_body(out, res.body)) return {{}, false};
    found = true;
  }
}

// Appends raw to dst (preserving any existing prefix in dst), safely detaching if raw aliases dst's buffer.
inline void copy_packet_append(std::vector<uint8_t>& dst, std::span<const uint8_t> raw) {
  if (raw.empty()) return;
  const uintptr_t dst_start = reinterpret_cast<uintptr_t>(dst.data());
  const uintptr_t dst_end = dst_start + dst.capacity();
  const uintptr_t raw_start = reinterpret_cast<uintptr_t>(raw.data());
  const uintptr_t raw_end = raw_start + raw.size();
  if (dst.capacity() > 0 && dst_start < raw_end && raw_start < dst_end) {
    std::vector<uint8_t> detached;
    detached.reserve(dst.size() + raw.size());
    detached.insert(detached.end(), dst.begin(), dst.end());
    detached.insert(detached.end(), raw.begin(), raw.end());
    dst = std::move(detached);
    return;
  }
  dst.insert(dst.end(), raw.begin(), raw.end());
}

[[nodiscard]] inline std::vector<uint8_t> copy_packet(std::vector<uint8_t> dst,
                                                      std::span<const uint8_t> raw) {
  dst.clear();
  copy_packet_append(dst, raw);
  return dst;
}

// Zero-decode wire splicing: preserves any existing prefix in dst, never mutates raw even if
// dst's backing buffer overlaps raw, copies unmodified contiguous field spans verbatim, and
// invokes append_dirty only for fields whose bit is set in dirty_mask.
template <typename AppendDirtyFn>
inline void splice_fields_into(std::vector<uint8_t>& dst,
                               std::span<const uint8_t> raw,
                               uint64_t dirty_mask,
                               uint16_t max_field,
                               AppendDirtyFn&& append_dirty) {
  const size_t n = raw.size();
  std::vector<uint8_t> raw_copy;
  if (n > 0 && dst.capacity() > 0) {
    const uintptr_t dst_start = reinterpret_cast<uintptr_t>(dst.data());
    const uintptr_t dst_end = dst_start + dst.capacity();
    const uintptr_t raw_start = reinterpret_cast<uintptr_t>(raw.data());
    const uintptr_t raw_end = raw_start + n;
    if (dst_start < raw_end && raw_start < dst_end) {
      // Snapshot raw first so appending into dst (or reallocating dst) never corrupts raw.
      raw_copy.assign(raw.begin(), raw.end());
      raw = raw_copy;
    }
  }
  if (dst.capacity() < dst.size() + n + 32) {
    dst.reserve(dst.size() + n + 32);
  }

  size_t pos = 0;
  size_t clean_start = 0;
  size_t valid_end = 0;
  uint64_t emitted = 0;

  while (pos < n) {
    const size_t field_start = pos;
    uint32_t fnum = 0;
    uint8_t wt = 0;
    const uint8_t b0 = raw[pos];
    if ((b0 & 1u) != 0) {
      fnum = static_cast<uint32_t>(b0 >> 5);
      wt = static_cast<uint8_t>((b0 >> 1) & 0x0Fu);
      ++pos;
    } else if ((b0 & 2u) != 0 && pos + 2 <= n) {
      const uint16_t u = static_cast<uint16_t>(read_u16_le(raw.data() + pos) >> 2);
      fnum = static_cast<uint32_t>(u >> 4);
      wt = static_cast<uint8_t>(u & 0x0Fu);
      pos += 2;
    } else {
      const auto [tag, tn] = decode_varint(raw.subspan(pos));
      if (tn == 0 || (tag >> 4) > MAX_FIELD_NUMBER) {
        break;
      }
      fnum = static_cast<uint32_t>(tag >> 4);
      wt = static_cast<uint8_t>(tag & 0x0Fu);
      pos += tn;
    }

    if (fnum == 0 && wt != WireSubMsg) {
      break;
    }

    const auto skip_res = skip_value_checked(raw, pos, wt);
    if (!skip_res.ok) break;
    pos = skip_res.next_pos;

    const size_t field_end = pos;
    valid_end = field_end;

    const uint64_t bit = (fnum < 64u) ? (uint64_t{1} << fnum) : uint64_t{0};
    if ((dirty_mask & bit) != 0) {
      if (clean_start < field_start) {
        dst.insert(dst.end(), raw.begin() + static_cast<ptrdiff_t>(clean_start),
                   raw.begin() + static_cast<ptrdiff_t>(field_start));
      }
      if ((emitted & bit) == 0) {
        append_dirty(dst, static_cast<uint16_t>(fnum));
        emitted |= bit;
      }
      clean_start = field_end;
    } else {
      emitted |= bit;
    }
  }

  if (clean_start < valid_end) {
    dst.insert(dst.end(), raw.begin() + static_cast<ptrdiff_t>(clean_start),
               raw.begin() + static_cast<ptrdiff_t>(valid_end));
  }

  const uint64_t rem = dirty_mask & ~emitted;
  if (rem != 0) {
    for (uint16_t fnum = 1; fnum <= max_field && fnum < 64u; ++fnum) {
      if ((rem & (uint64_t{1} << fnum)) != 0) {
        append_dirty(dst, fnum);
      }
    }
  }
}

template <typename AppendDirtyFn>
[[nodiscard]] inline std::vector<uint8_t> splice_fields(std::vector<uint8_t> dst,
                                                        std::span<const uint8_t> raw,
                                                        uint64_t dirty_mask,
                                                        uint16_t max_field,
                                                        AppendDirtyFn&& append_dirty) {
  splice_fields_into(dst, raw, dirty_mask, max_field, std::forward<AppendDirtyFn>(append_dirty));
  return dst;
}

// Slice-oriented overload matching Go's SpliceFields(dst[:prefix_len], raw, ...):
// preserves prefix bytes from dst_backing and never mutates raw inside dst_backing.
template <typename AppendDirtyFn>
[[nodiscard]] inline std::vector<uint8_t> splice_fields(std::span<uint8_t> dst_backing,
                                                        size_t prefix_len,
                                                        std::span<const uint8_t> raw,
                                                        uint64_t dirty_mask,
                                                        uint16_t max_field,
                                                        AppendDirtyFn&& append_dirty) {
  std::vector<uint8_t> out(dst_backing.begin(),
                           dst_backing.begin() + static_cast<ptrdiff_t>(prefix_len));
  splice_fields_into(out, raw, dirty_mask, max_field, std::forward<AppendDirtyFn>(append_dirty));
  return out;
}

}  // namespace omnibuf
