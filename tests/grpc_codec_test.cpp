#include <cstdint>
#include <cstdio>
#include <map>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "omnibuf/grpc.hpp"
#include "test_harness.hpp"

namespace {

struct RpcPeerEvent {
  uint32_t asn = 0;
  std::string router_id;
  int32_t rssi_dbm = 0;
  std::vector<std::string> tags;
  std::unordered_map<std::string, std::string> labels;

  bool operator==(const RpcPeerEvent&) const = default;
};

}  // namespace

template <>
struct omnibuf::StructTraits<RpcPeerEvent> {
  static constexpr std::string_view type_name = "omnibuf.test.RpcPeerEvent";
  static constexpr auto fields() {
    using T = RpcPeerEvent;
    return std::make_tuple(
        omnibuf::field<1, &T::asn>("asn"),
        omnibuf::field<2, &T::router_id>("router_id"),
        omnibuf::field<3, &T::rssi_dbm, omnibuf::WireOverride::Sint>("rssi_dbm"),
        omnibuf::field<4, &T::tags>("tags"),
        omnibuf::field<5, &T::labels>("labels"));
  }
};

namespace {

void test_constants() {
  EXPECT(omnibuf::grpc_codec::kCodecName == "omnibuf");
  EXPECT(omnibuf::grpc_codec::kSelfDescribingCodecName == "x-omnibuf");
  EXPECT(omnibuf::grpc_codec::kContentTypeGrpc == "application/grpc+omnibuf");
}

void test_compact_round_trip() {
  RpcPeerEvent original{
      .asn = 64512,
      .router_id = "edge-rtr-fra01",
      .rssi_dbm = -62,
      .tags = {"bgp", "ipv6"},
      .labels = {{"site", "fra"}, {"tier", "edge"}},
  };

  auto encoded = omnibuf::grpc_codec::serialize(original);
  EXPECT(encoded.ok());

  auto meta = omnibuf::read_field0_schema_meta(encoded.value());
  EXPECT(!meta.has_value());

  auto decoded = omnibuf::grpc_codec::deserialize<RpcPeerEvent>(encoded.value());
  EXPECT(decoded.ok());
  EXPECT(decoded.value() == original);
}

void test_self_describing_and_deterministic() {
  RpcPeerEvent msg_a{
      .asn = 100,
      .router_id = "iad-01",
      .rssi_dbm = -45,
      .tags = {"core"},
      .labels = {{"z", "1"}, {"a", "2"}, {"m", "3"}},
  };
  RpcPeerEvent msg_b{
      .asn = 100,
      .router_id = "iad-01",
      .rssi_dbm = -45,
      .tags = {"core"},
      .labels = {{"a", "2"}, {"m", "3"}, {"z", "1"}},
  };

  auto enc_a = omnibuf::grpc_codec::serialize(msg_a, /*deterministic=*/true,
                                              /*self_describing=*/true);
  auto enc_b = omnibuf::grpc_codec::serialize(msg_b, /*deterministic=*/true,
                                              /*self_describing=*/true);
  EXPECT(enc_a.ok());
  EXPECT(enc_b.ok());
  EXPECT(enc_a.value() == enc_b.value());

  auto meta = omnibuf::read_field0_schema_meta(enc_a.value());
  EXPECT(meta.has_value());
  EXPECT(meta->type_name == "RpcPeerEvent");
  EXPECT(meta->fields.at(1) == "asn");

  auto decoded = omnibuf::grpc_codec::deserialize<RpcPeerEvent>(enc_a.value());
  EXPECT(decoded.ok());
  EXPECT(decoded.value() == msg_a);
}

void test_decode_errors_and_limits() {
  const std::vector<uint8_t> malformed{0x00};
  auto bad = omnibuf::grpc_codec::deserialize<RpcPeerEvent>(malformed);
  EXPECT(!bad.ok());

  RpcPeerEvent large_tags{.tags = {"a", "b", "c", "d"}};
  auto encoded = omnibuf::grpc_codec::serialize(large_tags);
  EXPECT(encoded.ok());

  omnibuf::DecodeOptions tight_opts{};
  tight_opts.max_total_elements = 2;
  auto rejected = omnibuf::grpc_codec::deserialize<RpcPeerEvent>(encoded.value(), tight_opts);
  EXPECT(!rejected.ok());

  tight_opts.max_total_elements = 4;
  auto accepted = omnibuf::grpc_codec::deserialize<RpcPeerEvent>(encoded.value(), tight_opts);
  EXPECT(accepted.ok());
  EXPECT(accepted.value() == large_tags);
}

}  // namespace

void run_grpc_traits_mock_tests();

int main() {
  RUN_CASE(test_constants);
  RUN_CASE(test_compact_round_trip);
  RUN_CASE(test_self_describing_and_deterministic);
  RUN_CASE(test_decode_errors_and_limits);
  run_grpc_traits_mock_tests();
  std::printf("grpc_codec_test: %d passed, %d failed\n", omnibuf_test::g_passed,
              omnibuf_test::g_failed);
  return omnibuf_test::g_failed == 0 ? 0 : 1;
}
