#pragma once

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include "omnibuf/decoder.hpp"
#include "omnibuf/field0.hpp"
#include "omnibuf/utf8.hpp"
#include "omnibuf/varint.hpp"

namespace omnibuf {

struct LocatedField {
  std::span<const uint8_t> val_slice;
  uint8_t wire_type = 0;
  bool ok = false;
  bool packet_valid = false;
};

struct OneofBranchSpec {
  uint32_t field_num = 0;
  uint16_t exp_mask = 0;
};

struct OpaqueFieldDirSpec {
  uint16_t exp_mask = 0;
  int16_t oneof_group = -1;
  bool is_bool = false;
};

// Arena owned lazily by an Opaque root to retain merged multi-segment submessages
// and materialized dirty submessage buffers across chained view expressions.
struct OpaqueArena {
  struct Slot {
    const void* owner = nullptr;
    uint32_t key = 0;
    std::shared_ptr<const std::vector<uint8_t>> buf{};
  };
  std::vector<Slot> slots{};
  std::vector<std::shared_ptr<const std::vector<uint8_t>>> retired{};

  [[nodiscard]] const std::shared_ptr<const std::vector<uint8_t>>* find(
      const void* owner,
      uint32_t key) const noexcept {
    for (const auto& s : slots) {
      if (s.owner == owner && s.key == key && s.buf != nullptr) return &s.buf;
    }
    return nullptr;
  }

  const std::shared_ptr<const std::vector<uint8_t>>& store(const void* owner,
                                                           uint32_t key,
                                                           std::vector<uint8_t>&& data) {
    auto sp = std::make_shared<const std::vector<uint8_t>>(std::move(data));
    for (auto& s : slots) {
      if (s.owner == owner && s.key == key) {
        if (s.buf != nullptr) retired.push_back(std::move(s.buf));
        s.buf = std::move(sp);
        return s.buf;
      }
    }
    slots.push_back(Slot{owner, key, std::move(sp)});
    return slots.back().buf;
  }

  void invalidate(const void* owner, uint32_t key) {
    for (auto& s : slots) {
      if (s.owner == owner && s.key == key) {
        if (s.buf != nullptr) retired.push_back(std::move(s.buf));
        s.buf.reset();
        return;
      }
    }
  }

