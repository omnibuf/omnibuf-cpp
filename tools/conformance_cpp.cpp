#include <unistd.h>

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "omnibuf/bench/peer.omnibuf.pb.hpp"
#include "omnibuf/conformance/conformance.omnibuf.pb.hpp"
#include "omnibuf/conformance/editions_2023.omnibuf.pb.hpp"
#include "omnibuf/conformance/editions_2024.omnibuf.pb.hpp"
#include "omnibuf/conformance/editions_2026.omnibuf.pb.hpp"
#include "omnibuf/omnibuf.hpp"

namespace ipc {

constexpr uint32_t kWireFormatJson = 2;

constexpr uint32_t kTierPlugin = 1;
constexpr uint32_t kTierProtobufTypes = 2;
constexpr uint32_t kTierAnnotatedStruct = 3;
constexpr uint32_t kTierBlind = 4;

constexpr uint32_t kMaxFrameBytes = 16u * 1024u * 1024u;

struct ConformanceRequest {
  std::string test_name{};
  std::string message_type{};
  uint32_t tier = 0;
  uint32_t requested_output_format = 0;
  std::vector<uint8_t> omnibuf_payload{};
  std::string json_payload{};
  bool self_describing = false;
  bool test_opaque_splice = false;
  bool test_opaque_splice_shrink = false;
};

struct ConformanceResponse {
  std::string parse_error{};
  std::string serialize_error{};
  std::string runtime_error{};
  std::string skipped{};
  std::vector<uint8_t> omnibuf_payload{};
  std::string json_payload{};
};

}  // namespace ipc

namespace annotated {

struct Location {
  std::string region{};
  std::string datacenter{};
  std::string city{};
  uint32_t rack = 0;
  float latitude = 0.0f;
  float longitude = 0.0f;
};

struct Neighbor {
  uint32_t peer_asn = 0;
  std::string router{};
  uint32_t latency_us = 0;
  bool established = false;
};

struct PeerUpdate {
  uint32_t asn = 0;
  std::string router_id{};
  bool active = false;
  uint64_t uptime_sec = 0;
  int32_t rssi_dbm = 0;
  double cpu_load = 0.0;
  std::vector<std::string> tags{};
  std::vector<uint32_t> prefixes{};
  std::optional<Location> location{};
  std::vector<Neighbor> neighbors{};
  uint64_t sequence = 0;
};

struct AllTypesSubMsg {
  uint32_t id = 0;
  std::string label{};
  bool flag = false;
  int64_t score = 0;
};

struct TestAllTypes {
  uint32_t f1_uint32 = 0;
  uint64_t f2_uint64 = 0;
  int32_t f3_sint32 = 0;
  int64_t f4_sint64 = 0;
  bool f5_bool = false;
  std::string f6_string{};
  std::vector<uint8_t> f7_bytes{};
  std::unique_ptr<AllTypesSubMsg> f8_sub_msg{};
  std::vector<std::string> f9_string_arr{};
  std::vector<uint32_t> f10_uint32_arr{};
  std::vector<AllTypesSubMsg> f11_sub_msg_arr{};
  std::map<std::string, uint64_t> f12_map_str_uint{};
  float f13_float32 = 0.0f;
  double f14_float64 = 0.0;
  uint32_t f15_fixed32 = 0;
  uint64_t f16_fixed64 = 0;
  std::string f20_overflow_str{};
};

struct TreeNode {
  uint32_t id = 0;
  std::string name{};
  int64_t weight = 0;
  omnibuf::Box<TreeNode> primary{};
  std::vector<TreeNode> children{};
  std::vector<omnibuf::Box<TreeNode>> branches{};
  std::map<std::string, uint64_t> labels{};
};

struct NestedNIC {
  uint32_t if_index = 0;
  std::string mac{};
  std::vector<uint32_t> ips{};
};

struct NestedServer {
  uint32_t server_id = 0;
  std::string hostname{};
  std::vector<NestedNIC> nics{};
};

struct NestedRack {
  uint32_t rack_id = 0;
  std::string zone{};
  std::vector<NestedServer> servers{};
};

struct NestedCluster {
  std::string cluster_name{};
  std::vector<NestedRack> racks{};
  uint64_t epoch = 0;
};

}  // namespace annotated

template <>
struct omnibuf::StructTraits<ipc::ConformanceRequest> {
  static constexpr std::string_view type_name = "omnibuf.conformance.ConformanceRequest";
  static constexpr auto fields() {
    using T = ipc::ConformanceRequest;
    return std::make_tuple(
        omnibuf::field<1, &T::test_name>("test_name"),
        omnibuf::field<2, &T::message_type>("message_type"),
        omnibuf::field<3, &T::tier>("tier"),
        omnibuf::field<4, &T::requested_output_format>("requested_output_format"),
        omnibuf::field<5, &T::omnibuf_payload>("omnibuf_payload"),
        omnibuf::field<6, &T::json_payload>("json_payload"),
        omnibuf::field<7, &T::self_describing>("self_describing"),
        omnibuf::field<8, &T::test_opaque_splice>("test_opaque_splice"),
        omnibuf::field<9, &T::test_opaque_splice_shrink>("test_opaque_splice_shrink"));
  }
};

