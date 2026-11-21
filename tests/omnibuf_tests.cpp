#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <map>
#include <memory>
#include <memory_resource>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "omnibuf/bench/peer.omnibuf.pb.hpp"
#include "omnibuf/conformance/conformance.omnibuf.pb.hpp"
#include "omnibuf/omnibuf.hpp"
#include "omnibuf/test/edge.omnibuf.pb.hpp"

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

#define CHECK_THROWS(expr, exc_type)                                              \
  do {                                                                            \
    bool caught = false;                                                          \
    try {                                                                         \
      (void)(expr);                                                               \
    } catch (const exc_type&) {                                                   \
      caught = true;                                                              \
    }                                                                             \
    if (!caught) {                                                                \
      std::fprintf(stderr, "FAIL [%s:%d]: expected %s from %s\n", __FILE__,       \
                   __LINE__, #exc_type, #expr);                                   \
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
  omnibuf::Box<RecursiveNode> boxed_backup{};
  std::vector<RecursiveNode> children{};
  std::vector<std::unique_ptr<RecursiveNode>> branches{};
  std::map<std::string, uint64_t> labels{};
};

struct UnorderedMapShowcase {
  std::unordered_map<std::string, uint64_t> metrics{};
  std::unordered_map<int32_t, std::string> sint_keyed{};
  std::map<uint64_t, uint32_t> fixed_keyed{};
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
        omnibuf::field<7, &T::labels>("labels"),
        omnibuf::field<8, &T::boxed_backup>("boxed_backup"));
  }
};

