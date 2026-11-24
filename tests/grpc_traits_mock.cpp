#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "test_harness.hpp"

// Minimal mock of <grpcpp/support/serialization_traits.h> to compile and verify
// omnibuf::grpc_codec::SerializationTraitsHelper without requiring grpc++ installed.
#define GRPCPP_SUPPORT_SERIALIZATION_TRAITS_H

namespace grpc {

enum class StatusCode {
  OK = 0,
  INTERNAL = 13,
  DATA_LOSS = 15,
};

class Status {
 public:
  static const Status OK;

  Status() noexcept = default;
  Status(StatusCode code, std::string message)
      : code_(code), message_(std::move(message)) {}

  [[nodiscard]] bool ok() const noexcept { return code_ == StatusCode::OK; }
  [[nodiscard]] StatusCode error_code() const noexcept { return code_; }
  [[nodiscard]] const std::string& error_message() const noexcept { return message_; }

 private:
  StatusCode code_ = StatusCode::OK;
  std::string message_;
};

inline const Status Status::OK{};

class Slice {
 public:
  Slice(const void* buf, size_t len) : data_(len) {
    if (len > 0) {
      std::memcpy(data_.data(), buf, len);
    }
  }

  [[nodiscard]] const uint8_t* begin() const noexcept { return data_.data(); }
  [[nodiscard]] size_t size() const noexcept { return data_.size(); }

 private:
  std::vector<uint8_t> data_;
};

class ByteBuffer {
 public:
  ByteBuffer() = default;
  ByteBuffer(const Slice* slices, size_t nslices) {
    slices_.reserve(nslices);
    for (size_t i = 0; i < nslices; ++i) {
      slices_.push_back(slices[i]);
    }
  }

  void Swap(ByteBuffer* other) noexcept { slices_.swap(other->slices_); }

  Status Dump(std::vector<Slice>* slices) const {
    *slices = slices_;
    return Status::OK;
  }

 private:
  std::vector<Slice> slices_;
};

template <typename T>
struct SerializationTraits;

}  // namespace grpc

#include "omnibuf/grpc.hpp"

namespace {

struct MockGrpcPing {
  uint32_t asn = 0;
  std::string router_id;
  int32_t rssi_dbm = 0;

  bool operator==(const MockGrpcPing&) const = default;
};

}  // namespace

template <>
struct omnibuf::StructTraits<MockGrpcPing> {
  static constexpr std::string_view type_name = "omnibuf.test.MockGrpcPing";
  static constexpr auto fields() {
    using T = MockGrpcPing;
    return std::make_tuple(
        omnibuf::field<1, &T::asn>("asn"),
        omnibuf::field<2, &T::router_id>("router_id"),
        omnibuf::field<3, &T::rssi_dbm, omnibuf::WireOverride::Sint>("rssi_dbm"));
  }
};

template <>
struct grpc::SerializationTraits<MockGrpcPing>
    : omnibuf::grpc_codec::SerializationTraitsHelper<MockGrpcPing, true, true> {};

namespace {

void test_serialization_traits_helper_round_trip() {
  MockGrpcPing original{.asn = 64512, .router_id = "edge-rtr-fra01", .rssi_dbm = -62};
  grpc::ByteBuffer bb;
  bool own_buffer = false;

  grpc::Status ser = grpc::SerializationTraits<MockGrpcPing>::Serialize(original, &bb, &own_buffer);
  EXPECT(ser.ok());
  EXPECT(own_buffer);

  // Verify self-describing Field #0 was emitted and survives multi-slice reassembly.
  std::vector<grpc::Slice> slices;
  EXPECT(bb.Dump(&slices).ok());
  EXPECT(slices.size() == 1);
  auto meta = omnibuf::read_field0_schema_meta(
      std::span<const uint8_t>(slices[0].begin(), slices[0].size()));
  EXPECT(meta.has_value());
  EXPECT(meta->type_name == "MockGrpcPing");

  // Split payload across two slices to verify multi-slice ByteBuffer deserialization.
  const size_t half = slices[0].size() / 2;
  grpc::Slice split[2] = {
      grpc::Slice(slices[0].begin(), half),
      grpc::Slice(slices[0].begin() + half, slices[0].size() - half),
  };
  grpc::ByteBuffer split_bb(split, 2);

  MockGrpcPing decoded{};
  grpc::Status deser = grpc::SerializationTraits<MockGrpcPing>::Deserialize(&split_bb, &decoded);
  EXPECT(deser.ok());
  EXPECT(decoded == original);
}

void test_serialization_traits_helper_errors() {
  MockGrpcPing msg{};
  bool own_buffer = false;
  EXPECT(!grpc::SerializationTraits<MockGrpcPing>::Serialize(msg, nullptr, &own_buffer).ok());
  EXPECT(!grpc::SerializationTraits<MockGrpcPing>::Deserialize(nullptr, &msg).ok());

  const uint8_t malformed[] = {0x00};
  grpc::Slice bad_slice(malformed, sizeof(malformed));
  grpc::ByteBuffer bad_bb(&bad_slice, 1);
  grpc::Status bad_status = grpc::SerializationTraits<MockGrpcPing>::Deserialize(&bad_bb, &msg);
  EXPECT(!bad_status.ok());
  EXPECT(bad_status.error_code() == grpc::StatusCode::DATA_LOSS);
}

}  // namespace

void run_grpc_traits_mock_tests() {
  RUN_CASE(test_serialization_traits_helper_round_trip);
  RUN_CASE(test_serialization_traits_helper_errors);
}
