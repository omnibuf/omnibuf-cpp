#pragma once

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>

namespace omnibuf_bench_pb {

struct PeerHandle;

struct PeerHandleDeleter {
  void operator()(PeerHandle* p) const noexcept;
};

using PeerHandlePtr = std::unique_ptr<PeerHandle, PeerHandleDeleter>;

PeerHandlePtr make_sample_handle();
PeerHandlePtr make_shifted_handle();
PeerHandlePtr make_routing_32_handle();

std::string encode_handle(const PeerHandle& h);
bool decode_and_checksum(std::span<const uint8_t> wire, uint64_t& checksum_out);
bool lookup_city(std::span<const uint8_t> wire, std::string& city_out);
bool mutate_sequence(std::span<const uint8_t> wire, std::string& out_wire);
bool mutate_embedded_grow(std::span<const uint8_t> wire,
                          std::string_view grown_dc,
                          std::string& out_wire);
std::string libprotobuf_version();

}  // namespace omnibuf_bench_pb
