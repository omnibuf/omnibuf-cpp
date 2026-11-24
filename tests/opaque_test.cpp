#ifndef _WIN32
#include <sys/wait.h>
#include <unistd.h>
#endif

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "heap_tracker.hpp"
#include "omnibuf/bench/peer.omnibuf.pb.hpp"
#include "omnibuf/omnibuf.hpp"
#include "omnibuf/test/edge.omnibuf.pb.hpp"
#include "test_harness.hpp"

namespace {

void test_chained_opaque_submsg_lifetime() {
  constexpr std::string_view kExpectedCity = "Frankfurt-am-Main-Long-String-For-Heap";
  constexpr std::string_view kExpectedDirtyCity = "Dirty-Frankfurt-am-Main-Long-String-For-Heap";

  omnibuf::bench::PeerUpdate seg1;
  seg1.location = omnibuf::bench::Location{.region = "eu"};
  omnibuf::bench::PeerUpdate seg2;
  seg2.location = omnibuf::bench::Location{.city = std::string(kExpectedCity)};

  std::vector<uint8_t> pkt = seg1.marshal_omnibuf();
  const auto pkt2 = seg2.marshal_omnibuf();
  pkt.insert(pkt.end(), pkt2.begin(), pkt2.end());

  const auto [merged_probe, merged_ok] = omnibuf::read_submsg(pkt, 9u);
  EXPECT(merged_ok);
  const size_t merged_sz = merged_probe.size();

#if !OMNIBUF_TRACK_HEAP && !defined(_WIN32)
  // Under ASan, isolate the UAF check in a child process so an abort is recorded as FAIL
  // without terminating the remaining test cases.
  const pid_t pid = ::fork();
  if (pid == 0) {
    omnibuf::bench::OpaquePeerUpdate view(pkt);
    std::string_view city = view.get_location().get_city_view();
    omnibuf_test::scrub_freed_heap_slots(merged_sz, static_cast<uint8_t>('X'));
    const bool ok1 = (city == kExpectedCity);

    view.set_location(omnibuf::bench::Location{.city = std::string(kExpectedDirtyCity)});
    std::string_view dirty_city = view.get_location().get_city_view();
    omnibuf_test::scrub_freed_heap_slots(merged_sz + 8, static_cast<uint8_t>('Y'));
    const bool ok2 = (dirty_city == kExpectedDirtyCity);
    ::_exit((ok1 && ok2) ? 0 : 1);
  }
  int status = 0;
  (void)::waitpid(pid, &status, 0);
  EXPECT(WIFEXITED(status) && WEXITSTATUS(status) == 0);
#else
  omnibuf::bench::OpaquePeerUpdate view(pkt);
  std::string_view city = view.get_location().get_city_view();
  omnibuf_test::scrub_freed_heap_slots(merged_sz, static_cast<uint8_t>('X'));
  EXPECT_MSG(city == kExpectedCity, "multi-segment view.get_location().get_city_view() dangled");

  const omnibuf::bench::Location dirty_loc{.city = std::string(kExpectedDirtyCity)};
  const size_t dirty_loc_sz = dirty_loc.marshal_omnibuf().size();
  view.set_location(dirty_loc);
  std::string_view dirty_city = view.get_location().get_city_view();
  omnibuf_test::scrub_freed_heap_slots(dirty_loc_sz, static_cast<uint8_t>('Y'));
  EXPECT_MSG(dirty_city == kExpectedDirtyCity,
             "dirty submsg view.get_location().get_city_view() dangled");
#endif
}

void test_opaque_marshal_append_preserves_prefix_and_self_alias() {
  omnibuf::bench::PeerUpdate peer;
  peer.asn = 64512;
  peer.router_id = "rtr-01";
  peer.sequence = 10;
  const std::vector<uint8_t> raw = peer.marshal_omnibuf();

  // 1. Clean Opaque::marshal_append must preserve existing prefix bytes.
  {
    omnibuf::bench::OpaquePeerUpdate clean_view(raw);
    const std::vector<uint8_t> prefix = {0xAA, 0xBB, 0xCC, 0xDD};
    const auto out = clean_view.marshal_append(prefix);
    EXPECT(out.size() == prefix.size() + raw.size());
    EXPECT(out.size() > 4 && out[0] == 0xAA && out[1] == 0xBB && out[2] == 0xCC &&
           out[3] == 0xDD);
  }

  // 2. Dirty Opaque::marshal_append must also preserve existing prefix bytes.
  {
    omnibuf::bench::OpaquePeerUpdate dirty_view(raw);
    dirty_view.set_sequence(99);
    const std::vector<uint8_t> prefix = {0xAA, 0xBB, 0xCC, 0xDD};
    const auto out = dirty_view.marshal_append(prefix);
    EXPECT(out.size() > 4 && out[0] == 0xAA && out[1] == 0xBB && out[2] == 0xCC &&
           out[3] == 0xDD);
    if (out.size() > 4) {
      omnibuf::bench::PeerUpdate dec;
      EXPECT(dec.unmarshal_omnibuf(std::span<const uint8_t>(out).subspan(4)).ok());
      EXPECT(dec.asn == 64512 && dec.sequence == 99);
    }
  }

  // 3. Self-aliased marshal_into when raw_view_ points into dst and dst reallocates:
  //    subsequent reads on view (or re-marshaling) must not read freed memory.
  {
    std::vector<uint8_t> self_buf = raw;
    self_buf.shrink_to_fit();
    const size_t old_cap = self_buf.capacity();
    omnibuf::bench::OpaquePeerUpdate alias_view{std::span<const uint8_t>{self_buf}};
    alias_view.set_sequence(777);
    alias_view.marshal_into(self_buf);
    omnibuf_test::scrub_freed_heap_slots(old_cap, static_cast<uint8_t>('Z'));
    EXPECT_MSG(alias_view.get_router_id_view() == "rtr-01",
               "self-aliased marshal_into left raw_view_ dangling into old dst buffer");
  }
}

void test_opaque_oneof_mutual_exclusion_and_unmarshal_resets_dirty() {
  using omnibuf::test::EdgeShowcase;
  using omnibuf::test::OpaqueEdgeShowcase;

  // 1. Concatenated packets switching active oneof branch:
  //    pkt1 sets action_code = 99 (field 11), pkt2 sets action_name = "go" (field 12).
  EdgeShowcase es1;
  es1.set_action_code(99);
  EdgeShowcase es2;
  es2.set_action_name("go");

  std::vector<uint8_t> concat = es1.marshal_omnibuf();
  const auto pkt2 = es2.marshal_omnibuf();
  concat.insert(concat.end(), pkt2.begin(), pkt2.end());

  EdgeShowcase std_dec;
  EXPECT(std_dec.unmarshal_omnibuf(concat).ok());
  EXPECT(std_dec.has_action_name() && std_dec.action_name == "go");
  EXPECT(!std_dec.has_action_code() && std_dec.action_code == 0);

  OpaqueEdgeShowcase op_concat(concat);
  EXPECT_MSG(
      op_concat.get_action_code() == 0,
      "OpaqueEdgeShowcase::get_action_code() returned superseded oneof branch 99 instead of 0");
  const auto try_code = op_concat.try_get_action_code();
  EXPECT(try_code.ok() && try_code.value() == 0);
  EXPECT(op_concat.get_action_name_view() == "go");

  // 2. Opaque::unmarshal(clean_pkt) after dirty mutation must release DirtyHeap / reset active
  //    oneof state.
  const std::vector<uint8_t> clean_pkt = es2.marshal_omnibuf();
  OpaqueEdgeShowcase op_reuse(clean_pkt);
#if OMNIBUF_TRACK_HEAP
  const std::ptrdiff_t baseline_bytes =
      omnibuf_test::g_live_heap_bytes.load(std::memory_order_relaxed);
#endif
  op_reuse.set_action_name(std::string(65536, 'D'));
#if OMNIBUF_TRACK_HEAP
  const std::ptrdiff_t dirty_bytes =
      omnibuf_test::g_live_heap_bytes.load(std::memory_order_relaxed);
  EXPECT(dirty_bytes - baseline_bytes >= 65536);
#endif
  op_reuse.unmarshal(concat);
  EXPECT_MSG(op_reuse.get_action_code() == 0,
             "OpaqueEdgeShowcase::unmarshal(concat) failed oneof mutual exclusion");
#if OMNIBUF_TRACK_HEAP
  const std::ptrdiff_t after_unmarshal_bytes =
      omnibuf_test::g_live_heap_bytes.load(std::memory_order_relaxed);
  EXPECT_MSG(after_unmarshal_bytes - baseline_bytes < 4096,
             "OpaqueEdgeShowcase::unmarshal retained 64 KiB DirtyHeap allocation");
#endif
}

void test_opaque_try_get_rejects_corrupt_field0() {
  omnibuf::SchemaMeta bad_meta;
  bad_meta.type_name = "PeerUpdate";
  bad_meta.fields[1] = "XX";

  std::vector<uint8_t> pkt = omnibuf::encode_field0_schema_meta(bad_meta);
  // Corrupt the 2-byte ASCII field name "XX" in-place to an invalid UTF-8 sequence (\xC3\x28).
  bool patched = false;
  for (size_t i = 0; i + 1 < pkt.size(); ++i) {
    if (pkt[i] == 'X' && pkt[i + 1] == 'X') {
      pkt[i] = 0xC3u;
      pkt[i + 1] = 0x28u;
      patched = true;
      break;
    }
  }
  EXPECT(patched);
  omnibuf::append_tag(pkt, 1, omnibuf::WireUint);
  omnibuf::append_varint(pkt, 123);

  omnibuf::bench::PeerUpdate std_peer;
  EXPECT(!std_peer.unmarshal_omnibuf(pkt).ok());

  omnibuf::bench::OpaquePeerUpdate op(pkt);
  EXPECT(!op.validate().ok());

  const auto res = op.try_get_asn();
  EXPECT_MSG(!res.ok(),
             "OpaquePeerUpdate::try_get_asn() succeeded despite corrupt UTF-8 in Field #0");
}

void test_opaque_and_view_getters_exception_specification() {
  static_assert(!noexcept(std::declval<const omnibuf::bench::OpaquePeerUpdate&>().get_asn()));
  static_assert(
      !noexcept(std::declval<const omnibuf::bench::OpaquePeerUpdate&>().get_router_id_view()));
  static_assert(!noexcept(std::declval<const omnibuf::bench::PeerUpdateView&>().get_asn()));
  static_assert(!noexcept(std::declval<const omnibuf::bench::PeerUpdateView&>().get_router_id()));
  static_assert(!noexcept(std::declval<const omnibuf::test::OpaqueWideMessage&>().get_f1()));
  static_assert(!noexcept(omnibuf::read_submsg_arr_elem(std::span<const uint8_t>{}, 1u, 0u)));

#if OMNIBUF_TRACK_HEAP
  // Build a WideMessage (>64 fields so ensure_dir_() allocates dir_offsets_) with Field #0.
  omnibuf::test::WideMessage wm;
  wm.f1 = 77;
  const std::vector<uint8_t> pkt = wm.marshal_omnibuf({.self_describing = true});
  omnibuf::test::OpaqueWideMessage opaque(pkt);

  bool caught_bad_alloc = false;
  omnibuf_test::g_fail_allocs.store(true, std::memory_order_relaxed);
  try {
    (void)opaque.get_f1();
  } catch (const std::bad_alloc&) {
    caught_bad_alloc = true;
  }
  omnibuf_test::g_fail_allocs.store(false, std::memory_order_relaxed);
  EXPECT_MSG(caught_bad_alloc, "OpaqueWideMessage::get_f1() did not propagate std::bad_alloc");

  // View getter validates Field #0 in-place with zero heap allocations.
  omnibuf::test::WideMessageView view(pkt);
  int32_t view_val = 0;
  omnibuf_test::g_fail_allocs.store(true, std::memory_order_relaxed);
  try {
    view_val = view.get_f1();
  } catch (...) {
    omnibuf_test::g_fail_allocs.store(false, std::memory_order_relaxed);
    throw;
  }
  omnibuf_test::g_fail_allocs.store(false, std::memory_order_relaxed);
  EXPECT_MSG(view_val == 77, "ViewWideMessage::get_f1() did not read f1 without heap allocation");
#endif
}

void test_opaque_container_element_limits_and_map_depth() {
  // 1) map<int32, InnerPayload> depth check: root(0) -> map(1) -> InnerPayload(2) -> 31 nested =
  //    33 > 32.
  std::vector<uint8_t> inner_bytes;
  // Build 31 nested next_inner (field #3, WireSubMsg) layers inside InnerPayload.
  for (int i = 0; i < 31; ++i) {
    std::vector<uint8_t> wrap;
    omnibuf::append_tag(wrap, 3, omnibuf::WireSubMsg);
    omnibuf::append_varint(wrap, inner_bytes.size());
    wrap.insert(wrap.end(), inner_bytes.begin(), inner_bytes.end());
    inner_bytes = std::move(wrap);
  }
  // Wrap inner_bytes as a single map<int32, InnerPayload> entry in EdgeShowcase.by_id (field #7).
  std::vector<uint8_t> map_body;
  map_body.push_back(omnibuf::WireSint);
  map_body.push_back(omnibuf::WireSubMsg);
  omnibuf::append_varint(map_body, 1);  // count = 1
  omnibuf::append_varint(map_body, omnibuf::encode_zigzag(1));
  omnibuf::append_varint(map_body, inner_bytes.size());
  map_body.insert(map_body.end(), inner_bytes.begin(), inner_bytes.end());

  std::vector<uint8_t> pkt;
  omnibuf::append_tag(pkt, 7, omnibuf::WireMap);
  omnibuf::append_varint(pkt, map_body.size());
  pkt.insert(pkt.end(), map_body.begin(), map_body.end());

  omnibuf::test::OpaqueEdgeShowcase opaque(pkt);
  const auto res = opaque.try_get_by_id();
  EXPECT_MSG(!res.ok(), "OpaqueEdgeShowcase::try_get_by_id() bypassed max_depth on map<K, SubMsg>");

  // 2) read_scalar_map / try_read_scalar_map enforce DecodeState max_total_elements.
  std::vector<uint8_t> smap_body;
  smap_body.push_back(omnibuf::WireUint);
  smap_body.push_back(omnibuf::WireUint);
  omnibuf::append_varint(smap_body, 10);
  for (uint32_t i = 0; i < 10; ++i) {
    omnibuf::append_varint(smap_body, i);
    omnibuf::append_varint(smap_body, i * 2);
  }
  std::vector<uint8_t> smap_pkt;
  omnibuf::append_tag(smap_pkt, 1, omnibuf::WireMap);
  omnibuf::append_varint(smap_pkt, smap_body.size());
  smap_pkt.insert(smap_pkt.end(), smap_body.begin(), smap_body.end());

  omnibuf::DecodeState small_state{omnibuf::DecodeOptions{.max_total_elements = 5}, 0};
  const auto mres = omnibuf::try_read_scalar_map<omnibuf::WireUint, omnibuf::WireUint,
                                                 std::map<uint32_t, uint32_t>>(
      smap_pkt, 1, &small_state);
  EXPECT_MSG(!mres.ok(), "try_read_scalar_map bypassed max_total_elements");

  // 3) Opaque repeated scalar getter enforces DecodeOptions::max_total_elements (16M) across
  //    concatenated arrays on PeerUpdate.prefixes (field #8).
  constexpr size_t kHalfPlusOne = (omnibuf::DecodeOptions{}.max_total_elements / 2u) + 1u;
  std::vector<uint8_t> big_arr_body;
  big_arr_body.reserve(kHalfPlusOne + 16);
  big_arr_body.push_back(omnibuf::WireUint);
  omnibuf::append_varint(big_arr_body, kHalfPlusOne);
  big_arr_body.insert(big_arr_body.end(), kHalfPlusOne, 0x01u);

  std::vector<uint8_t> big_pkt;
  big_pkt.reserve(big_arr_body.size() * 2 + 32);
  for (int seg = 0; seg < 2; ++seg) {
    omnibuf::append_tag(big_pkt, 8, omnibuf::WireArray);  // PeerUpdate.prefixes = field #8
    omnibuf::append_varint(big_pkt, big_arr_body.size());
    big_pkt.insert(big_pkt.end(), big_arr_body.begin(), big_arr_body.end());
  }
  omnibuf::bench::OpaquePeerUpdate big_opaque(big_pkt);
  const auto arr_res = big_opaque.try_get_prefixes();
  EXPECT_MSG(!arr_res.ok(), "OpaquePeerUpdate::try_get_prefixes() bypassed max_total_elements");
}

void test_read_submsg_validates_segment_boundaries() {
  // Segment 1: truncated field #1 WireUint tag (0x21 = (1<<5)|(0<<1)|1) with NO payload byte.
  // Segment 2: single byte 0x03 (which if blindly glued onto seg 1 would decode as field #1 = 1!).
  std::vector<uint8_t> pkt;
  omnibuf::append_tag(pkt, 1, omnibuf::WireSubMsg);
  omnibuf::append_varint(pkt, 1);
  pkt.push_back(0x21u);  // truncated WireUint field #1
  omnibuf::append_tag(pkt, 1, omnibuf::WireSubMsg);
  omnibuf::append_varint(pkt, 1);
  pkt.push_back(0x03u);  // would complete varint if concatenated blindly

  const auto [merged, ok] = omnibuf::read_submsg(pkt, 1);
  EXPECT_MSG(!ok, "read_submsg concatenated truncated segment without boundary validation");
  EXPECT(merged.empty());

  const std::array<omnibuf::OneofBranchSpec, 1> grp = {
      omnibuf::OneofBranchSpec{1u, static_cast<uint16_t>(1u << omnibuf::WireSubMsg)}};
  const auto oneof_res = omnibuf::read_oneof_submsg(pkt, 1, grp, false);
  EXPECT_MSG(!oneof_res.valid, "read_oneof_submsg concatenated truncated segment");
}

void test_read_int_at_out_of_bounds_guards() {
  const std::vector<uint8_t> empty;
  EXPECT(omnibuf::read_uint_at(empty, 0) == 0);
  EXPECT(omnibuf::read_uint_at(empty, 1) == 0);
  EXPECT(omnibuf::read_uint_at(empty, 1024) == 0);
  EXPECT(omnibuf::read_sint_at(empty, 0) == 0);
  EXPECT(omnibuf::read_sint_at(empty, 1) == 0);
  EXPECT(omnibuf::read_sint_at(empty, 1024) == 0);

  const std::vector<uint8_t> pkt = {0x0Bu};  // 1-byte varint 5
  EXPECT(omnibuf::read_uint_at(pkt, 0) == 5);
  EXPECT(omnibuf::read_uint_at(pkt, 1) == 0);
  EXPECT(omnibuf::read_uint_at(pkt, 2) == 0);
  EXPECT(omnibuf::read_uint_at(pkt, 999) == 0);
  EXPECT(omnibuf::read_sint_at(pkt, 1) == 0);
  EXPECT(omnibuf::read_sint_at(pkt, 2) == 0);
  EXPECT(omnibuf::read_sint_at(pkt, 999) == 0);
}

}  // namespace

int main() {
  RUN_CASE(test_chained_opaque_submsg_lifetime);
  RUN_CASE(test_opaque_marshal_append_preserves_prefix_and_self_alias);
  RUN_CASE(test_opaque_oneof_mutual_exclusion_and_unmarshal_resets_dirty);
  RUN_CASE(test_opaque_try_get_rejects_corrupt_field0);
  RUN_CASE(test_opaque_and_view_getters_exception_specification);
  RUN_CASE(test_opaque_container_element_limits_and_map_depth);
  RUN_CASE(test_read_submsg_validates_segment_boundaries);
  RUN_CASE(test_read_int_at_out_of_bounds_guards);

  std::printf("\nOpaque Test Summary: %d passed, %d failed\n", omnibuf_test::g_passed,
              omnibuf_test::g_failed);
  return omnibuf_test::g_failed == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
