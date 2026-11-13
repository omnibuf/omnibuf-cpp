#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "omnibuf/bench/peer.omnibuf.pb.hpp"
#include "omnibuf/conformance/conformance.omnibuf.pb.hpp"
#include "omnibuf/omnibuf.hpp"

namespace {

int g_passed = 0;
int g_failed = 0;

#define CHECK(cond)                                                               \
  do {                                                                            \
    if (!(cond)) {                                                                \
      std::fprintf(stderr, "FAIL [%s:%d]: %s\n", __FILE__, __LINE__, #cond);      \
      ++g_failed;                                                                 \
      return;                                                                     \
    }                                                                             \
  } while (0)

#define RUN_TEST(fn)                                     \
  do {                                                   \
    const int before = g_failed;                         \
    fn();                                                \
    if (g_failed == before) {                            \
      ++g_passed;                                        \
      std::printf("PASS %s\n", #fn);                     \
    }                                                    \
  } while (0)

struct RecursiveNode {
  uint32_t id = 0;
  std::string name{};
  int64_t weight = 0;
  std::unique_ptr<RecursiveNode> primary{};
  std::vector<RecursiveNode> children{};
  std::vector<std::unique_ptr<RecursiveNode>> branches{};
  std::map<std::string, uint64_t> labels{};
};

}  // namespace

template <>
struct omnibuf::StructTraits<RecursiveNode> {
  static constexpr std::string_view type_name = "omnibuf.test.RecursiveNode";
  static constexpr auto fields() {
    using T = RecursiveNode;
    return std::make_tuple(
        omnibuf::field<1, &T::id>("id"),
        omnibuf::field<2, &T::name>("name"),
        omnibuf::field<3, &T::weight, omnibuf::WireOverride::Sint>("weight"),
        omnibuf::field<4, &T::primary>("primary"),
        omnibuf::field<5, &T::children>("children"),
        omnibuf::field<6, &T::branches>("branches"),
        omnibuf::field<7, &T::labels>("labels"));
  }
};

namespace {

void test_prefix_varint_and_zigzag() {
  const uint64_t boundaries[] = {
      0ULL,
      1ULL,
      127ULL,
      128ULL,
      16511ULL,
      16512ULL,
      2113663ULL,
      2113664ULL,
      2113665ULL,
      (uint64_t{1} << 32) - 1,
      uint64_t{1} << 32,
      (uint64_t{1} << 53) + 7,
      std::numeric_limits<uint64_t>::max(),
  };

  for (uint64_t want : boundaries) {
    std::vector<uint8_t> buf;
    omnibuf::append_varint(buf, want);
    CHECK(buf.size() == omnibuf::varint_size(want));
    const auto [got, n] = omnibuf::decode_varint(buf);
    CHECK(n == buf.size());
    CHECK(got == want);
  }

  // Reserved prefixes 0x08..0xF8 with low 3 bits zero must be rejected
  for (uint32_t b = 0x08; b <= 0xF8; b += 8) {
    const uint8_t raw[9] = {static_cast<uint8_t>(b), 0, 0, 0, 0, 0, 0, 0, 0};
    const auto [val, n] = omnibuf::decode_varint(raw);
    (void)val;
    CHECK(n == 0);
  }

  // ZigZag round-trip at signed boundaries
  const int64_t signed_vals[] = {
      0,
      -1,
      1,
      -2,
      2,
      -64,
      64,
      std::numeric_limits<int32_t>::min(),
      std::numeric_limits<int32_t>::max(),
      std::numeric_limits<int64_t>::min(),
      std::numeric_limits<int64_t>::max(),
  };
  for (int64_t s : signed_vals) {
    const uint64_t zz = omnibuf::encode_zigzag(s);
    CHECK(omnibuf::decode_zigzag(zz) == s);
  }

  // Backpatch container length across 2-byte (<=16383) and 4-byte (>=16384) boundary
  for (size_t payload_sz : {size_t{0}, size_t{127}, size_t{16383}, size_t{16384}, size_t{20000}}) {
    std::vector<uint8_t> pkt = {0xAA, 0x00, 0x00};
    const size_t len_pos = 1;
    pkt.insert(pkt.end(), payload_sz, 0x42);
    omnibuf::backpatch_container_len(pkt, len_pos);
    const auto [decoded_len, ln] = omnibuf::decode_varint(std::span<const uint8_t>(pkt).subspan(1));
    CHECK(ln == (payload_sz <= 16383 ? 2u : 4u));
    CHECK(decoded_len == payload_sz);
    CHECK(pkt.size() == 1 + ln + payload_sz);
  }
}

void test_strict_utf8() {
  CHECK(omnibuf::is_valid_utf8("Frankfurt-am-Main 1234567890"));
  CHECK(omnibuf::is_valid_utf8("\xC3\xA4\xE2\x82\xAC\xF0\x9F\x9A\x80"));

  // Overlong 2-byte NUL
  const uint8_t overlong[] = {0xC0, 0x80};
  CHECK(!omnibuf::is_valid_utf8(std::span<const uint8_t>(overlong)));

  // UTF-16 surrogate half U+D800
  const uint8_t surrogate[] = {0xED, 0xA0, 0x80};
  CHECK(!omnibuf::is_valid_utf8(std::span<const uint8_t>(surrogate)));

  // Above U+10FFFF
  const uint8_t above_max[] = {0xF4, 0x90, 0x80, 0x80};
  CHECK(!omnibuf::is_valid_utf8(std::span<const uint8_t>(above_max)));

  // Truncated 3-byte sequence
  const uint8_t truncated[] = {0xE2, 0x82};
  CHECK(!omnibuf::is_valid_utf8(std::span<const uint8_t>(truncated)));
}

void test_encoder_overflow_and_depth_limit() {
  // Build 40 nested sub-messages via Encoder (exercises dynamic stack growth too)
  omnibuf::Encoder enc;
  constexpr int kDeepLevels = 40;
  for (int i = 0; i < kDeepLevels; ++i) {
    enc.begin_submsg(1);
  }
  enc.write_uint(1, 42);
  for (int i = 0; i < kDeepLevels; ++i) {
    enc.end_submsg();
  }
  const auto deep_pkt = std::move(enc).finish();

  // Depth 40 exceeds MAX_CONTAINER_DEPTH (32) and must be rejected without crashing
  CHECK(!omnibuf::decode_blind(deep_pkt).ok());
  omnibuf::conformance::TestAllTypes gen_msg;
  CHECK(!gen_msg.unmarshal_omnibuf(deep_pkt).ok());

  // Also verify Encoder handles >70 open containers (overflowing inline stack of 64)
  omnibuf::Encoder huge_enc;
  for (int i = 0; i < 72; ++i) {
    huge_enc.begin_submsg(1);
  }
  huge_enc.write_bool(1, true);
  for (int i = 0; i < 72; ++i) {
    huge_enc.end_submsg();
  }
  const auto huge_pkt = std::move(huge_enc).finish();
  CHECK(!huge_pkt.empty());
  CHECK(!omnibuf::validate_message_bytes(huge_pkt, 0).ok());

  // Exactly 32 nested levels must succeed
  omnibuf::Encoder ok_enc;
  for (int i = 0; i < omnibuf::MAX_CONTAINER_DEPTH; ++i) {
    ok_enc.begin_submsg(1);
  }
  ok_enc.write_uint(1, 7);
  for (int i = 0; i < omnibuf::MAX_CONTAINER_DEPTH; ++i) {
    ok_enc.end_submsg();
  }
  const auto ok_pkt = std::move(ok_enc).finish();
  CHECK(omnibuf::decode_blind(ok_pkt).ok());
}

void test_wire_invariants_and_extended() {
  // Empty array with trailing byte: field #9 WireArray, len=3, elem=WireString(10), count=0(0x01), extra=0x00
  std::vector<uint8_t> bad_empty_arr;
  omnibuf::append_tag(bad_empty_arr, 9, omnibuf::WireArray);
  omnibuf::append_varint(bad_empty_arr, 3);
  bad_empty_arr.push_back(omnibuf::WireString);
  omnibuf::append_varint(bad_empty_arr, 0);
  bad_empty_arr.push_back(0x00);
  CHECK(!omnibuf::decode_blind(bad_empty_arr).ok());
  omnibuf::conformance::TestAllTypes m1;
  CHECK(!m1.unmarshal_omnibuf(bad_empty_arr).ok());

  // Empty map with trailing byte
  std::vector<uint8_t> bad_empty_map;
  omnibuf::append_tag(bad_empty_map, 12, omnibuf::WireMap);
  omnibuf::append_varint(bad_empty_map, 4);
  bad_empty_map.push_back(omnibuf::WireString);
  bad_empty_map.push_back(omnibuf::WireUint);
  omnibuf::append_varint(bad_empty_map, 0);
  bad_empty_map.push_back(0x00);
  CHECK(!omnibuf::decode_blind(bad_empty_map).ok());
  CHECK(!m1.unmarshal_omnibuf(bad_empty_map).ok());

  // Bool array with non-canonical byte 0x02 must fail
  std::vector<uint8_t> bad_bool_arr;
  omnibuf::append_tag(bad_bool_arr, 1, omnibuf::WireArray);
  omnibuf::append_varint(bad_bool_arr, 3);
  bad_bool_arr.push_back(omnibuf::WireBoolFalse);
  omnibuf::append_varint(bad_bool_arr, 1);
  bad_bool_arr.push_back(0x02);
  CHECK(!omnibuf::decode_blind(bad_bool_arr).ok());

  // WireExtended with ExtType 0x42 (in 0x00..0x7F): skipped by schema decoders, rejected by blind
  std::vector<uint8_t> ext_pkt;
  omnibuf::append_tag(ext_pkt, 1, omnibuf::WireUint);
  omnibuf::append_varint(ext_pkt, 123);
  omnibuf::append_tag(ext_pkt, 99, omnibuf::WireExtended);
  omnibuf::append_varint(ext_pkt, 3);
  ext_pkt.push_back(0x42);
  ext_pkt.push_back(0xDE);
  ext_pkt.push_back(0xAD);

  CHECK(!omnibuf::decode_blind(ext_pkt).ok());
  omnibuf::conformance::TestAllTypes m2;
  CHECK(m2.unmarshal_omnibuf(ext_pkt).ok());
  CHECK(m2.f1_uint32 == 123);
}

void test_field0_and_blind_json() {
  omnibuf::conformance::TestAllTypes src;
  src.f1_uint32 = 42;
  src.f2_uint64 = 18446744073709551615ULL;
  src.f4_sint64 = std::numeric_limits<int64_t>::min();
  src.f6_string = "hello\n\"world\"";
  src.f7_bytes = {0xDE, 0xAD, 0xBE, 0xEF};
  src.f13_float32 = -0.0f;
  src.f14_float64 = 0.42875;
  src.f8_sub_msg = omnibuf::conformance::AllTypesSubMsg{
      .id = 7,
      .label = "nested",
      .flag = true,
      .score = -99,
  };

  const auto wire = src.marshal_omnibuf({.deterministic = true, .self_describing = true});
  CHECK(!wire.empty() && wire[0] == 0x19u);

  const auto meta = omnibuf::read_field0_schema_meta(wire);
  CHECK(meta.has_value());
  CHECK(meta->type_name == "TestAllTypes");
  CHECK(meta->fields.at(1) == "f1_uint32");
  CHECK(meta->children.at(8)->type_name == "AllTypesSubMsg");

  const auto json_res = omnibuf::decode_blind_json(wire);
  CHECK(json_res.ok());
  const std::string& js = json_res.value();
  CHECK(js.find("\"f2_uint64\":18446744073709551615") != std::string::npos);
  CHECK(js.find("\"f4_sint64\":-9223372036854775808") != std::string::npos);
  CHECK(js.find("\"f7_bytes\":\"3q2+7w==\"") != std::string::npos);
  CHECK(js.find("\"f13_float32\":-0") != std::string::npos);

  // Non-finite float falls back to "{}" when fallback_on_non_finite is enabled
  omnibuf::Encoder nan_enc;
  nan_enc.write_float64(1, std::numeric_limits<double>::quiet_NaN());
  const auto nan_pkt = std::move(nan_enc).finish();
  const auto nan_blind = omnibuf::decode_blind(nan_pkt);
  CHECK(nan_blind.ok());
  const std::string nan_json = omnibuf::stringify_blind_json(nan_blind.value(), true);
  CHECK(nan_json == "{}");

  // High 29-bit field numbers must not wrap into 16-bit
  omnibuf::Encoder hi_enc;
  hi_enc.write_uint(65536, 11);
  hi_enc.write_uint(536870911, 22);
  const auto hi_pkt = std::move(hi_enc).finish();
  const auto hi_json = omnibuf::decode_blind_json(hi_pkt);
  CHECK(hi_json.ok());
  CHECK(hi_json.value().find("\"field_65536\":11") != std::string::npos);
  CHECK(hi_json.value().find("\"field_536870911\":22") != std::string::npos);
}

void test_opaque_and_aliased_splice() {
  omnibuf::bench::PeerUpdate peer;
  peer.asn = 64512;
  peer.router_id = "edge-rtr-fra01";
  peer.active = true;
  peer.uptime_sec = 1849203;
  peer.rssi_dbm = -62;
  peer.cpu_load = 0.42875;
  peer.tags = {"tier1", "ebgp"};
  peer.prefixes = {167772160, 172032000};
  peer.location = omnibuf::bench::Location{
      .region = "eu-central-1",
      .datacenter = "FRA-FR5",
      .city = "Frankfurt",
      .rack = 42,
      .latitude = 50.1109f,
      .longitude = 8.6821f,
  };
  peer.neighbors = {
      omnibuf::bench::Neighbor{.peer_asn = 3356, .router = "level3", .latency_us = 850, .established = true},
  };
  peer.sequence = 9876543210ULL;

  const auto raw = peer.marshal_omnibuf();
  omnibuf::bench::OpaquePeerUpdate view(raw);
  CHECK(view.get_asn() == 64512);
  CHECK(view.get_location().get_city_view() == "Frankfurt");
  CHECK(view.neighbors_len() == 1);
  CHECK(view.get_neighbors(0).get_peer_asn() == 3356);

  view.set_sequence(9876543211ULL);
  const auto mutated = view.marshal();

  omnibuf::bench::PeerUpdate decoded;
  CHECK(decoded.unmarshal_omnibuf(mutated).ok());
  peer.sequence = 9876543211ULL;
  CHECK(decoded == peer);

  // Verify aliased prefix splice preserves prefix and never mutates raw tail
  std::vector<uint8_t> storage = {0xAA, 0xBB, 0xCC};
  storage.reserve(3 + raw.size() + 128);
  storage.insert(storage.end(), raw.begin(), raw.end());
  const auto splice_cb = [](std::vector<uint8_t>& b, uint16_t fnum) {
    omnibuf::append_tag(b, fnum, omnibuf::WireUint);
    omnibuf::append_varint(b, 9876543211ULL);
  };
  const auto got = omnibuf::splice_fields(
      std::span<uint8_t>(storage), 3, std::span<const uint8_t>(storage.data() + 3, raw.size()),
      uint64_t{1} << 11, 11, splice_cb);
  CHECK(got[0] == 0xAA && got[1] == 0xBB && got[2] == 0xCC);
  CHECK(std::equal(raw.begin(), raw.end(), storage.data() + 3));
  CHECK(std::equal(mutated.begin(), mutated.end(), got.begin() + 3, got.end()));

  // Also verify in-place splice_fields_into when raw aliases storage's unused capacity
  const uint8_t* raw_ptr = storage.data() + 3;
  const size_t raw_len = raw.size();
  storage.resize(3);
  omnibuf::splice_fields_into(storage, std::span<const uint8_t>(raw_ptr, raw_len),
                              uint64_t{1} << 11, 11, splice_cb);
  CHECK(storage == got);
}

void test_recursive_struct_and_descriptors() {
  RecursiveNode root;
  root.id = 1;
  root.name = "root";
  root.weight = -42;
  root.primary = std::make_unique<RecursiveNode>();
  root.primary->id = 2;
  root.primary->name = "primary-child";
  RecursiveNode c1;
  c1.id = 3;
  c1.name = "vec-child";
  root.children.push_back(std::move(c1));
  auto b1 = std::make_unique<RecursiveNode>();
  b1->id = 4;
  b1->name = "branch-ptr";
  root.branches.push_back(std::move(b1));
  root.labels["env"] = 7;

  const auto bytes_res = omnibuf::marshal(root, {.deterministic = true, .self_describing = true});
  CHECK(bytes_res.ok());

  RecursiveNode out;
  out.id = 999;
  out.primary = std::make_unique<RecursiveNode>();
  out.primary->id = 888;
  CHECK(omnibuf::unmarshal(bytes_res.value(), out).ok());
  CHECK(out.id == 1 && out.name == "root" && out.weight == -42);
  CHECK(out.primary != nullptr && out.primary->id == 2);
  CHECK(out.children.size() == 1 && out.children[0].id == 3);
  CHECK(out.branches.size() == 1 && out.branches[0]->id == 4);
  CHECK(out.labels.at("env") == 7);

  // DescriptorPool + DynamicMessage round-trip from schemas.fds.pb
#ifdef OMNIBUF_SCHEMAS_FDS_PATH
  omnibuf::proto::DescriptorPool pool;
  CHECK(pool.load_file(OMNIBUF_SCHEMAS_FDS_PATH).ok());
  const auto* desc = pool.find_message("omnibuf.conformance.TestAllTypes");
  CHECK(desc != nullptr);

  omnibuf::conformance::TestAllTypes orig;
  orig.f1_uint32 = 77;
  orig.f6_string = "dynamic";
  orig.f12_map_str_uint = {{"a", 1}, {"b", 2}};
  const auto orig_wire = orig.marshal_omnibuf({.deterministic = true});

  omnibuf::proto::DynamicMessage dmsg;
  CHECK(omnibuf::proto::from_binary(*desc, orig_wire, dmsg).ok());
  const auto dyn_wire = omnibuf::proto::to_binary(*desc, dmsg, {.deterministic = true});
  CHECK(dyn_wire.ok());
  CHECK(dyn_wire.value() == orig_wire);
#endif
}

}  // namespace

int main() {
  RUN_TEST(test_prefix_varint_and_zigzag);
  RUN_TEST(test_strict_utf8);
  RUN_TEST(test_encoder_overflow_and_depth_limit);
  RUN_TEST(test_wire_invariants_and_extended);
  RUN_TEST(test_field0_and_blind_json);
  RUN_TEST(test_opaque_and_aliased_splice);
  RUN_TEST(test_recursive_struct_and_descriptors);

  std::printf("\nSummary: %d passed, %d failed\n", g_passed, g_failed);
  return g_failed == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