template <>
struct omnibuf::StructTraits<UnorderedMapShowcase> {
  static constexpr std::string_view type_name = "omnibuf.test.UnorderedMapShowcase";
  static constexpr auto fields() {
    using T = UnorderedMapShowcase;
    return std::make_tuple(
        omnibuf::field<1, &T::metrics>("metrics"),
        omnibuf::field<2, &T::sint_keyed, omnibuf::WireOverride::Default,
                       omnibuf::WireOverride::Sint>("sint_keyed"),
        omnibuf::field<3, &T::fixed_keyed, omnibuf::WireOverride::Fixed32,
                       omnibuf::WireOverride::Fixed64>("fixed_keyed"));
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

  // Verify Encoder handles >70 open containers (overflowing inline stack of 64)
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

  // >127 submessage array elements must expand the 1B count slot via patch_reserved_varint
  omnibuf::Encoder arr_enc;
  arr_enc.begin_submsg_arr(9);
  for (uint32_t i = 0; i < 150; ++i) {
    arr_enc.begin_submsg_elem();
    arr_enc.write_uint(1, i + 1000u);
    arr_enc.end_submsg_elem();
  }
  arr_enc.end_submsg_arr();
  const auto arr_pkt = std::move(arr_enc).finish();
  const auto arr_blind = omnibuf::decode_blind(arr_pkt);
  CHECK(arr_blind.ok());
  const auto* f9_arr =
      std::get_if<omnibuf::BlindArray>(&arr_blind.value().at("field_9").v);
  CHECK(f9_arr != nullptr && f9_arr->size() == 150);

  // Stack underflow and unclosed container guards
  omnibuf::Encoder underflow_enc;
  CHECK_THROWS(underflow_enc.end_submsg(), std::logic_error);
  omnibuf::Encoder unclosed_enc;
  unclosed_enc.begin_submsg(1);
  CHECK_THROWS(std::move(unclosed_enc).finish(), std::logic_error);
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

  // Public stringify_blind_json / decode_blind_json must return an error on NaN and Inf
  for (double bad_f : {std::numeric_limits<double>::quiet_NaN(),
                       std::numeric_limits<double>::infinity(),
                       -std::numeric_limits<double>::infinity()}) {
    omnibuf::Encoder nan_enc;
    nan_enc.write_float64(1, bad_f);
    const auto nan_pkt = std::move(nan_enc).finish();
    const auto nan_blind = omnibuf::decode_blind(nan_pkt);
    CHECK(nan_blind.ok());
    CHECK(!omnibuf::stringify_blind_json(nan_blind.value()).ok());
    CHECK(!omnibuf::decode_blind_json(nan_pkt).ok());
  }

  // Recursive TreeNode self_describing Field #0 must preserve child field names across depths
  omnibuf::conformance::TreeNode tree;
  tree.id = 1;
  tree.name = "root";
  tree.weight = -5;
  omnibuf::conformance::TreeNode left;
  left.id = 2;
  left.name = "left";
  omnibuf::conformance::TreeNode leaf;
  leaf.id = 3;
  leaf.name = "leaf";
  left.primary.emplace(std::move(leaf));
  tree.primary.emplace(std::move(left));

  const auto tree_wire = tree.marshal_omnibuf({.deterministic = true, .self_describing = true});
  const auto tree_obj = omnibuf::decode_blind(tree_wire);
  CHECK(tree_obj.ok());
  const auto* p1 = std::get_if<omnibuf::BlindObject>(&tree_obj.value().at("primary").v);
  CHECK(p1 != nullptr && std::get<std::string>(p1->at("name").v) == "left");
  const auto* p2 = std::get_if<omnibuf::BlindObject>(&p1->at("primary").v);
  CHECK(p2 != nullptr && std::get<std::string>(p2->at("name").v) == "leaf");
  const auto tree_json = omnibuf::decode_blind_json(tree_wire);
  CHECK(tree_json.ok());
  CHECK(tree_json.value().find("\"name\":\"left\"") != std::string::npos);
  CHECK(tree_json.value().find("\"name\":\"leaf\"") != std::string::npos);

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
  peer.cpu_load = -0.0;
  peer.tags = {"tier1", "ebgp"};
  peer.prefixes = {167772160, 172032000};
  peer.location = omnibuf::bench::Location{
      .region = "eu-central-1",
      .datacenter = "FRA-FR5",
      .city = "Frankfurt",
      .rack = 42,
      .latitude = -0.0f,
      .longitude = 8.6821f,
  };
  peer.neighbors = {
      omnibuf::bench::Neighbor{
          .peer_asn = 3356, .router = "level3", .latency_us = 850, .established = true},
  };
  peer.sequence = 9876543210ULL;

  const auto raw = peer.marshal_omnibuf();
  omnibuf::bench::OpaquePeerUpdate view(raw);
  CHECK(view.validate().ok());
  CHECK(view.get_asn() == 64512);
  CHECK(omnibuf::float64_to_bits(view.get_cpu_load()) == omnibuf::float64_to_bits(-0.0));
  CHECK(omnibuf::float32_to_bits(view.get_location().get_latitude()) ==
        omnibuf::float32_to_bits(-0.0f));
  CHECK(view.get_location().get_city_view() == "Frankfurt");
  CHECK(view.neighbors_len() == 1);
  CHECK(view.get_neighbors(0).get_peer_asn() == 3356);

  view.set_sequence(9876543211ULL);
  const auto try_mutated = view.try_marshal();
  CHECK(try_mutated.ok());
  const auto& mutated = try_mutated.value();

  omnibuf::bench::PeerUpdate decoded;
  CHECK(decoded.unmarshal_omnibuf(mutated).ok());
  peer.sequence = 9876543211ULL;
  CHECK(decoded == peer);
  CHECK(omnibuf::float64_to_bits(decoded.cpu_load) == omnibuf::float64_to_bits(-0.0));
  CHECK(omnibuf::float32_to_bits(decoded.location->latitude) == omnibuf::float32_to_bits(-0.0f));

  // Verify aliased prefix splice preserves prefix and never mutates raw tail
  std::vector<uint8_t> storage = {0xAA, 0xBB, 0xCC};
  storage.reserve(3 + raw.size() + 128);
  storage.insert(storage.end(), raw.begin(), raw.end());
  const auto splice_cb = [](std::vector<uint8_t>& b, uint32_t fnum) -> const char* {
    omnibuf::append_tag(b, fnum, omnibuf::WireUint);
    omnibuf::append_varint(b, 9876543211ULL);
    return nullptr;
  };
  const auto got = omnibuf::splice_fields(
      std::span<uint8_t>(storage), 3, std::span<const uint8_t>(storage.data() + 3, raw.size()),
      uint64_t{1} << 11, 11, splice_cb);
  CHECK(got[0] == 0xAA && got[1] == 0xBB && got[2] == 0xCC);
  CHECK(std::equal(raw.begin(), raw.end(), storage.data() + 3));
  CHECK(std::equal(mutated.begin(), mutated.end(), got.begin() + 3, got.end()));

  // ASan-safe self-aliasing splice_fields_into (live elements inside dst; appends after prefix)
  std::vector<uint8_t> self_buf = raw;
  omnibuf::splice_fields_into(self_buf, std::span<const uint8_t>(self_buf.data(), self_buf.size()),
                              uint64_t{1} << 11, 11, splice_cb);
  CHECK(self_buf.size() == raw.size() + mutated.size());
  CHECK(std::equal(raw.begin(), raw.end(), self_buf.begin()));
  CHECK(std::equal(mutated.begin(), mutated.end(),
                   self_buf.begin() + static_cast<std::ptrdiff_t>(raw.size()), self_buf.end()));

  // Corrupt packet splicing must fail cleanly via try_marshal()
  std::vector<uint8_t> corrupt = raw;
  corrupt.push_back(0xFF);
  omnibuf::bench::OpaquePeerUpdate bad_view(corrupt);
  bad_view.set_sequence(42);
  CHECK(!bad_view.try_marshal().ok());
}

void test_edge_showcase_and_wide_message() {
  using omnibuf::test::EdgeShowcase;
  using omnibuf::test::EdgeStatus;
  using omnibuf::test::OpaqueEdgeShowcase;
  using omnibuf::test::OpaqueWideMessage;
  using omnibuf::test::WideMessage;

  // 1. proto3_optional distinguishes unset from explicit zero; zero-valued active oneof preserved
  EdgeShowcase es;
  es.opt_count = 0;  // explicit zero!
  es.opt_flag = false;
  // opt_label and opt_status intentionally left std::nullopt
  es.statuses = {EdgeStatus::EDGE_STATUS_READY, EdgeStatus::EDGE_STATUS_OFFLINE};
  es.modes = {EdgeShowcase::NestedMode::NESTED_MODE_FAST,
              EdgeShowcase::NestedMode::NESTED_MODE_SAFE};

  EdgeShowcase::InnerPayload inner1;
  inner1.code = 10;
  inner1.detail = "head";
  EdgeShowcase::InnerPayload inner2;
  inner2.code = 20;
  inner2.detail = "tail";
  inner1.next_inner.emplace(std::move(inner2));
  es.by_id[-7] = inner1;
  es.by_flag[true] = EdgeStatus::EDGE_STATUS_DRAINING;
  es.by_flag[false] = EdgeStatus::EDGE_STATUS_UNSPECIFIED;
  es.annotations["k"] = "v";
  es.blobs[0x0102030405060708ULL] = {0xCA, 0xFE};
  es.set_action_code(0);  // zero-valued active oneof branch!
  es.and_ = "kw-and";
  es.class_ = 99;
  es.clear_ = true;
  es.schema_meta_ = 1234;

  const auto wire_res = es.try_marshal_omnibuf({.deterministic = true, .self_describing = true});
  CHECK(wire_res.ok());
  const auto& wire = wire_res.value();

  EdgeShowcase out;
  out.opt_label = "stale";
  out.set_action_name("stale-branch");
  CHECK(out.unmarshal_omnibuf(wire).ok());
  CHECK(out == es);
  CHECK(out.opt_count.has_value() && *out.opt_count == 0);
  CHECK(out.opt_flag.has_value() && !*out.opt_flag);
  CHECK(!out.opt_label.has_value());
  CHECK(!out.opt_status.has_value());
  CHECK(out.has_action_code() && out.action_code == 0);
  CHECK(!out.has_action_name());

  // Wire-type mismatch on another oneof branch must NOT clobber the active branch
  std::vector<uint8_t> mismatch_wire = es.marshal_omnibuf();
  // Field 12 (action_name) expects WireString, append WireUint instead -> skipped, branch 11 stays active
  omnibuf::append_tag(mismatch_wire, 12, omnibuf::WireUint);
  omnibuf::append_varint(mismatch_wire, 777);
  EdgeShowcase mismatch_out;
  CHECK(mismatch_out.unmarshal_omnibuf(mismatch_wire).ok());
  CHECK(mismatch_out.has_action_code() && mismatch_out.action_code == 0);

  // OpaqueEdgeShowcase getters, lookups, and oneof splice
  OpaqueEdgeShowcase oview(wire);
  CHECK(oview.validate().ok());
  CHECK(oview.get_opt_count() == std::optional<uint32_t>{0});
  CHECK(oview.get_opt_label() == std::nullopt);
  CHECK(oview.get_statuses() == es.statuses);
  CHECK(oview.get_modes() == es.modes);
  CHECK(oview.lookup_by_flag(true).second &&
        oview.lookup_by_flag(true).first == EdgeStatus::EDGE_STATUS_DRAINING);
  CHECK(oview.lookup_annotations("k") == std::make_pair(std::string("v"), true));
  CHECK(oview.lookup_blobs(0x0102030405060708ULL).second);
  const auto [sub_view, sub_found] = oview.lookup_by_id(-7);
  CHECK(sub_found && sub_view.get_code() == 10 && sub_view.get_detail_view() == "head");
  CHECK(oview.get_and_view() == "kw-and" && oview.get_class() == 99 && oview.get_clear());

  // Mutate oneof branch via OpaqueEdgeShowcase (switches from action_code=0 to action_name="go")
  oview.set_action_name("go");
  oview.set_opt_label(std::string("now-set"));
  const auto spliced_es = oview.try_marshal();
  CHECK(spliced_es.ok());
  EdgeShowcase decoded_spliced;
  CHECK(decoded_spliced.unmarshal_omnibuf(spliced_es.value()).ok());
  CHECK(decoded_spliced.has_action_name() && decoded_spliced.action_name == "go");
  CHECK(!decoded_spliced.has_action_code());
  CHECK(decoded_spliced.opt_label == std::optional<std::string>{"now-set"});

  // 2. WideMessage (>64 fields): tests multi-word seen_words and Opaque dirty_ bitset on field #70
  WideMessage wide;
  wide.f1 = 11;
  wide.f64 = 640;
  wide.f65 = 650;
  wide.f66_str = "sixty-six";
  wide.f67_sub = EdgeShowcase::InnerPayload{.code = 67, .detail = "sub67", .next_inner = {}};
  wide.f69_arr = {100, 200, 300};
  wide.f70_tail = 70000000000ULL;

  const auto wide_wire = wide.marshal_omnibuf({.deterministic = true});
  WideMessage wide_dec;
  CHECK(wide_dec.unmarshal_omnibuf(wide_wire).ok());
  CHECK(wide_dec == wide);

  OpaqueWideMessage wide_view(wide_wire);
  CHECK(wide_view.get_f1() == 11);
  CHECK(wide_view.get_f64() == 640);
  CHECK(wide_view.get_f65() == 650);
  CHECK(wide_view.get_f66_str_view() == "sixty-six");
  CHECK(wide_view.get_f70_tail() == 70000000000ULL);

  wide_view.set_f1(12);
  wide_view.set_f65(651);
  wide_view.set_f70_tail(99999999999ULL);
  const auto wide_spliced = wide_view.try_marshal();
  CHECK(wide_spliced.ok());
  WideMessage wide_after;
  CHECK(wide_after.unmarshal_omnibuf(wide_spliced.value()).ok());
  CHECK(wide_after.f1 == 12 && wide_after.f64 == 640 && wide_after.f65 == 651);
  CHECK(wide_after.f70_tail == 99999999999ULL);
}

void test_large_container_shift_and_limits() {
  // >16 KiB nested sub-message + >16 KiB submsg array backpatch expansion round-trip
  omnibuf::bench::PeerUpdate large;
  large.asn = 64512;
  large.router_id = "edge-rtr-large";
  large.location = omnibuf::bench::Location{
      .region = "eu-central-1",
      .datacenter = std::string(20480, 'd'),
      .city = "Frankfurt",
      .rack = 42,
      .latitude = 50.1109f,
      .longitude = 8.6821f,
  };
  large.neighbors = {
      omnibuf::bench::Neighbor{
          .peer_asn = 3356, .router = std::string(18432, 'n'), .latency_us = 850, .established = true},
  };
  large.sequence = 7;

  const auto large_wire = large.marshal_omnibuf();
  CHECK(large_wire.size() > 38912);
  omnibuf::bench::PeerUpdate large_dec;
  CHECK(large_dec.unmarshal_omnibuf(large_wire).ok());
  CHECK(large_dec == large);

  // DecodeOptions limits across generated, StructTraits, Blind, and Opaque::validate()
  CHECK(!large_dec.unmarshal_omnibuf(large_wire, {.max_message_bytes = 4096}).ok());
  CHECK(!omnibuf::decode_blind(large_wire, {.max_message_bytes = 4096}).ok());
  omnibuf::bench::OpaquePeerUpdate large_view(large_wire);
  CHECK(!large_view.validate({.max_message_bytes = 4096}).ok());

  // max_total_elements limit on array elements
  omnibuf::bench::PeerUpdate many_prefixes;
  many_prefixes.prefixes.assign(200, 0x0A000001u);
  const auto pref_wire = many_prefixes.marshal_omnibuf();
  omnibuf::bench::PeerUpdate pref_out;
  CHECK(!pref_out.unmarshal_omnibuf(pref_wire, {.max_total_elements = 50}).ok());
  CHECK(!omnibuf::decode_blind(pref_wire, {.max_total_elements = 50}).ok());

  // max_depth limit
  omnibuf::bench::PeerUpdate nested_peer;
  nested_peer.location = omnibuf::bench::Location{.city = "Berlin"};
  const auto nested_wire = nested_peer.marshal_omnibuf();
  CHECK(!pref_out.unmarshal_omnibuf(nested_wire, {.max_depth = 0}).ok());
  CHECK(pref_out.unmarshal_omnibuf(nested_wire, {.max_depth = 2}).ok());
}

void test_recursive_struct_unordered_map_and_descriptors() {
  RecursiveNode root;
  root.id = 1;
  root.name = "root";
  root.weight = -42;
  root.primary = std::make_unique<RecursiveNode>();
  root.primary->id = 2;
  root.primary->name = "primary-child";
  RecursiveNode backup;
  backup.id = 9;
  backup.name = "boxed";
  root.boxed_backup.emplace(std::move(backup));
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
  CHECK(out.boxed_backup.has_value() && out.boxed_backup->id == 9);
  CHECK(out.children.size() == 1 && out.children[0].id == 3);
  CHECK(out.branches.size() == 1 && out.branches[0]->id == 4);
  CHECK(out.labels.at("env") == 7);

  // Unordered map canonical deterministic key sorting + KeyOverride (Sint & Fixed64)
  UnorderedMapShowcase um1;
  um1.metrics = {{"zeta", 3}, {"alpha", 1}, {"mu", 2}};
  um1.sint_keyed = {{-100, "neg"}, {50, "pos"}, {0, "zero"}};
  um1.fixed_keyed = {{0xAABBCCDDEEFF0011ULL, 77}};

  UnorderedMapShowcase um2;
  um2.metrics = {{"mu", 2}, {"zeta", 3}, {"alpha", 1}};
  um2.sint_keyed = {{50, "pos"}, {-100, "neg"}, {0, "zero"}};
  um2.fixed_keyed = {{0xAABBCCDDEEFF0011ULL, 77}};

  const auto um1_wire = omnibuf::marshal(um1, {.deterministic = true});
  const auto um2_wire = omnibuf::marshal(um2, {.deterministic = true});
  CHECK(um1_wire.ok() && um2_wire.ok());
  CHECK(um1_wire.value() == um2_wire.value());

  UnorderedMapShowcase um_out;
  CHECK(omnibuf::unmarshal(um1_wire.value(), um_out).ok());
  CHECK(um_out.metrics == um1.metrics);
  CHECK(um_out.sint_keyed == um1.sint_keyed);
  CHECK(um_out.fixed_keyed == um1.fixed_keyed);

  // DescriptorPool validation of unsupported constructs
  {
    auto make_single_field_fds = [](omnibuf::proto::FieldDescriptorProto f,
                                    std::string syntax = "proto2") {
      omnibuf::proto::FileDescriptorSet fds;
      omnibuf::proto::FileDescriptorProto file;
      file.name = "bad.proto";
      file.package = "bad";
      file.syntax = std::move(syntax);
      omnibuf::proto::DescriptorProto msg;
      msg.name = "BadMsg";
      msg.fields.push_back(std::move(f));
      file.message_types.push_back(std::move(msg));
      fds.files.push_back(std::move(file));
      return fds;
    };

    // 1. TYPE_GROUP rejected
    omnibuf::proto::FieldDescriptorProto grp;
    grp.name = "g";
    grp.number = 1;
    grp.label = omnibuf::proto::FieldLabel::Optional;
    grp.type = omnibuf::proto::FieldType::Group;
    grp.has_type = true;
    omnibuf::proto::DescriptorPool p1;
    CHECK(!p1.add_file_descriptor_set(make_single_field_fds(grp)).ok());

    // 2. LABEL_REQUIRED rejected
    omnibuf::proto::FieldDescriptorProto req;
    req.name = "r";
    req.number = 1;
    req.label = omnibuf::proto::FieldLabel::Required;
    req.type = omnibuf::proto::FieldType::Uint32;
    req.has_type = true;
    omnibuf::proto::DescriptorPool p2;
    CHECK(!p2.add_file_descriptor_set(make_single_field_fds(req)).ok());

    // 3. Custom proto2 default_value rejected
    omnibuf::proto::FieldDescriptorProto def;
    def.name = "d";
    def.number = 1;
    def.label = omnibuf::proto::FieldLabel::Optional;
    def.type = omnibuf::proto::FieldType::Uint32;
    def.has_type = true;
    def.default_value = "42";
    omnibuf::proto::DescriptorPool p3;
    CHECK(!p3.add_file_descriptor_set(make_single_field_fds(def)).ok());

    // 4. Unknown FieldType / FieldLabel rejected
    omnibuf::proto::FieldDescriptorProto unk_t;
    unk_t.name = "u";
    unk_t.number = 1;
    unk_t.label = omnibuf::proto::FieldLabel::Optional;
    unk_t.type = omnibuf::proto::FieldType::Unknown;
    unk_t.has_type = false;
    omnibuf::proto::DescriptorPool p4;
    CHECK(!p4.add_file_descriptor_set(make_single_field_fds(unk_t)).ok());

    omnibuf::proto::FieldDescriptorProto unk_l;
    unk_l.name = "u";
    unk_l.number = 1;
    unk_l.label = omnibuf::proto::FieldLabel::Unknown;
    unk_l.type = omnibuf::proto::FieldType::Uint32;
    unk_l.has_type = true;
    omnibuf::proto::DescriptorPool p5;
    CHECK(!p5.add_file_descriptor_set(make_single_field_fds(unk_l)).ok());

    // 5. Proto2 extensions rejected
    omnibuf::proto::FileDescriptorSet ext_fds;
    omnibuf::proto::FileDescriptorProto ext_file;
    ext_file.name = "ext.proto";
    ext_file.syntax = "proto2";
    ext_file.has_extensions = true;
    ext_fds.files.push_back(std::move(ext_file));
    omnibuf::proto::DescriptorPool p6;
    CHECK(!p6.add_file_descriptor_set(ext_fds).ok());
  }

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

  // Also verify DynamicMessage respects DecodeOptions resource limits
  CHECK(!omnibuf::proto::from_binary(*desc, orig_wire, dmsg, {.max_message_bytes = 4}).ok());
#endif
}

}  // namespace

