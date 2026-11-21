#ifdef __APPLE__
#include <sys/sysctl.h>
#endif

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <optional>
#include <span>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "omnibuf/bench/peer.omnibuf.pb.hpp"
#include "omnibuf/omnibuf.hpp"
#ifdef OMNIBUF_HAS_LIBPROTOBUF
#include "libprotobuf_peer.hpp"
#endif

// RegisterStruct traits for omnibuf::bench::{Location, Neighbor, PeerUpdate}
// so OmniBuf (C++ Struct) benchmarks StructTraits<T> reflection on the exact same data model.
template <>
struct omnibuf::StructTraits<omnibuf::bench::Location> {
  static constexpr std::string_view type_name = "omnibuf.bench.Location";
  static constexpr auto fields() {
    using T = omnibuf::bench::Location;
    return std::make_tuple(
        omnibuf::field<1, &T::region>("region"),
        omnibuf::field<2, &T::datacenter>("datacenter"),
        omnibuf::field<3, &T::city>("city"),
        omnibuf::field<4, &T::rack>("rack"),
        omnibuf::field<5, &T::latitude>("latitude"),
        omnibuf::field<6, &T::longitude>("longitude"));
  }
};

template <>
struct omnibuf::StructTraits<omnibuf::bench::Neighbor> {
  static constexpr std::string_view type_name = "omnibuf.bench.Neighbor";
  static constexpr auto fields() {
    using T = omnibuf::bench::Neighbor;
    return std::make_tuple(
        omnibuf::field<1, &T::peer_asn>("peer_asn"),
        omnibuf::field<2, &T::router>("router"),
        omnibuf::field<3, &T::latency_us>("latency_us"),
        omnibuf::field<4, &T::established>("established"));
  }
};

template <>
struct omnibuf::StructTraits<omnibuf::bench::PeerUpdate> {
  static constexpr std::string_view type_name = "omnibuf.bench.PeerUpdate";
  static constexpr auto fields() {
    using T = omnibuf::bench::PeerUpdate;
    return std::make_tuple(
        omnibuf::field<1, &T::asn>("asn"),
        omnibuf::field<2, &T::router_id>("router_id"),
        omnibuf::field<3, &T::active>("active"),
        omnibuf::field<4, &T::uptime_sec>("uptime_sec"),
        omnibuf::field<5, &T::rssi_dbm, omnibuf::WireOverride::Sint>("rssi_dbm"),
        omnibuf::field<6, &T::cpu_load>("cpu_load"),
        omnibuf::field<7, &T::tags>("tags"),
        omnibuf::field<8, &T::prefixes>("prefixes"),
        omnibuf::field<9, &T::location>("location"),
        omnibuf::field<10, &T::neighbors>("neighbors"),
        omnibuf::field<11, &T::sequence>("sequence"));
  }
};