  const std::shared_ptr<const std::vector<uint8_t>>& retain_ephemeral(
      std::vector<uint8_t>&& data) {
    retired.push_back(std::make_shared<const std::vector<uint8_t>>(std::move(data)));
    return retired.back();
  }
};

// Scans field headers in O(1) time per field using Prefix-Varint length prefixes
// without parsing inside containers, returning the last matching occurrence ("last write wins").
[[nodiscard]] inline LocatedField locate_field_mask(std::span<const uint8_t> packet,
                                                    uint32_t target_field,
                                                    uint16_t exp_mask) {
  if (target_field == 0) return {};
  size_t pos = 0;
  const size_t n = packet.size();
  LocatedField result{};
  DecodeState f0_state{};

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

    if (fnum == 0) {
      auto f0 = validate_field0(packet, pos, wt, 0, f0_state);
      if (!f0) return {};
      pos = f0.value();
      continue;
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
  result.packet_valid = true;
  return result;
}

[[nodiscard]] inline LocatedField locate_oneof_field(
    std::span<const uint8_t> packet,
    uint32_t target_field,
    std::span<const OneofBranchSpec> group) {
  if (target_field == 0) return {};
  size_t pos = 0;
  const size_t n = packet.size();
  LocatedField result{};
  DecodeState f0_state{};

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

    if (fnum == 0) {
      auto f0 = validate_field0(packet, pos, wt, 0, f0_state);
      if (!f0) return {};
      pos = f0.value();
      continue;
    }

    const uint16_t wt_bit = static_cast<uint16_t>(uint16_t{1} << wt);
    for (const auto& br : group) {
      if (br.field_num == fnum && (br.exp_mask & wt_bit) != 0) {
        if (fnum == target_field) {
          result.val_slice = packet.subspan(pos);
          result.wire_type = wt;
          result.ok = true;
        } else {
          result.val_slice = {};
          result.wire_type = 0;
          result.ok = false;
        }
        break;
      }
    }

    const auto skip_res = skip_value_checked(packet, pos, wt);
    if (!skip_res.ok) return {};
    pos = skip_res.next_pos;
  }
  result.packet_valid = true;
  return result;
}

[[nodiscard]] inline LocatedField locate_field(std::span<const uint8_t> packet,
                                               uint32_t target_field) {
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
                                                              uint64_t min_len) {
  if (target_field == 0) return {};
  const size_t n = packet.size();
  DecodeState f0_state{};
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

    if (fnum == 0) {
      auto f0 = validate_field0(packet, pos, wt, 0, f0_state);
      if (!f0) return {};
      pos = f0.value();
      continue;
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

// Unvalidated O(1) container field hopper for fast infallible Opaque::get_* paths.
[[nodiscard]] inline NextContainerResult next_container_field_fast(std::span<const uint8_t> packet,
                                                                   size_t pos,
                                                                   uint32_t target_field,
                                                                   uint8_t exp_wt,
                                                                   uint64_t min_len) noexcept {
  if (target_field == 0) return {};
  const size_t n = packet.size();
  while (pos < n) {
    uint32_t fnum = 0;
    uint8_t wt = 0;
    const uint8_t b0 = packet[pos];
    if ((b0 & 1u) != 0) [[likely]] {
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

    if (fnum == 0 && wt != WireSubMsg) return {};
    if (fnum == target_field && wt == exp_wt) {
      const auto sl = read_len_slice_fast(packet, pos, min_len);
      if (!sl) return {};
      return {sl.span(), sl.next_pos, true, true};
    }

    const auto skip_res = fast_skip_value(packet, pos, wt);
    if (!skip_res.ok) return {};
    pos = skip_res.next_pos;
  }
  return {{}, n, false, true};
}

// Direct offset readers used by Opaque's lazy field-offset directory.
[[nodiscard]] inline uint64_t read_uint_at(std::span<const uint8_t> packet, size_t pos) noexcept {
  if (pos >= packet.size()) [[unlikely]] {
    return 0;
  }
  if ((packet[pos] & 1u) != 0) [[likely]] {
    return static_cast<uint64_t>(packet[pos] >> 1);
  }
  const auto [v, n] = decode_varint(packet.subspan(pos));
  return n > 0 ? v : 0;
}

[[nodiscard]] inline int64_t read_sint_at(std::span<const uint8_t> packet, size_t pos) noexcept {
  if (pos >= packet.size()) [[unlikely]] {
    return 0;
  }
  if ((packet[pos] & 1u) != 0) [[likely]] {
    return decode_zigzag(static_cast<uint64_t>(packet[pos] >> 1));
  }
  const auto [v, n] = decode_varint(packet.subspan(pos));
  return n > 0 ? decode_zigzag(v) : 0;
}

[[nodiscard]] inline std::string_view read_string_view_at(std::span<const uint8_t> packet,
                                                          size_t pos) noexcept {
  const auto sr = read_str_slice_fast(packet, pos);
  if (!sr) return {};
  return std::string_view(reinterpret_cast<const char*>(sr.ptr), sr.len);
}

[[nodiscard]] inline std::span<const uint8_t> read_bytes_view_at(std::span<const uint8_t> packet,
                                                                 size_t pos) noexcept {
  const auto br = read_len_slice_fast(packet, pos, 0);
  if (!br) return {};
  return br.span();
}

// Lazy zero-decode scalar, string, bytes, submsg, array, and map readers.
[[nodiscard]] inline std::pair<uint64_t, bool> read_uint(std::span<const uint8_t> packet,
                                                         uint32_t field_num) {
  const auto loc = locate_field_mask(packet, field_num, uint16_t{1} << WireUint);
  if (!loc.ok) return {0, false};
  const auto [v, n] = decode_varint(loc.val_slice);
  return {v, n > 0};
}

[[nodiscard]] inline std::pair<int64_t, bool> read_sint(std::span<const uint8_t> packet,
                                                        uint32_t field_num) {
  const auto loc = locate_field_mask(packet, field_num, uint16_t{1} << WireSint);
  if (!loc.ok) return {0, false};
  const auto [v, n] = decode_varint(loc.val_slice);
  if (n == 0) return {0, false};
  return {decode_zigzag(v), true};
}

[[nodiscard]] inline std::pair<bool, bool> read_bool(std::span<const uint8_t> packet,
                                                     uint32_t field_num) {
  const auto loc = locate_field_mask(
      packet, field_num, (uint16_t{1} << WireBoolFalse) | (uint16_t{1} << WireBoolTrue));
  if (!loc.ok) return {false, false};
  return {loc.wire_type == WireBoolTrue, true};
}

[[nodiscard]] inline std::pair<uint32_t, bool> read_fixed32(std::span<const uint8_t> packet,
                                                            uint32_t field_num) {
  const auto loc = locate_field_mask(packet, field_num, uint16_t{1} << WireFixed32);
  if (!loc.ok || loc.val_slice.size() < 4) return {0, false};
  return {read_u32_le(loc.val_slice.data()), true};
}

[[nodiscard]] inline std::pair<int32_t, bool> read_sfixed32(std::span<const uint8_t> packet,
                                                            uint32_t field_num) {
  const auto loc = locate_field_mask(packet, field_num, uint16_t{1} << WireSfixed32);
  if (!loc.ok || loc.val_slice.size() < 4) return {0, false};
  return {static_cast<int32_t>(read_u32_le(loc.val_slice.data())), true};
}

[[nodiscard]] inline std::pair<float, bool> read_float32(std::span<const uint8_t> packet,
                                                         uint32_t field_num) {
  const auto loc = locate_field_mask(packet, field_num, uint16_t{1} << WireFloat32);
  if (!loc.ok || loc.val_slice.size() < 4) return {0.0f, false};
  return {bits_to_float32(read_u32_le(loc.val_slice.data())), true};
}

[[nodiscard]] inline std::pair<uint64_t, bool> read_fixed64(std::span<const uint8_t> packet,
                                                            uint32_t field_num) {
  const auto loc = locate_field_mask(packet, field_num, uint16_t{1} << WireFixed64);
  if (!loc.ok || loc.val_slice.size() < 8) return {0, false};
  return {read_u64_le(loc.val_slice.data()), true};
}

[[nodiscard]] inline std::pair<int64_t, bool> read_sfixed64(std::span<const uint8_t> packet,
                                                            uint32_t field_num) {
  const auto loc = locate_field_mask(packet, field_num, uint16_t{1} << WireSfixed64);
  if (!loc.ok || loc.val_slice.size() < 8) return {0, false};
  return {static_cast<int64_t>(read_u64_le(loc.val_slice.data())), true};
}

[[nodiscard]] inline std::pair<double, bool> read_float64(std::span<const uint8_t> packet,
                                                          uint32_t field_num) {
  const auto loc = locate_field_mask(packet, field_num, uint16_t{1} << WireFloat64);
  if (!loc.ok || loc.val_slice.size() < 8) return {0.0, false};
  return {bits_to_float64(read_u64_le(loc.val_slice.data())), true};
}

[[nodiscard]] inline std::pair<std::string_view, bool> read_string_view(
    std::span<const uint8_t> packet,
    uint32_t field_num) {
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
    uint32_t field_num) {
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
  DecodeState state{};
  while (true) {
    const auto res = next_container_field(packet, pos, field_num, WireSubMsg, 0);
    if (!res.valid) return {{}, false};
    if (!res.matched) return {std::move(merged), found};
    auto vr = validate_message_bytes(res.body, 1, state);
    if (!vr) return {{}, false};
    merged.insert(merged.end(), res.body.begin(), res.body.end());
    found = true;
    pos = res.next_pos;
  }
}

struct OneofSubmsgResult {
  std::span<const uint8_t> single{};
  std::vector<uint8_t> merged{};
  size_t count = 0;
  bool valid = false;
};

[[nodiscard]] inline OneofSubmsgResult read_submsg_zero_copy(std::span<const uint8_t> packet,
                                                             uint32_t field_num,
                                                             bool deep_validate) {
  if (field_num == 0) return {};
  OneofSubmsgResult out{};
  size_t pos = 0;
  while (true) {
    const auto res = deep_validate ? next_container_field(packet, pos, field_num, WireSubMsg, 0)
                                   : next_container_field_fast(packet, pos, field_num, WireSubMsg, 0);
    if (!res.valid) return {};
    if (!res.matched) {
      out.valid = true;
      return out;
    }
    if (deep_validate) {
      auto vr = validate_message_bytes(res.body, 1);
      if (!vr) return {};
    }
    if (out.count == 0) {
      out.single = res.body;
      out.count = 1;
    } else {
      if (out.count == 1) {
        out.merged.assign(out.single.begin(), out.single.end());
        out.single = {};
      }
      out.merged.insert(out.merged.end(), res.body.begin(), res.body.end());
      ++out.count;
    }
    pos = res.next_pos;
  }
}

[[nodiscard]] inline std::pair<std::span<const uint8_t>, bool> locate_last_submsg_slice(
    std::span<const uint8_t> packet,
    uint32_t field_num) noexcept {
  std::span<const uint8_t> last{};
  bool found = false;
  size_t pos = 0;
  while (true) {
    const auto res = next_container_field_fast(packet, pos, field_num, WireSubMsg, 0);
    if (!res.valid) return {{}, false};
    if (!res.matched) return {last, found};
    last = res.body;
    found = true;
    pos = res.next_pos;
  }
}

[[nodiscard]] inline Result<std::optional<std::span<const uint8_t>>> try_locate_last_submsg_slice(
    std::span<const uint8_t> packet,
    uint32_t field_num) {
  std::span<const uint8_t> last{};
  bool found = false;
  size_t pos = 0;
  while (true) {
    const auto res = next_container_field(packet, pos, field_num, WireSubMsg, 0);
    if (!res.valid) return Error{"omnibuf: corrupt packet while reading WireSubMsg"};
    if (!res.matched) {
      if (!found) return std::optional<std::span<const uint8_t>>{std::nullopt};
      return std::optional<std::span<const uint8_t>>{last};
    }
    auto vr = validate_message_bytes(res.body, 1);
    if (!vr) return Error{vr.error()};
    last = res.body;
    found = true;
    pos = res.next_pos;
  }
}

[[nodiscard]] inline std::pair<std::span<const uint8_t>, bool> locate_last_oneof_submsg_slice(
    std::span<const uint8_t> packet,
    uint32_t target_field,
    std::span<const OneofBranchSpec> group) {
  const auto loc = locate_oneof_field(packet, target_field, group);
  if (!loc.ok || loc.wire_type != WireSubMsg) return {{}, false};
  const auto sl = read_len_slice_fast(loc.val_slice, 0, 0);
  if (!sl) return {{}, false};
  return {sl.span(), true};
}

[[nodiscard]] inline Result<std::optional<std::span<const uint8_t>>>
try_locate_last_oneof_submsg_slice(std::span<const uint8_t> packet,
                                   uint32_t target_field,
                                   std::span<const OneofBranchSpec> group) {
  const auto loc = locate_oneof_field(packet, target_field, group);
  if (!loc.packet_valid) return Error{"omnibuf: corrupt packet"};
  if (!loc.ok || loc.wire_type != WireSubMsg) {
    return std::optional<std::span<const uint8_t>>{std::nullopt};
  }
  const auto sl = read_len_slice_fast(loc.val_slice, 0, 0);
  if (!sl) return Error{sl.err};
  auto vr = validate_message_bytes(sl.span(), 1);
  if (!vr) return Error{vr.error()};
  return std::optional<std::span<const uint8_t>>{sl.span()};
}

template <typename MapOrdFn>
[[nodiscard]] inline uint8_t build_opaque_offset_dir(
    std::span<const uint8_t> packet,
    std::span<const OpaqueFieldDirSpec> specs,
    std::span<uint64_t> present_words,
    std::span<uint32_t> offsets,
    MapOrdFn&& map_fnum_to_ord) {
  for (uint64_t& w : present_words) {
    w = 0;
  }
  const size_t n = packet.size();
  if (n > UINT32_MAX) return 4u;
  size_t pos = 0;
  DecodeState f0_state{};

  while (pos < n) {
    uint32_t fnum = 0;
    uint8_t wt = 0;
    const uint8_t b0 = packet[pos];
    if ((b0 & 1u) != 0) [[likely]] {
      fnum = static_cast<uint32_t>(b0 >> 5);
      wt = static_cast<uint8_t>((b0 >> 1) & 0x0Fu);
      ++pos;
    } else if ((b0 & 2u) != 0) {
      if (pos + 2 > n) return 3u;
      const uint16_t u = static_cast<uint16_t>(read_u16_le(packet.data() + pos) >> 2);
      fnum = static_cast<uint32_t>(u >> 4);
      wt = static_cast<uint8_t>(u & 0x0Fu);
      pos += 2;
    } else {
      const auto [tag, tn] = decode_varint(packet.subspan(pos));
      if (tn == 0 || (tag >> 4) > MAX_FIELD_NUMBER) return 3u;
      fnum = static_cast<uint32_t>(tag >> 4);
      wt = static_cast<uint8_t>(tag & 0x0Fu);
      pos += tn;
    }

    if (fnum == 0) {
      auto f0 = validate_field0(packet, pos, wt, 0, f0_state);
      if (!f0) return 3u;
      pos = f0.value();
      continue;
    }

    const auto skip_res = skip_value_checked(packet, pos, wt);
    if (!skip_res.ok) return 3u;

    const int ord_i = map_fnum_to_ord(fnum);
    if (ord_i >= 0) {
      const size_t ord = static_cast<size_t>(ord_i);
      if (ord < specs.size()) {
        const auto& sp = specs[ord];
        if ((sp.exp_mask & static_cast<uint16_t>(uint16_t{1} << wt)) != 0) {
          if (sp.oneof_group >= 0) {
            for (size_t sib = 0; sib < specs.size(); ++sib) {
              if (specs[sib].oneof_group == sp.oneof_group) {
                present_words[sib >> 6] &= ~(uint64_t{1} << (sib & 63u));
              }
            }
          }
          present_words[ord >> 6] |= (uint64_t{1} << (ord & 63u));
          offsets[ord] = sp.is_bool ? ((wt == WireBoolTrue) ? 1u : 0u)
                                    : static_cast<uint32_t>(pos);
        }
      }
    }
    pos = skip_res.next_pos;
  }
  return 2u;
}

[[nodiscard]] inline OneofSubmsgResult read_oneof_submsg(
    std::span<const uint8_t> packet,
    uint32_t target_field,
    std::span<const OneofBranchSpec> group,
    bool deep_validate) {
  if (target_field == 0) return {};
  size_t pos = 0;
  const size_t n = packet.size();
  OneofSubmsgResult res{};
  DecodeState f0_state{};

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

    if (fnum == 0) {
      if (deep_validate) {
        auto f0 = validate_field0(packet, pos, wt, 0, f0_state);
        if (!f0) return {};
        pos = f0.value();
      } else {
        if (wt != WireSubMsg) return {};
        const auto sk = fast_skip_value(packet, pos, wt);
        if (!sk.ok) return {};
        pos = sk.next_pos;
      }
      continue;
    }

    if (fnum == target_field && wt == WireSubMsg) {
      const auto sl = read_len_slice_fast(packet, pos, 0);
      if (!sl) return {};
      if (deep_validate) {
        auto vr = validate_message_bytes(sl.span(), 1);
        if (!vr) return {};
      }
      if (res.count == 0) {
        res.single = sl.span();
        res.count = 1;
      } else {
        if (res.count == 1) {
          if (!deep_validate) {
            auto vr0 = validate_message_bytes(res.single, 1, f0_state);
            if (!vr0) return {};
          }
          res.merged.assign(res.single.begin(), res.single.end());
          res.single = {};
        }
        if (!deep_validate) {
          auto vr_cur = validate_message_bytes(sl.span(), 1, f0_state);
          if (!vr_cur) return {};
        }
        res.merged.insert(res.merged.end(), sl.ptr, sl.ptr + sl.len);
        ++res.count;
      }
      pos = sl.next_pos;
      continue;
    }

    const uint16_t wt_bit = static_cast<uint16_t>(uint16_t{1} << wt);
    for (const auto& br : group) {
      if (br.field_num == fnum && fnum != target_field && (br.exp_mask & wt_bit) != 0) {
        res.single = {};
        res.merged.clear();
        res.count = 0;
        break;
      }
    }

    const auto skip_res =
        deep_validate ? skip_value_checked(packet, pos, wt) : fast_skip_value(packet, pos, wt);
    if (!skip_res.ok) return {};
    pos = skip_res.next_pos;
  }
  res.valid = true;
  return res;
}

[[nodiscard]] inline size_t counted_arr_len(std::span<const uint8_t> packet,
                                            uint32_t field_num,
                                            uint8_t elem_wt) noexcept {
  size_t total = 0;
  size_t pos = 0;
  while (true) {
    const auto res = next_container_field_fast(packet, pos, field_num, WireArray, 2);
    if (!res.valid) return 0;
    if (!res.matched) return total;
    pos = res.next_pos;
    const auto hdr = parse_array_header(res.body);
    if (!hdr.ok || hdr.elem_wt != elem_wt) return 0;
    if (hdr.count > SIZE_MAX - total) return 0;
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
    const auto res = next_container_field_fast(packet, pos, field_num, WireArray, 2);
    if (!res.valid) return 0;
    if (!res.matched) return total;
    pos = res.next_pos;
    const auto [c, ok] = count_submsg_arr_elems(res.body);
    if (!ok || c > SIZE_MAX - total) return 0;
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
    size_t index) {
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

template <typename VecT, typename DecodeFn>
[[nodiscard]] inline Result<VecT> try_read_arrays(std::span<const uint8_t> packet,
                                                  uint32_t field_num,
                                                  DecodeFn&& decode_fn) {
  VecT dst;
  size_t pos = 0;
  while (true) {
    const auto res = next_container_field(packet, pos, field_num, WireArray, 2);
    if (!res.valid) {
      return Error{"omnibuf: corrupt packet while reading WireArray"};
    }
    if (!res.matched) return dst;
    pos = res.next_pos;
    if (!decode_fn(dst, res.body)) {
      return Error{"omnibuf: invalid WireArray payload"};
    }
  }
}

[[nodiscard]] inline Result<std::optional<uint64_t>> try_read_uint(std::span<const uint8_t> packet,
                                                                   uint32_t field_num) {
  const auto loc = locate_field_mask(packet, field_num, uint16_t{1} << WireUint);
  if (!loc.packet_valid) return Error{"omnibuf: corrupt packet"};
  if (!loc.ok) return std::optional<uint64_t>{std::nullopt};
  const auto [v, n] = decode_varint(loc.val_slice);
  if (n == 0) return Error{"omnibuf: corrupt WireUint varint"};
  return std::optional<uint64_t>{v};
}

[[nodiscard]] inline Result<std::optional<int64_t>> try_read_sint(std::span<const uint8_t> packet,
                                                                  uint32_t field_num) {
  const auto loc = locate_field_mask(packet, field_num, uint16_t{1} << WireSint);
  if (!loc.packet_valid) return Error{"omnibuf: corrupt packet"};
  if (!loc.ok) return std::optional<int64_t>{std::nullopt};
  const auto [v, n] = decode_varint(loc.val_slice);
  if (n == 0) return Error{"omnibuf: corrupt WireSint varint"};
  return std::optional<int64_t>{decode_zigzag(v)};
}

[[nodiscard]] inline Result<std::optional<bool>> try_read_bool(std::span<const uint8_t> packet,
                                                               uint32_t field_num) {
  const auto loc = locate_field_mask(
      packet, field_num, (uint16_t{1} << WireBoolFalse) | (uint16_t{1} << WireBoolTrue));
  if (!loc.packet_valid) return Error{"omnibuf: corrupt packet"};
  if (!loc.ok) return std::optional<bool>{std::nullopt};
  return std::optional<bool>{loc.wire_type == WireBoolTrue};
}

[[nodiscard]] inline Result<std::optional<uint32_t>> try_read_fixed32(
    std::span<const uint8_t> packet,
    uint32_t field_num) {
  const auto loc = locate_field_mask(packet, field_num, uint16_t{1} << WireFixed32);
  if (!loc.packet_valid) return Error{"omnibuf: corrupt packet"};
  if (!loc.ok) return std::optional<uint32_t>{std::nullopt};
  if (loc.val_slice.size() < 4) return Error{"omnibuf: truncated WireFixed32"};
  return std::optional<uint32_t>{read_u32_le(loc.val_slice.data())};
}

[[nodiscard]] inline Result<std::optional<int32_t>> try_read_sfixed32(
    std::span<const uint8_t> packet,
    uint32_t field_num) {
  const auto loc = locate_field_mask(packet, field_num, uint16_t{1} << WireSfixed32);
  if (!loc.packet_valid) return Error{"omnibuf: corrupt packet"};
  if (!loc.ok) return std::optional<int32_t>{std::nullopt};
  if (loc.val_slice.size() < 4) return Error{"omnibuf: truncated WireSfixed32"};
  return std::optional<int32_t>{static_cast<int32_t>(read_u32_le(loc.val_slice.data()))};
}

[[nodiscard]] inline Result<std::optional<float>> try_read_float32(std::span<const uint8_t> packet,
                                                                   uint32_t field_num) {
  const auto loc = locate_field_mask(packet, field_num, uint16_t{1} << WireFloat32);
  if (!loc.packet_valid) return Error{"omnibuf: corrupt packet"};
  if (!loc.ok) return std::optional<float>{std::nullopt};
  if (loc.val_slice.size() < 4) return Error{"omnibuf: truncated WireFloat32"};
  return std::optional<float>{bits_to_float32(read_u32_le(loc.val_slice.data()))};
}

[[nodiscard]] inline Result<std::optional<uint64_t>> try_read_fixed64(
    std::span<const uint8_t> packet,
    uint32_t field_num) {
  const auto loc = locate_field_mask(packet, field_num, uint16_t{1} << WireFixed64);
  if (!loc.packet_valid) return Error{"omnibuf: corrupt packet"};
  if (!loc.ok) return std::optional<uint64_t>{std::nullopt};
  if (loc.val_slice.size() < 8) return Error{"omnibuf: truncated WireFixed64"};
  return std::optional<uint64_t>{read_u64_le(loc.val_slice.data())};
}

[[nodiscard]] inline Result<std::optional<int64_t>> try_read_sfixed64(
    std::span<const uint8_t> packet,
    uint32_t field_num) {
  const auto loc = locate_field_mask(packet, field_num, uint16_t{1} << WireSfixed64);
  if (!loc.packet_valid) return Error{"omnibuf: corrupt packet"};
  if (!loc.ok) return std::optional<int64_t>{std::nullopt};
  if (loc.val_slice.size() < 8) return Error{"omnibuf: truncated WireSfixed64"};
  return std::optional<int64_t>{static_cast<int64_t>(read_u64_le(loc.val_slice.data()))};
}

[[nodiscard]] inline Result<std::optional<double>> try_read_float64(std::span<const uint8_t> packet,
                                                                    uint32_t field_num) {
  const auto loc = locate_field_mask(packet, field_num, uint16_t{1} << WireFloat64);
  if (!loc.packet_valid) return Error{"omnibuf: corrupt packet"};
  if (!loc.ok) return std::optional<double>{std::nullopt};
  if (loc.val_slice.size() < 8) return Error{"omnibuf: truncated WireFloat64"};
  return std::optional<double>{bits_to_float64(read_u64_le(loc.val_slice.data()))};
}

[[nodiscard]] inline Result<std::optional<std::string_view>> try_read_string_view(
    std::span<const uint8_t> packet,
    uint32_t field_num) {
  const auto loc = locate_field_mask(packet, field_num, uint16_t{1} << WireString);
  if (!loc.packet_valid) return Error{"omnibuf: corrupt packet"};
  if (!loc.ok) return std::optional<std::string_view>{std::nullopt};
  if (loc.val_slice.empty()) return Error{"omnibuf: truncated WireString"};
  const auto [s_len, sn] = decode_varint(loc.val_slice);
  if (sn == 0 || s_len > static_cast<uint64_t>(loc.val_slice.size() - sn)) {
    return Error{"omnibuf: truncated WireString"};
  }
  const auto body = loc.val_slice.subspan(sn, static_cast<size_t>(s_len));
  if (!is_valid_utf8(body)) return Error{"omnibuf: invalid UTF-8 in WireString"};
  return std::optional<std::string_view>{
      std::string_view(reinterpret_cast<const char*>(body.data()), body.size())};
}

[[nodiscard]] inline Result<std::optional<std::span<const uint8_t>>> try_read_bytes_view(
    std::span<const uint8_t> packet,
    uint32_t field_num) {
  const auto loc = locate_field_mask(packet, field_num, uint16_t{1} << WireBytes);
  if (!loc.packet_valid) return Error{"omnibuf: corrupt packet"};
  if (!loc.ok) return std::optional<std::span<const uint8_t>>{std::nullopt};
  if (loc.val_slice.empty()) return Error{"omnibuf: truncated WireBytes"};
  const auto [b_len, bn] = decode_varint(loc.val_slice);
  if (bn == 0 || b_len > static_cast<uint64_t>(loc.val_slice.size() - bn)) {
    return Error{"omnibuf: truncated WireBytes"};
  }
  return std::optional<std::span<const uint8_t>>{
      loc.val_slice.subspan(bn, static_cast<size_t>(b_len))};
}

[[nodiscard]] inline std::pair<uint64_t, bool> read_oneof_uint(
    std::span<const uint8_t> packet,
    uint32_t field_num,
    std::span<const OneofBranchSpec> group) {
  const auto loc = locate_oneof_field(packet, field_num, group);
  if (!loc.ok) return {0, false};
  const auto [v, n] = decode_varint(loc.val_slice);
  return {v, n > 0};
}

[[nodiscard]] inline std::pair<int64_t, bool> read_oneof_sint(
    std::span<const uint8_t> packet,
    uint32_t field_num,
    std::span<const OneofBranchSpec> group) {
  const auto loc = locate_oneof_field(packet, field_num, group);
  if (!loc.ok) return {0, false};
  const auto [v, n] = decode_varint(loc.val_slice);
  if (n == 0) return {0, false};
  return {decode_zigzag(v), true};
}

[[nodiscard]] inline std::pair<bool, bool> read_oneof_bool(
    std::span<const uint8_t> packet,
    uint32_t field_num,
    std::span<const OneofBranchSpec> group) {
  const auto loc = locate_oneof_field(packet, field_num, group);
  if (!loc.ok) return {false, false};
  return {loc.wire_type == WireBoolTrue, true};
}

[[nodiscard]] inline std::pair<uint32_t, bool> read_oneof_fixed32(
    std::span<const uint8_t> packet,
    uint32_t field_num,
    std::span<const OneofBranchSpec> group) {
  const auto loc = locate_oneof_field(packet, field_num, group);
  if (!loc.ok || loc.val_slice.size() < 4) return {0, false};
  return {read_u32_le(loc.val_slice.data()), true};
}

[[nodiscard]] inline std::pair<int32_t, bool> read_oneof_sfixed32(
    std::span<const uint8_t> packet,
    uint32_t field_num,
    std::span<const OneofBranchSpec> group) {
  const auto loc = locate_oneof_field(packet, field_num, group);
  if (!loc.ok || loc.val_slice.size() < 4) return {0, false};
  return {static_cast<int32_t>(read_u32_le(loc.val_slice.data())), true};
}

[[nodiscard]] inline std::pair<float, bool> read_oneof_float32(
    std::span<const uint8_t> packet,
    uint32_t field_num,
    std::span<const OneofBranchSpec> group) {
  const auto loc = locate_oneof_field(packet, field_num, group);
  if (!loc.ok || loc.val_slice.size() < 4) return {0.0f, false};
  return {bits_to_float32(read_u32_le(loc.val_slice.data())), true};
}

[[nodiscard]] inline std::pair<uint64_t, bool> read_oneof_fixed64(
    std::span<const uint8_t> packet,
    uint32_t field_num,
    std::span<const OneofBranchSpec> group) {
  const auto loc = locate_oneof_field(packet, field_num, group);
  if (!loc.ok || loc.val_slice.size() < 8) return {0, false};
  return {read_u64_le(loc.val_slice.data()), true};
}

[[nodiscard]] inline std::pair<int64_t, bool> read_oneof_sfixed64(
    std::span<const uint8_t> packet,
    uint32_t field_num,
    std::span<const OneofBranchSpec> group) {
  const auto loc = locate_oneof_field(packet, field_num, group);
  if (!loc.ok || loc.val_slice.size() < 8) return {0, false};
  return {static_cast<int64_t>(read_u64_le(loc.val_slice.data())), true};
}

[[nodiscard]] inline std::pair<double, bool> read_oneof_float64(
    std::span<const uint8_t> packet,
    uint32_t field_num,
    std::span<const OneofBranchSpec> group) {
  const auto loc = locate_oneof_field(packet, field_num, group);
  if (!loc.ok || loc.val_slice.size() < 8) return {0.0, false};
  return {bits_to_float64(read_u64_le(loc.val_slice.data())), true};
}

[[nodiscard]] inline std::pair<std::string_view, bool> read_oneof_string_view(
    std::span<const uint8_t> packet,
    uint32_t field_num,
    std::span<const OneofBranchSpec> group) {
  const auto loc = locate_oneof_field(packet, field_num, group);
  if (!loc.ok || loc.val_slice.empty()) return {{}, false};
  const auto sr = read_str_slice_fast(loc.val_slice, 0);
  if (!sr) return {{}, false};
  return {std::string_view(reinterpret_cast<const char*>(sr.ptr), sr.len), true};
}

[[nodiscard]] inline std::pair<std::span<const uint8_t>, bool> read_oneof_bytes_view(
    std::span<const uint8_t> packet,
    uint32_t field_num,
    std::span<const OneofBranchSpec> group) {
  const auto loc = locate_oneof_field(packet, field_num, group);
  if (!loc.ok || loc.val_slice.empty()) return {{}, false};
  const auto br = read_len_slice_fast(loc.val_slice, 0, 0);
  if (!br) return {{}, false};
  return {br.span(), true};
}

[[nodiscard]] inline Result<std::optional<uint64_t>> try_read_oneof_uint(
    std::span<const uint8_t> packet,
    uint32_t field_num,
    std::span<const OneofBranchSpec> group) {
  const auto loc = locate_oneof_field(packet, field_num, group);
  if (!loc.packet_valid) return Error{"omnibuf: corrupt packet"};
  if (!loc.ok) return std::optional<uint64_t>{std::nullopt};
  const auto [v, n] = decode_varint(loc.val_slice);
  if (n == 0) return Error{"omnibuf: corrupt WireUint varint"};
  return std::optional<uint64_t>{v};
}

[[nodiscard]] inline Result<std::optional<int64_t>> try_read_oneof_sint(
    std::span<const uint8_t> packet,
    uint32_t field_num,
    std::span<const OneofBranchSpec> group) {
  const auto loc = locate_oneof_field(packet, field_num, group);
  if (!loc.packet_valid) return Error{"omnibuf: corrupt packet"};
  if (!loc.ok) return std::optional<int64_t>{std::nullopt};
  const auto [v, n] = decode_varint(loc.val_slice);
  if (n == 0) return Error{"omnibuf: corrupt WireSint varint"};
  return std::optional<int64_t>{decode_zigzag(v)};
}

[[nodiscard]] inline Result<std::optional<bool>> try_read_oneof_bool(
    std::span<const uint8_t> packet,
    uint32_t field_num,
    std::span<const OneofBranchSpec> group) {
  const auto loc = locate_oneof_field(packet, field_num, group);
  if (!loc.packet_valid) return Error{"omnibuf: corrupt packet"};
  if (!loc.ok) return std::optional<bool>{std::nullopt};
  return std::optional<bool>{loc.wire_type == WireBoolTrue};
}

[[nodiscard]] inline Result<std::optional<uint32_t>> try_read_oneof_fixed32(
    std::span<const uint8_t> packet,
    uint32_t field_num,
    std::span<const OneofBranchSpec> group) {
  const auto loc = locate_oneof_field(packet, field_num, group);
  if (!loc.packet_valid) return Error{"omnibuf: corrupt packet"};
  if (!loc.ok) return std::optional<uint32_t>{std::nullopt};
  if (loc.val_slice.size() < 4) return Error{"omnibuf: truncated WireFixed32"};
  return std::optional<uint32_t>{read_u32_le(loc.val_slice.data())};
}

[[nodiscard]] inline Result<std::optional<int32_t>> try_read_oneof_sfixed32(
    std::span<const uint8_t> packet,
    uint32_t field_num,
    std::span<const OneofBranchSpec> group) {
  const auto loc = locate_oneof_field(packet, field_num, group);
  if (!loc.packet_valid) return Error{"omnibuf: corrupt packet"};
  if (!loc.ok) return std::optional<int32_t>{std::nullopt};
  if (loc.val_slice.size() < 4) return Error{"omnibuf: truncated WireSfixed32"};
  return std::optional<int32_t>{static_cast<int32_t>(read_u32_le(loc.val_slice.data()))};
}

[[nodiscard]] inline Result<std::optional<float>> try_read_oneof_float32(
    std::span<const uint8_t> packet,
    uint32_t field_num,
    std::span<const OneofBranchSpec> group) {
  const auto loc = locate_oneof_field(packet, field_num, group);
  if (!loc.packet_valid) return Error{"omnibuf: corrupt packet"};
  if (!loc.ok) return std::optional<float>{std::nullopt};
  if (loc.val_slice.size() < 4) return Error{"omnibuf: truncated WireFloat32"};
  return std::optional<float>{bits_to_float32(read_u32_le(loc.val_slice.data()))};
}

[[nodiscard]] inline Result<std::optional<uint64_t>> try_read_oneof_fixed64(
    std::span<const uint8_t> packet,
    uint32_t field_num,
    std::span<const OneofBranchSpec> group) {
  const auto loc = locate_oneof_field(packet, field_num, group);
  if (!loc.packet_valid) return Error{"omnibuf: corrupt packet"};
  if (!loc.ok) return std::optional<uint64_t>{std::nullopt};
  if (loc.val_slice.size() < 8) return Error{"omnibuf: truncated WireFixed64"};
  return std::optional<uint64_t>{read_u64_le(loc.val_slice.data())};
}

[[nodiscard]] inline Result<std::optional<int64_t>> try_read_oneof_sfixed64(
    std::span<const uint8_t> packet,
    uint32_t field_num,
    std::span<const OneofBranchSpec> group) {
  const auto loc = locate_oneof_field(packet, field_num, group);
  if (!loc.packet_valid) return Error{"omnibuf: corrupt packet"};
  if (!loc.ok) return std::optional<int64_t>{std::nullopt};
  if (loc.val_slice.size() < 8) return Error{"omnibuf: truncated WireSfixed64"};
  return std::optional<int64_t>{static_cast<int64_t>(read_u64_le(loc.val_slice.data()))};
}

[[nodiscard]] inline Result<std::optional<double>> try_read_oneof_float64(
    std::span<const uint8_t> packet,
    uint32_t field_num,
    std::span<const OneofBranchSpec> group) {
  const auto loc = locate_oneof_field(packet, field_num, group);
  if (!loc.packet_valid) return Error{"omnibuf: corrupt packet"};
  if (!loc.ok) return std::optional<double>{std::nullopt};
  if (loc.val_slice.size() < 8) return Error{"omnibuf: truncated WireFloat64"};
  return std::optional<double>{bits_to_float64(read_u64_le(loc.val_slice.data()))};
}

[[nodiscard]] inline Result<std::optional<std::string_view>> try_read_oneof_string_view(
    std::span<const uint8_t> packet,
    uint32_t field_num,
    std::span<const OneofBranchSpec> group) {
  const auto loc = locate_oneof_field(packet, field_num, group);
  if (!loc.packet_valid) return Error{"omnibuf: corrupt packet"};
  if (!loc.ok) return std::optional<std::string_view>{std::nullopt};
  const auto sr = read_str_slice_fast(loc.val_slice, 0);
  if (!sr) return Error{sr.err};
  return std::optional<std::string_view>{
      std::string_view(reinterpret_cast<const char*>(sr.ptr), sr.len)};
}

[[nodiscard]] inline Result<std::optional<std::span<const uint8_t>>> try_read_oneof_bytes_view(
    std::span<const uint8_t> packet,
    uint32_t field_num,
    std::span<const OneofBranchSpec> group) {
  const auto loc = locate_oneof_field(packet, field_num, group);
  if (!loc.packet_valid) return Error{"omnibuf: corrupt packet"};
  if (!loc.ok) return std::optional<std::span<const uint8_t>>{std::nullopt};
  const auto br = read_len_slice_fast(loc.val_slice, 0, 0);
  if (!br) return Error{br.err};
  return std::optional<std::span<const uint8_t>>{br.span()};
}

[[nodiscard]] inline Result<std::optional<std::vector<uint8_t>>> try_read_submsg(
    std::span<const uint8_t> packet,
    uint32_t field_num) {
  std::vector<uint8_t> merged;
  bool found = false;
  size_t pos = 0;
  while (true) {
    const auto res = next_container_field(packet, pos, field_num, WireSubMsg, 0);
    if (!res.valid) return Error{"omnibuf: corrupt packet while reading WireSubMsg"};
    if (!res.matched) {
      if (!found) return std::optional<std::vector<uint8_t>>{std::nullopt};
      return std::optional<std::vector<uint8_t>>{std::move(merged)};
    }
    auto vr = validate_message_bytes(res.body, 1);
    if (!vr) return Error{vr.error()};
    merged.insert(merged.end(), res.body.begin(), res.body.end());
    found = true;
    pos = res.next_pos;
  }
}

template <uint8_t KeyWT, uint8_t ValWT, typename K, typename V, typename TargetK>
[[nodiscard]] inline std::pair<V, bool> lookup_scalar_map(std::span<const uint8_t> packet,
                                                          uint32_t field_num,
                                                          const TargetK& target_key) {
  V last_val{};
  bool found = false;
  size_t pos = 0;
  while (true) {
    const auto res = next_container_field(packet, pos, field_num, WireMap, 3);
    if (!res.valid) return {V{}, false};
    if (!res.matched) return {std::move(last_val), found};
    pos = res.next_pos;
    const auto hdr = parse_map_header(res.body);
    if (!hdr.ok || hdr.key_wt != KeyWT || hdr.val_wt != ValWT) return {V{}, false};
    size_t p = hdr.pos;
    for (size_t i = 0; i < hdr.count; ++i) {
      if constexpr (KeyWT == WireString) {
        const auto k_res = read_str_slice_fast(res.body, p);
        if (!k_res) return {V{}, false};
        p = k_res.next_pos;
        V v{};
        if (!decode_container_scalar<ValWT, V>(res.body, p, v)) return {V{}, false};
        const std::string_view k_sv(reinterpret_cast<const char*>(k_res.ptr), k_res.len);
        if (k_sv == target_key) {
          last_val = std::move(v);
          found = true;
        }
      } else {
        K k{};
        V v{};
        if (!decode_container_scalar<KeyWT, K>(res.body, p, k)) return {V{}, false};
        if (!decode_container_scalar<ValWT, V>(res.body, p, v)) return {V{}, false};
        if (k == target_key) {
          last_val = std::move(v);
          found = true;
        }
      }
    }
    if (p != res.body.size()) return {V{}, false};
  }
}

template <uint8_t KeyWT, typename K, typename TargetK>
[[nodiscard]] inline std::pair<std::span<const uint8_t>, bool> lookup_submsg_map_slice(
    std::span<const uint8_t> packet,
    uint32_t field_num,
    const TargetK& target_key) {
  std::span<const uint8_t> last_slice{};
  bool found = false;
  size_t pos = 0;
  while (true) {
    const auto res = next_container_field(packet, pos, field_num, WireMap, 3);
    if (!res.valid) return {{}, false};
    if (!res.matched) return {last_slice, found};
    pos = res.next_pos;
    const auto hdr = parse_map_header(res.body);
    if (!hdr.ok || hdr.key_wt != KeyWT || hdr.val_wt != WireSubMsg) return {{}, false};
    size_t p = hdr.pos;
    for (size_t i = 0; i < hdr.count; ++i) {
      bool key_eq = false;
      if constexpr (KeyWT == WireString) {
        const auto k_res = read_str_slice_fast(res.body, p);
        if (!k_res) return {{}, false};
        p = k_res.next_pos;
        key_eq = (std::string_view(reinterpret_cast<const char*>(k_res.ptr), k_res.len) ==
                  target_key);
      } else {
        K k{};
        if (!decode_container_scalar<KeyWT, K>(res.body, p, k)) return {{}, false};
        key_eq = (k == target_key);
      }
      const auto sub_res = read_len_slice_fast(res.body, p, 0);
      if (!sub_res) return {{}, false};
      p = sub_res.next_pos;
      if (key_eq) {
        last_slice = sub_res.span();
        found = true;
      }
    }
    if (p != res.body.size()) return {{}, false};
  }
}

template <uint8_t KeyWT, uint8_t ValWT, typename MapT>
[[nodiscard]] inline std::pair<MapT, bool> read_scalar_map(std::span<const uint8_t> packet,
                                                           uint32_t field_num,
                                                           DecodeState* state = nullptr) {
  DecodeState local_state{};
  DecodeState* st = state ? state : &local_state;
  MapT out;
  bool found = false;
  size_t pos = 0;
  while (true) {
    const auto res = next_container_field(packet, pos, field_num, WireMap, 3);
    if (!res.valid) return {MapT{}, false};
    if (!res.matched) return {std::move(out), found};
    pos = res.next_pos;
    if (!decode_scalar_map_body<KeyWT, ValWT>(out, res.body, st)) return {MapT{}, false};
    found = true;
  }
}

template <uint8_t KeyWT, uint8_t ValWT, typename MapT>
[[nodiscard]] inline Result<MapT> try_read_scalar_map(std::span<const uint8_t> packet,
                                                      uint32_t field_num,
                                                      DecodeState* state = nullptr) {
  DecodeState local_state{};
  DecodeState* st = state ? state : &local_state;
  MapT out;
  size_t pos = 0;
  while (true) {
    const auto res = next_container_field(packet, pos, field_num, WireMap, 3);
    if (!res.valid) return Error{"omnibuf: corrupt packet while reading WireMap"};
    if (!res.matched) return out;
    pos = res.next_pos;
    if (!decode_scalar_map_body<KeyWT, ValWT>(out, res.body, st)) {
      return Error{"omnibuf: invalid WireMap payload"};
    }
  }
}

template <uint8_t KeyWT, typename MapT, typename SubDecodeFn>
[[nodiscard]] inline std::pair<MapT, bool> read_submsg_map(std::span<const uint8_t> packet,
                                                           uint32_t field_num,
                                                           SubDecodeFn&& decode_sub,
                                                           DecodeState* state = nullptr) {
  DecodeState local_state{};
  DecodeState* st = state ? state : &local_state;
  MapT out;
  bool found = false;
  size_t pos = 0;
  while (true) {
    const auto res = next_container_field(packet, pos, field_num, WireMap, 3);
    if (!res.valid) return {MapT{}, false};
    if (!res.matched) return {std::move(out), found};
    pos = res.next_pos;
    if (!decode_submsg_map_body<KeyWT>(out, res.body, decode_sub, st)) return {MapT{}, false};
    found = true;
  }
}

template <uint8_t KeyWT, typename MapT, typename SubDecodeFn>
[[nodiscard]] inline Result<MapT> try_read_submsg_map(std::span<const uint8_t> packet,
                                                      uint32_t field_num,
                                                      SubDecodeFn&& decode_sub,
                                                      DecodeState* state = nullptr) {
  DecodeState local_state{};
  DecodeState* st = state ? state : &local_state;
  MapT out;
  size_t pos = 0;
  while (true) {
    const auto res = next_container_field(packet, pos, field_num, WireMap, 3);
    if (!res.valid) return Error{"omnibuf: corrupt packet while reading WireMap"};
    if (!res.matched) return out;
    pos = res.next_pos;
    if (!decode_submsg_map_body<KeyWT>(out, res.body, decode_sub, st)) {
      return Error{"omnibuf: invalid WireMap submessage payload"};
    }
  }
}

[[nodiscard]] inline std::pair<uint64_t, bool> lookup_map_string_uint(
    std::span<const uint8_t> packet,
    uint32_t field_num,
    std::string_view target_key) {
  return lookup_scalar_map<WireString, WireUint, std::string, uint64_t>(packet, field_num,
                                                                        target_key);
}

[[nodiscard]] inline std::pair<std::map<std::string, uint64_t>, bool> read_map_string_uint(
    std::span<const uint8_t> packet,
    uint32_t field_num) {
  return read_scalar_map<WireString, WireUint, std::map<std::string, uint64_t>>(packet, field_num);
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

namespace detail {

template <typename AppendDirtyFn, typename FieldNumT>
[[nodiscard]] inline const char* invoke_append_dirty(AppendDirtyFn& fn,
                                                     std::vector<uint8_t>& dst,
                                                     FieldNumT fnum) {
  using Ret = decltype(fn(dst, fnum));
  if constexpr (std::is_void_v<Ret>) {
    fn(dst, fnum);
    return nullptr;
  } else if constexpr (std::is_same_v<Ret, const char*>) {
    return fn(dst, fnum);
  } else {
    auto res = fn(dst, fnum);
    if (!res) return "omnibuf: failed to encode dirty field";
    return nullptr;
  }
}

}  // namespace detail

// Zero-decode wire splicing supporting arbitrary field counts via multi-word dirty bitsets.
// When DeepValidate is true (try_splice_fields_into), validates Field #0 and inner container
// structure on every field in raw. When DeepValidate is false (splice_fields_into), skips
// untouched containers in O(1) via Prefix-Varint length prefixes while still rejecting
// top-level truncated tags/lengths and invalid Field #0 blocks.
namespace detail {

template <bool DeepValidate, typename AppendDirtyFn>
[[nodiscard]] inline Result<void> splice_fields_into_impl(std::vector<uint8_t>& dst,
                                                          std::span<const uint8_t> raw,
                                                          std::span<const uint64_t> dirty_words,
                                                          uint32_t max_field,
                                                          AppendDirtyFn&& append_dirty) {
  const size_t orig_dst_size = dst.size();
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
  if (n <= SIZE_MAX - 160 && dst.size() <= SIZE_MAX - (n + 160) &&
      dst.capacity() < dst.size() + n + 160) {
    dst.reserve(dst.size() + n + 160);
  }

  std::array<uint64_t, 4> inline_emitted{};
  std::vector<uint64_t> heap_emitted;
  std::span<uint64_t> emitted;
  if (dirty_words.size() <= inline_emitted.size()) {
    emitted = std::span<uint64_t>(inline_emitted.data(), dirty_words.size());
  } else {
    heap_emitted.assign(dirty_words.size(), 0);
    emitted = std::span<uint64_t>(heap_emitted);
  }

  auto is_dirty = [&](uint32_t fnum) noexcept -> bool {
    const size_t w = fnum >> 6;
    return w < dirty_words.size() && ((dirty_words[w] >> (fnum & 63u)) & 1u) != 0;
  };
  auto is_emitted = [&](uint32_t fnum) noexcept -> bool {
    const size_t w = fnum >> 6;
    return w < emitted.size() && ((emitted[w] >> (fnum & 63u)) & 1u) != 0;
  };
  auto mark_emitted = [&](uint32_t fnum) noexcept {
    const size_t w = fnum >> 6;
    if (w < emitted.size()) {
      emitted[w] |= (uint64_t{1} << (fnum & 63u));
    }
  };

  DecodeState val_state{};
  size_t pos = 0;
  size_t clean_start = 0;

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
        dst.resize(orig_dst_size);
        return Error{"omnibuf: corrupt or truncated tag in splice_fields_into"};
      }
      fnum = static_cast<uint32_t>(tag >> 4);
      wt = static_cast<uint8_t>(tag & 0x0Fu);
      pos += tn;
    }

    if (fnum == 0) {
      auto f0 = validate_field0(raw, pos, wt, 0, val_state);
      if (!f0) {
        dst.resize(orig_dst_size);
        return Error{f0.error()};
      }
      pos = f0.value();
    } else if constexpr (DeepValidate) {
      auto vr = validate_value(raw, pos, wt, 0, val_state);
      if (!vr) {
        dst.resize(orig_dst_size);
        return Error{vr.error()};
      }
      pos = vr.value();
    } else {
      const auto skip_res = skip_value_checked(raw, pos, wt);
      if (!skip_res.ok) {
        dst.resize(orig_dst_size);
        return Error{"omnibuf: corrupt or truncated field payload in splice_fields_into"};
      }
      pos = skip_res.next_pos;
    }
    const size_t field_end = pos;

    if (is_dirty(fnum)) {
      if (clean_start < field_start) {
        dst.insert(dst.end(), raw.begin() + static_cast<ptrdiff_t>(clean_start),
                   raw.begin() + static_cast<ptrdiff_t>(field_start));
      }
      if (!is_emitted(fnum)) {
        if constexpr (std::is_invocable_v<AppendDirtyFn&, std::vector<uint8_t>&, uint32_t>) {
          if (const char* err = invoke_append_dirty(append_dirty, dst, fnum)) {
            dst.resize(orig_dst_size);
            return Error{err};
          }
        } else {
          if (const char* err =
                  invoke_append_dirty(append_dirty, dst, static_cast<uint16_t>(fnum))) {
            dst.resize(orig_dst_size);
            return Error{err};
          }
        }
        mark_emitted(fnum);
      }
      clean_start = field_end;
    } else {
      mark_emitted(fnum);
    }
  }

  if (clean_start < n) {
    dst.insert(dst.end(), raw.begin() + static_cast<ptrdiff_t>(clean_start),
               raw.begin() + static_cast<ptrdiff_t>(n));
  }

  for (size_t w = 0; w < dirty_words.size(); ++w) {
    uint64_t rem = dirty_words[w] & ~emitted[w];
    if (w == 0) {
      rem &= ~uint64_t{1};
    }
    while (rem != 0) {
      const int bit_idx = std::countr_zero(rem);
      const uint32_t fnum = static_cast<uint32_t>((w << 6) + static_cast<size_t>(bit_idx));
      if (fnum > max_field) {
        break;
      }
      if constexpr (std::is_invocable_v<AppendDirtyFn&, std::vector<uint8_t>&, uint32_t>) {
        if (const char* err = invoke_append_dirty(append_dirty, dst, fnum)) {
          dst.resize(orig_dst_size);
          return Error{err};
        }
      } else {
        if (const char* err =
                invoke_append_dirty(append_dirty, dst, static_cast<uint16_t>(fnum))) {
          dst.resize(orig_dst_size);
          return Error{err};
        }
      }
      rem &= (rem - 1u);
    }
  }

  return Result<void>::success();
}

// Ordinal-indexed zero-decode wire splicer used by generated Opaque<T> classes so dirty_
// scales with dense field count (num_fields) rather than sparse maximum tag numbers.
template <bool DeepValidate, typename MapOrdFn, typename AppendOrdFn>
[[nodiscard]] inline Result<void> splice_fields_ordinal_into_impl(
    std::vector<uint8_t>& dst,
    std::span<const uint8_t> raw,
    std::span<const uint64_t> dirty_words,
    size_t num_fields,
    MapOrdFn&& map_fnum_to_ord,
    AppendOrdFn&& append_dirty_ord) {
  const size_t orig_dst_size = dst.size();
  const size_t n = raw.size();
  std::vector<uint8_t> raw_copy;
  if (n > 0 && dst.capacity() > 0) {
    const uintptr_t dst_start = reinterpret_cast<uintptr_t>(dst.data());
    const uintptr_t dst_end = dst_start + dst.capacity();
    const uintptr_t raw_start = reinterpret_cast<uintptr_t>(raw.data());
    const uintptr_t raw_end = raw_start + n;
    if (dst_start < raw_end && raw_start < dst_end) {
      raw_copy.assign(raw.begin(), raw.end());
      raw = raw_copy;
    }
  }
  if (n <= SIZE_MAX - 160 && dst.size() <= SIZE_MAX - (n + 160) &&
      dst.capacity() < dst.size() + n + 160) {
    dst.reserve(dst.size() + n + 160);
  }

  std::array<uint64_t, 4> inline_emitted{};
  std::vector<uint64_t> heap_emitted;
  std::span<uint64_t> emitted;
  if (dirty_words.size() <= inline_emitted.size()) {
    emitted = std::span<uint64_t>(inline_emitted.data(), dirty_words.size());
  } else {
    heap_emitted.assign(dirty_words.size(), 0);
    emitted = std::span<uint64_t>(heap_emitted);
  }

  auto is_dirty_ord = [&](size_t ord) noexcept -> bool {
    const size_t w = ord >> 6;
    return w < dirty_words.size() && ((dirty_words[w] >> (ord & 63u)) & 1u) != 0;
  };
  auto is_emitted_ord = [&](size_t ord) noexcept -> bool {
    const size_t w = ord >> 6;
    return w < emitted.size() && ((emitted[w] >> (ord & 63u)) & 1u) != 0;
  };
  auto mark_emitted_ord = [&](size_t ord) noexcept {
    const size_t w = ord >> 6;
    if (w < emitted.size()) {
      emitted[w] |= (uint64_t{1} << (ord & 63u));
    }
  };

  DecodeState val_state{};
  size_t pos = 0;
  size_t clean_start = 0;

  while (pos < n) {
    const size_t field_start = pos;
    uint32_t fnum = 0;
    uint8_t wt = 0;
    const uint8_t b0 = raw[pos];
    if ((b0 & 1u) != 0) [[likely]] {
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
        dst.resize(orig_dst_size);
        return Error{"omnibuf: corrupt or truncated tag in splice_fields_into"};
      }
      fnum = static_cast<uint32_t>(tag >> 4);
      wt = static_cast<uint8_t>(tag & 0x0Fu);
      pos += tn;
    }

    if (fnum == 0) {
      if constexpr (DeepValidate) {
        auto f0 = validate_field0(raw, pos, wt, 0, val_state);
        if (!f0) {
          dst.resize(orig_dst_size);
          return Error{f0.error()};
        }
        pos = f0.value();
      } else {
        if (wt != WireSubMsg) {
          dst.resize(orig_dst_size);
          return Error{"omnibuf: invalid Field #0 wire type"};
        }
        const auto sk = fast_skip_value(raw, pos, wt);
        if (!sk.ok) {
          dst.resize(orig_dst_size);
          return Error{"omnibuf: truncated Field #0"};
        }
        pos = sk.next_pos;
      }
    } else if constexpr (DeepValidate) {
      auto vr = validate_value(raw, pos, wt, 0, val_state);
      if (!vr) {
        dst.resize(orig_dst_size);
        return Error{vr.error()};
      }
      pos = vr.value();
    } else {
      const auto skip_res = fast_skip_value(raw, pos, wt);
      if (!skip_res.ok) {
        dst.resize(orig_dst_size);
        return Error{"omnibuf: corrupt or truncated field payload in splice_fields_into"};
      }
      pos = skip_res.next_pos;
    }
    const size_t field_end = pos;

    const int ord_i = map_fnum_to_ord(fnum);
    if (ord_i >= 0) {
      const size_t ord = static_cast<size_t>(ord_i);
      if (is_dirty_ord(ord)) {
        if (clean_start < field_start) {
          dst.insert(dst.end(), raw.begin() + static_cast<ptrdiff_t>(clean_start),
                     raw.begin() + static_cast<ptrdiff_t>(field_start));
        }
        if (!is_emitted_ord(ord)) {
          if (const char* err = invoke_append_dirty(append_dirty_ord, dst, ord)) {
            dst.resize(orig_dst_size);
            return Error{err};
          }
          mark_emitted_ord(ord);
        }
        clean_start = field_end;
      } else {
        mark_emitted_ord(ord);
      }
    }
  }

  if (clean_start < n) {
    dst.insert(dst.end(), raw.begin() + static_cast<ptrdiff_t>(clean_start),
               raw.begin() + static_cast<ptrdiff_t>(n));
  }

  for (size_t w = 0; w < dirty_words.size(); ++w) {
    uint64_t rem = dirty_words[w] & ~emitted[w];
    while (rem != 0) {
      const int bit_idx = std::countr_zero(rem);
      const size_t ord = (w << 6) + static_cast<size_t>(bit_idx);
      if (ord >= num_fields) {
        break;
      }
      if (const char* err = invoke_append_dirty(append_dirty_ord, dst, ord)) {
        dst.resize(orig_dst_size);
        return Error{err};
      }
      rem &= (rem - 1u);
    }
  }

  return Result<void>::success();
}

}  // namespace detail

template <typename AppendDirtyFn>
[[nodiscard]] inline Result<void> try_splice_fields_into(std::vector<uint8_t>& dst,
                                                         std::span<const uint8_t> raw,
                                                         std::span<const uint64_t> dirty_words,
                                                         uint32_t max_field,
                                                         AppendDirtyFn&& append_dirty) {
  return detail::splice_fields_into_impl<true>(dst, raw, dirty_words, max_field,
                                               std::forward<AppendDirtyFn>(append_dirty));
}

template <typename AppendDirtyFn>
[[nodiscard]] inline Result<void> try_splice_fields_into(std::vector<uint8_t>& dst,
                                                         std::span<const uint8_t> raw,
                                                         uint64_t dirty_mask,
                                                         uint16_t max_field,
                                                         AppendDirtyFn&& append_dirty) {
  const std::array<uint64_t, 1> words{dirty_mask};
  return try_splice_fields_into(dst, raw, std::span<const uint64_t>(words), max_field,
                                std::forward<AppendDirtyFn>(append_dirty));
}

template <typename AppendDirtyFn>
inline void splice_fields_into(std::vector<uint8_t>& dst,
                               std::span<const uint8_t> raw,
                               std::span<const uint64_t> dirty_words,
                               uint32_t max_field,
                               AppendDirtyFn&& append_dirty) {
  auto res = detail::splice_fields_into_impl<false>(dst, raw, dirty_words, max_field,
                                                    std::forward<AppendDirtyFn>(append_dirty));
  if (!res) {
    throw std::invalid_argument(res.error());
  }
}

template <typename AppendDirtyFn>
inline void splice_fields_into(std::vector<uint8_t>& dst,
                               std::span<const uint8_t> raw,
                               uint64_t dirty_mask,
                               uint16_t max_field,
                               AppendDirtyFn&& append_dirty) {
  const std::array<uint64_t, 1> words{dirty_mask};
  splice_fields_into(dst, raw, std::span<const uint64_t>(words), max_field,
                     std::forward<AppendDirtyFn>(append_dirty));
}

template <typename AppendDirtyFn>
[[nodiscard]] inline Result<std::vector<uint8_t>> try_splice_fields(
    std::vector<uint8_t> dst,
    std::span<const uint8_t> raw,
    std::span<const uint64_t> dirty_words,
    uint32_t max_field,
    AppendDirtyFn&& append_dirty) {
  auto res = try_splice_fields_into(dst, raw, dirty_words, max_field,
                                    std::forward<AppendDirtyFn>(append_dirty));
  if (!res) return Error{res.error()};
  return dst;
}

template <typename AppendDirtyFn>
[[nodiscard]] inline Result<std::vector<uint8_t>> try_splice_fields(
    std::vector<uint8_t> dst,
    std::span<const uint8_t> raw,
    uint64_t dirty_mask,
    uint16_t max_field,
    AppendDirtyFn&& append_dirty) {
  auto res = try_splice_fields_into(dst, raw, dirty_mask, max_field,
                                    std::forward<AppendDirtyFn>(append_dirty));
  if (!res) return Error{res.error()};
  return dst;
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
// bounds-checks prefix_len <= dst_backing.size(), preserves prefix bytes from dst_backing,
// and never mutates raw inside dst_backing.
template <typename AppendDirtyFn>
[[nodiscard]] inline Result<std::vector<uint8_t>> try_splice_fields(
    std::span<uint8_t> dst_backing,
    size_t prefix_len,
    std::span<const uint8_t> raw,
    uint64_t dirty_mask,
    uint16_t max_field,
    AppendDirtyFn&& append_dirty) {
  if (prefix_len > dst_backing.size()) {
    return Error{"omnibuf: prefix_len exceeds dst_backing size"};
  }
  std::vector<uint8_t> out(dst_backing.begin(),
                           dst_backing.begin() + static_cast<ptrdiff_t>(prefix_len));
  auto res = try_splice_fields_into(out, raw, dirty_mask, max_field,
                                    std::forward<AppendDirtyFn>(append_dirty));
  if (!res) return Error{res.error()};
  return out;
}

template <typename AppendDirtyFn>
[[nodiscard]] inline std::vector<uint8_t> splice_fields(std::span<uint8_t> dst_backing,
                                                        size_t prefix_len,
                                                        std::span<const uint8_t> raw,
                                                        uint64_t dirty_mask,
                                                        uint16_t max_field,
                                                        AppendDirtyFn&& append_dirty) {
  if (prefix_len > dst_backing.size()) {
    throw std::out_of_range("omnibuf: prefix_len exceeds dst_backing size");
  }
  std::vector<uint8_t> out(dst_backing.begin(),
                           dst_backing.begin() + static_cast<ptrdiff_t>(prefix_len));
  splice_fields_into(out, raw, dirty_mask, max_field, std::forward<AppendDirtyFn>(append_dirty));
  return out;
}

}  // namespace omnibuf