struct OwnedPacket {
  std::string name;
  std::vector<uint8_t> payload;
  std::vector<std::string> tags;
  std::vector<std::vector<uint8_t>> chunks;
  std::map<std::string, uint64_t> counters;
};

struct ZeroCopyPacket {
  std::string_view name;
  std::span<const uint8_t> payload;
  std::vector<std::string_view> tags;
  std::vector<std::span<const uint8_t>> chunks;
  std::map<std::string_view, uint64_t> counters;
};

struct PmrPacket {
  std::pmr::string name;
  std::pmr::vector<uint8_t> payload;
  std::pmr::vector<std::pmr::string> tags;
  std::pmr::vector<uint32_t> numbers;
  std::pmr::map<std::pmr::string, uint64_t> counters;
  uint32_t seq = 0;
};

namespace omnibuf {

template <>
struct StructTraits<OwnedPacket> {
  static constexpr std::string_view type_name = "OwnedPacket";
  static constexpr auto fields() {
    return std::make_tuple(field<1, &OwnedPacket::name>("name"),
                           field<2, &OwnedPacket::payload>("payload"),
                           field<3, &OwnedPacket::tags>("tags"),
                           field<4, &OwnedPacket::chunks>("chunks"),
                           field<5, &OwnedPacket::counters>("counters"));
  }
};

template <>
struct StructTraits<ZeroCopyPacket> {
  static constexpr std::string_view type_name = "ZeroCopyPacket";
  static constexpr auto fields() {
    return std::make_tuple(field<1, &ZeroCopyPacket::name>("name"),
                           field<2, &ZeroCopyPacket::payload>("payload"),
                           field<3, &ZeroCopyPacket::tags>("tags"),
                           field<4, &ZeroCopyPacket::chunks>("chunks"),
                           field<5, &ZeroCopyPacket::counters>("counters"));
  }
};

template <>
struct StructTraits<PmrPacket> {
  static constexpr std::string_view type_name = "PmrPacket";
  static constexpr auto fields() {
    return std::make_tuple(field<1, &PmrPacket::name>("name"),
                           field<2, &PmrPacket::payload>("payload"),
                           field<3, &PmrPacket::tags>("tags"),
                           field<4, &PmrPacket::numbers>("numbers"),
                           field<5, &PmrPacket::counters>("counters"),
                           field<6, &PmrPacket::seq>("seq"));
  }
};

}  // namespace omnibuf