namespace {

using omnibuf::bench::Location;
using omnibuf::bench::Neighbor;
using omnibuf::bench::OpaquePeerUpdate;
using omnibuf::bench::PeerUpdate;

template <typename T>
inline void do_not_optimize(const T& value) {
#if defined(__GNUC__) || defined(__clang__)
  asm volatile("" : : "r,m"(value) : "memory");
#else
  volatile const T* sink = &value;
  (void)sink;
#endif
}

inline void sink_bytes(const std::vector<uint8_t>& out) {
  const uint8_t* ptr = out.data();
  const size_t sz = out.size();
  do_not_optimize(ptr);
  do_not_optimize(sz);
}

#ifdef OMNIBUF_HAS_LIBPROTOBUF
inline void sink_string(const std::string& out) {
  const char* ptr = out.data();
  const size_t sz = out.size();
  do_not_optimize(ptr);
  do_not_optimize(sz);
}
#endif

inline void clobber_memory() {
#if defined(__GNUC__) || defined(__clang__)
  asm volatile("" : : : "memory");
#endif
}

inline uint64_t mix_u64(uint64_t h, uint64_t v) noexcept {
  h ^= v + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
  return h;
}

inline uint64_t mix_str(uint64_t h, std::string_view s) noexcept {
  h = mix_u64(h, s.size());
  uint64_t fnv = 14695981039346656037ULL;
  for (unsigned char c : s) {
    fnv ^= static_cast<uint64_t>(c);
    fnv *= 1099511628211ULL;
  }
  return mix_u64(h, fnv);
}

// Folds all 11 PeerUpdate fields (including every nested Location and Neighbor field)
// into a single 64-bit digest so -O3 cannot dead-code-eliminate unread decoded fields.
uint64_t compute_peer_checksum(const PeerUpdate& p) noexcept {
  uint64_t h = 0xcbf29ce484222325ULL;
  h = mix_u64(h, p.asn);
  h = mix_str(h, p.router_id);
  h = mix_u64(h, p.active ? 1ULL : 0ULL);
  h = mix_u64(h, p.uptime_sec);
  h = mix_u64(h, static_cast<uint64_t>(static_cast<int64_t>(p.rssi_dbm)));
  h = mix_u64(h, omnibuf::float64_to_bits(p.cpu_load));

  h = mix_u64(h, p.tags.size());
  for (const auto& t : p.tags) {
    h = mix_str(h, t);
  }

  h = mix_u64(h, p.prefixes.size());
  for (uint32_t pref : p.prefixes) {
    h = mix_u64(h, pref);
  }

  h = mix_u64(h, p.location.has_value() ? 1ULL : 0ULL);
  if (p.location.has_value()) {
    const auto& loc = *p.location;
    h = mix_str(h, loc.region);
    h = mix_str(h, loc.datacenter);
    h = mix_str(h, loc.city);
    h = mix_u64(h, loc.rack);
    h = mix_u64(h, omnibuf::float32_to_bits(loc.latitude));
    h = mix_u64(h, omnibuf::float32_to_bits(loc.longitude));
  }

  h = mix_u64(h, p.neighbors.size());
  for (const auto& nb : p.neighbors) {
    h = mix_u64(h, nb.peer_asn);
    h = mix_str(h, nb.router);
    h = mix_u64(h, nb.latency_us);
    h = mix_u64(h, nb.established ? 1ULL : 0ULL);
  }

  h = mix_u64(h, p.sequence);
  return h;
}

// Lightweight O(1) sink over all 11 PeerUpdate fields so -O3 cannot dead-code-eliminate
// decoded fields without adding ~150 ns of byte-by-byte FNV-1a hashing to the timed loop.
inline uint64_t digest_peer_light(const PeerUpdate& p) noexcept {
  uint64_t d = static_cast<uint64_t>(p.asn) ^ p.router_id.size() ^
               (p.active ? 1ULL : 0ULL) ^ p.uptime_sec ^
               static_cast<uint64_t>(static_cast<int64_t>(p.rssi_dbm)) ^
               omnibuf::float64_to_bits(p.cpu_load) ^ p.tags.size() ^ p.prefixes.size() ^
               p.neighbors.size() ^ p.sequence;
  if (p.location.has_value()) {
    const auto& loc = *p.location;
    d ^= loc.region.size() ^ loc.datacenter.size() ^ loc.city.size() ^
         static_cast<uint64_t>(loc.rack);
  }
  return d;
}

PeerUpdate create_sample_peer_update() {
  PeerUpdate p;
  p.asn = 64512;
  p.router_id = "edge-rtr-fra01.core.backbone.net";
  p.active = true;
  p.uptime_sec = 1849203;
  p.rssi_dbm = -62;
  p.cpu_load = 0.42875;
  p.tags = {"tier1", "ebgp", "eu-central", "ixp-de-cix"};
  p.prefixes = {167772160u, 172032000u, 3232235520u, 2886729728u, 134744072u, 134743040u};
  p.location = Location{
      .region = "eu-central-1",
      .datacenter = "FRA-Equinix-FR5",
      .city = "Frankfurt",
      .rack = 42,
      .latitude = 50.1109f,
      .longitude = 8.6821f,
  };
  p.neighbors = {
      Neighbor{.peer_asn = 3356, .router = "ae0.fra1.level3.net", .latency_us = 850, .established = true},
      Neighbor{.peer_asn = 1299, .router = "ffm-b5-link.telia.net", .latency_us = 920, .established = true},
      Neighbor{.peer_asn = 174,
               .router = "be2040.ccr41.fra03.atlas.cogentco.com",
               .latency_us = 1120,
               .established = true},
      Neighbor{.peer_asn = 2914,
               .router = "ae-12.r24.frnkge08.de.bb.gin.ntt.net",
               .latency_us = 980,
               .established = false},
  };
  p.sequence = 9876543210ULL;
  return p;
}

PeerUpdate create_peer_update_32_neighbors() {
  PeerUpdate p = create_sample_peer_update();
  const std::vector<Neighbor> base = p.neighbors;
  p.neighbors.clear();
  p.neighbors.reserve(base.size() * 8);
  for (int rep = 0; rep < 8; ++rep) {
    for (const auto& nb : base) {
      p.neighbors.push_back(nb);
    }
  }
  return p;
}

// ============================================================================
// Protobuf LEB128 Wire Codec (proto3 canonical encoding for PeerUpdate, 341B)
// Uses symmetric FastWriter buffer growth and strict UTF-8 string validation.
// ============================================================================

namespace pb_leb128 {

inline void write_leb128(omnibuf::FastWriter& w, uint64_t v) {
  w.ensure(10);
  uint8_t* ptr = w.raw_cur();
  while (v >= 0x80u) {
    *ptr++ = static_cast<uint8_t>((v & 0x7Fu) | 0x80u);
    v >>= 7;
  }
  *ptr++ = static_cast<uint8_t>(v);
  w.set_raw_cur(ptr);
}

inline size_t leb128_size(uint64_t v) noexcept {
  size_t n = 1;
  while (v >= 0x80u) {
    v >>= 7;
    ++n;
  }
  return n;
}

inline uint32_t zigzag32(int32_t v) noexcept {
  return (static_cast<uint32_t>(v) << 1) ^ static_cast<uint32_t>(v >> 31);
}

inline int32_t unzigzag32(uint32_t v) noexcept {
  return static_cast<int32_t>((v >> 1) ^ (~(v & 1u) + 1u));
}

inline bool write_str_field(omnibuf::FastWriter& w, uint32_t fnum, std::string_view s) {
  if (!omnibuf::is_valid_utf8(s)) [[unlikely]] {
    return false;
  }
  if (s.empty()) return true;
  write_leb128(w, (static_cast<uint64_t>(fnum) << 3) | 2u);
  write_leb128(w, s.size());
  w.write_bytes(s.data(), s.size());
  return true;
}

inline size_t location_body_size(const Location& loc) noexcept {
  size_t sz = 0;
  if (!loc.region.empty()) sz += 1 + leb128_size(loc.region.size()) + loc.region.size();
  if (!loc.datacenter.empty()) sz += 1 + leb128_size(loc.datacenter.size()) + loc.datacenter.size();
  if (!loc.city.empty()) sz += 1 + leb128_size(loc.city.size()) + loc.city.size();
  if (loc.rack != 0) sz += 1 + leb128_size(loc.rack);
  if (omnibuf::float32_to_bits(loc.latitude) != 0) sz += 1 + 4;
  if (omnibuf::float32_to_bits(loc.longitude) != 0) sz += 1 + 4;
  return sz;
}

inline size_t neighbor_body_size(const Neighbor& nb) noexcept {
  size_t sz = 0;
  if (nb.peer_asn != 0) sz += 1 + leb128_size(nb.peer_asn);
  if (!nb.router.empty()) sz += 1 + leb128_size(nb.router.size()) + nb.router.size();
  if (nb.latency_us != 0) sz += 1 + leb128_size(nb.latency_us);
  if (nb.established) sz += 2;
  return sz;
}

std::vector<uint8_t> encode_peer_update(const PeerUpdate& msg) {
  std::vector<uint8_t> out;
  omnibuf::FastWriter w(out, 512);

  if (msg.asn != 0) {
    write_leb128(w, (1ULL << 3) | 0u);
    write_leb128(w, msg.asn);
  }
  if (!write_str_field(w, 2, msg.router_id)) {
    throw std::invalid_argument("pb_leb128: invalid UTF-8");
  }
  if (msg.active) {
    w.write_byte((3u << 3) | 0u);
    w.write_byte(1u);
  }
  if (msg.uptime_sec != 0) {
    write_leb128(w, (4ULL << 3) | 0u);
    write_leb128(w, msg.uptime_sec);
  }
  if (msg.rssi_dbm != 0) {
    write_leb128(w, (5ULL << 3) | 0u);
    write_leb128(w, zigzag32(msg.rssi_dbm));
  }
  if (omnibuf::float64_to_bits(msg.cpu_load) != 0) {
    w.write_byte((6u << 3) | 1u);
    w.write_u64_raw(omnibuf::float64_to_bits(msg.cpu_load));
  }
  for (const auto& tag : msg.tags) {
    if (!omnibuf::is_valid_utf8(tag)) [[unlikely]] {
      throw std::invalid_argument("pb_leb128: invalid UTF-8");
    }
    write_leb128(w, (7ULL << 3) | 2u);
    write_leb128(w, tag.size());
    w.write_bytes(tag.data(), tag.size());
  }
  if (!msg.prefixes.empty()) {
    size_t packed_len = 0;
    for (uint32_t p : msg.prefixes) {
      packed_len += leb128_size(p);
    }
    write_leb128(w, (8ULL << 3) | 2u);
    write_leb128(w, packed_len);
    for (uint32_t p : msg.prefixes) {
      write_leb128(w, p);
    }
  }
  if (msg.location.has_value()) {
    const auto& loc = *msg.location;
    const size_t loc_sz = location_body_size(loc);
    write_leb128(w, (9ULL << 3) | 2u);
    write_leb128(w, loc_sz);
    if (!write_str_field(w, 1, loc.region) ||
        !write_str_field(w, 2, loc.datacenter) ||
        !write_str_field(w, 3, loc.city)) {
      throw std::invalid_argument("pb_leb128: invalid UTF-8");
    }
    if (loc.rack != 0) {
      write_leb128(w, (4ULL << 3) | 0u);
      write_leb128(w, loc.rack);
    }
    if (omnibuf::float32_to_bits(loc.latitude) != 0) {
      w.write_byte((5u << 3) | 5u);
      w.write_u32_raw(omnibuf::float32_to_bits(loc.latitude));
    }
    if (omnibuf::float32_to_bits(loc.longitude) != 0) {
      w.write_byte((6u << 3) | 5u);
      w.write_u32_raw(omnibuf::float32_to_bits(loc.longitude));
    }
  }
  for (const auto& nb : msg.neighbors) {
    const size_t nb_sz = neighbor_body_size(nb);
    write_leb128(w, (10ULL << 3) | 2u);
    write_leb128(w, nb_sz);
    if (nb.peer_asn != 0) {
      write_leb128(w, (1ULL << 3) | 0u);
      write_leb128(w, nb.peer_asn);
    }
    if (!write_str_field(w, 2, nb.router)) {
      throw std::invalid_argument("pb_leb128: invalid UTF-8");
    }
    if (nb.latency_us != 0) {
      write_leb128(w, (3ULL << 3) | 0u);
      write_leb128(w, nb.latency_us);
    }
    if (nb.established) {
      w.write_byte((4u << 3) | 0u);
      w.write_byte(1u);
    }
  }
  if (msg.sequence != 0) {
    write_leb128(w, (11ULL << 3) | 0u);
    write_leb128(w, msg.sequence);
  }

  w.finish();
  return out;
}

inline bool read_leb128(std::span<const uint8_t> b, size_t& pos, uint64_t& out) noexcept {
  if (pos < b.size() && (b[pos] & 0x80u) == 0) [[likely]] {
    out = b[pos++];
    return true;
  }
  uint64_t result = 0;
  uint32_t shift = 0;
  while (pos < b.size()) {
    if (shift >= 64) return false;
    const uint8_t byte = b[pos++];
    result |= static_cast<uint64_t>(byte & 0x7Fu) << shift;
    if ((byte & 0x80u) == 0) {
      out = result;
      return true;
    }
    shift += 7;
  }
  return false;
}

inline bool skip_pb_wire(std::span<const uint8_t> b, size_t& pos, uint32_t wt) noexcept {
  uint64_t tmp = 0;
  switch (wt) {
    case 0:
      return read_leb128(b, pos, tmp);
    case 1:
      if (b.size() - pos < 8) return false;
      pos += 8;
      return true;
    case 2:
      if (!read_leb128(b, pos, tmp) || tmp > b.size() - pos) return false;
      pos += static_cast<size_t>(tmp);
      return true;
    case 5:
      if (b.size() - pos < 4) return false;
      pos += 4;
      return true;
    default:
      return false;
  }
}

bool decode_location(std::span<const uint8_t> b, Location& loc) {
  size_t pos = 0;
  while (pos < b.size()) {
    uint64_t tag = 0;
    if (!read_leb128(b, pos, tag)) return false;
    const uint32_t fnum = static_cast<uint32_t>(tag >> 3);
    const uint32_t wt = static_cast<uint32_t>(tag & 7u);
    if ((fnum == 1 || fnum == 2 || fnum == 3) && wt == 2) {
      uint64_t len = 0;
      if (!read_leb128(b, pos, len) || len > b.size() - pos) return false;
      const size_t n = static_cast<size_t>(len);
      if (!omnibuf::is_valid_utf8(b.subspan(pos, n))) return false;
      const char* ptr = reinterpret_cast<const char*>(b.data() + pos);
      if (fnum == 1) loc.region.assign(ptr, n);
      else if (fnum == 2) loc.datacenter.assign(ptr, n);
      else loc.city.assign(ptr, n);
      pos += n;
    } else if (fnum == 4 && wt == 0) {
      uint64_t v = 0;
      if (!read_leb128(b, pos, v)) return false;
      loc.rack = static_cast<uint32_t>(v);
    } else if ((fnum == 5 || fnum == 6) && wt == 5) {
      if (b.size() - pos < 4) return false;
      const float fv = omnibuf::bits_to_float32(omnibuf::read_u32_le(b.data() + pos));
      if (fnum == 5) loc.latitude = fv;
      else loc.longitude = fv;
      pos += 4;
    } else if (!skip_pb_wire(b, pos, wt)) {
      return false;
    }
  }
  return true;
}

bool decode_neighbor(std::span<const uint8_t> b, Neighbor& nb) {
  size_t pos = 0;
  while (pos < b.size()) {
    uint64_t tag = 0;
    if (!read_leb128(b, pos, tag)) return false;
    const uint32_t fnum = static_cast<uint32_t>(tag >> 3);
    const uint32_t wt = static_cast<uint32_t>(tag & 7u);
    if (fnum == 1 && wt == 0) {
      uint64_t v = 0;
      if (!read_leb128(b, pos, v)) return false;
      nb.peer_asn = static_cast<uint32_t>(v);
    } else if (fnum == 2 && wt == 2) {
      uint64_t len = 0;
      if (!read_leb128(b, pos, len) || len > b.size() - pos) return false;
      const size_t n = static_cast<size_t>(len);
      if (!omnibuf::is_valid_utf8(b.subspan(pos, n))) return false;
      nb.router.assign(reinterpret_cast<const char*>(b.data() + pos), n);
      pos += n;
    } else if (fnum == 3 && wt == 0) {
      uint64_t v = 0;
      if (!read_leb128(b, pos, v)) return false;
      nb.latency_us = static_cast<uint32_t>(v);
    } else if (fnum == 4 && wt == 0) {
      uint64_t v = 0;
      if (!read_leb128(b, pos, v)) return false;
      nb.established = (v != 0);
    } else if (!skip_pb_wire(b, pos, wt)) {
      return false;
    }
  }
  return true;
}

bool decode_peer_update(std::span<const uint8_t> b, PeerUpdate& out) {
  size_t pos = 0;
  while (pos < b.size()) {
    uint64_t tag = 0;
    if (!read_leb128(b, pos, tag)) return false;
    const uint32_t fnum = static_cast<uint32_t>(tag >> 3);
    const uint32_t wt = static_cast<uint32_t>(tag & 7u);
    switch (fnum) {
      case 1: {
        uint64_t v = 0;
        if (wt != 0 || !read_leb128(b, pos, v)) return false;
        out.asn = static_cast<uint32_t>(v);
        break;
      }
      case 2: {
        uint64_t len = 0;
        if (wt != 2 || !read_leb128(b, pos, len) || len > b.size() - pos) return false;
        const size_t n = static_cast<size_t>(len);
        if (!omnibuf::is_valid_utf8(b.subspan(pos, n))) return false;
        out.router_id.assign(reinterpret_cast<const char*>(b.data() + pos), n);
        pos += n;
        break;
      }
      case 3: {
        uint64_t v = 0;
        if (wt != 0 || !read_leb128(b, pos, v)) return false;
        out.active = (v != 0);
        break;
      }
      case 4: {
        uint64_t v = 0;
        if (wt != 0 || !read_leb128(b, pos, v)) return false;
        out.uptime_sec = v;
        break;
      }
      case 5: {
        uint64_t v = 0;
        if (wt != 0 || !read_leb128(b, pos, v)) return false;
        out.rssi_dbm = unzigzag32(static_cast<uint32_t>(v));
        break;
      }
      case 6: {
        if (wt != 1 || b.size() - pos < 8) return false;
        out.cpu_load = omnibuf::bits_to_float64(omnibuf::read_u64_le(b.data() + pos));
        pos += 8;
        break;
      }
      case 7: {
        uint64_t len = 0;
        if (wt != 2 || !read_leb128(b, pos, len) || len > b.size() - pos) return false;
        const size_t n = static_cast<size_t>(len);
        if (!omnibuf::is_valid_utf8(b.subspan(pos, n))) return false;
        out.tags.emplace_back(reinterpret_cast<const char*>(b.data() + pos), n);
        pos += n;
        break;
      }
      case 8: {
        if (wt == 2) {
          uint64_t len = 0;
          if (!read_leb128(b, pos, len) || len > b.size() - pos) return false;
          const size_t end = pos + static_cast<size_t>(len);
          while (pos < end) {
            uint64_t pv = 0;
            if (!read_leb128(b.subspan(0, end), pos, pv)) return false;
            out.prefixes.push_back(static_cast<uint32_t>(pv));
          }
        } else if (wt == 0) {
          uint64_t pv = 0;
          if (!read_leb128(b, pos, pv)) return false;
          out.prefixes.push_back(static_cast<uint32_t>(pv));
        } else {
          return false;
        }
        break;
      }
      case 9: {
        uint64_t len = 0;
        if (wt != 2 || !read_leb128(b, pos, len) || len > b.size() - pos) return false;
        if (!out.location.has_value()) out.location.emplace();
        if (!decode_location(b.subspan(pos, static_cast<size_t>(len)), *out.location)) return false;
        pos += static_cast<size_t>(len);
        break;
      }
      case 10: {
        uint64_t len = 0;
        if (wt != 2 || !read_leb128(b, pos, len) || len > b.size() - pos) return false;
        Neighbor nb;
        if (!decode_neighbor(b.subspan(pos, static_cast<size_t>(len)), nb)) return false;
        out.neighbors.push_back(std::move(nb));
        pos += static_cast<size_t>(len);
        break;
      }
      case 11: {
        uint64_t v = 0;
        if (wt != 0 || !read_leb128(b, pos, v)) return false;
        out.sequence = v;
        break;
      }
      default:
        if (!skip_pb_wire(b, pos, wt)) return false;
        break;
    }
  }
  return true;
}

}  // namespace pb_leb128

// ============================================================================
// Self-Contained C++20 RFC 8259 JSON Codec for PeerUpdate
// Uses symmetric FastWriter(out, 512) buffer growth and UTF-8 validation.
// ============================================================================

namespace json_codec {

inline void write_lit(omnibuf::FastWriter& w, std::string_view lit) {
  w.write_bytes(lit.data(), lit.size());
}

inline void write_u64(omnibuf::FastWriter& w, uint64_t v) {
  w.ensure(24);
  char* cur = reinterpret_cast<char*>(w.raw_cur());
  const auto [ptr, ec] = std::to_chars(cur, cur + 24, v);
  if (ec == std::errc{}) {
    w.set_raw_cur(reinterpret_cast<uint8_t*>(ptr));
  }
}

inline void write_i32(omnibuf::FastWriter& w, int32_t v) {
  w.ensure(16);
  char* cur = reinterpret_cast<char*>(w.raw_cur());
  const auto [ptr, ec] = std::to_chars(cur, cur + 16, v);
  if (ec == std::errc{}) {
    w.set_raw_cur(reinterpret_cast<uint8_t*>(ptr));
  }
}

inline void write_f32(omnibuf::FastWriter& w, float v) {
  w.ensure(32);
  char* cur = reinterpret_cast<char*>(w.raw_cur());
  const auto [ptr, ec] = std::to_chars(cur, cur + 32, v);
  if (ec == std::errc{}) {
    w.set_raw_cur(reinterpret_cast<uint8_t*>(ptr));
  }
}

inline void write_f64(omnibuf::FastWriter& w, double v) {
  w.ensure(32);
  char* cur = reinterpret_cast<char*>(w.raw_cur());
  const auto [ptr, ec] = std::to_chars(cur, cur + 32, v);
  if (ec == std::errc{}) {
    w.set_raw_cur(reinterpret_cast<uint8_t*>(ptr));
  }
}

inline void write_json_str(omnibuf::FastWriter& w, std::string_view s) {
  if (!omnibuf::is_valid_utf8(s)) [[unlikely]] {
    throw std::invalid_argument("json_codec: invalid UTF-8");
  }
  bool needs_escape = false;
  for (unsigned char c : s) {
    if (c < 0x20u || c == '"' || c == '\\') {
      needs_escape = true;
      break;
    }
  }
  if (!needs_escape) [[likely]] {
    w.ensure(s.size() + 2);
    uint8_t* ptr = w.raw_cur();
    *ptr++ = '"';
    if (!s.empty()) {
      std::memcpy(ptr, s.data(), s.size());
      ptr += s.size();
    }
    *ptr++ = '"';
    w.set_raw_cur(ptr);
    return;
  }
  static constexpr char kHex[] = "0123456789abcdef";
  w.write_byte('"');
  for (unsigned char c : s) {
    switch (c) {
      case '"': write_lit(w, "\\\""); break;
      case '\\': write_lit(w, "\\\\"); break;
      case '\b': write_lit(w, "\\b"); break;
      case '\f': write_lit(w, "\\f"); break;
      case '\n': write_lit(w, "\\n"); break;
      case '\r': write_lit(w, "\\r"); break;
      case '\t': write_lit(w, "\\t"); break;
      default:
        if (c < 0x20u) {
          char esc[6] = {'\\', 'u', '0', '0', kHex[(c >> 4) & 0x0Fu], kHex[c & 0x0Fu]};
          w.write_bytes(esc, 6);
        } else {
          w.write_byte(c);
        }
        break;
    }
  }
  w.write_byte('"');
}

std::vector<uint8_t> encode_peer_update(const PeerUpdate& p) {
  std::vector<uint8_t> out;
  omnibuf::FastWriter w(out, 512);
  write_lit(w, "{\"asn\":");
  write_u64(w, p.asn);
  write_lit(w, ",\"router_id\":");
  write_json_str(w, p.router_id);
  write_lit(w, ",\"active\":");
  write_lit(w, p.active ? "true" : "false");
  write_lit(w, ",\"uptime_sec\":");
  write_u64(w, p.uptime_sec);
  write_lit(w, ",\"rssi_dbm\":");
  write_i32(w, p.rssi_dbm);
  write_lit(w, ",\"cpu_load\":");
  write_f64(w, p.cpu_load);
  write_lit(w, ",\"tags\":[");
  for (size_t i = 0; i < p.tags.size(); ++i) {
    if (i > 0) w.write_byte(',');
    write_json_str(w, p.tags[i]);
  }
  write_lit(w, "],\"prefixes\":[");
  for (size_t i = 0; i < p.prefixes.size(); ++i) {
    if (i > 0) w.write_byte(',');
    write_u64(w, p.prefixes[i]);
  }
  w.write_byte(']');
  if (p.location.has_value()) {
    const auto& loc = *p.location;
    write_lit(w, ",\"location\":{\"region\":");
    write_json_str(w, loc.region);
    write_lit(w, ",\"datacenter\":");
    write_json_str(w, loc.datacenter);
    write_lit(w, ",\"city\":");
    write_json_str(w, loc.city);
    write_lit(w, ",\"rack\":");
    write_u64(w, loc.rack);
    write_lit(w, ",\"latitude\":");
    write_f32(w, loc.latitude);
    write_lit(w, ",\"longitude\":");
    write_f32(w, loc.longitude);
    w.write_byte('}');
  }
  write_lit(w, ",\"neighbors\":[");
  for (size_t i = 0; i < p.neighbors.size(); ++i) {
    if (i > 0) w.write_byte(',');
    const auto& nb = p.neighbors[i];
    write_lit(w, "{\"peer_asn\":");
    write_u64(w, nb.peer_asn);
    write_lit(w, ",\"router\":");
    write_json_str(w, nb.router);
    write_lit(w, ",\"latency_us\":");
    write_u64(w, nb.latency_us);
    write_lit(w, ",\"established\":");
    write_lit(w, nb.established ? "true" : "false");
    w.write_byte('}');
  }
  write_lit(w, "],\"sequence\":");
  write_u64(w, p.sequence);
  w.write_byte('}');
  w.finish();
  return out;
}

class JsonParser {
 public:
  explicit JsonParser(std::string_view src) noexcept : src_(src) {}