template <>
struct omnibuf::StructTraits<ipc::ConformanceResponse> {
  static constexpr std::string_view type_name = "omnibuf.conformance.ConformanceResponse";
  static constexpr auto fields() {
    using T = ipc::ConformanceResponse;
    return std::make_tuple(
        omnibuf::field<1, &T::parse_error>("parse_error"),
        omnibuf::field<2, &T::serialize_error>("serialize_error"),
        omnibuf::field<3, &T::runtime_error>("runtime_error"),
        omnibuf::field<4, &T::skipped>("skipped"),
        omnibuf::field<5, &T::omnibuf_payload>("omnibuf_payload"),
        omnibuf::field<6, &T::json_payload>("json_payload"));
  }
};

template <>
struct omnibuf::StructTraits<annotated::Location> {
  static constexpr std::string_view type_name = "omnibuf.bench.Location";
  static constexpr auto fields() {
    using T = annotated::Location;
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
struct omnibuf::StructTraits<annotated::Neighbor> {
  static constexpr std::string_view type_name = "omnibuf.bench.Neighbor";
  static constexpr auto fields() {
    using T = annotated::Neighbor;
    return std::make_tuple(
        omnibuf::field<1, &T::peer_asn>("peer_asn"),
        omnibuf::field<2, &T::router>("router"),
        omnibuf::field<3, &T::latency_us>("latency_us"),
        omnibuf::field<4, &T::established>("established"));
  }
};

template <>
struct omnibuf::StructTraits<annotated::PeerUpdate> {
  static constexpr std::string_view type_name = "omnibuf.bench.PeerUpdate";
  static constexpr auto fields() {
    using T = annotated::PeerUpdate;
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

template <>
struct omnibuf::StructTraits<annotated::AllTypesSubMsg> {
  static constexpr std::string_view type_name = "omnibuf.conformance.AllTypesSubMsg";
  static constexpr auto fields() {
    using T = annotated::AllTypesSubMsg;
    return std::make_tuple(
        omnibuf::field<1, &T::id>("id"),
        omnibuf::field<2, &T::label>("label"),
        omnibuf::field<3, &T::flag>("flag"),
        omnibuf::field<4, &T::score, omnibuf::WireOverride::Sint>("score"));
  }
};

template <>
struct omnibuf::StructTraits<annotated::TestAllTypes> {
  static constexpr std::string_view type_name = "omnibuf.conformance.TestAllTypes";
  static constexpr auto fields() {
    using T = annotated::TestAllTypes;
    return std::make_tuple(
        omnibuf::field<1, &T::f1_uint32>("f1_uint32"),
        omnibuf::field<2, &T::f2_uint64>("f2_uint64"),
        omnibuf::field<3, &T::f3_sint32, omnibuf::WireOverride::Sint>("f3_sint32"),
        omnibuf::field<4, &T::f4_sint64, omnibuf::WireOverride::Sint>("f4_sint64"),
        omnibuf::field<5, &T::f5_bool>("f5_bool"),
        omnibuf::field<6, &T::f6_string>("f6_string"),
        omnibuf::field<7, &T::f7_bytes>("f7_bytes"),
        omnibuf::field<8, &T::f8_sub_msg>("f8_sub_msg"),
        omnibuf::field<9, &T::f9_string_arr>("f9_string_arr"),
        omnibuf::field<10, &T::f10_uint32_arr>("f10_uint32_arr"),
        omnibuf::field<11, &T::f11_sub_msg_arr>("f11_sub_msg_arr"),
        omnibuf::field<12, &T::f12_map_str_uint>("f12_map_str_uint"),
        omnibuf::field<13, &T::f13_float32>("f13_float32"),
        omnibuf::field<14, &T::f14_float64>("f14_float64"),
        omnibuf::field<15, &T::f15_fixed32, omnibuf::WireOverride::Fixed32>("f15_fixed32"),
        omnibuf::field<16, &T::f16_fixed64, omnibuf::WireOverride::Fixed64>("f16_fixed64"),
        omnibuf::field<20, &T::f20_overflow_str>("f20_overflow_str"));
  }
};

template <>
struct omnibuf::StructTraits<annotated::TreeNode> {
  static constexpr std::string_view type_name = "omnibuf.conformance.TreeNode";
  static constexpr auto fields() {
    using T = annotated::TreeNode;
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

template <>
struct omnibuf::StructTraits<annotated::NestedNIC> {
  static constexpr std::string_view type_name = "omnibuf.conformance.NestedNIC";
  static constexpr auto fields() {
    using T = annotated::NestedNIC;
    return std::make_tuple(
        omnibuf::field<1, &T::if_index>("if_index"),
        omnibuf::field<2, &T::mac>("mac"),
        omnibuf::field<3, &T::ips>("ips"));
  }
};

template <>
struct omnibuf::StructTraits<annotated::NestedServer> {
  static constexpr std::string_view type_name = "omnibuf.conformance.NestedServer";
  static constexpr auto fields() {
    using T = annotated::NestedServer;
    return std::make_tuple(
        omnibuf::field<1, &T::server_id>("server_id"),
        omnibuf::field<2, &T::hostname>("hostname"),
        omnibuf::field<3, &T::nics>("nics"));
  }
};

template <>
struct omnibuf::StructTraits<annotated::NestedRack> {
  static constexpr std::string_view type_name = "omnibuf.conformance.NestedRack";
  static constexpr auto fields() {
    using T = annotated::NestedRack;
    return std::make_tuple(
        omnibuf::field<1, &T::rack_id>("rack_id"),
        omnibuf::field<2, &T::zone>("zone"),
        omnibuf::field<3, &T::servers>("servers"));
  }
};

template <>
struct omnibuf::StructTraits<annotated::NestedCluster> {
  static constexpr std::string_view type_name = "omnibuf.conformance.NestedCluster";
  static constexpr auto fields() {
    using T = annotated::NestedCluster;
    return std::make_tuple(
        omnibuf::field<1, &T::cluster_name>("cluster_name"),
        omnibuf::field<2, &T::racks>("racks"),
        omnibuf::field<3, &T::epoch>("epoch"));
  }
};

namespace {

std::string repeat_str(std::string_view part, size_t count) {
  std::string out;
  out.reserve(part.size() * count);
  for (size_t i = 0; i < count; ++i) {
    out.append(part);
  }
  return out;
}

std::optional<std::string> check_splice_aliased_prefix(std::span<const uint8_t> packet) {
  auto append_dirty = [](std::vector<uint8_t>& dst, uint32_t fnum) -> const char* {
    omnibuf::append_varint(dst, (static_cast<uint64_t>(fnum) << 4) | omnibuf::WireUint);
    omnibuf::append_varint(dst, uint64_t{1} << 40);
    return nullptr;
  };

  auto spliced_res =
      omnibuf::try_splice_fields({}, packet, uint64_t{1} << 1, 2, append_dirty);
  if (!spliced_res) {
    return spliced_res.error();
  }
  const std::vector<uint8_t>& spliced = spliced_res.value();

  for (size_t prefix_len : {size_t{0}, size_t{3}, size_t{32}}) {
    std::vector<uint8_t> prefix(prefix_len, uint8_t{0xa5});
    std::vector<uint8_t> storage;
    storage.reserve(prefix_len + packet.size() + 512);
    storage.insert(storage.end(), prefix.begin(), prefix.end());
    storage.insert(storage.end(), packet.begin(), packet.end());

    const std::span<const uint8_t> aliased_raw(storage.data() + prefix_len, packet.size());

    std::vector<uint8_t> want;
    want.reserve(prefix.size() + spliced.size());
    want.insert(want.end(), prefix.begin(), prefix.end());
    want.insert(want.end(), spliced.begin(), spliced.end());

    const auto got_res = omnibuf::try_splice_fields(
        std::span<uint8_t>(storage), prefix_len, aliased_raw, uint64_t{1} << 1, 2, append_dirty);
    if (!got_res) {
      return got_res.error();
    }
    if (got_res.value() != want) {
      return "opaque splice with aliased prefix changed prefix or output";
    }
    if (!std::equal(packet.begin(), packet.end(), storage.data() + prefix_len)) {
      return "opaque splice with aliased prefix modified source packet";
    }

    // Exercise self-aliasing splice_fields_into when raw points inside dst's live buffer.
    std::vector<uint8_t> self_buf;
    self_buf.reserve(packet.size() + 256);
    self_buf.insert(self_buf.end(), packet.begin(), packet.end());
    const std::span<const uint8_t> self_raw(self_buf.data(), self_buf.size());
    std::vector<uint8_t> self_want = self_buf;
    self_want.insert(self_want.end(), spliced.begin(), spliced.end());
    const auto self_res =
        omnibuf::try_splice_fields_into(self_buf, self_raw, uint64_t{1} << 1, 2, append_dirty);
    if (!self_res) {
      return self_res.error();
    }
    if (self_buf != self_want) {
      return "opaque splice_fields_into with self-aliased raw corrupted output";
    }
  }
  return std::nullopt;
}

ipc::ConformanceResponse handle_blind(const ipc::ConformanceRequest& req) {
  ipc::ConformanceResponse resp;
  auto blind_res = omnibuf::decode_blind(req.omnibuf_payload);
  if (!blind_res) {
    resp.parse_error = blind_res.error();
    return resp;
  }
  if (req.requested_output_format == ipc::kWireFormatJson) {
    auto js_res = omnibuf::stringify_blind_json(blind_res.value());
    if (!js_res) {
      resp.json_payload = "{}";
    } else {
      resp.json_payload = std::move(js_res.value());
    }
  }
  return resp;
}

template <typename MsgT, typename ChildT, typename StatusT, typename OpaqueT>
ipc::ConformanceResponse handle_editions_plugin(const ipc::ConformanceRequest& req,
                                                StatusT stale_status,
                                                std::string_view type_label) {
  ipc::ConformanceResponse resp;

  MsgT msg;
  msg.explicit_int = 999;
  msg.explicit_bool = true;
  msg.explicit_string = "stale";
  msg.explicit_bytes = {0xff};
  msg.explicit_double = 99.5;
  msg.status = stale_status;
  msg.implicit_count = 999;
  msg.implicit_flag = true;
  msg.child = ChildT{.id = 999, .note = "stale"};
  msg.delimited_child = ChildT{.id = 999, .note = "stale"};
  msg.packed_vals = {999};
  msg.expanded_vals = {999};
  msg.scores = {{"stale", 999}};
  msg.set_choice_text("stale");

  auto dec = msg.unmarshal_omnibuf(req.omnibuf_payload);
  if (!dec) {
    resp.parse_error = dec.error();
    return resp;
  }

  OpaqueT op;
  op.unmarshal(req.omnibuf_payload);
  const auto op_dbl = op.get_explicit_double();
  const bool dbl_match =
      (op_dbl.has_value() == msg.explicit_double.has_value()) &&
      (!op_dbl.has_value() ||
       omnibuf::float64_to_bits(*op_dbl) == omnibuf::float64_to_bits(*msg.explicit_double));

  const ChildT want_child = msg.child.value_or(ChildT{});
  const auto op_child = op.get_child();
  const auto op_try_child = op.try_get_child();
  const ChildT want_delim = msg.delimited_child.value_or(ChildT{});
  const auto op_delim = op.get_delimited_child();
  const auto op_try_delim = op.try_get_delimited_child();
  const ChildT want_choice_child =
      (msg.has_choice_child() && msg.choice_child.has_value()) ? *msg.choice_child : ChildT{};
  const auto op_choice_child = op.get_choice_child();
  const auto op_try_choice_child = op.try_get_choice_child();

  if (op.get_explicit_int() != msg.explicit_int ||
      op.get_explicit_bool() != msg.explicit_bool ||
      op.get_explicit_string() != msg.explicit_string ||
      op.get_explicit_bytes() != msg.explicit_bytes || !dbl_match ||
      op.get_status() != msg.status || op.get_implicit_count() != msg.implicit_count ||
      op.get_implicit_flag() != msg.implicit_flag || !op_try_child.ok() ||
      op_try_child.value().has_value() != msg.child.has_value() ||
      op_child.get_id() != want_child.id || op_child.get_note() != want_child.note ||
      !op_try_delim.ok() ||
      op_try_delim.value().has_value() != msg.delimited_child.has_value() ||
      op_delim.get_id() != want_delim.id || op_delim.get_note() != want_delim.note ||
      op.packed_vals_len() != msg.packed_vals.size() || op.get_packed_vals() != msg.packed_vals ||
      op.expanded_vals_len() != msg.expanded_vals.size() ||
      op.get_expanded_vals() != msg.expanded_vals || op.get_scores() != msg.scores ||
      op.get_choice_int() != (msg.has_choice_int() ? msg.choice_int : 0) ||
      op.get_choice_text() != (msg.has_choice_text() ? msg.choice_text : std::string{}) ||
      op.get_choice_flag() != (msg.has_choice_flag() ? msg.choice_flag : false) ||
      !op_try_choice_child.ok() ||
      op_try_choice_child.value().has_value() !=
          (msg.has_choice_child() && msg.choice_child.has_value()) ||
      op_choice_child.get_id() != want_choice_child.id ||
      op_choice_child.get_note() != want_choice_child.note) {
    resp.parse_error =
        "opaque " + std::string(type_label) + " getter mismatch with decoded struct";
    return resp;
  }

  auto enc = msg.try_marshal_omnibuf(
      {.deterministic = true, .self_describing = req.self_describing});
  if (!enc) {
    resp.serialize_error = enc.error();
    return resp;
  }
  resp.omnibuf_payload = std::move(enc.value());
  return resp;
}

ipc::ConformanceResponse handle_plugin_tier(const ipc::ConformanceRequest& req) {
  ipc::ConformanceResponse resp;

  if (req.message_type == "omnibuf.bench.PeerUpdate") {
    if (req.test_opaque_splice || req.test_opaque_splice_shrink) {
      omnibuf::bench::OpaquePeerUpdate probe;
      probe.unmarshal(req.omnibuf_payload);
      probe.set_sequence(probe.get_sequence() + uint64_t{1});
      auto pr = probe.try_marshal();
      if (!pr) {
        resp.parse_error = pr.error();
        return resp;
      }
    }

    omnibuf::bench::PeerUpdate msg;
    msg.asn = 999999;
    msg.router_id = "stale-router";
    msg.active = true;
    msg.tags = {"stale-tag"};
    msg.prefixes = {999};
    msg.location = omnibuf::bench::Location{
        .region = "stale",
        .datacenter = "",
        .city = "stale-city",
        .rack = 999,
        .latitude = 0.0f,
        .longitude = 0.0f,
    };
    msg.neighbors = {omnibuf::bench::Neighbor{.peer_asn = 999, .router = "stale-neighbor"}};
    msg.sequence = 999999;

    auto dec = msg.unmarshal_omnibuf(req.omnibuf_payload);
    if (!dec) {
      resp.parse_error = dec.error();
      return resp;
    }

    if (req.test_opaque_splice || req.test_opaque_splice_shrink) {
      if (auto err = check_splice_aliased_prefix(req.omnibuf_payload)) {
        resp.runtime_error = *err;
        return resp;
      }
    }

    omnibuf::bench::OpaquePeerUpdate op;
    op.unmarshal(req.omnibuf_payload);
    const omnibuf::bench::Location want_loc =
        msg.location.value_or(omnibuf::bench::Location{});
    const auto op_loc = op.get_location();
    if (op.get_asn() != msg.asn || op.get_router_id() != msg.router_id ||
        op.get_active() != msg.active || op.get_uptime_sec() != msg.uptime_sec ||
        op.get_rssi_dbm() != msg.rssi_dbm ||
        omnibuf::float64_to_bits(op.get_cpu_load()) != omnibuf::float64_to_bits(msg.cpu_load) ||
        op.tags_len() != msg.tags.size() || op.prefixes_len() != msg.prefixes.size() ||
        op_loc.get_city() != want_loc.city ||
        omnibuf::float32_to_bits(op_loc.get_latitude()) !=
            omnibuf::float32_to_bits(want_loc.latitude) ||
        omnibuf::float32_to_bits(op_loc.get_longitude()) !=
            omnibuf::float32_to_bits(want_loc.longitude) ||
        op.neighbors_len() != msg.neighbors.size() || op.get_sequence() != msg.sequence) {
      resp.parse_error = "opaque PeerUpdate getter mismatch with decoded struct";
      return resp;
    }

    if (req.test_opaque_splice) {
      op.set_asn(1'000'000);
      op.set_router_id(repeat_str("edge-rtr-fra01-", 20));
      op.set_active(!op.get_active());
      op.set_uptime_sec(std::numeric_limits<uint64_t>::max());
      op.set_rssi_dbm(-1'000'000);
      op.set_cpu_load(99.875);
      op.set_tags({"tag-grow-a", "tag-grow-b", repeat_str("long-tag-", 20)});
      op.set_prefixes({100000, 200000, 300000, 400000, 500000, 600000});
      op.set_location(omnibuf::bench::Location{
          .region = "eu-central-grown",
          .datacenter = repeat_str("fra-dc-", 25),
          .city = "Berlin",
          .rack = 999999,
          .latitude = 52.52f,
          .longitude = 13.405f,
      });
      op.set_sequence(op.get_sequence() + uint64_t{1});
      auto out = op.try_marshal();
      if (!out) {
        resp.serialize_error = out.error();
        return resp;
      }
      resp.omnibuf_payload = std::move(out.value());
      return resp;
    }

    if (req.test_opaque_splice_shrink) {
      op.set_asn(1);
      op.set_router_id("r");
      op.set_active(false);
      op.set_uptime_sec(5);
      op.set_rssi_dbm(-1);
      op.set_cpu_load(0.5);
      op.set_tags({"t"});
      op.set_prefixes({1});
      op.set_location(omnibuf::bench::Location{
          .region = "r",
          .datacenter = "d",
          .city = "c",
          .rack = 1,
          .latitude = 1.0f,
          .longitude = 2.0f,
      });
      op.set_sequence(2);
      auto out = op.try_marshal();
      if (!out) {
        resp.serialize_error = out.error();
        return resp;
      }
      resp.omnibuf_payload = std::move(out.value());
      return resp;
    }

    auto enc = msg.try_marshal_omnibuf({.deterministic = true, .self_describing = req.self_describing});
    if (!enc) {
      resp.serialize_error = enc.error();
      return resp;
    }
    resp.omnibuf_payload = std::move(enc.value());
    return resp;
  }

  if (req.message_type == "omnibuf.conformance.TestAllTypes") {
    if (req.test_opaque_splice || req.test_opaque_splice_shrink) {
      omnibuf::conformance::OpaqueTestAllTypes probe;
      probe.unmarshal(req.omnibuf_payload);
      probe.set_f1_uint32(probe.get_f1_uint32() + 1u);
      auto pr = probe.try_marshal();
      if (!pr) {
        resp.parse_error = pr.error();
        return resp;
      }
    }

    omnibuf::conformance::TestAllTypes msg;
    msg.f1_uint32 = 999999;
    msg.f5_bool = true;
    msg.f6_string = "stale-str";
    msg.f7_bytes = {0xff};
    msg.f8_sub_msg = omnibuf::conformance::AllTypesSubMsg{
        .id = 999,
        .label = "stale-sub",
        .flag = true,
        .score = -999,
    };
    msg.f9_string_arr = {"stale"};
    msg.f10_uint32_arr = {999};
    msg.f11_sub_msg_arr = {
        omnibuf::conformance::AllTypesSubMsg{.id = 999, .label = "stale-elem"}};
    msg.f12_map_str_uint = {{"stale", 999}};
    msg.f20_overflow_str = "stale-overflow";

    auto dec = msg.unmarshal_omnibuf(req.omnibuf_payload);
    if (!dec) {
      resp.parse_error = dec.error();
      return resp;
    }

    if (req.test_opaque_splice || req.test_opaque_splice_shrink) {
      if (auto err = check_splice_aliased_prefix(req.omnibuf_payload)) {
        resp.runtime_error = *err;
        return resp;
      }
    }

    omnibuf::conformance::OpaqueTestAllTypes op;
    op.unmarshal(req.omnibuf_payload);
    (void)op.lookup_f12_map_str_uint("k");
    const omnibuf::conformance::AllTypesSubMsg want_sub =
        msg.f8_sub_msg.value_or(omnibuf::conformance::AllTypesSubMsg{});
    const auto op_sub = op.get_f8_sub_msg();

    if (op.get_f1_uint32() != msg.f1_uint32 || op.get_f2_uint64() != msg.f2_uint64 ||
        op.get_f3_sint32() != msg.f3_sint32 || op.get_f4_sint64() != msg.f4_sint64 ||
        op.get_f5_bool() != msg.f5_bool || op.get_f6_string() != msg.f6_string ||
        op.get_f7_bytes() != msg.f7_bytes || op_sub.get_id() != want_sub.id ||
        op_sub.get_label() != want_sub.label || op_sub.get_flag() != want_sub.flag ||
        op_sub.get_score() != want_sub.score ||
        op.f9_string_arr_len() != msg.f9_string_arr.size() ||
        op.f10_uint32_arr_len() != msg.f10_uint32_arr.size() ||
        op.f11_sub_msg_arr_len() != msg.f11_sub_msg_arr.size() ||
        omnibuf::float32_to_bits(op.get_f13_float32()) !=
            omnibuf::float32_to_bits(msg.f13_float32) ||
        omnibuf::float64_to_bits(op.get_f14_float64()) !=
            omnibuf::float64_to_bits(msg.f14_float64) ||
        op.get_f15_fixed32() != msg.f15_fixed32 || op.get_f16_fixed64() != msg.f16_fixed64 ||
        op.get_f20_overflow_str() != msg.f20_overflow_str) {
      resp.parse_error = "opaque TestAllTypes getter mismatch with decoded struct";
      return resp;
    }

    if (req.test_opaque_splice) {
      op.set_f1_uint32(1'000'000);
      op.set_f2_uint64(std::numeric_limits<uint64_t>::max());
      op.set_f3_sint32(-1'000'000);
      op.set_f4_sint64(std::numeric_limits<int64_t>::min());
      op.set_f5_bool(!op.get_f5_bool());
      op.set_f6_string(repeat_str("grown-str-", 30));
      std::vector<uint8_t> grown_bytes;
      grown_bytes.reserve(260);
      for (int i = 0; i < 130; ++i) {
        grown_bytes.push_back(0xab);
        grown_bytes.push_back(0xcd);
      }
      op.set_f7_bytes(std::move(grown_bytes));
      op.set_f8_sub_msg(omnibuf::conformance::AllTypesSubMsg{
          .id = 888888,
          .label = repeat_str("sub-grown-", 20),
          .flag = true,
          .score = -999999999,
      });
      op.set_f9_string_arr({"arr-grow-1", "arr-grow-2", repeat_str("arr-grow-3-", 15)});
      op.set_f10_uint32_arr({100000, 200000, 300000, 400000, 500000});
      op.set_f11_sub_msg_arr({
          omnibuf::conformance::AllTypesSubMsg{
              .id = 100001,
              .label = repeat_str("elem0-", 25),
              .flag = true,
              .score = -500000,
          },
          omnibuf::conformance::AllTypesSubMsg{
              .id = 100002,
              .label = "elem1-grown",
              .flag = false,
              .score = 500000,
          },
      });
      op.set_f12_map_str_uint({
          {"k1", 1000000},
          {"k2", std::numeric_limits<uint64_t>::max()},
          {"k3_long_key", 424242},
      });
      op.set_f13_float32(-1234.5f);
      op.set_f14_float64(3.141592653589793);
      op.set_f15_fixed32(0xdeadbeefu);
      op.set_f16_fixed64(0xcafebabedeadbeefULL);
      op.set_f20_overflow_str(repeat_str("overflow20-", 20));
      auto out = op.try_marshal();
      if (!out) {
        resp.serialize_error = out.error();
        return resp;
      }
      resp.omnibuf_payload = std::move(out.value());
      return resp;
    }

    if (req.test_opaque_splice_shrink) {
      op.set_f1_uint32(1);
      op.set_f2_uint64(2);
      op.set_f3_sint32(-1);
      op.set_f4_sint64(-2);
      op.set_f5_bool(false);
      op.set_f6_string("s");
      op.set_f7_bytes({0x01});
      op.set_f8_sub_msg(omnibuf::conformance::AllTypesSubMsg{
          .id = 1,
          .label = "x",
          .flag = false,
          .score = -1,
      });
      op.set_f9_string_arr({"a"});
      op.set_f10_uint32_arr({7});
      op.set_f11_sub_msg_arr({
          omnibuf::conformance::AllTypesSubMsg{
              .id = 1,
              .label = "e",
              .flag = true,
              .score = 1,
          },
      });
      op.set_f12_map_str_uint({{"k", 1}});
      op.set_f13_float32(0.5f);
      op.set_f14_float64(-0.25);
      op.set_f15_fixed32(7);
      op.set_f16_fixed64(9);
      op.set_f20_overflow_str("z");
      auto out = op.try_marshal();
      if (!out) {
        resp.serialize_error = out.error();
        return resp;
      }
      resp.omnibuf_payload = std::move(out.value());
      return resp;
    }

    auto enc = msg.try_marshal_omnibuf({.deterministic = true, .self_describing = req.self_describing});
    if (!enc) {
      resp.serialize_error = enc.error();
      return resp;
    }
    resp.omnibuf_payload = std::move(enc.value());
    return resp;
  }

  if (req.message_type == "omnibuf.conformance.TreeNode") {
    omnibuf::conformance::TreeNode msg;
    msg.id = 999;
    msg.name = "stale";
    msg.primary = omnibuf::conformance::TreeNode{.id = 444};
    auto dec = msg.unmarshal_omnibuf(req.omnibuf_payload);
    if (!dec) {
      resp.parse_error = dec.error();
      return resp;
    }
    auto enc = msg.try_marshal_omnibuf({.deterministic = true, .self_describing = req.self_describing});
    if (!enc) {
      resp.serialize_error = enc.error();
      return resp;
    }
    resp.omnibuf_payload = std::move(enc.value());
    return resp;
  }

  if (req.message_type == "omnibuf.conformance.NestedCluster") {
    omnibuf::conformance::NestedCluster msg;
    msg.cluster_name = "stale";
    msg.epoch = 999;
    auto dec = msg.unmarshal_omnibuf(req.omnibuf_payload);
    if (!dec) {
      resp.parse_error = dec.error();
      return resp;
    }
    auto enc = msg.try_marshal_omnibuf({.deterministic = true, .self_describing = req.self_describing});
    if (!enc) {
      resp.serialize_error = enc.error();
      return resp;
    }
    resp.omnibuf_payload = std::move(enc.value());
    return resp;
  }

  if (req.message_type == "omnibuf.conformance.TestEditions2023") {
    return handle_editions_plugin<omnibuf::conformance::TestEditions2023,
                                  omnibuf::conformance::Editions2023Child,
                                  omnibuf::conformance::Editions2023Status,
                                  omnibuf::conformance::OpaqueTestEditions2023>(
        req, omnibuf::conformance::Editions2023Status::EDITIONS2023_STATUS_PAUSED,
        "TestEditions2023");
  }

  if (req.message_type == "omnibuf.conformance.TestEditions2024") {
    return handle_editions_plugin<omnibuf::conformance::TestEditions2024,
                                  omnibuf::conformance::Editions2024Child,
                                  omnibuf::conformance::Editions2024Status,
                                  omnibuf::conformance::OpaqueTestEditions2024>(
        req, omnibuf::conformance::Editions2024Status::EDITIONS2024_STATUS_PAUSED,
        "TestEditions2024");
  }

  if (req.message_type == "omnibuf.conformance.TestEditions2026") {
    return handle_editions_plugin<omnibuf::conformance::TestEditions2026,
                                  omnibuf::conformance::Editions2026Child,
                                  omnibuf::conformance::Editions2026Status,
                                  omnibuf::conformance::OpaqueTestEditions2026>(
        req, omnibuf::conformance::Editions2026Status::EDITIONS2026_STATUS_PAUSED,
        "TestEditions2026");
  }

  resp.skipped = "unsupported message_type in plugin tier: " + req.message_type;
  return resp;
}

ipc::ConformanceResponse handle_protobuf_types_tier(const ipc::ConformanceRequest& req,
                                                    const omnibuf::proto::DescriptorPool& pool) {
  ipc::ConformanceResponse resp;
  const auto* desc = pool.find_message(req.message_type);
  if (desc == nullptr) {
    resp.skipped = "missing descriptor for " + req.message_type;
    return resp;
  }

  omnibuf::proto::DynamicMessage dmsg;
  // Pre-populate stale state to verify DynamicMessage reuse clears unseen fields.
  dmsg.mutable_fields()[1].present = true;
  dmsg.mutable_fields()[1].scalar = uint32_t{999999};

  auto dec = omnibuf::proto::from_binary(*desc, req.omnibuf_payload, dmsg);
  if (!dec) {
    resp.parse_error = dec.error();
    return resp;
  }

  auto enc = omnibuf::proto::to_binary(
      *desc, dmsg, {.deterministic = true, .self_describing = req.self_describing});
  if (!enc) {
    resp.serialize_error = enc.error();
    return resp;
  }
  resp.omnibuf_payload = std::move(enc.value());
  return resp;
}

template <omnibuf::RegisteredStruct T>
ipc::ConformanceResponse roundtrip_annotated(const ipc::ConformanceRequest& req, T&& stale_init) {
  ipc::ConformanceResponse resp;
  T msg = std::move(stale_init);
  auto dec = omnibuf::unmarshal(req.omnibuf_payload, msg);
  if (!dec) {
    resp.parse_error = dec.error();
    return resp;
  }
  auto enc = omnibuf::marshal(msg, {.deterministic = true, .self_describing = req.self_describing});
  if (!enc) {
    resp.serialize_error = enc.error();
    return resp;
  }
  resp.omnibuf_payload = std::move(enc.value());
  return resp;
}

ipc::ConformanceResponse handle_annotated_struct_tier(const ipc::ConformanceRequest& req) {
  if (req.message_type == "omnibuf.bench.PeerUpdate") {
    annotated::PeerUpdate stale;
    stale.asn = 777777;
    stale.router_id = "stale";
    stale.location = annotated::Location{.region = "stale", .city = "stale-city"};
    return roundtrip_annotated(req, std::move(stale));
  }
  if (req.message_type == "omnibuf.conformance.TreeNode") {
    annotated::TreeNode stale;
    stale.id = 888;
    stale.name = "stale-node";
    stale.primary = annotated::TreeNode{.id = 444};
    return roundtrip_annotated(req, std::move(stale));
  }
  if (req.message_type == "omnibuf.conformance.NestedCluster") {
    annotated::NestedCluster stale;
    stale.cluster_name = "stale-cluster";
    stale.epoch = 999;
    return roundtrip_annotated(req, std::move(stale));
  }
  if (req.message_type == "omnibuf.conformance.TestAllTypes") {
    annotated::TestAllTypes stale;
    stale.f1_uint32 = 555555;
    stale.f6_string = "stale";
    stale.f8_sub_msg = std::make_unique<annotated::AllTypesSubMsg>(
        annotated::AllTypesSubMsg{.id = 123, .label = "stale", .flag = true, .score = -5});
    return roundtrip_annotated(req, std::move(stale));
  }
  ipc::ConformanceResponse resp;
  resp.skipped = "unsupported message_type in annotated struct tier: " + req.message_type;
  return resp;
}

enum class ReadStatus {
  Ok,
  CleanEof,
  Error,
};

ReadStatus read_exact(int fd, uint8_t* dst, size_t len) {
  size_t off = 0;
  while (off < len) {
    const ssize_t n = ::read(fd, dst + off, len - off);
    if (n == 0) {
      return (off == 0) ? ReadStatus::CleanEof : ReadStatus::Error;
    }
    if (n < 0) {
      if (errno == EINTR) {
        continue;
      }
      return ReadStatus::Error;
    }
    off += static_cast<size_t>(n);
  }
  return ReadStatus::Ok;
}

bool write_exact(int fd, const uint8_t* src, size_t len) {
  size_t off = 0;
  while (off < len) {
    const ssize_t n = ::write(fd, src + off, len - off);
    if (n < 0) {
      if (errno == EINTR) {
        continue;
      }
      return false;
    }
    if (n == 0) {
      return false;
    }
    off += static_cast<size_t>(n);
  }
  return true;
}

bool load_descriptors(omnibuf::proto::DescriptorPool& pool, const char* argv0) {
  std::vector<std::filesystem::path> combined_candidates;
#ifdef OMNIBUF_SCHEMAS_FDS_PATH
  combined_candidates.emplace_back(OMNIBUF_SCHEMAS_FDS_PATH);
#endif
  if (argv0 != nullptr) {
    const auto exe_dir = std::filesystem::path(argv0).parent_path();
    combined_candidates.push_back(exe_dir / "gen" / "schemas.fds.pb");
  }
  combined_candidates.emplace_back("build/gen/schemas.fds.pb");

  for (const auto& p : combined_candidates) {
    if (std::filesystem::exists(p) && pool.load_file(p.string()).ok()) {
      return pool.find_message("omnibuf.bench.PeerUpdate") != nullptr &&
             pool.find_message("omnibuf.conformance.TestAllTypes") != nullptr &&
             pool.find_message("omnibuf.conformance.TestEditions2023") != nullptr &&
             pool.find_message("omnibuf.conformance.TestEditions2024") != nullptr &&
             pool.find_message("omnibuf.conformance.TestEditions2026") != nullptr;
    }
  }
  return false;
}

}  // namespace

int main(int argc, char** argv) {
  omnibuf::proto::DescriptorPool pool;
  if (!load_descriptors(pool, argc > 0 ? argv[0] : nullptr)) {
    std::fprintf(stderr, "conformance-cpp: failed to load .fds.pb descriptors\n");
    return 1;
  }

  std::vector<uint8_t> frame_buf;
  while (true) {
    uint8_t len_bytes[4];
    const ReadStatus st = read_exact(STDIN_FILENO, len_bytes, 4);
    if (st == ReadStatus::CleanEof) {
      break;
    }
    if (st != ReadStatus::Ok) {
      std::fprintf(stderr, "conformance-cpp: failed reading frame header\n");
      return 1;
    }

    const uint32_t frame_len = omnibuf::read_u32_le(len_bytes);
    if (frame_len > ipc::kMaxFrameBytes) {
      std::fprintf(stderr, "conformance-cpp: frame length %u exceeds limit\n", frame_len);
      return 1;
    }

    frame_buf.resize(frame_len);
    if (frame_len > 0) {
      if (read_exact(STDIN_FILENO, frame_buf.data(), frame_len) != ReadStatus::Ok) {
        std::fprintf(stderr, "conformance-cpp: truncated frame payload\n");
        return 1;
      }
    }

    ipc::ConformanceRequest req;
    auto req_dec = omnibuf::unmarshal(frame_buf, req);
    if (!req_dec) {
      std::fprintf(stderr, "conformance-cpp: invalid ConformanceRequest: %s\n",
                   req_dec.error().c_str());
      return 1;
    }

    ipc::ConformanceResponse resp;
    switch (req.tier) {
      case ipc::kTierBlind:
        resp = handle_blind(req);
        break;
      case ipc::kTierPlugin:
        resp = handle_plugin_tier(req);
        break;
      case ipc::kTierProtobufTypes:
        resp = handle_protobuf_types_tier(req, pool);
        break;
      case ipc::kTierAnnotatedStruct:
        resp = handle_annotated_struct_tier(req);
        break;
      default:
        resp.runtime_error = "unsupported tier: " + std::to_string(req.tier);
        break;
    }

    auto out_res = omnibuf::marshal(resp);
    if (!out_res) {
      std::fprintf(stderr, "conformance-cpp: failed encoding ConformanceResponse: %s\n",
                   out_res.error().c_str());
      return 1;
    }
    const auto& out_payload = out_res.value();
    uint8_t out_len[4];
    omnibuf::write_u32_le(out_len, static_cast<uint32_t>(out_payload.size()));
    if (!write_exact(STDOUT_FILENO, out_len, 4)) {
      std::fprintf(stderr, "conformance-cpp: failed writing response length\n");
      return 1;
    }
    if (!out_payload.empty() &&
        !write_exact(STDOUT_FILENO, out_payload.data(), out_payload.size())) {
      std::fprintf(stderr, "conformance-cpp: failed writing response payload\n");
      return 1;
    }
  }

  return 0;
}
