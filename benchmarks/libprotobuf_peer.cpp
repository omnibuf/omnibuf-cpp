#include "libprotobuf_peer.hpp"

#include <bit>
#include <cstdint>
#include <string>
#include <string_view>

#include "libpb_peer.pb.h"

namespace omnibuf_bench_pb {

namespace {

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

uint64_t compute_pb_checksum(const libpb::bench::PeerUpdate& p) noexcept {
  uint64_t h = 0xcbf29ce484222325ULL;
  h = mix_u64(h, p.asn());
  h = mix_str(h, p.router_id());
  h = mix_u64(h, p.active() ? 1ULL : 0ULL);
  h = mix_u64(h, p.uptime_sec());
  h = mix_u64(h, static_cast<uint64_t>(static_cast<int64_t>(p.rssi_dbm())));
  h = mix_u64(h, std::bit_cast<uint64_t>(p.cpu_load()));

  h = mix_u64(h, static_cast<uint64_t>(p.tags_size()));
  for (const auto& t : p.tags()) {
    h = mix_str(h, t);
  }

  h = mix_u64(h, static_cast<uint64_t>(p.prefixes_size()));
  for (uint32_t pref : p.prefixes()) {
    h = mix_u64(h, pref);
  }

  h = mix_u64(h, p.has_location() ? 1ULL : 0ULL);
  if (p.has_location()) {
    const auto& loc = p.location();
    h = mix_str(h, loc.region());
    h = mix_str(h, loc.datacenter());
    h = mix_str(h, loc.city());
    h = mix_u64(h, loc.rack());
    h = mix_u64(h, std::bit_cast<uint32_t>(loc.latitude()));
    h = mix_u64(h, std::bit_cast<uint32_t>(loc.longitude()));
  }

  h = mix_u64(h, static_cast<uint64_t>(p.neighbors_size()));
  for (const auto& nb : p.neighbors()) {
    h = mix_u64(h, nb.peer_asn());
    h = mix_str(h, nb.router());
    h = mix_u64(h, nb.latency_us());
    h = mix_u64(h, nb.established() ? 1ULL : 0ULL);
  }

  h = mix_u64(h, p.sequence());
  return h;
}

void populate_sample(libpb::bench::PeerUpdate& p) {
  p.set_asn(64512);
  p.set_router_id("edge-rtr-fra01.core.backbone.net");
  p.set_active(true);
  p.set_uptime_sec(1849203);
  p.set_rssi_dbm(-62);
  p.set_cpu_load(0.42875);
  p.add_tags("tier1");
  p.add_tags("ebgp");
  p.add_tags("eu-central");
  p.add_tags("ixp-de-cix");
  p.add_prefixes(167772160u);
  p.add_prefixes(172032000u);
  p.add_prefixes(3232235520u);
  p.add_prefixes(2886729728u);
  p.add_prefixes(134744072u);
  p.add_prefixes(134743040u);

  auto* loc = p.mutable_location();
  loc->set_region("eu-central-1");
  loc->set_datacenter("FRA-Equinix-FR5");
  loc->set_city("Frankfurt");
  loc->set_rack(42);
  loc->set_latitude(50.1109f);
  loc->set_longitude(8.6821f);

  auto add_nb = [&](uint32_t asn, const char* router, uint32_t lat, bool est) {
    auto* nb = p.add_neighbors();
    nb->set_peer_asn(asn);
    nb->set_router(router);
    nb->set_latency_us(lat);
    nb->set_established(est);
  };
  add_nb(3356, "ae0.fra1.level3.net", 850, true);
  add_nb(1299, "ffm-b5-link.telia.net", 920, true);
  add_nb(174, "be2040.ccr41.fra03.atlas.cogentco.com", 1120, true);
  add_nb(2914, "ae-12.r24.frnkge08.de.bb.gin.ntt.net", 980, false);

  p.set_sequence(9876543210ULL);
}

}  // namespace

struct PeerHandle {
  libpb::bench::PeerUpdate msg;
};

void PeerHandleDeleter::operator()(PeerHandle* p) const noexcept { delete p; }

PeerHandlePtr make_sample_handle() {
  PeerHandlePtr h(new PeerHandle());
  populate_sample(h->msg);
  return h;
}

PeerHandlePtr make_shifted_handle() {
  PeerHandlePtr h = make_sample_handle();
  h->msg.mutable_location()->set_datacenter(std::string(20480, 'd'));
  h->msg.mutable_neighbors(0)->set_router(std::string(18432, 'n'));
  return h;
}

PeerHandlePtr make_routing_32_handle() {
  PeerHandlePtr h = make_sample_handle();
  const auto base = h->msg.neighbors();
  h->msg.clear_neighbors();
  for (int rep = 0; rep < 8; ++rep) {
    for (const auto& nb : base) {
      *h->msg.add_neighbors() = nb;
    }
  }
  return h;
}

std::string encode_handle(const PeerHandle& h) {
  std::string out;
  if (!h.msg.SerializeToString(&out)) {
    out.clear();
  }
  return out;
}

bool decode_and_checksum(std::span<const uint8_t> wire, uint64_t& checksum_out) {
  libpb::bench::PeerUpdate msg;
  if (!msg.ParseFromArray(wire.data(), static_cast<int>(wire.size()))) {
    return false;
  }
  checksum_out = compute_pb_checksum(msg);
  return true;
}

bool lookup_city(std::span<const uint8_t> wire, std::string& city_out) {
  libpb::bench::PeerUpdate msg;
  if (!msg.ParseFromArray(wire.data(), static_cast<int>(wire.size())) || !msg.has_location()) {
    return false;
  }
  city_out = msg.location().city();
  return true;
}

bool mutate_sequence(std::span<const uint8_t> wire, std::string& out_wire) {
  libpb::bench::PeerUpdate msg;
  if (!msg.ParseFromArray(wire.data(), static_cast<int>(wire.size())) || !msg.has_location() ||
      msg.location().city() != "Frankfurt") {
    return false;
  }
  msg.set_sequence(msg.sequence() + 1ULL);
  return msg.SerializeToString(&out_wire);
}

bool mutate_embedded_grow(std::span<const uint8_t> wire,
                          std::string_view grown_dc,
                          std::string& out_wire) {
  libpb::bench::PeerUpdate msg;
  if (!msg.ParseFromArray(wire.data(), static_cast<int>(wire.size())) || !msg.has_location()) {
    return false;
  }
  msg.mutable_location()->set_datacenter(grown_dc.data(), grown_dc.size());
  return msg.SerializeToString(&out_wire);
}

std::string libprotobuf_version() {
  return google::protobuf::internal::VersionString(GOOGLE_PROTOBUF_VERSION);
}

}  // namespace omnibuf_bench_pb