  bool parse_peer(PeerUpdate& out) {
    if (!consume('{')) return false;
    skip_ws();
    if (try_consume('}')) return true;
    while (true) {
      std::string_view key;
      if (!parse_key_view(key) || !consume(':')) return false;
      skip_ws();
      if (key == "asn") {
        uint64_t v = 0;
        if (!parse_u64(v)) return false;
        out.asn = static_cast<uint32_t>(v);
      } else if (key == "router_id") {
        if (!parse_string(out.router_id)) return false;
      } else if (key == "active") {
        if (!parse_bool(out.active)) return false;
      } else if (key == "uptime_sec") {
        if (!parse_u64(out.uptime_sec)) return false;
      } else if (key == "rssi_dbm") {
        if (!parse_i32(out.rssi_dbm)) return false;
      } else if (key == "cpu_load") {
        if (!parse_f64(out.cpu_load)) return false;
      } else if (key == "tags") {
        if (!consume('[')) return false;
        skip_ws();
        if (!try_consume(']')) {
          while (true) {
            std::string t;
            if (!parse_string(t)) return false;
            out.tags.push_back(std::move(t));
            skip_ws();
            if (try_consume(']')) break;
            if (!consume(',')) return false;
          }
        }
      } else if (key == "prefixes") {
        if (!consume('[')) return false;
        skip_ws();
        if (!try_consume(']')) {
          while (true) {
            uint64_t pv = 0;
            if (!parse_u64(pv)) return false;
            out.prefixes.push_back(static_cast<uint32_t>(pv));
            skip_ws();
            if (try_consume(']')) break;
            if (!consume(',')) return false;
          }
        }
      } else if (key == "location") {
        Location loc;
        if (!parse_location(loc)) return false;
        out.location = std::move(loc);
      } else if (key == "neighbors") {
        if (!consume('[')) return false;
        skip_ws();
        if (!try_consume(']')) {
          while (true) {
            Neighbor nb;
            if (!parse_neighbor(nb)) return false;
            out.neighbors.push_back(std::move(nb));
            skip_ws();
            if (try_consume(']')) break;
            if (!consume(',')) return false;
          }
        }
      } else if (key == "sequence") {
        if (!parse_u64(out.sequence)) return false;
      } else {
        return false;
      }
      skip_ws();
      if (try_consume('}')) return true;
      if (!consume(',')) return false;
    }
  }

