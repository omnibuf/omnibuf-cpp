#include <sys/sysctl.h>

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
#include <string>
#include <string_view>
#include <vector>

#include "omnibuf/bench/peer.omnibuf.pb.hpp"
#include "omnibuf/omnibuf.hpp"

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
  asm volatile("" : : "r,m"(value) : "memory");
}

inline void clobber_memory() {
  asm volatile("" : : : "memory");
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

// ============================================================================
// Protobuf LEB128 Wire Codec (proto3 canonical encoding for PeerUpdate, 341B)
// ============================================================================

namespace pb_leb128 {

inline void append_leb128(std::vector<uint8_t>& out, uint64_t v) {
  while (v >= 0x80u) {
    out.push_back(static_cast<uint8_t>((v & 0x7Fu) | 0x80u));
    v >>= 7;
  }
  out.push_back(static_cast<uint8_t>(v));
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

inline void append_str_field(std::vector<uint8_t>& out, uint32_t fnum, std::string_view s) {
  if (s.empty()) return;
  append_leb128(out, (static_cast<uint64_t>(fnum) << 3) | 2u);
  append_leb128(out, s.size());
  out.insert(out.end(), s.begin(), s.end());
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
  out.reserve(512);

  if (msg.asn != 0) {
    append_leb128(out, (1ULL << 3) | 0u);
    append_leb128(out, msg.asn);
  }
  append_str_field(out, 2, msg.router_id);
  if (msg.active) {
    out.push_back((3u << 3) | 0u);
    out.push_back(1u);
  }
  if (msg.uptime_sec != 0) {
    append_leb128(out, (4ULL << 3) | 0u);
    append_leb128(out, msg.uptime_sec);
  }
  if (msg.rssi_dbm != 0) {
    append_leb128(out, (5ULL << 3) | 0u);
    append_leb128(out, zigzag32(msg.rssi_dbm));
  }
  if (omnibuf::float64_to_bits(msg.cpu_load) != 0) {
    out.push_back((6u << 3) | 1u);
    omnibuf::append_u64_le(out, omnibuf::float64_to_bits(msg.cpu_load));
  }
  for (const auto& tag : msg.tags) {
    append_leb128(out, (7ULL << 3) | 2u);
    append_leb128(out, tag.size());
    out.insert(out.end(), tag.begin(), tag.end());
  }
  if (!msg.prefixes.empty()) {
    size_t packed_len = 0;
    for (uint32_t p : msg.prefixes) {
      packed_len += leb128_size(p);
    }
    append_leb128(out, (8ULL << 3) | 2u);
    append_leb128(out, packed_len);
    for (uint32_t p : msg.prefixes) {
      append_leb128(out, p);
    }
  }
  if (msg.location.has_value()) {
    const auto& loc = *msg.location;
    const size_t loc_sz = location_body_size(loc);
    append_leb128(out, (9ULL << 3) | 2u);
    append_leb128(out, loc_sz);
    append_str_field(out, 1, loc.region);
    append_str_field(out, 2, loc.datacenter);
    append_str_field(out, 3, loc.city);
    if (loc.rack != 0) {
      append_leb128(out, (4ULL << 3) | 0u);
      append_leb128(out, loc.rack);
    }
    if (omnibuf::float32_to_bits(loc.latitude) != 0) {
      out.push_back((5u << 3) | 5u);
      omnibuf::append_u32_le(out, omnibuf::float32_to_bits(loc.latitude));
    }
    if (omnibuf::float32_to_bits(loc.longitude) != 0) {
      out.push_back((6u << 3) | 5u);
      omnibuf::append_u32_le(out, omnibuf::float32_to_bits(loc.longitude));
    }
  }
  for (const auto& nb : msg.neighbors) {
    const size_t nb_sz = neighbor_body_size(nb);
    append_leb128(out, (10ULL << 3) | 2u);
    append_leb128(out, nb_sz);
    if (nb.peer_asn != 0) {
      append_leb128(out, (1ULL << 3) | 0u);
      append_leb128(out, nb.peer_asn);
    }
    append_str_field(out, 2, nb.router);
    if (nb.latency_us != 0) {
      append_leb128(out, (3ULL << 3) | 0u);
      append_leb128(out, nb.latency_us);
    }
    if (nb.established) {
      out.push_back((4u << 3) | 0u);
      out.push_back(1u);
    }
  }
  if (msg.sequence != 0) {
    append_leb128(out, (11ULL << 3) | 0u);
    append_leb128(out, msg.sequence);
  }
  return out;
}

inline bool read_leb128(std::span<const uint8_t> b, size_t& pos, uint64_t& out) noexcept {
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
// ============================================================================

namespace json_codec {

inline void append_u64(std::string& out, uint64_t v) {
  char buf[24];
  const auto [ptr, ec] = std::to_chars(buf, buf + sizeof(buf), v);
  if (ec == std::errc{}) out.append(buf, static_cast<size_t>(ptr - buf));
}

inline void append_i32(std::string& out, int32_t v) {
  char buf[16];
  const auto [ptr, ec] = std::to_chars(buf, buf + sizeof(buf), v);
  if (ec == std::errc{}) out.append(buf, static_cast<size_t>(ptr - buf));
}

inline void append_f32(std::string& out, float v) {
  char buf[32];
  const auto [ptr, ec] = std::to_chars(buf, buf + sizeof(buf), v);
  if (ec == std::errc{}) out.append(buf, static_cast<size_t>(ptr - buf));
}

inline void append_f64(std::string& out, double v) {
  char buf[32];
  const auto [ptr, ec] = std::to_chars(buf, buf + sizeof(buf), v);
  if (ec == std::errc{}) out.append(buf, static_cast<size_t>(ptr - buf));
}

inline void append_str(std::string& out, std::string_view s) {
  omnibuf::detail::append_json_escaped_string(out, s);
}

std::string encode_peer_update(const PeerUpdate& p) {
  std::string out;
  out.reserve(896);
  out += "{\"asn\":";
  append_u64(out, p.asn);
  out += ",\"router_id\":";
  append_str(out, p.router_id);
  out += ",\"active\":";
  out += (p.active ? "true" : "false");
  out += ",\"uptime_sec\":";
  append_u64(out, p.uptime_sec);
  out += ",\"rssi_dbm\":";
  append_i32(out, p.rssi_dbm);
  out += ",\"cpu_load\":";
  append_f64(out, p.cpu_load);
  out += ",\"tags\":[";
  for (size_t i = 0; i < p.tags.size(); ++i) {
    if (i > 0) out.push_back(',');
    append_str(out, p.tags[i]);
  }
  out += "],\"prefixes\":[";
  for (size_t i = 0; i < p.prefixes.size(); ++i) {
    if (i > 0) out.push_back(',');
    append_u64(out, p.prefixes[i]);
  }
  out.push_back(']');
  if (p.location.has_value()) {
    const auto& loc = *p.location;
    out += ",\"location\":{\"region\":";
    append_str(out, loc.region);
    out += ",\"datacenter\":";
    append_str(out, loc.datacenter);
    out += ",\"city\":";
    append_str(out, loc.city);
    out += ",\"rack\":";
    append_u64(out, loc.rack);
    out += ",\"latitude\":";
    append_f32(out, loc.latitude);
    out += ",\"longitude\":";
    append_f32(out, loc.longitude);
    out.push_back('}');
  }
  out += ",\"neighbors\":[";
  for (size_t i = 0; i < p.neighbors.size(); ++i) {
    if (i > 0) out.push_back(',');
    const auto& nb = p.neighbors[i];
    out += "{\"peer_asn\":";
    append_u64(out, nb.peer_asn);
    out += ",\"router\":";
    append_str(out, nb.router);
    out += ",\"latency_us\":";
    append_u64(out, nb.latency_us);
    out += ",\"established\":";
    out += (nb.established ? "true" : "false");
    out.push_back('}');
  }
  out += "],\"sequence\":";
  append_u64(out, p.sequence);
  out.push_back('}');
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
      std::string key;
      if (!parse_string(key) || !consume(':')) return false;
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

  bool parse_string(std::string& dst) {
    skip_ws();
    if (pos_ >= src_.size() || src_[pos_] != '"') return false;
    ++pos_;
    const size_t start = pos_;
    while (pos_ < src_.size()) {
      const char c = src_[pos_];
      if (c == '"') {
        dst.assign(src_.data() + start, pos_ - start);
        ++pos_;
        return true;
      }
      if (c == '\\') {
        dst.assign(src_.data() + start, pos_ - start);
        while (pos_ < src_.size()) {
          const char ch = src_[pos_++];
          if (ch == '"') return true;
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
      std::string k;
      if (!parse_string(k) || !consume(':')) return false;
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
      std::string k;
      if (!parse_string(k) || !consume(':')) return false;
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

bool decode_peer_update(std::string_view js, PeerUpdate& out) {
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

template <typename Fn>
BenchRow run_bench(std::string name,
                   Fn&& fn,
                   bool highlight = false,
                   bool baseline = false,
                   std::optional<size_t> wire_bytes = std::nullopt) {
  using Clock = std::chrono::steady_clock;

  // Warmup phase (~50ms)
  const auto warmup_start = Clock::now();
  size_t warmup_iters = 0;
  do {
    for (int i = 0; i < 512; ++i) {
      fn();
      clobber_memory();
    }
    warmup_iters += 512;
  } while (Clock::now() - warmup_start < std::chrono::milliseconds(50));
  (void)warmup_iters;

  // Calibrate batch size for a ~250ms measurement window
  size_t iters = 4096;
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
    if (elapsed_ns >= 250'000'000.0) {
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

std::string detect_cpu_model() {
  char buf[256] = {};
  size_t len = sizeof(buf);
  if (::sysctlbyname("machdep.cpu.brand_string", buf, &len, nullptr, 0) == 0 && len > 0) {
    return std::string(buf);
  }
  return "Apple Silicon";
}

std::string detect_runtime() {
#ifdef __clang_version__
  std::string ver = __clang_version__;
  const size_t space = ver.find(' ');
  return "Apple Clang " + (space == std::string::npos ? ver : ver.substr(0, space)) + " (C++20)";
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
                                      const std::string& cpu,
                                      const std::vector<BenchCategory>& categories) {
  std::ostringstream out;
  out << "{\n";
  out << "  \"language\": \"cpp\",\n";
  out << "  \"runtime\": \"" << runtime << "\",\n";
  out << "  \"platform\": \"darwin\",\n";
  out << "  \"arch\": \"arm64\",\n";
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
  const PeerUpdate sample = create_sample_peer_update();

  const std::vector<uint8_t> omnibuf_gen_bytes = sample.marshal_omnibuf();
  const auto omnibuf_struct_res = omnibuf::marshal(sample);
  if (!omnibuf_struct_res) {
    std::fprintf(stderr, "omnibuf_bench: struct marshal failed\n");
    return EXIT_FAILURE;
  }
  const std::vector<uint8_t>& omnibuf_struct_bytes = omnibuf_struct_res.value();
  const std::vector<uint8_t> protobuf_bytes = pb_leb128::encode_peer_update(sample);
  const std::string json_str = json_codec::encode_peer_update(sample);

  // Strict wire-size and round-trip invariants before timing
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
    if (!d1.unmarshal_omnibuf(omnibuf_gen_bytes).ok() || d1 != sample) {
      std::fprintf(stderr, "omnibuf_bench: generated round-trip mismatch\n");
      return EXIT_FAILURE;
    }
    if (!omnibuf::unmarshal(omnibuf_struct_bytes, d2).ok() || d2 != sample) {
      std::fprintf(stderr, "omnibuf_bench: struct round-trip mismatch\n");
      return EXIT_FAILURE;
    }
    if (!pb_leb128::decode_peer_update(protobuf_bytes, d3) || d3 != sample) {
      std::fprintf(stderr, "omnibuf_bench: protobuf round-trip mismatch\n");
      return EXIT_FAILURE;
    }
    if (!json_codec::decode_peer_update(json_str, d4) || d4 != sample) {
      std::fprintf(stderr, "omnibuf_bench: json round-trip mismatch\n");
      return EXIT_FAILURE;
    }
  }

  const std::string runtime = detect_runtime();
  const std::string cpu = detect_cpu_model();
  std::printf("============================================================================\n");
  std::printf("OmniBuf-CPP Benchmark Suite (%s / darwin-arm64 / %s)\n", runtime.c_str(),
              cpu.c_str());
  std::printf("Workload: SamplePeerUpdate (OmniBuf = %zuB, Protobuf = %zuB, JSON = %zuB)\n",
              omnibuf_gen_bytes.size(), protobuf_bytes.size(), json_str.size());
  std::printf("============================================================================\n");

  std::vector<BenchCategory> categories;

  // 1. Encode
  BenchCategory encode_cat{
      .id = "encode",
      .title = "Full Message Encode Speed (PeerUpdate)",
      .results = {
          run_bench(
              "OmniBuf (Generated)",
              [&] {
                do_not_optimize(sample);
                auto out = sample.marshal_omnibuf();
                do_not_optimize(out);
              },
              true, true, omnibuf_gen_bytes.size()),
          run_bench(
              "OmniBuf (C++ Struct)",
              [&] {
                do_not_optimize(sample);
                auto out = omnibuf::marshal(sample);
                do_not_optimize(out);
              },
              true, false, omnibuf_struct_bytes.size()),
          run_bench(
              "Protobuf (LEB128 Wire)",
              [&] {
                do_not_optimize(sample);
                auto out = pb_leb128::encode_peer_update(sample);
                do_not_optimize(out);
              },
              false, false, protobuf_bytes.size()),
          run_bench(
              "JSON",
              [&] {
                do_not_optimize(sample);
                auto out = json_codec::encode_peer_update(sample);
                do_not_optimize(out);
              },
              false, false, json_str.size()),
      },
  };
  finalize_and_print_category(encode_cat);
  categories.push_back(std::move(encode_cat));

  // 2. Decode
  BenchCategory decode_cat{
      .id = "decode",
      .title = "Full Message Decode Speed (PeerUpdate)",
      .results = {
          run_bench(
              "OmniBuf (Generated)",
              [&] {
                std::span<const uint8_t> sp(omnibuf_gen_bytes);
                do_not_optimize(sp);
                PeerUpdate msg;
                auto res = msg.unmarshal_omnibuf(sp);
                do_not_optimize(res);
                do_not_optimize(msg);
              },
              true, true),
          run_bench(
              "OmniBuf (C++ Struct)",
              [&] {
                std::span<const uint8_t> sp(omnibuf_struct_bytes);
                do_not_optimize(sp);
                PeerUpdate msg;
                auto res = omnibuf::unmarshal(sp, msg);
                do_not_optimize(res);
                do_not_optimize(msg);
              },
              true, false),
          run_bench(
              "Protobuf (LEB128 Wire)",
              [&] {
                std::span<const uint8_t> sp(protobuf_bytes);
                do_not_optimize(sp);
                PeerUpdate msg;
                bool ok = pb_leb128::decode_peer_update(sp, msg);
                do_not_optimize(ok);
                do_not_optimize(msg);
              }),
          run_bench(
              "JSON",
              [&] {
                std::string_view sv(json_str);
                do_not_optimize(sv);
                PeerUpdate msg;
                bool ok = json_codec::decode_peer_update(sv, msg);
                do_not_optimize(ok);
                do_not_optimize(msg);
              }),
      },
  };
  finalize_and_print_category(decode_cat);
  categories.push_back(std::move(decode_cat));

  // 3. Lookup nested city (peer.location.city)
  BenchCategory lookup_cat{
      .id = "lookup_city",
      .title = "Read a Single Nested Field (peer.location.city)",
      .results = {
          run_bench(
              "OmniBuf (Generated)",
              [&] {
                std::span<const uint8_t> sp(omnibuf_gen_bytes);
                do_not_optimize(sp);
                OpaquePeerUpdate view(sp);
                std::string_view city = view.get_location().get_city_view();
                if (city.size() != 9) std::abort();
                do_not_optimize(city);
              },
              true, true),
          run_bench(
              "Protobuf (LEB128 Wire)",
              [&] {
                std::span<const uint8_t> sp(protobuf_bytes);
                do_not_optimize(sp);
                PeerUpdate msg;
                if (!pb_leb128::decode_peer_update(sp, msg) || !msg.location.has_value()) {
                  std::abort();
                }
                do_not_optimize(msg.location->city);
              }),
          run_bench(
              "JSON",
              [&] {
                std::string_view sv(json_str);
                do_not_optimize(sv);
                PeerUpdate msg;
                if (!json_codec::decode_peer_update(sv, msg) || !msg.location.has_value()) {
                  std::abort();
                }
                do_not_optimize(msg.location->city);
              }),
      },
  };
  finalize_and_print_category(lookup_cat);
  categories.push_back(std::move(lookup_cat));

  // 4. Selective single-field mutation (peer.sequence)
  BenchCategory mutate_cat{
      .id = "mutate_sequence",
      .title = "Selective Single-Field Mutation (peer.sequence)",
      .results = {
          run_bench(
              "OmniBuf (Generated)",
              [&] {
                std::span<const uint8_t> sp(omnibuf_gen_bytes);
                do_not_optimize(sp);
                OpaquePeerUpdate view(sp);
                view.set_sequence(9876543211ULL);
                auto out = view.marshal();
                do_not_optimize(out);
              },
              true, true),
          run_bench(
              "Protobuf (LEB128 Wire)",
              [&] {
                std::span<const uint8_t> sp(protobuf_bytes);
                do_not_optimize(sp);
                PeerUpdate msg;
                if (!pb_leb128::decode_peer_update(sp, msg)) std::abort();
                msg.sequence = 9876543211ULL;
                auto out = pb_leb128::encode_peer_update(msg);
                do_not_optimize(out);
              }),
          run_bench(
              "JSON",
              [&] {
                std::string_view sv(json_str);
                do_not_optimize(sv);
                PeerUpdate msg;
                if (!json_codec::decode_peer_update(sv, msg)) std::abort();
                msg.sequence = 9876543211ULL;
                auto out = json_codec::encode_peer_update(msg);
                do_not_optimize(out);
              }),
      },
  };
  finalize_and_print_category(mutate_cat);
  categories.push_back(std::move(mutate_cat));

  std::filesystem::path out_path = "testdata/benchmarks.json";
  if (argc > 1 && argv[1] != nullptr) {
    out_path = argv[1];
  } else {
#ifdef OMNIBUF_BENCH_JSON_PATH
    out_path = OMNIBUF_BENCH_JSON_PATH;
#endif
  }

  std::filesystem::create_directories(out_path.parent_path());
  std::ofstream out_file(out_path, std::ios::binary);
  if (!out_file) {
    std::fprintf(stderr, "omnibuf_bench: failed to write %s\n", out_path.string().c_str());
    return EXIT_FAILURE;
  }
  out_file << serialize_benchmarks_json(runtime, cpu, categories);
  out_file.close();

  std::printf("\nWrote structured benchmark results to %s\n", out_path.string().c_str());
  return EXIT_SUCCESS;
}
