#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <memory>
#include <memory_resource>
#include <new>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "heap_tracker.hpp"
#include "omnibuf/bench/peer.omnibuf.pb.hpp"
#include "omnibuf/conformance/conformance.omnibuf.pb.hpp"
#include "omnibuf/omnibuf.hpp"
#include "test_harness.hpp"

namespace {

struct DepthStructNode {
  uint32_t id = 0;
  std::unique_ptr<DepthStructNode> next{};
};

enum UnscopedState {
  STATE_A = 0,
  STATE_B = 1,
};

enum class SignedFixedEnum : int32_t {
  NEG = -5,
  ZERO = 0,
};

struct UnscopedEnumHolder {
  UnscopedState state = STATE_A;
};

struct SignedFixedEnumHolder {
  SignedFixedEnum val = SignedFixedEnum::ZERO;
};

struct OptContainerStruct {
  std::optional<std::vector<uint32_t>> nums;
  std::optional<std::map<std::string, uint32_t>> kv;
};

struct BadUtf8NameStruct {
  uint32_t x = 0;
};

struct PmrMapStruct {
  std::pmr::unordered_map<std::pmr::string, uint32_t> items;
};

}  // namespace

template <>
struct omnibuf::StructTraits<DepthStructNode> {
  static constexpr std::string_view type_name = "omnibuf.test.DepthStructNode";
  static constexpr auto fields() {
    using T = DepthStructNode;
    return std::make_tuple(omnibuf::field<1, &T::id>("id"), omnibuf::field<2, &T::next>("next"));
  }
};

template <>
struct omnibuf::StructTraits<UnscopedEnumHolder> {
  static constexpr std::string_view type_name = "omnibuf.test.UnscopedEnumHolder";
  static constexpr auto fields() {
    using T = UnscopedEnumHolder;
    return std::make_tuple(omnibuf::field<1, &T::state>("state"));
  }
};

template <>
struct omnibuf::StructTraits<SignedFixedEnumHolder> {
  static constexpr std::string_view type_name = "omnibuf.test.SignedFixedEnumHolder";
  static constexpr auto fields() {
    using T = SignedFixedEnumHolder;
    return std::make_tuple(omnibuf::field<1, &T::val, omnibuf::WireOverride::Fixed32>("val"));
  }
};

template <>
struct omnibuf::StructTraits<OptContainerStruct> {
  static constexpr std::string_view type_name = "omnibuf.test.OptContainerStruct";
  static constexpr auto fields() {
    return std::make_tuple(omnibuf::field<1, &OptContainerStruct::nums>("nums"),
                           omnibuf::field<2, &OptContainerStruct::kv>("kv"));
  }
};

template <>
struct omnibuf::StructTraits<BadUtf8NameStruct> {
  static constexpr std::string_view type_name = "omnibuf.test.Bad\xC3\x28Name";
  static constexpr auto fields() {
    return std::make_tuple(omnibuf::field<1, &BadUtf8NameStruct::x>("x"));
  }
};

template <>
struct omnibuf::StructTraits<PmrMapStruct> {
  static constexpr std::string_view type_name = "omnibuf.test.PmrMapStruct";
  static constexpr auto fields() {
    return std::make_tuple(omnibuf::field<1, &PmrMapStruct::items>("items"));
  }
};

