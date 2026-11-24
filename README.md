# omnibuf-cpp

[![OmniBuf Spec: v1 DRAFT-1](https://img.shields.io/badge/OmniBuf%20Spec-v1%20DRAFT--1-FF4F18)](https://omnibuf.com/spec)

Header-only C++20 library (`include/omnibuf/`) and `protoc-gen-omnibuf-cpp` code generator for [OmniBuf Wire Specification v1 DRAFT-1](https://omnibuf.com/spec). Full docs live at [omnibuf.com/cpp](https://omnibuf.com/cpp).

## Install

Link `omnibuf::omnibuf` through CMake `FetchContent`, `CPM.cmake`, or `find_package(omnibuf CONFIG REQUIRED)`:

```cmake
include(FetchContent)
FetchContent_Declare(
  omnibuf
  GIT_REPOSITORY https://github.com/omnibuf/omnibuf-cpp.git
  GIT_TAG        v0.1.0
)
FetchContent_MakeAvailable(omnibuf)

target_link_libraries(my_service PRIVATE omnibuf::omnibuf)
```

## Quick usage

```cpp
#include "omnibuf/omnibuf.hpp"

struct PeerEvent {
  uint32_t asn = 0;
  std::string router_id;
  int32_t rssi_dbm = 0;
};

template <>
struct omnibuf::StructTraits<PeerEvent> {
  static constexpr std::string_view type_name = "PeerEvent";
  static constexpr auto fields() {
    return std::make_tuple(
        omnibuf::field<1, &PeerEvent::asn>("asn"),
        omnibuf::field<2, &PeerEvent::router_id>("router_id"),
        omnibuf::field<4, &PeerEvent::rssi_dbm, omnibuf::WireOverride::Sint>("rssi_dbm"));
  }
};

auto encoded = omnibuf::marshal(
    PeerEvent{.asn = 64512, .router_id = "edge-rtr-fra01", .rssi_dbm = -62},
    {.self_describing = true});

PeerEvent decoded;
auto status = omnibuf::unmarshal(encoded.value(), decoded);
```

## Implementation

- **Zero external dependencies:** Targets standard C++20 (`<bit>`, `<span>`, `<string_view>`, `<charconv>`, `<optional>`) on Clang and GCC.
- **Native structs (`omnibuf::StructTraits<T>`):** Serializes plain C++20 structs, `std::optional<T>`, `omnibuf::Box<T>`, vectors, maps, enums, and wire overrides (`Sint`, `Fixed32`, `Fixed64`).
- **Generated headers and `Opaque` views (`protoc-gen-omnibuf-cpp`):** Emits value structs and zero-decode `Opaque<Message>` wrappers that return `std::string_view` slices and splice updates via `try_marshal` and `try_splice_fields`.
- **Dynamic descriptors (`omnibuf::proto::DescriptorPool`):** Loads binary `FileDescriptorSet` images from `buf build` for runtime `DynamicMessage` encode and decode.
- **Buffer ownership:** `Opaque` views borrow packet storage; keep the backing buffer alive and unmodified while views are live. `FastWriter` holds exclusive access to its backing vector until writing ends.
- **Conformance:** `conformance-cpp` passes all 758/758 cross-language conformance cases (`just conformance`).

## License

Apache-2.0