 private:
  void skip_ws() noexcept {
    while (pos_ < src_.size()) {
      const char c = src_[pos_];
      if (c == ' ' || c == '\t' || c == '\n' || c == '\r') ++pos_;
      else break;
    }
  }

  bool consume(char want) noexcept {
    skip_ws();
    if (pos_ < src_.size() && src_[pos_] == want) {
      ++pos_;
      return true;
    }
    return false;
  }

  bool try_consume(char want) noexcept {
    if (pos_ < src_.size() && src_[pos_] == want) {
      ++pos_;
      return true;
    }
    return false;
  }

  bool parse_key_view(std::string_view& key) noexcept {
    skip_ws();
    if (pos_ >= src_.size() || src_[pos_] != '"') return false;
    ++pos_;
    const size_t start = pos_;
    while (pos_ < src_.size()) {
      if (src_[pos_] == '"') {
        key = src_.substr(start, pos_ - start);
        ++pos_;
        return true;
      }
      if (src_[pos_] == '\\') return false;
      ++pos_;
    }
    return false;
  }

  bool parse_string(std::string& dst) {
    skip_ws();
    if (pos_ >= src_.size() || src_[pos_] != '"') return false;
    ++pos_;
    const size_t start = pos_;
    while (pos_ < src_.size()) {
      const char c = src_[pos_];
      if (c == '"') {
        const std::string_view slice(src_.data() + start, pos_ - start);
        if (!omnibuf::is_valid_utf8(slice)) return false;
        dst.assign(slice.data(), slice.size());
        ++pos_;
        return true;
      }
      if (c == '\\') {
        dst.assign(src_.data() + start, pos_ - start);
        while (pos_ < src_.size()) {
          const char ch = src_[pos_++];
          if (ch == '"') {
            return omnibuf::is_valid_utf8(dst);
          }
          if (ch == '\\' && pos_ < src_.size()) {
            const char esc = src_[pos_++];
            switch (esc) {
              case '"': dst.push_back('"'); break;
              case '\\': dst.push_back('\\'); break;
              case 'n': dst.push_back('\n'); break;
              case 'r': dst.push_back('\r'); break;
              case 't': dst.push_back('\t'); break;
              default: dst.push_back(esc); break;
            }
          } else {
            dst.push_back(ch);
          }
        }
        return false;
      }
      ++pos_;
    }
    return false;
  }

  bool parse_bool(bool& out) noexcept {
    skip_ws();
    if (src_.substr(pos_, 4) == "true") {
      out = true;
      pos_ += 4;
      return true;
    }
    if (src_.substr(pos_, 5) == "false") {
      out = false;
      pos_ += 5;
      return true;
    }
    return false;
  }

  bool parse_u64(uint64_t& out) noexcept {
    skip_ws();
    const char* begin = src_.data() + pos_;
    const char* end = src_.data() + src_.size();
    const auto [ptr, ec] = std::from_chars(begin, end, out);
    if (ec != std::errc{}) return false;
    pos_ = static_cast<size_t>(ptr - src_.data());
    return true;
  }

  bool parse_i32(int32_t& out) noexcept {
    skip_ws();
    const char* begin = src_.data() + pos_;
    const char* end = src_.data() + src_.size();
    const auto [ptr, ec] = std::from_chars(begin, end, out);
    if (ec != std::errc{}) return false;
    pos_ = static_cast<size_t>(ptr - src_.data());
    return true;
  }

  bool parse_f32(float& out) noexcept {
    skip_ws();
    const char* begin = src_.data() + pos_;
    const char* end = src_.data() + src_.size();
    const auto [ptr, ec] = std::from_chars(begin, end, out);
    if (ec != std::errc{}) return false;
    pos_ = static_cast<size_t>(ptr - src_.data());
    return true;
  }