namespace {

void test_encode_enforces_max_container_depth() {
  constexpr int kExcessDepth = 35;  // > omnibuf::MAX_CONTAINER_DEPTH (32)

  // 1. Generated struct (TreeNode) of depth 35
  {
    omnibuf::conformance::TreeNode cur;
    cur.id = static_cast<uint32_t>(kExcessDepth);
    cur.name = "leaf";
    for (int d = kExcessDepth - 1; d >= 0; --d) {
      omnibuf::conformance::TreeNode parent;
      parent.id = static_cast<uint32_t>(d);
      parent.primary.emplace(std::move(cur));
      cur = std::move(parent);
    }
    const auto res = cur.try_marshal_omnibuf();
    EXPECT_MSG(!res.ok(), "TreeNode::try_marshal_omnibuf succeeded at depth 35 > 32");

    bool threw = false;
    try {
      (void)cur.marshal_omnibuf();
    } catch (const std::exception&) {
      threw = true;
    }
    EXPECT_MSG(threw, "TreeNode::marshal_omnibuf did not throw at depth 35 > 32");
  }

  // 2. StructTraits recursive struct of depth 35
  {
    auto cur = std::make_unique<DepthStructNode>();
    cur->id = static_cast<uint32_t>(kExcessDepth);
    for (int d = kExcessDepth - 1; d >= 1; --d) {
      auto parent = std::make_unique<DepthStructNode>();
      parent->id = static_cast<uint32_t>(d);
      parent->next = std::move(cur);
      cur = std::move(parent);
    }
    DepthStructNode root;
    root.id = 0;
    root.next = std::move(cur);

    const auto sres = omnibuf::marshal(root);
    EXPECT_MSG(!sres.ok(), "omnibuf::marshal(StructTraits) succeeded at depth 35 > 32");
  }

  // 3. DynamicMessage (omnibuf.conformance.TreeNode) of depth 35
#ifdef OMNIBUF_SCHEMAS_FDS_PATH
  {
    omnibuf::proto::DescriptorPool pool;
    if (pool.load_file(OMNIBUF_SCHEMAS_FDS_PATH).ok()) {
      const auto* desc = pool.find_message("omnibuf.conformance.TreeNode");
      EXPECT(desc != nullptr);
      if (desc != nullptr) {
        auto cur = std::make_unique<omnibuf::proto::DynamicMessage>();
        auto& leaf_id = cur->mutable_fields()[1];
        leaf_id.present = true;
        leaf_id.scalar = uint32_t{35};
        for (int d = kExcessDepth - 1; d >= 0; --d) {
          auto parent = std::make_unique<omnibuf::proto::DynamicMessage>();
          auto& pid = parent->mutable_fields()[1];
          pid.present = true;
          pid.scalar = static_cast<uint32_t>(d);
          auto& psub = parent->mutable_fields()[4];  // TreeNode.primary = 4
          psub.present = true;
          psub.submsg = std::move(cur);
          cur = std::move(parent);
        }
        const auto dres = omnibuf::proto::to_binary(*desc, *cur);
        EXPECT_MSG(!dres.ok(), "omnibuf::proto::to_binary succeeded at depth 35 > 32");
      }
    }
  }
#endif
}

void test_struct_traits_enum_wire_types() {
  // 1. Unscoped enum with non-negative enumerators must still encode as WireSint (1), NOT
  //    WireUint (0).
  {
    UnscopedEnumHolder h{.state = STATE_B};
    const auto res = omnibuf::marshal(h);
    EXPECT(res.ok());
    if (res.ok() && !res.value().empty()) {
      const auto [tag, tn] = omnibuf::decode_varint(res.value());
      EXPECT(tn > 0);
      const uint8_t wt = static_cast<uint8_t>(tag & 0x0Fu);
      EXPECT_MSG(wt == omnibuf::WireSint,
                 "UnscopedEnumHolder marshaled with WireUint(0) instead of WireSint(1)");
    }
  }

  // 2. Scoped signed enum (int32_t) with WireOverride::Fixed32 must encode as WireSfixed32 (6),
  //    NOT WireFixed32 (4).
  {
    SignedFixedEnumHolder h{.val = SignedFixedEnum::NEG};
    const auto res = omnibuf::marshal(h);
    EXPECT(res.ok());
    if (res.ok() && !res.value().empty()) {
      const auto [tag, tn] = omnibuf::decode_varint(res.value());
      EXPECT(tn > 0);
      const uint8_t wt = static_cast<uint8_t>(tag & 0x0Fu);
      EXPECT_MSG(
          wt == omnibuf::WireSfixed32,
          "SignedFixedEnum with Fixed32 override marshaled as WireFixed32(4) instead of WireSfixed32(6)");
    }
  }
}

void test_decode_blind_element_budget_not_multiplied_by_depth() {
  // Depth 20 nested submessages containing a single leaf WireArray(WireUint) of 1,000 elements.
  omnibuf::Encoder enc;
  constexpr int kNesting = 20;
  for (int i = 0; i < kNesting; ++i) {
    enc.begin_submsg(1);
  }

  std::vector<uint8_t> arr_field;
  omnibuf::append_tag(arr_field, 2, omnibuf::WireArray);
  const std::vector<uint32_t> elems(1000, 7u);
  omnibuf::encode_uint_arr(arr_field, std::span<const uint32_t>(elems));
  enc.write_raw(arr_field);

  for (int i = 0; i < kNesting; ++i) {
    enc.end_submsg();
  }
  const std::vector<uint8_t> pkt = std::move(enc).finish();

  // Single-pass validator accepts 1,000 <= 1,500.
  omnibuf::DecodeState vstate{omnibuf::DecodeOptions{.max_total_elements = 1500}, 0};
  EXPECT(omnibuf::validate_message_bytes(pkt, 0, vstate).ok());

  // decode_blind must also succeed under max_total_elements = 1500 instead of charging
  // 21 * 1000 = 21,000.
  const auto blind_res = omnibuf::decode_blind(pkt, {.max_total_elements = 1500});
  EXPECT_MSG(blind_res.ok(), blind_res.error().c_str());
}

void test_fast_writer_exception_leaves_no_trailing_bytes() {
  std::vector<uint8_t> dst = {1, 2, 3};
  omnibuf::bench::Location bad_loc;
  bad_loc.region = "eu";
  bad_loc.city = std::string("\xC3\x28", 2);  // invalid UTF-8 triggers throw

  bool caught = false;
  try {
    bad_loc.marshal_omnibuf_append(dst);
  } catch (const std::invalid_argument&) {
    caught = true;
  }

  EXPECT(caught);
  EXPECT_MSG(dst.size() == 3, "marshal_omnibuf_append left trailing headroom/partial bytes in dst");
  EXPECT(dst == (std::vector<uint8_t>{1, 2, 3}));
}

void test_reused_unmarshal_preserves_bounded_capacity() {
  omnibuf::bench::PeerUpdate src;
  src.asn = 64512;
  src.router_id = "edge-rtr-fra01.core.backbone.net";
  src.tags = {"tier1", "ebgp", "eu-central", "ixp-de-cix"};
  src.prefixes = {167772160u, 172032000u, 3232235520u};
  src.neighbors = {
      omnibuf::bench::Neighbor{
          .peer_asn = 3356, .router = "ae0.fra1", .latency_us = 850, .established = true},
      omnibuf::bench::Neighbor{
          .peer_asn = 1299, .router = "ffm-b5", .latency_us = 920, .established = true},
  };
  const std::vector<uint8_t> wire = src.marshal_omnibuf();

  omnibuf::bench::PeerUpdate reused;
  for (int i = 0; i < 1000; ++i) {
    const auto res = reused.unmarshal_omnibuf(wire);
    EXPECT_MSG(res.ok(), res.error().c_str());
  }
  EXPECT(reused == src);
  EXPECT_MSG(reused.tags.capacity() <= 64, "tags capacity grew unbounded across reused unmarshal");
  EXPECT_MSG(reused.prefixes.capacity() <= 64,
             "prefixes capacity grew unbounded across reused unmarshal");
  EXPECT_MSG(reused.neighbors.capacity() <= 64,
             "neighbors capacity grew unbounded across reused unmarshal");
}

void test_repeated_field0_blocks_merge_linearly() {
  omnibuf::SchemaMeta base;
  base.type_name = "demo.Large";
  for (uint32_t i = 1; i <= 50; ++i) {
    base.fields[i] = "field_name_" + std::to_string(i);
  }
  std::vector<uint8_t> pkt = omnibuf::encode_field0_schema_meta(base);
  // Append 25,000 empty 2-byte Field #0 blocks (Tag 0x19, ByteLen 0 -> 0x01).
  constexpr size_t kEmptyBlocks = 25000;
  pkt.reserve(pkt.size() + kEmptyBlocks * 2 + 4);
  for (size_t i = 0; i < kEmptyBlocks; ++i) {
    pkt.push_back(0x19u);
    pkt.push_back(0x01u);
  }
  // Append one data field #1 = 42.
  omnibuf::append_tag(pkt, 1, omnibuf::WireUint);
  omnibuf::append_varint(pkt, 42);

  const auto t0 = std::chrono::steady_clock::now();
  const auto meta = omnibuf::read_field0_schema_meta(pkt);
  const auto blind = omnibuf::decode_blind(pkt);
  const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                              std::chrono::steady_clock::now() - t0)
                              .count();

