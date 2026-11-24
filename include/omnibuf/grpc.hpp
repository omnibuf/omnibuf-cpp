#pragma once

// gRPC codec helpers for OmniBuf (`grpc::SerializationTraits`).

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

#include "omnibuf/struct_codec.hpp"

namespace omnibuf::grpc_codec {

inline constexpr std::string_view kCodecName = "omnibuf";
inline constexpr std::string_view kSelfDescribingCodecName = "x-omnibuf";
inline constexpr std::string_view kContentTypeGrpc = "application/grpc+omnibuf";

template <RegisteredStruct T>
[[nodiscard]] inline Result<std::vector<uint8_t>> serialize(const T& msg,
                                                            bool deterministic = false,
                                                            bool self_describing = false) {
  return omnibuf::marshal(
      msg, MarshalOptions{.deterministic = deterministic, .self_describing = self_describing});
}

template <RegisteredStruct T>
[[nodiscard]] inline Result<T> deserialize(std::span<const uint8_t> data,
                                           DecodeOptions opts = {}) {
  T msg{};
  auto status = omnibuf::unmarshal(data, msg, opts);
  if (!status) {
    return Error{status.error()};
  }
  return msg;
}

#ifdef GRPCPP_SUPPORT_SERIALIZATION_TRAITS_H

// Helper base for specializing grpc::SerializationTraits<T> on a StructTraits type:
//
//   template <>
//   struct grpc::SerializationTraits<MyMessage>
//       : omnibuf::grpc_codec::SerializationTraitsHelper<MyMessage> {};
template <RegisteredStruct T, bool Deterministic = false, bool SelfDescribing = false>
struct SerializationTraitsHelper {
  static grpc::Status Serialize(const T& msg, grpc::ByteBuffer* bb, bool* own_buffer) {
    if (bb == nullptr || own_buffer == nullptr) {
      return grpc::Status(grpc::StatusCode::INTERNAL, "omnibuf: null output ByteBuffer");
    }
    auto encoded = serialize(msg, Deterministic, SelfDescribing);
    if (!encoded) {
      return grpc::Status(grpc::StatusCode::INTERNAL, encoded.error());
    }
    const auto& bytes = encoded.value();
    grpc::Slice slice(bytes.data(), bytes.size());
    grpc::ByteBuffer tmp(&slice, 1);
    bb->Swap(&tmp);
    *own_buffer = true;
    return grpc::Status::OK;
  }

  static grpc::Status Deserialize(grpc::ByteBuffer* buffer, T* msg) {
    if (buffer == nullptr || msg == nullptr) {
      return grpc::Status(grpc::StatusCode::INTERNAL, "omnibuf: null input ByteBuffer or message");
    }
    std::vector<grpc::Slice> slices;
    grpc::Status dump_status = buffer->Dump(&slices);
    if (!dump_status.ok()) {
      return dump_status;
    }
    size_t total = 0;
    for (const auto& s : slices) {
      total += s.size();
    }
    std::vector<uint8_t> flat;
    flat.reserve(total);
    for (const auto& s : slices) {
      const auto* ptr = static_cast<const uint8_t*>(s.begin());
      flat.insert(flat.end(), ptr, ptr + s.size());
    }
    auto status = omnibuf::unmarshal(std::span<const uint8_t>(flat), *msg);
    if (!status) {
      return grpc::Status(grpc::StatusCode::DATA_LOSS, status.error());
    }
    return grpc::Status::OK;
  }
};

#endif  // GRPCPP_SUPPORT_SERIALIZATION_TRAITS_H

}  // namespace omnibuf::grpc_codec