  bool parse_f64(double& out) noexcept {
    skip_ws();
    const char* begin = src_.data() + pos_;
    const char* end = src_.data() + src_.size();
    const auto [ptr, ec] = std::from_chars(begin, end, out);
    if (ec != std::errc{}) return false;
    pos_ = static_cast<size_t>(ptr - src_.data());
    return true;
  }

  bool parse_location(Location& loc) {
    if (!consume('{')) return false;
    skip_ws();
    if (try_consume('}')) return true;
    while (true) {
      std::string_view k;
      if (!parse_key_view(k) || !consume(':')) return false;
      if (k == "region") {
        if (!parse_string(loc.region)) return false;
      } else if (k == "datacenter") {
        if (!parse_string(loc.datacenter)) return false;
      } else if (k == "city") {
        if (!parse_string(loc.city)) return false;
      } else if (k == "rack") {
        uint64_t r = 0;
        if (!parse_u64(r)) return false;
        loc.rack = static_cast<uint32_t>(r);
      } else if (k == "latitude") {
        if (!parse_f32(loc.latitude)) return false;
      } else if (k == "longitude") {
        if (!parse_f32(loc.longitude)) return false;
      } else {
        return false;
      }
      skip_ws();
      if (try_consume('}')) return true;
      if (!consume(',')) return false;
    }
  }

  bool parse_neighbor(Neighbor& nb) {
    if (!consume('{')) return false;
    skip_ws();
    if (try_consume('}')) return true;
    while (true) {
      std::string_view k;
      if (!parse_key_view(k) || !consume(':')) return false;
      if (k == "peer_asn") {
        uint64_t v = 0;
        if (!parse_u64(v)) return false;
        nb.peer_asn = static_cast<uint32_t>(v);
      } else if (k == "router") {
        if (!parse_string(nb.router)) return false;
      } else if (k == "latency_us") {
        uint64_t v = 0;
        if (!parse_u64(v)) return false;
        nb.latency_us = static_cast<uint32_t>(v);
      } else if (k == "established") {
        if (!parse_bool(nb.established)) return false;
      } else {
        return false;
      }
      skip_ws();
      if (try_consume('}')) return true;
      if (!consume(',')) return false;
    }
  }