  EXPECT(meta.has_value());
  EXPECT(meta->type_name == "demo.Large");
  EXPECT(meta->fields.size() == 50);
  EXPECT_MSG(blind.ok(), blind.error().c_str());
  EXPECT(blind.value().contains("field_name_1"));
  EXPECT(std::get<uint64_t>(blind.value().at("field_name_1").v) == 42u);
  EXPECT_MSG(elapsed_ms < 2000, "Repeated Field #0 merge took too long (possible O(N^2))");
}

void test_optional_container_concatenates_segments() {
  std::vector<uint8_t> pkt;
  // Two concatenated field #1 WireArray(WireUint): [10, 20] then [30].
  {
    omnibuf::append_tag(pkt, 1, omnibuf::WireArray);
    const std::vector<uint32_t> a1 = {10u, 20u};
    omnibuf::encode_uint_arr(pkt, std::span<const uint32_t>(a1));
    omnibuf::append_tag(pkt, 1, omnibuf::WireArray);
    const std::vector<uint32_t> a2 = {30u};
    omnibuf::encode_uint_arr(pkt, std::span<const uint32_t>(a2));
  }
  // Two concatenated field #2 WireMap(WireString, WireUint): {"a":1,"b":2} then {"b":20,"c":3}.
  auto append_kv_map = [&](const std::vector<std::pair<std::string, uint32_t>>& entries) {
    std::vector<uint8_t> body;
    body.push_back(omnibuf::WireString);
    body.push_back(omnibuf::WireUint);
    omnibuf::append_varint(body, entries.size());
    for (const auto& [k, v] : entries) {
      omnibuf::append_varint(body, k.size());
      body.insert(body.end(), k.begin(), k.end());
      omnibuf::append_varint(body, v);
    }
    omnibuf::append_tag(pkt, 2, omnibuf::WireMap);
    omnibuf::append_varint(pkt, body.size());
    pkt.insert(pkt.end(), body.begin(), body.end());
  };
  append_kv_map({{"a", 1u}, {"b", 2u}});
  append_kv_map({{"b", 20u}, {"c", 3u}});

  OptContainerStruct decoded;
  const auto res = omnibuf::unmarshal(pkt, decoded);
  EXPECT_MSG(res.ok(), res.error().c_str());
  EXPECT(decoded.nums.has_value());
  EXPECT(*decoded.nums == (std::vector<uint32_t>{10u, 20u, 30u}));
  EXPECT(decoded.kv.has_value());
  EXPECT(decoded.kv->size() == 3);
  EXPECT(decoded.kv->at("a") == 1u);
  EXPECT(decoded.kv->at("b") == 20u);
  EXPECT(decoded.kv->at("c") == 3u);
}