void test_fast_writer_aliasing() {
  for (size_t headroom : {size_t{0}, size_t{64}}) {
    std::vector<uint8_t> bytes{1, 2, 3, 4};
    omnibuf::FastWriter writer(bytes, headroom);
    writer.write_bytes(bytes.data(), 4);
    writer.finish();
    CHECK(bytes == (std::vector<uint8_t>{1, 2, 3, 4, 1, 2, 3, 4}));
  }
  std::vector<uint8_t> bytes{1, 2, 3, 4};
  omnibuf::FastWriter writer(bytes, 0);
  writer.write_bytes(bytes.data() + 1, 2);
  writer.finish();
  CHECK(bytes == (std::vector<uint8_t>{1, 2, 3, 4, 2, 3}));
}

void test_zero_copy_view_and_perf_upgrades() {
  // 1. Lightweight <Msg>View zero-copy getters
  static_assert(sizeof(omnibuf::bench::PeerUpdateView) == sizeof(std::span<const uint8_t>));
  static_assert(std::is_trivially_copyable_v<omnibuf::bench::PeerUpdateView>);

  omnibuf::bench::PeerUpdate peer;
  peer.asn = 65001;
  peer.router_id = "edge-fra-01";
  peer.sequence = 4242;
  peer.active = true;
  peer.prefixes = {10u, 20u, 30u};
  peer.location = omnibuf::bench::Location{.region = "eu-central", .city = "Frankfurt"};
  const std::vector<uint8_t> wire = peer.marshal_omnibuf();

  const omnibuf::bench::PeerUpdateView view(wire);
  CHECK(view.validate().ok());
  CHECK(view.get_asn() == 65001u);
  CHECK(view.get_sequence() == 4242u);
  CHECK(view.get_active());
  CHECK(view.prefixes_len() == 3u);

  const std::string_view rid = view.get_router_id();
  CHECK(rid == "edge-fra-01");
  CHECK(reinterpret_cast<const uint8_t*>(rid.data()) >= wire.data() &&
        reinterpret_cast<const uint8_t*>(rid.data()) < wire.data() + wire.size());

  const omnibuf::bench::LocationView loc = view.get_location();
  CHECK(!loc.empty());
  CHECK(loc.get_region() == "eu-central");
  const std::string_view city = loc.get_city();
  CHECK(city == "Frankfurt");
  CHECK(reinterpret_cast<const uint8_t*>(city.data()) >= wire.data() &&
        reinterpret_cast<const uint8_t*>(city.data()) < wire.data() + wire.size());

  const auto try_loc = view.try_get_location();
  CHECK(try_loc.ok() && try_loc.value().has_value());
  CHECK(try_loc.value()->get_city() == "Frankfurt");

  // Oneof mutual exclusion on View over concatenated packets
  omnibuf::test::EdgeShowcase es1;
  es1.set_action_code(77);
  omnibuf::test::EdgeShowcase es2;
  es2.set_action_name("run");
  std::vector<uint8_t> concat = es1.marshal_omnibuf();
  const auto es2_wire = es2.marshal_omnibuf();
  concat.insert(concat.end(), es2_wire.begin(), es2_wire.end());

  const omnibuf::test::EdgeShowcaseView ev(concat);
  CHECK(ev.get_action_code() == 0);
  CHECK(ev.get_action_name() == "run");

  // 2. Capacity-preserving clear_unseen reuse on decode_omnibuf_raw
  omnibuf::bench::PeerUpdate big_peer;
  big_peer.asn = 65100;
  big_peer.router_id = std::string(1024, 'R');
  big_peer.prefixes.assign(256, 9u);
  big_peer.location = omnibuf::bench::Location{.region = "us", .city = std::string(512, 'C')};
  const auto big_wire = big_peer.marshal_omnibuf();

  omnibuf::bench::PeerUpdate reuse;
  CHECK(reuse.unmarshal_omnibuf(big_wire).ok());
  const size_t rid_cap = reuse.router_id.capacity();
  const size_t pref_cap = reuse.prefixes.capacity();
  const size_t city_cap = reuse.location->city.capacity();

  omnibuf::bench::PeerUpdate small_peer;
  small_peer.asn = 12;
  small_peer.router_id = "ok";
  small_peer.location = omnibuf::bench::Location{.region = "ap"};
  const auto small_wire = small_peer.marshal_omnibuf();

  CHECK(reuse.unmarshal_omnibuf(small_wire).ok());
  CHECK(reuse.asn == 12u);
  CHECK(reuse.router_id == "ok");
  CHECK(reuse.router_id.capacity() >= rid_cap);
  CHECK(reuse.prefixes.empty());
  CHECK(reuse.prefixes.capacity() >= pref_cap);
  CHECK(reuse.location.has_value() && reuse.location->region == "ap" &&
        reuse.location->city.empty());
  CHECK(reuse.location->city.capacity() >= city_cap);

  // 3. >16 KiB container backpatch heuristic wire equivalence
  omnibuf::bench::PeerUpdate huge_peer;
  huge_peer.asn = 65200;
  huge_peer.location = omnibuf::bench::Location{
      .region = "eu",
      .city = std::string(24000, 'A'),
  };
  CHECK(huge_peer.location->min_wire_size() > 16383u);
  const auto huge_wire = huge_peer.marshal_omnibuf();
  omnibuf::bench::PeerUpdate huge_dec;
  CHECK(huge_dec.unmarshal_omnibuf(huge_wire).ok());
  CHECK(huge_dec == huge_peer);

  // 4. Zero-copy StructTraits fields (std::string_view and std::span<const uint8_t>)
  OwnedPacket src_pkt;
  src_pkt.name = "zero-copy-endpoint";
  src_pkt.payload = {0xDE, 0xAD, 0xBE, 0xEF, 0x42};
  src_pkt.tags = {"fast", "zero-copy", "simd"};
  src_pkt.chunks = {{1, 2, 3}, {4, 5}};
  src_pkt.counters = {{"rx", 1001}, {"tx", 2002}};

  const auto pkt_wire_res = omnibuf::marshal(src_pkt, {.deterministic = true});
  CHECK(pkt_wire_res.ok());
  const auto& pkt_wire = pkt_wire_res.value();

  ZeroCopyPacket zc_pkt;
  CHECK(omnibuf::unmarshal(pkt_wire, zc_pkt).ok());
  CHECK(zc_pkt.name == "zero-copy-endpoint");
  CHECK(reinterpret_cast<const uint8_t*>(zc_pkt.name.data()) >= pkt_wire.data() &&
        reinterpret_cast<const uint8_t*>(zc_pkt.name.data()) < pkt_wire.data() + pkt_wire.size());
  CHECK(zc_pkt.payload.size() == 5u && zc_pkt.payload[0] == 0xDE && zc_pkt.payload[4] == 0x42);
  CHECK(zc_pkt.payload.data() >= pkt_wire.data() &&
        zc_pkt.payload.data() < pkt_wire.data() + pkt_wire.size());
  CHECK(zc_pkt.tags.size() == 3u && zc_pkt.tags[1] == "zero-copy");
  CHECK(reinterpret_cast<const uint8_t*>(zc_pkt.tags[1].data()) >= pkt_wire.data() &&
        reinterpret_cast<const uint8_t*>(zc_pkt.tags[1].data()) <
            pkt_wire.data() + pkt_wire.size());
  CHECK(zc_pkt.chunks.size() == 2u && zc_pkt.chunks[0].size() == 3u);
  CHECK(zc_pkt.chunks[0].data() >= pkt_wire.data() &&
        zc_pkt.chunks[0].data() < pkt_wire.data() + pkt_wire.size());
  CHECK(zc_pkt.counters.size() == 2u && zc_pkt.counters.at("rx") == 1001u);

  const auto zc_remarshal = omnibuf::marshal(zc_pkt, {.deterministic = true});
  CHECK(zc_remarshal.ok());
  CHECK(zc_remarshal.value() == pkt_wire);

  // 5. Arena / std::pmr::monotonic_buffer_resource decoding via omnibuf::unmarshal_pmr
  alignas(64) std::array<std::byte, 8192> arena_buf{};
  std::pmr::monotonic_buffer_resource arena(arena_buf.data(), arena_buf.size(),
                                            std::pmr::null_memory_resource());
  PmrPacket pmr_src{
      std::pmr::string("pmr-service-node-identifier-longer-than-sso-threshold", &arena),
      std::pmr::vector<uint8_t>({9, 8, 7, 6, 5, 4}, &arena),
      std::pmr::vector<std::pmr::string>(&arena),
      std::pmr::vector<uint32_t>(96, std::numeric_limits<uint32_t>::max(), &arena),
      std::pmr::map<std::pmr::string, uint64_t>(&arena),
      777u,
  };
  pmr_src.tags.emplace_back("alpha-tag-longer-than-small-string-buffer");
  pmr_src.tags.emplace_back("beta-tag-longer-than-small-string-buffer");
  pmr_src.counters.emplace("packets_in", 99999u);

  const auto pmr_wire_res = omnibuf::marshal(pmr_src, {.deterministic = true});
  CHECK(pmr_wire_res.ok());

  alignas(64) std::array<std::byte, 8192> decode_arena_buf{};
  std::pmr::monotonic_buffer_resource decode_arena(
      decode_arena_buf.data(), decode_arena_buf.size(), std::pmr::null_memory_resource());
  PmrPacket pmr_dec;
  CHECK(omnibuf::unmarshal_pmr(pmr_wire_res.value(), pmr_dec, &decode_arena).ok());
  CHECK(pmr_dec.name == pmr_src.name);
  CHECK(pmr_dec.payload == pmr_src.payload);
  CHECK(pmr_dec.tags == pmr_src.tags);
  CHECK(pmr_dec.numbers == pmr_src.numbers);
  CHECK(pmr_dec.counters == pmr_src.counters);
  CHECK(pmr_dec.seq == 777u);
  const auto* name_bytes = reinterpret_cast<const std::byte*>(pmr_dec.name.data());
  CHECK(name_bytes >= decode_arena_buf.data() &&
        name_bytes < decode_arena_buf.data() + decode_arena_buf.size());

  const auto pmr_remarshal = omnibuf::marshal(pmr_dec, {.deterministic = true});
  CHECK(pmr_remarshal.ok());
  CHECK(pmr_remarshal.value() == pmr_wire_res.value());

  // Reusing pmr_dec on an empty packet must clear unseen scalar seq while preserving decode_arena
  CHECK(omnibuf::unmarshal_pmr(std::span<const uint8_t>{}, pmr_dec, &decode_arena).ok());
  CHECK(pmr_dec.seq == 0u);
  CHECK(pmr_dec.name.empty());
  CHECK(pmr_dec.name.get_allocator().resource() == &decode_arena);

  // 6. Exhaustive SIMD / overlapping-load UTF-8 validation across lengths 0..128
  for (size_t len = 0; len <= 128; ++len) {
    std::string s(len, 'x');
    CHECK(omnibuf::is_valid_utf8(s));
    for (size_t pos = 0; pos < len; ++pos) {
      s[pos] = static_cast<char>(0x80);
      CHECK(!omnibuf::is_valid_utf8(s));
      s[pos] = static_cast<char>(0xFF);
      CHECK(!omnibuf::is_valid_utf8(s));
      s[pos] = 'x';
    }
    if (len >= 2) {
      for (size_t pos = 0; pos + 1 < len; ++pos) {
        s[pos] = static_cast<char>(0xC3);
        s[pos + 1] = static_cast<char>(0xA9);  // U+00E9
        CHECK(omnibuf::is_valid_utf8(s));
        s[pos] = 'x';
        s[pos + 1] = 'x';
      }
    }
  }
}

int main() {
  RUN_TEST(test_fast_writer_aliasing);
  RUN_TEST(test_prefix_varint_and_zigzag);
  RUN_TEST(test_strict_utf8);
  RUN_TEST(test_encoder_overflow_and_depth_limit);
  RUN_TEST(test_wire_invariants_and_extended);
  RUN_TEST(test_field0_and_blind_json);
  RUN_TEST(test_opaque_and_aliased_splice);
  RUN_TEST(test_edge_showcase_and_wide_message);
  RUN_TEST(test_large_container_shift_and_limits);
  RUN_TEST(test_recursive_struct_unordered_map_and_descriptors);
  RUN_TEST(test_zero_copy_view_and_perf_upgrades);

  std::printf("\nSummary: %d passed, %d failed\n", g_passed, g_failed);
  return g_failed == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