  std::string_view src_;
  size_t pos_ = 0;
};

bool decode_peer_update(std::span<const uint8_t> js_bytes, PeerUpdate& out) {
  const std::string_view js(reinterpret_cast<const char*>(js_bytes.data()), js_bytes.size());
  JsonParser parser(js);
  return parser.parse_peer(out);
}

}  // namespace json_codec

// ============================================================================
// Benchmark Harness & Structured JSON Output
// ============================================================================

struct BenchRow {
  std::string name;
  int64_t ns_per_op = 0;
  int64_t ops_per_sec = 0;
  std::optional<size_t> wire_bytes;
  bool highlight = false;
  bool baseline = false;
};

struct BenchCategory {
  std::string id;
  std::string title;
  std::vector<BenchRow> results;
};

struct BenchConfig {
  bool smoke = false;
};

template <typename Fn>
BenchRow run_bench(const BenchConfig& cfg,
                   std::string name,
                   Fn&& fn,
                   bool highlight = false,
                   bool baseline = false,
                   std::optional<size_t> wire_bytes = std::nullopt) {
  using Clock = std::chrono::steady_clock;

  const auto warmup_window =
      cfg.smoke ? std::chrono::milliseconds(2) : std::chrono::milliseconds(50);
  const double target_window_ns = cfg.smoke ? 5'000'000.0 : 250'000'000.0;
  const size_t warmup_batch = cfg.smoke ? 32 : 512;

  const auto warmup_start = Clock::now();
  do {
    for (size_t i = 0; i < warmup_batch; ++i) {
      fn();
      clobber_memory();
    }
  } while (Clock::now() - warmup_start < warmup_window);

  size_t iters = cfg.smoke ? 64 : 1024;
  double elapsed_ns = 0.0;
  while (true) {
    const auto t0 = Clock::now();
    for (size_t i = 0; i < iters; ++i) {
      fn();
      clobber_memory();
    }
    const auto t1 = Clock::now();
    elapsed_ns =
        static_cast<double>(std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count());
    if (elapsed_ns >= target_window_ns) {
      break;
    }
    iters *= 2;
  }

  const double raw_ns = elapsed_ns / static_cast<double>(iters);
  const int64_t ns_per_op = std::max<int64_t>(1, static_cast<int64_t>(std::llround(raw_ns)));
  const int64_t ops_per_sec =
      static_cast<int64_t>(std::llround(1'000'000'000.0 / std::max(0.1, raw_ns)));

  return BenchRow{
      .name = std::move(name),
      .ns_per_op = ns_per_op,
      .ops_per_sec = ops_per_sec,
      .wire_bytes = wire_bytes,
      .highlight = highlight,
      .baseline = baseline,
  };
}

std::string detect_platform() {
#if defined(__APPLE__)
  return "darwin";
#elif defined(__linux__)
  return "linux";
#elif defined(_WIN32)
  return "windows";
#else
  return "posix";
#endif
}

std::string detect_arch() {
#if defined(__aarch64__) || defined(_M_ARM64)
  return "arm64";
#elif defined(__x86_64__) || defined(_M_X64)
  return "amd64";
#else
  return "unknown";
#endif
}

std::string detect_cpu_model() {
#ifdef __APPLE__
  char buf[256] = {};
  size_t len = sizeof(buf);
  if (::sysctlbyname("machdep.cpu.brand_string", buf, &len, nullptr, 0) == 0 && len > 0) {
    return std::string(buf);
  }
  return "Apple Silicon";
#elif defined(__linux__)
  std::ifstream cpuinfo("/proc/cpuinfo");
  std::string line;
  while (std::getline(cpuinfo, line)) {
    if (line.rfind("model name", 0) == 0) {
      const size_t colon = line.find(':');
      if (colon != std::string::npos && colon + 2 < line.size()) {
        return line.substr(colon + 2);
      }
    }
  }
  return "Linux CPU";
#else
  return "CPU";
#endif
}

std::string detect_runtime() {
#if defined(__apple_build_version__) && defined(__clang_version__)
  std::string ver = __clang_version__;
  const size_t space = ver.find(' ');
  return "Apple Clang " + (space == std::string::npos ? ver : ver.substr(0, space)) + " (C++20)";
#elif defined(__clang_version__)
  std::string ver = __clang_version__;
  const size_t space = ver.find(' ');
  return "Clang " + (space == std::string::npos ? ver : ver.substr(0, space)) + " (C++20)";
#elif defined(__GNUC__)
  return "GCC " + std::to_string(__GNUC__) + "." + std::to_string(__GNUC_MINOR__) + " (C++20)";
#else
  return "C++20";
#endif
}

void finalize_and_print_category(BenchCategory& cat) {
  std::sort(cat.results.begin(), cat.results.end(),
            [](const BenchRow& a, const BenchRow& b) { return a.ns_per_op < b.ns_per_op; });
  std::printf("\n%s\n", cat.title.c_str());
  std::printf("----------------------------------------------------------------------------\n");
  for (const auto& r : cat.results) {
    if (r.wire_bytes.has_value()) {
      std::printf("  %-26s %8lld ns/op   %11lld ops/s   (%zu B)\n", r.name.c_str(),
                  static_cast<long long>(r.ns_per_op), static_cast<long long>(r.ops_per_sec),
                  *r.wire_bytes);
    } else {
      std::printf("  %-26s %8lld ns/op   %11lld ops/s\n", r.name.c_str(),
                  static_cast<long long>(r.ns_per_op), static_cast<long long>(r.ops_per_sec));
    }
  }
}

std::string serialize_benchmarks_json(const std::string& runtime,
                                      const std::string& platform,
                                      const std::string& arch,
                                      const std::string& cpu,
                                      const std::vector<BenchCategory>& categories) {
  std::ostringstream out;
  out << "{\n";
  out << "  \"language\": \"cpp\",\n";
  out << "  \"runtime\": \"" << runtime << "\",\n";
  out << "  \"platform\": \"" << platform << "\",\n";
  out << "  \"arch\": \"" << arch << "\",\n";
  out << "  \"cpu\": \"" << cpu << "\",\n";
  out << "  \"workload\": \"PeerUpdate\",\n";
  out << "  \"categories\": [\n";
  for (size_t ci = 0; ci < categories.size(); ++ci) {
    const auto& cat = categories[ci];
    out << "    {\n";
    out << "      \"id\": \"" << cat.id << "\",\n";
    out << "      \"title\": \"" << cat.title << "\",\n";
    out << "      \"unit\": \"ns\",\n";
    out << "      \"results\": [\n";
    for (size_t ri = 0; ri < cat.results.size(); ++ri) {
      const auto& r = cat.results[ri];
      out << "        {\n";
      out << "          \"name\": \"" << r.name << "\",\n";
      out << "          \"nsPerOp\": " << r.ns_per_op << ",\n";
      out << "          \"opsPerSec\": " << r.ops_per_sec;
      if (r.wire_bytes.has_value()) {
        out << ",\n          \"wireBytes\": " << *r.wire_bytes;
      }
      if (r.highlight) {
        out << ",\n          \"highlight\": true";
      }
      if (r.baseline) {
        out << ",\n          \"baseline\": true";
      }
      out << "\n        }" << (ri + 1 < cat.results.size() ? "," : "") << "\n";
    }
    out << "      ]\n";
    out << "    }" << (ci + 1 < categories.size() ? "," : "") << "\n";
  }
  out << "  ]\n";
  out << "}\n";
  return out.str();
}

}  // namespace

int main(int argc, char** argv) {
  BenchConfig cfg;
  std::optional<std::filesystem::path> explicit_out_path;
  for (int i = 1; i < argc; ++i) {
    const std::string_view arg(argv[i]);
    if (arg == "--smoke") {
      cfg.smoke = true;
    } else {
      explicit_out_path = std::filesystem::path(arg);
    }
  }

  const PeerUpdate sample = create_sample_peer_update();
  const uint64_t expected_sample_checksum = compute_peer_checksum(sample);

  const std::vector<uint8_t> omnibuf_gen_bytes = sample.marshal_omnibuf();
  const auto omnibuf_struct_res = omnibuf::marshal(sample);
  if (!omnibuf_struct_res) {
    std::fprintf(stderr, "omnibuf_bench: struct marshal failed\n");
    return EXIT_FAILURE;
  }
  const std::vector<uint8_t>& omnibuf_struct_bytes = omnibuf_struct_res.value();
  const std::vector<uint8_t> protobuf_bytes = pb_leb128::encode_peer_update(sample);
  const std::vector<uint8_t> json_bytes = json_codec::encode_peer_update(sample);
#ifdef OMNIBUF_HAS_LIBPROTOBUF
  const auto libpb_sample = omnibuf_bench_pb::make_sample_handle();
  const std::string libpb_sample_str = omnibuf_bench_pb::encode_handle(*libpb_sample);
  const std::vector<uint8_t> libpb_bytes(libpb_sample_str.begin(), libpb_sample_str.end());
  if (libpb_bytes.size() != 341) {
    std::fprintf(stderr, "omnibuf_bench: expected 341B libprotobuf packet, got %zu\n",
                 libpb_bytes.size());
    return EXIT_FAILURE;
  }
  {
    uint64_t libpb_cs = 0;
    if (!omnibuf_bench_pb::decode_and_checksum(libpb_bytes, libpb_cs) ||
        libpb_cs != expected_sample_checksum) {
      std::fprintf(stderr, "omnibuf_bench: libprotobuf round-trip checksum mismatch\n");
      return EXIT_FAILURE;
    }
  }
  const auto libpb_shifted = omnibuf_bench_pb::make_shifted_handle();
  const std::string libpb_shifted_str = omnibuf_bench_pb::encode_handle(*libpb_shifted);
  const auto libpb_routing_32 = omnibuf_bench_pb::make_routing_32_handle();
  const std::string libpb_routing_32_str = omnibuf_bench_pb::encode_handle(*libpb_routing_32);
  const std::vector<uint8_t> libpb_routing_32_bytes(libpb_routing_32_str.begin(),
                                                    libpb_routing_32_str.end());
#endif

  // Strict wire-size and full 11-field checksum invariants before timing
  if (omnibuf_gen_bytes.size() != 366 || omnibuf_struct_bytes.size() != 366 ||
      omnibuf_gen_bytes != omnibuf_struct_bytes) {
    std::fprintf(stderr, "omnibuf_bench: expected 366B OmniBuf packet, got %zu / %zu\n",
                 omnibuf_gen_bytes.size(), omnibuf_struct_bytes.size());
    return EXIT_FAILURE;
  }
  if (protobuf_bytes.size() != 341) {
    std::fprintf(stderr, "omnibuf_bench: expected 341B Protobuf packet, got %zu\n",
                 protobuf_bytes.size());
    return EXIT_FAILURE;
  }

  {
    PeerUpdate d1, d2, d3, d4;
    if (!d1.unmarshal_omnibuf(omnibuf_gen_bytes).ok() || d1 != sample ||
        compute_peer_checksum(d1) != expected_sample_checksum) {
      std::fprintf(stderr, "omnibuf_bench: generated round-trip or checksum mismatch\n");
      return EXIT_FAILURE;
    }
    if (!omnibuf::unmarshal(omnibuf_struct_bytes, d2).ok() || d2 != sample ||
        compute_peer_checksum(d2) != expected_sample_checksum) {
      std::fprintf(stderr, "omnibuf_bench: struct round-trip or checksum mismatch\n");
      return EXIT_FAILURE;
    }
    if (!pb_leb128::decode_peer_update(protobuf_bytes, d3) || d3 != sample ||
        compute_peer_checksum(d3) != expected_sample_checksum) {
      std::fprintf(stderr, "omnibuf_bench: protobuf round-trip or checksum mismatch\n");
      return EXIT_FAILURE;
    }
    if (!json_codec::decode_peer_update(json_bytes, d4) || d4 != sample ||
        compute_peer_checksum(d4) != expected_sample_checksum) {
      std::fprintf(stderr, "omnibuf_bench: json round-trip or checksum mismatch\n");
      return EXIT_FAILURE;
    }
  }

  // Upfront mutation verification for mutate_sequence (city == "Frankfurt", sequence + 1)
  PeerUpdate expected_mutated = sample;
  expected_mutated.sequence += 1ULL;
  const uint64_t expected_mutated_checksum = compute_peer_checksum(expected_mutated);
  {
    OpaquePeerUpdate view(omnibuf_gen_bytes);
    if (view.get_location().get_city_view() != "Frankfurt") return EXIT_FAILURE;
    view.set_sequence(view.get_sequence() + 1ULL);
    const auto m_omni = view.marshal();
    PeerUpdate dm1;
    if (!dm1.unmarshal_omnibuf(m_omni).ok() ||
        compute_peer_checksum(dm1) != expected_mutated_checksum) {
      std::fprintf(stderr, "omnibuf_bench: mutate_sequence OmniBuf verification failed\n");
      return EXIT_FAILURE;
    }

    PeerUpdate mp;
    if (!pb_leb128::decode_peer_update(protobuf_bytes, mp) || !mp.location ||
        mp.location->city != "Frankfurt") {
      return EXIT_FAILURE;
    }
    mp.sequence += 1ULL;
    const auto m_pb = pb_leb128::encode_peer_update(mp);
    PeerUpdate dm2;
    if (!pb_leb128::decode_peer_update(m_pb, dm2) ||
        compute_peer_checksum(dm2) != expected_mutated_checksum) {
      std::fprintf(stderr, "omnibuf_bench: mutate_sequence Protobuf verification failed\n");
      return EXIT_FAILURE;
    }

    PeerUpdate mj;
    if (!json_codec::decode_peer_update(json_bytes, mj) || !mj.location ||
        mj.location->city != "Frankfurt") {
      return EXIT_FAILURE;
    }
    mj.sequence += 1ULL;
    const auto m_js = json_codec::encode_peer_update(mj);
    PeerUpdate dm3;
    if (!json_codec::decode_peer_update(m_js, dm3) ||
        compute_peer_checksum(dm3) != expected_mutated_checksum) {
      std::fprintf(stderr, "omnibuf_bench: mutate_sequence JSON verification failed\n");
      return EXIT_FAILURE;
    }
  }

  // Stress workload 5: Large Embedded Message Encode (>16 KiB container length header shifts)
  PeerUpdate shifted_sample = sample;
  shifted_sample.location->datacenter = std::string(20480, 'd');
  shifted_sample.neighbors[0].router = std::string(18432, 'n');
  const uint64_t expected_shifted_checksum = compute_peer_checksum(shifted_sample);

  const std::vector<uint8_t> shifted_omni_gen = shifted_sample.marshal_omnibuf();
  const auto shifted_omni_struct_res = omnibuf::marshal(shifted_sample);
  if (!shifted_omni_struct_res || shifted_omni_struct_res.value() != shifted_omni_gen) {
    std::fprintf(stderr, "omnibuf_bench: shifted OmniBuf encode mismatch\n");
    return EXIT_FAILURE;
  }
  const std::vector<uint8_t> shifted_pb = pb_leb128::encode_peer_update(shifted_sample);
  const std::vector<uint8_t> shifted_json = json_codec::encode_peer_update(shifted_sample);
  {
    PeerUpdate s1, s2, s3, s4;
    if (!s1.unmarshal_omnibuf(shifted_omni_gen).ok() ||
        compute_peer_checksum(s1) != expected_shifted_checksum ||
        !omnibuf::unmarshal(shifted_omni_struct_res.value(), s2).ok() ||
        compute_peer_checksum(s2) != expected_shifted_checksum ||
        !pb_leb128::decode_peer_update(shifted_pb, s3) ||
        compute_peer_checksum(s3) != expected_shifted_checksum ||
        !json_codec::decode_peer_update(shifted_json, s4) ||
        compute_peer_checksum(s4) != expected_shifted_checksum) {
      std::fprintf(stderr, "omnibuf_bench: shifted round-trip checksum mismatch\n");
      return EXIT_FAILURE;
    }
  }

  // Stress workload 6: Embedded Message Growth (32 trailing neighbors, location grows to >16 KiB)
  const PeerUpdate routing_32 = create_peer_update_32_neighbors();
  const std::string grown_dc(20480, 'd');
  const std::vector<uint8_t> routing_32_omni = routing_32.marshal_omnibuf();
  const std::vector<uint8_t> routing_32_pb = pb_leb128::encode_peer_update(routing_32);
  const std::vector<uint8_t> routing_32_json = json_codec::encode_peer_update(routing_32);

  PeerUpdate expected_grown_32 = routing_32;
  expected_grown_32.location->datacenter = grown_dc;
  const uint64_t expected_grown_checksum = compute_peer_checksum(expected_grown_32);

  size_t grown_omni_wire_bytes = 0;
  size_t grown_pb_wire_bytes = 0;
  size_t grown_json_wire_bytes = 0;
  {
    OpaquePeerUpdate view(routing_32_omni);
    const auto loc_view = view.get_location();
    view.set_location(Location{
        .region = loc_view.get_region(),
        .datacenter = grown_dc,
        .city = loc_view.get_city(),
        .rack = loc_view.get_rack(),
        .latitude = loc_view.get_latitude(),
        .longitude = loc_view.get_longitude(),
    });
    const auto g_omni = view.marshal();
    grown_omni_wire_bytes = g_omni.size();
    PeerUpdate gd1;
    if (!gd1.unmarshal_omnibuf(g_omni).ok() ||
        compute_peer_checksum(gd1) != expected_grown_checksum) {
      std::fprintf(stderr, "omnibuf_bench: mutate_embedded_grow OmniBuf verification failed\n");
      return EXIT_FAILURE;
    }

    PeerUpdate gp;
    if (!pb_leb128::decode_peer_update(routing_32_pb, gp) || !gp.location) {
      return EXIT_FAILURE;
    }
    gp.location->datacenter = grown_dc;
    const auto g_pb = pb_leb128::encode_peer_update(gp);
    grown_pb_wire_bytes = g_pb.size();
    PeerUpdate gd2;
    if (!pb_leb128::decode_peer_update(g_pb, gd2) ||
        compute_peer_checksum(gd2) != expected_grown_checksum) {
      std::fprintf(stderr, "omnibuf_bench: mutate_embedded_grow Protobuf verification failed\n");
      return EXIT_FAILURE;
    }

    PeerUpdate gj;
    if (!json_codec::decode_peer_update(routing_32_json, gj) || !gj.location) {
      return EXIT_FAILURE;
    }
    gj.location->datacenter = grown_dc;
    const auto g_js = json_codec::encode_peer_update(gj);
    grown_json_wire_bytes = g_js.size();
    PeerUpdate gd3;
    if (!json_codec::decode_peer_update(g_js, gd3) ||
        compute_peer_checksum(gd3) != expected_grown_checksum) {
      std::fprintf(stderr, "omnibuf_bench: mutate_embedded_grow JSON verification failed\n");
      return EXIT_FAILURE;
    }
  }

  const std::string runtime = detect_runtime();
  const std::string platform = detect_platform();
  const std::string arch = detect_arch();
  const std::string cpu = detect_cpu_model();
  std::printf("============================================================================\n");
  std::printf("OmniBuf-CPP Benchmark Suite (%s / %s-%s / %s)\n", runtime.c_str(), platform.c_str(),
              arch.c_str(), cpu.c_str());
  std::printf("Workload: SamplePeerUpdate (OmniBuf = %zuB, Protobuf = %zuB, JSON = %zuB)\n",
              omnibuf_gen_bytes.size(), protobuf_bytes.size(), json_bytes.size());
  std::printf("============================================================================\n");

  std::vector<BenchCategory> categories;

  // 1. Encode
  BenchCategory encode_cat{
      .id = "encode",
      .title = "Full Message Encode Speed",
      .results = {
          run_bench(
              cfg,
              "OmniBuf (Generated)",
              [&] {
                do_not_optimize(sample);
                auto out = sample.marshal_omnibuf();
                sink_bytes(out);
              },
              true, true, omnibuf_gen_bytes.size()),
          run_bench(
              cfg,
              "OmniBuf (C++ Struct)",
              [&] {
                do_not_optimize(sample);
                auto out = omnibuf::marshal(sample);
                if (!out) std::abort();
                sink_bytes(out.value());
              },
              true, false, omnibuf_struct_bytes.size()),
          run_bench(
              cfg,
              "Protobuf (LEB128 Wire)",
              [&] {
                do_not_optimize(sample);
                auto out = pb_leb128::encode_peer_update(sample);
                sink_bytes(out);
              },
              false, false, protobuf_bytes.size()),
#ifdef OMNIBUF_HAS_LIBPROTOBUF
          run_bench(
              cfg,
              "Protobuf (libprotobuf)",
              [&] {
                const auto* ptr = libpb_sample.get();
                do_not_optimize(ptr);
                auto out = omnibuf_bench_pb::encode_handle(*ptr);
                sink_string(out);
              },
              false, false, libpb_bytes.size()),
#endif
          run_bench(
              cfg,
              "JSON",
              [&] {
                do_not_optimize(sample);
                auto out = json_codec::encode_peer_update(sample);
                sink_bytes(out);
              },
              false, false, json_bytes.size()),
      },
  };
  finalize_and_print_category(encode_cat);
  categories.push_back(std::move(encode_cat));

  // 2. Decode (full checksum verified before timing; lightweight sink prevents DCE in timed loop)
  BenchCategory decode_cat{
      .id = "decode",
      .title = "Full Message Decode Speed",
      .results = {
          run_bench(
              cfg,
              "OmniBuf (Generated)",
              [&] {
                std::span<const uint8_t> sp(omnibuf_gen_bytes);
                do_not_optimize(sp);
                PeerUpdate msg;
                if (!msg.unmarshal_omnibuf(sp).ok()) std::abort();
                const uint64_t d = digest_peer_light(msg);
                do_not_optimize(d);
              },
              true, true),
          run_bench(
              cfg,
              "OmniBuf (C++ Struct)",
              [&] {
                std::span<const uint8_t> sp(omnibuf_struct_bytes);
                do_not_optimize(sp);
                PeerUpdate msg;
                if (!omnibuf::unmarshal(sp, msg).ok()) std::abort();
                const uint64_t d = digest_peer_light(msg);
                do_not_optimize(d);
              },
              true, false),
          run_bench(
              cfg,
              "Protobuf (LEB128 Wire)",
              [&] {
                std::span<const uint8_t> sp(protobuf_bytes);
                do_not_optimize(sp);
                PeerUpdate msg;
                if (!pb_leb128::decode_peer_update(sp, msg)) std::abort();
                const uint64_t d = digest_peer_light(msg);
                do_not_optimize(d);
              }),
#ifdef OMNIBUF_HAS_LIBPROTOBUF
          run_bench(
              cfg,
              "Protobuf (libprotobuf)",
              [&] {
                std::span<const uint8_t> sp(libpb_bytes);
                do_not_optimize(sp);
                uint64_t d = 0;
                if (!omnibuf_bench_pb::decode_and_digest(sp, d)) {
                  std::abort();
                }
                do_not_optimize(d);
              }),
#endif
          run_bench(
              cfg,
              "JSON",
              [&] {
                std::span<const uint8_t> sp(json_bytes);
                do_not_optimize(sp);
                PeerUpdate msg;
                if (!json_codec::decode_peer_update(sp, msg)) std::abort();
                const uint64_t d = digest_peer_light(msg);
                do_not_optimize(d);
              }),
      },
  };
  finalize_and_print_category(decode_cat);
  categories.push_back(std::move(decode_cat));

  // 3. Lookup nested city (peer.location.city == "Frankfurt")
  BenchCategory lookup_cat{
      .id = "lookup_city",
      .title = "Read a Single Nested Field",
      .results = {
          run_bench(
              cfg,
              "OmniBuf (Generated)",
              [&] {
                std::span<const uint8_t> sp(omnibuf_gen_bytes);
                do_not_optimize(sp);
                OpaquePeerUpdate view(sp);
                std::string_view city = view.get_location().get_city_view();
                if (city != "Frankfurt") std::abort();
                const char* p = city.data();
                const size_t n = city.size();
                do_not_optimize(p);
                do_not_optimize(n);
              },
              true, true),
          run_bench(
              cfg,
              "Protobuf (LEB128 Wire)",
              [&] {
                std::span<const uint8_t> sp(protobuf_bytes);
                do_not_optimize(sp);
                PeerUpdate msg;
                if (!pb_leb128::decode_peer_update(sp, msg) || !msg.location.has_value() ||
                    msg.location->city != "Frankfurt") {
                  std::abort();
                }
                const char* p = msg.location->city.data();
                const size_t n = msg.location->city.size();
                do_not_optimize(p);
                do_not_optimize(n);
              }),
#ifdef OMNIBUF_HAS_LIBPROTOBUF
          run_bench(
              cfg,
              "Protobuf (libprotobuf)",
              [&] {
                std::span<const uint8_t> sp(libpb_bytes);
                do_not_optimize(sp);
                std::string city;
                if (!omnibuf_bench_pb::lookup_city(sp, city) || city != "Frankfurt") {
                  std::abort();
                }
                sink_string(city);
              }),
#endif
          run_bench(
              cfg,
              "JSON",
              [&] {
                std::span<const uint8_t> sp(json_bytes);
                do_not_optimize(sp);
                PeerUpdate msg;
                if (!json_codec::decode_peer_update(sp, msg) || !msg.location.has_value() ||
                    msg.location->city != "Frankfurt") {
                  std::abort();
                }
                const char* p = msg.location->city.data();
                const size_t n = msg.location->city.size();
                do_not_optimize(p);
                do_not_optimize(n);
              }),
      },
  };
  finalize_and_print_category(lookup_cat);
  categories.push_back(std::move(lookup_cat));

  // 4. Selective single-field mutation (check city == "Frankfurt", increment sequence + 1)
  BenchCategory mutate_cat{
      .id = "mutate_sequence",
      .title = "Selective Single-Field Mutation",
      .results = {
          run_bench(
              cfg,
              "OmniBuf (Generated)",
              [&] {
                std::span<const uint8_t> sp(omnibuf_gen_bytes);
                do_not_optimize(sp);
                OpaquePeerUpdate view(sp);
                if (view.get_location().get_city_view() != "Frankfurt") std::abort();
                view.set_sequence(view.get_sequence() + 1ULL);
                auto out = view.marshal();
                sink_bytes(out);
              },
              true, true),
          run_bench(
              cfg,
              "Protobuf (LEB128 Wire)",
              [&] {
                std::span<const uint8_t> sp(protobuf_bytes);
                do_not_optimize(sp);
                PeerUpdate msg;
                if (!pb_leb128::decode_peer_update(sp, msg) || !msg.location.has_value() ||
                    msg.location->city != "Frankfurt") {
                  std::abort();
                }
                msg.sequence += 1ULL;
                auto out = pb_leb128::encode_peer_update(msg);
                sink_bytes(out);
              }),
#ifdef OMNIBUF_HAS_LIBPROTOBUF
          run_bench(
              cfg,
              "Protobuf (libprotobuf)",
              [&] {
                std::span<const uint8_t> sp(libpb_bytes);
                do_not_optimize(sp);
                std::string out;
                if (!omnibuf_bench_pb::mutate_sequence(sp, out)) std::abort();
                sink_string(out);
              }),
#endif
          run_bench(
              cfg,
              "JSON",
              [&] {
                std::span<const uint8_t> sp(json_bytes);
                do_not_optimize(sp);
                PeerUpdate msg;
                if (!json_codec::decode_peer_update(sp, msg) || !msg.location.has_value() ||
                    msg.location->city != "Frankfurt") {
                  std::abort();
                }
                msg.sequence += 1ULL;
                auto out = json_codec::encode_peer_update(msg);
                sink_bytes(out);
              }),
      },
  };
  finalize_and_print_category(mutate_cat);
  categories.push_back(std::move(mutate_cat));

  // 5. Large Embedded Message Encode (>16 KiB sub-message and array 2B -> 4B length shifts)
  BenchCategory shift_encode_cat{
      .id = "encode_nested_shift",
      .title = "Large Embedded Message Encode",
      .results = {
          run_bench(
              cfg,
              "OmniBuf (Generated)",
              [&] {
                do_not_optimize(shifted_sample);
                auto out = shifted_sample.marshal_omnibuf();
                sink_bytes(out);
              },
              true, true, shifted_omni_gen.size()),
          run_bench(
              cfg,
              "OmniBuf (C++ Struct)",
              [&] {
                do_not_optimize(shifted_sample);
                auto out = omnibuf::marshal(shifted_sample);
                if (!out) std::abort();
                sink_bytes(out.value());
              },
              true, false, shifted_omni_gen.size()),
          run_bench(
              cfg,
              "Protobuf (LEB128 Wire)",
              [&] {
                do_not_optimize(shifted_sample);
                auto out = pb_leb128::encode_peer_update(shifted_sample);
                sink_bytes(out);
              },
              false, false, shifted_pb.size()),
#ifdef OMNIBUF_HAS_LIBPROTOBUF
          run_bench(
              cfg,
              "Protobuf (libprotobuf)",
              [&] {
                const auto* ptr = libpb_shifted.get();
                do_not_optimize(ptr);
                auto out = omnibuf_bench_pb::encode_handle(*ptr);
                sink_string(out);
              },
              false, false, libpb_shifted_str.size()),
#endif
          run_bench(
              cfg,
              "JSON",
              [&] {
                do_not_optimize(shifted_sample);
                auto out = json_codec::encode_peer_update(shifted_sample);
                sink_bytes(out);
              },
              false, false, shifted_json.size()),
      },
  };
  finalize_and_print_category(shift_encode_cat);
  categories.push_back(std::move(shift_encode_cat));

  // 6. Embedded Message Growth (peer.location grows to >16 KiB ahead of 32 trailing neighbors)
  BenchCategory grow_mutate_cat{
      .id = "mutate_embedded_grow",
      .title = "Embedded Message Growth",
      .results = {
          run_bench(
              cfg,
              "OmniBuf (Generated)",
              [&] {
                std::span<const uint8_t> sp(routing_32_omni);
                do_not_optimize(sp);
                OpaquePeerUpdate view(sp);
                const auto loc_view = view.get_location();
                view.set_location(Location{
                    .region = loc_view.get_region(),
                    .datacenter = grown_dc,
                    .city = loc_view.get_city(),
                    .rack = loc_view.get_rack(),
                    .latitude = loc_view.get_latitude(),
                    .longitude = loc_view.get_longitude(),
                });
                auto out = view.marshal();
                sink_bytes(out);
              },
              true, true, grown_omni_wire_bytes),
          run_bench(
              cfg,
              "Protobuf (LEB128 Wire)",
              [&] {
                std::span<const uint8_t> sp(routing_32_pb);
                do_not_optimize(sp);
                PeerUpdate msg;
                if (!pb_leb128::decode_peer_update(sp, msg) || !msg.location.has_value()) {
                  std::abort();
                }
                msg.location->datacenter = grown_dc;
                auto out = pb_leb128::encode_peer_update(msg);
                sink_bytes(out);
              },
              false, false, grown_pb_wire_bytes),
#ifdef OMNIBUF_HAS_LIBPROTOBUF
          run_bench(
              cfg,
              "Protobuf (libprotobuf)",
              [&] {
                std::span<const uint8_t> sp(libpb_routing_32_bytes);
                do_not_optimize(sp);
                std::string out;
                if (!omnibuf_bench_pb::mutate_embedded_grow(sp, grown_dc, out)) std::abort();
                sink_string(out);
              },
              false, false, grown_pb_wire_bytes),
#endif
          run_bench(
              cfg,
              "JSON",
              [&] {
                std::span<const uint8_t> sp(routing_32_json);
                do_not_optimize(sp);
                PeerUpdate msg;
                if (!json_codec::decode_peer_update(sp, msg) || !msg.location.has_value()) {
                  std::abort();
                }
                msg.location->datacenter = grown_dc;
                auto out = json_codec::encode_peer_update(msg);
                sink_bytes(out);
              },
              false, false, grown_json_wire_bytes),
      },
  };
  finalize_and_print_category(grow_mutate_cat);
  categories.push_back(std::move(grow_mutate_cat));

  if (cfg.smoke && !explicit_out_path.has_value()) {
    std::printf("\nSmoke verification passed across all 6 categories (JSON file unchanged).\n");
    return EXIT_SUCCESS;
  }

  std::filesystem::path out_path = "testdata/benchmarks.json";
  if (explicit_out_path.has_value()) {
    out_path = *explicit_out_path;
  } else {
#ifdef OMNIBUF_BENCH_JSON_PATH
    out_path = OMNIBUF_BENCH_JSON_PATH;
#endif
  }

  if (out_path.has_parent_path()) {
    std::filesystem::create_directories(out_path.parent_path());
  }
  std::ofstream out_file(out_path, std::ios::binary);
  if (!out_file) {
    std::fprintf(stderr, "omnibuf_bench: failed to write %s\n", out_path.string().c_str());
    return EXIT_FAILURE;
  }
  out_file << serialize_benchmarks_json(runtime, platform, arch, cpu, categories);
  out_file.close();

  std::printf("\nWrote structured benchmark results to %s\n", out_path.string().c_str());
  return EXIT_SUCCESS;
}