void test_schema_meta_utf8_validation_and_result_marshal() {
  omnibuf::SchemaMeta bad_type;
  bad_type.type_name = std::string("bad\xFF", 4);
  std::vector<uint8_t> dst = {0xAAu};
  const auto r1 = omnibuf::try_append_field0_schema_meta(dst, bad_type);
  EXPECT(!r1.ok());
  EXPECT(dst == (std::vector<uint8_t>{0xAAu}));

  omnibuf::SchemaMeta bad_field;
  bad_field.fields[1] = std::string("\xC3\x28", 2);
  const auto r2 = omnibuf::try_append_field0_schema_meta(dst, bad_field);
  EXPECT(!r2.ok());
  EXPECT(dst == (std::vector<uint8_t>{0xAAu}));

  // Result-returning omnibuf::marshal must return Error instead of throwing on SchemaMeta failure.
  BadUtf8NameStruct s{5};
  bool threw = false;
  try {
    const auto mr = omnibuf::marshal(s, {.self_describing = true});
    EXPECT(!mr.ok());
  } catch (...) {
    threw = true;
  }
  EXPECT_MSG(!threw, "omnibuf::marshal threw instead of returning Error on invalid SchemaMeta");
}

void test_blind_decode_nested_container_element_budget() {
  // Outer WireArray(WireArray) of 200 empty inner arrays.
  std::vector<uint8_t> inner_empty;
  inner_empty.push_back(omnibuf::WireUint);
  inner_empty.push_back(0x01u);  // count = 0

  std::vector<uint8_t> outer_body;
  outer_body.push_back(omnibuf::WireArray);
  omnibuf::append_varint(outer_body, 200);
  for (int i = 0; i < 200; ++i) {
    omnibuf::append_varint(outer_body, inner_empty.size());
    outer_body.insert(outer_body.end(), inner_empty.begin(), inner_empty.end());
  }
  std::vector<uint8_t> pkt;
  omnibuf::append_tag(pkt, 1, omnibuf::WireArray);
  omnibuf::append_varint(pkt, outer_body.size());
  pkt.insert(pkt.end(), outer_body.begin(), outer_body.end());

  const auto fail_res = omnibuf::decode_blind(pkt, {.max_total_elements = 100});
  EXPECT_MSG(!fail_res.ok(),
             "decode_blind bypassed max_total_elements in parse_blind_container_shape");

  // Legitimate nested array with 200 elements must still succeed under max_total_elements = 200
  // (no double-charge).
  const auto ok_res = omnibuf::decode_blind(pkt, {.max_total_elements = 200});
  EXPECT_MSG(ok_res.ok(), ok_res.error().c_str());
}

void test_pmr_unordered_map_rebind_exception_safety() {
  PmrMapStruct src;
  src.items.emplace(std::pmr::string("k1"), 11u);
  const auto wire = omnibuf::marshal(src);
  EXPECT_MSG(wire.ok(), wire.error().c_str());

  PmrMapStruct target;
  target.items.emplace(std::pmr::string("existing"), 99u);
  // Force allocation failure during rebind_pmr_member via null_memory_resource.
  // Note: unordered_map(Alloc(mr)) may allocate eagerly or lazily depending on stdlib;
  // either way, unmarshal_pmr throws std::bad_alloc without double-destroying target.items.
  bool caught = false;
  try {
    (void)omnibuf::unmarshal_pmr(wire.value(), target, std::pmr::null_memory_resource());
  } catch (const std::bad_alloc&) {
    caught = true;
  }
  EXPECT_MSG(caught, "unmarshal_pmr with null_memory_resource did not throw std::bad_alloc");
  // target destructor runs cleanly at scope exit (no UB / double-destroy).
}

void test_schema_meta_repeated_children_recursive_merge() {
  omnibuf::SchemaMeta c1;
  c1.type_name = "pkg.Child";
  c1.fields[10] = "old_a";
  c1.fields[11] = "old_b";
  auto gc1 = std::make_shared<omnibuf::SchemaMeta>();
  gc1->fields[1] = "grand_a";
  c1.children[20] = gc1;

  omnibuf::SchemaMeta c2;
  c2.fields[10] = "new_a";
  c2.fields[12] = "new_c";
  auto gc2 = std::make_shared<omnibuf::SchemaMeta>();
  gc2->fields[2] = "grand_b";
  c2.children[20] = gc2;

  std::vector<uint8_t> c1_body;
  omnibuf::append_schema_meta_body(c1_body, c1);
  std::vector<uint8_t> c2_body;
  omnibuf::append_schema_meta_body(c2_body, c2);

  auto append_child_map = [](std::vector<uint8_t>& dst, const std::vector<uint8_t>& child_body) {
    // Field #3: WireMap(WireUint, WireSubMsg), 1 entry with key = 1
    std::vector<uint8_t> mbody;
    mbody.push_back(omnibuf::WireUint);
    mbody.push_back(omnibuf::WireSubMsg);
    omnibuf::append_varint(mbody, 1);
    omnibuf::append_varint(mbody, 1);  // child field key = 1
    omnibuf::append_varint(mbody, child_body.size());
    mbody.insert(mbody.end(), child_body.begin(), child_body.end());
    dst.push_back(0x7Du);  // Tag (3 << 4) | WireMap(14) = 62 -> 0x7D
    omnibuf::append_varint(dst, mbody.size());
    dst.insert(dst.end(), mbody.begin(), mbody.end());
  };

  std::vector<uint8_t> single_meta_body;
  append_child_map(single_meta_body, c1_body);
  append_child_map(single_meta_body, c2_body);

  const auto res = omnibuf::decode_schema_meta(single_meta_body);
  EXPECT_MSG(res.ok(), res.error().c_str());
  const auto& child = res.value()->children.at(1);
  EXPECT(child->type_name == "pkg.Child");
  EXPECT(child->fields.size() == 3);
  EXPECT(child->fields.at(10) == "new_a");
  EXPECT(child->fields.at(11) == "old_b");
  EXPECT(child->fields.at(12) == "new_c");
  EXPECT(child->children.contains(20));
  const auto& grandchild = child->children.at(20);
  EXPECT(grandchild->fields.size() == 2);
  EXPECT(grandchild->fields.at(1) == "grand_a");
  EXPECT(grandchild->fields.at(2) == "grand_b");
}

void test_result_value_throws_on_error() {
  omnibuf::Result<int> err_lval(omnibuf::Error{"boom"});
  EXPECT(!err_lval.ok());
  bool threw_lval = false;
  try {
    (void)err_lval.value();
  } catch (const std::bad_optional_access&) {
    threw_lval = true;
  }
  EXPECT_MSG(threw_lval, "Result<T>::value() & did not throw on error");

  const omnibuf::Result<int>& err_clval = err_lval;
  bool threw_clval = false;
  try {
    (void)err_clval.value();
  } catch (const std::bad_optional_access&) {
    threw_clval = true;
  }
  EXPECT_MSG(threw_clval, "Result<T>::value() const& did not throw on error");

  bool threw_rval = false;
  try {
    (void)omnibuf::Result<int>(omnibuf::Error{"boom_rval"}).value();
  } catch (const std::bad_optional_access&) {
    threw_rval = true;
  }
  EXPECT_MSG(threw_rval, "Result<T>::value() && did not throw on error");

  omnibuf::Result<int> ok_val(42);
  EXPECT(ok_val.ok());
  EXPECT(ok_val.value() == 42);
}

void test_fast_writer_dtor_and_rollback_no_alloc_when_shrunk() {
  std::vector<uint8_t> buf(16, 0xAAu);
  {
    omnibuf::FastWriter w(buf);
    buf.clear();
    buf.shrink_to_fit();
#if OMNIBUF_TRACK_HEAP
    omnibuf_test::g_fail_allocs.store(true, std::memory_order_relaxed);
#endif
    // Destructor must not resize upward (which would allocate and terminate noexcept).
  }
#if OMNIBUF_TRACK_HEAP
  omnibuf_test::g_fail_allocs.store(false, std::memory_order_relaxed);
#endif
  EXPECT(buf.empty());

  buf.assign(16, 0xBBu);
  {
    omnibuf::FastWriter w(buf);
    buf.clear();
    buf.shrink_to_fit();
#if OMNIBUF_TRACK_HEAP
    omnibuf_test::g_fail_allocs.store(true, std::memory_order_relaxed);
#endif
    w.rollback();
#if OMNIBUF_TRACK_HEAP
    omnibuf_test::g_fail_allocs.store(false, std::memory_order_relaxed);
#endif
  }
  EXPECT(buf.empty());
}

}  // namespace

int main() {
  RUN_CASE(test_encode_enforces_max_container_depth);
  RUN_CASE(test_struct_traits_enum_wire_types);
  RUN_CASE(test_decode_blind_element_budget_not_multiplied_by_depth);
  RUN_CASE(test_fast_writer_exception_leaves_no_trailing_bytes);
  RUN_CASE(test_reused_unmarshal_preserves_bounded_capacity);
  RUN_CASE(test_repeated_field0_blocks_merge_linearly);
  RUN_CASE(test_optional_container_concatenates_segments);
  RUN_CASE(test_schema_meta_utf8_validation_and_result_marshal);
  RUN_CASE(test_blind_decode_nested_container_element_budget);
  RUN_CASE(test_pmr_unordered_map_rebind_exception_safety);
  RUN_CASE(test_schema_meta_repeated_children_recursive_merge);
  RUN_CASE(test_result_value_throws_on_error);
  RUN_CASE(test_fast_writer_dtor_and_rollback_no_alloc_when_shrunk);

  std::printf("\nWire & Traits Test Summary: %d passed, %d failed\n", omnibuf_test::g_passed,
              omnibuf_test::g_failed);
  return omnibuf_test::g_failed == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
