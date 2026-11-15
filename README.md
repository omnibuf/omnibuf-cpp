# OmniBuf for C++20

[![CI](https://github.com/omnibuf/omnibuf-cpp/actions/workflows/ci.yml/badge.svg)](https://github.com/omnibuf/omnibuf-cpp/actions/workflows/ci.yml)

**[Website](https://omnibuf.com)** · **[How It Works](https://omnibuf.com/how-it-works)** · **[Go Implementation](https://github.com/omnibuf/omnibuf-go)** · **[TypeScript Implementation](https://github.com/omnibuf/omnibuf-es)** · **[Wire Format Specification](https://omnibuf.com/spec)**

Self-contained C++20 header library (`include/omnibuf/`) and `protoc-gen-omnibuf-cpp` code generator for the **OmniBuf** binary wire format. Zero external C++ library dependencies.

1. **Single-instruction Prefix-Varint integers:** `std::countr_zero` decodes variable-length integers in one unaligned load and shift.
2. **Single-pass forward serialization:** Nested sub-messages, typed arrays, and maps reserve a 2-byte Prefix-Varint length slot and backpatch in place after writing payload bytes. Payloads exceeding 16,383 bytes expand cleanly to 4-byte or 9-byte headers.
3. **Zero-allocation Opaque views and splicing:** `Opaque*` wrappers jump across field headers via `locate_field`, return `std::string_view` slices directly into packet memory, and splice single-field edits via `try_splice_fields`.
4. **Schema-free wire inspection:** Explicit 4-bit wire types (`0..15`) disambiguate UTF-8 strings, raw bytes, sub-messages, homogeneous arrays, and typed maps on the wire. Optional Field `#0` carries human-readable field names inline.

---

## Requirements and Build

`omnibuf-cpp` targets standard **C++20** (`<bit>`, `<span>`, `<string_view>`, `<charconv>`, `<optional>`) on Clang or GCC. Build tooling (`cmake`, `buf`, and `just`) is pinned in `mise.toml`.

```bash
mise trust
mise install

just build
just test
just conformance
just bench
```

### Downstream CMake Integration

Link the `omnibuf::omnibuf` header-only target through `FetchContent`, `CPM.cmake`, or `find_package(omnibuf CONFIG REQUIRED)`:

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

---

## Buf Code Generation Workflow

Schema linting, C++20 header generation, and binary `FileDescriptorSet` builds run through the **Buf CLI** (`buf.yaml` v2 and `buf.gen.yaml` v2).

`buf.yaml`:

```yaml
version: v2
modules:
  - path: proto
lint:
  use:
    - STANDARD
breaking:
  use:
    - FILE
```

`buf.gen.yaml`:

```yaml
version: v2
plugins:
  - local: ./build/protoc-gen-omnibuf-cpp
    out: build/gen
```

Run code generation and descriptor compilation:

```bash
just proto
# Runs:
#   cmake --build build --target protoc-gen-omnibuf-cpp -j
#   buf lint
#   buf generate
#   buf build -o build/gen/schemas.fds.pb --exclude-source-info
```

---

## C++20 Library Usage

### 1. Generated Headers and Opaque Views

`protoc-gen-omnibuf-cpp` emits value-type C++20 structs alongside `Opaque<Message>` zero-decode wrappers for every `.proto` message. Explicit `proto3` `optional` fields, `oneof` unions, nested messages, generalized `map<K, V>` fields, recursive `omnibuf::Box<T>` sub-messages, and messages wider than 64 fields all generate ready-to-compile headers:

```cpp
#include "omnibuf/bench/peer.omnibuf.pb.hpp"

omnibuf::bench::PeerUpdate update;
update.asn = 64512;
update.router_id = "edge-rtr-fra01.core.backbone.net";
update.active = true;
update.location = omnibuf::bench::Location{
    .region = "eu-central-1",
    .datacenter = "FRA-Equinix-FR5",
    .city = "Frankfurt",
    .rack = 42,
};
update.sequence = 9876543210ULL;

// Single-pass forward encode
std::vector<uint8_t> packet = update.marshal_omnibuf();

// Zero-allocation nested lookup
omnibuf::bench::OpaquePeerUpdate view(packet);
std::string_view city = view.get_location().get_city_view();

// Fallible zero-decode field splice
view.set_sequence(9876543211ULL);
auto spliced = view.try_marshal();

// Bounded decode resource limits
omnibuf::bench::PeerUpdate decoded;
auto status = decoded.unmarshal_omnibuf(spliced.value(), {
    .max_depth = 16,
    .max_message_bytes = 1024 * 1024,
    .max_total_elements = 65536,
});
```

### 2. Native C++20 Structs

Specialize `omnibuf::StructTraits<T>` to serialize plain C++20 structs directly. Optional fields (`std::optional<T>`), copyable recursive wrappers (`omnibuf::Box<T>`), `std::unique_ptr<T>`, vectors, `std::map`, `std::unordered_map`, enums, and wire overrides (`WireOverride::Sint`, `Fixed32`, `Fixed64`) map directly onto OmniBuf wire types:

```cpp
#include "omnibuf/omnibuf.hpp"

struct PeerEvent {
  uint32_t asn = 0;
  std::string router_id;
  bool active = false;
  int32_t rssi_dbm = 0;
  std::vector<std::string> tags;
};

template <>
struct omnibuf::StructTraits<PeerEvent> {
  static constexpr std::string_view type_name = "PeerEvent";
  static constexpr auto fields() {
    return std::make_tuple(
        omnibuf::field<1, &PeerEvent::asn>("asn"),
        omnibuf::field<2, &PeerEvent::router_id>("router_id"),
        omnibuf::field<3, &PeerEvent::active>("active"),
        omnibuf::field<4, &PeerEvent::rssi_dbm, omnibuf::WireOverride::Sint>("rssi_dbm"),
        omnibuf::field<5, &PeerEvent::tags>("tags"));
  }
};

auto encoded = omnibuf::marshal(PeerEvent{
    .asn = 64512,
    .router_id = "edge-rtr-fra01",
    .active = true,
    .rssi_dbm = -62,
    .tags = {"tier1", "ebgp"},
}, {.self_describing = true});

PeerEvent decoded;
auto status = omnibuf::unmarshal(encoded.value(), decoded);
```

### 3. Dynamic Protobuf Descriptors

`omnibuf::proto::DescriptorPool` loads binary `FileDescriptorSet` images produced by `buf build -o schemas.fds.pb --exclude-source-info`. `omnibuf::proto::DynamicMessage` decodes and encodes OmniBuf wire packets dynamically against runtime message descriptors:

```cpp
omnibuf::proto::DescriptorPool pool;
auto loaded = pool.load_file("build/gen/schemas.fds.pb");

const omnibuf::proto::MessageDesc* desc = pool.find_message("omnibuf.bench.PeerUpdate");

omnibuf::proto::DynamicMessage dmsg;
auto ok = omnibuf::proto::from_binary(*desc, packet, dmsg);
auto reencoded = omnibuf::proto::to_binary(*desc, dmsg, {.deterministic = true});
```

### 4. Schema-Free Blind Inspection

`omnibuf::decode_blind` and `omnibuf::decode_blind_json` inspect arbitrary OmniBuf packets directly from their 4-bit wire tags, resolving human-readable keys automatically whenever Self-Describing Field `#0` metadata is present:

```cpp
auto json = omnibuf::decode_blind_json(encoded.value());
// {"active":true,"asn":64512,"router_id":"edge-rtr-fra01","rssi_dbm":-62,"tags":["tier1","ebgp"]}
```

---

## Cross-Language Conformance

`conformance-cpp` implements the 4-byte little-endian length-prefixed stdin/stdout conformance protocol across all four tiers and passes all **758 / 758** cases from the canonical runner:

```text
OmniBuf Conformance Suite (./build/conformance-cpp)
  TIER_PLUGIN:              317 passed
  TIER_PROTOBUF_TYPES:      144 passed
  TIER_ANNOTATED_STRUCT:    151 passed
  TIER_BLIND:               146 passed
  Total: 758 passed, 0 skipped, 0 failed (out of 758 cases)
```

---

## Benchmarks

Measured by `./build/omnibuf_bench` (`just bench`, `-O3 -DNDEBUG`, Apple Clang 21.0.0 C++20 on Apple M1 Pro, `darwin/arm64`) on the 11-field `SamplePeerUpdate` workload (`OmniBuf = 366 B`, `Protobuf = 341 B`, `JSON = 803 B`, reading `peer.location.city` and mutating `peer.sequence`). `omnibuf_bench` runs six categories in total, adding `Large Embedded Message Encode` and `Embedded Message Growth` stress workloads where nested payloads exceed 16,383 bytes and trigger 2-byte to 4-byte length-prefix expansion. Raw results are stored in [`testdata/benchmarks.json`](testdata/benchmarks.json).

| Category | OmniBuf Generated | OmniBuf C++ Struct | Protobuf LEB128 | JSON |
| :--- | ---: | ---: | ---: | ---: |
| **Full Message Encode Speed** | **281 ns/op** (366 B) | 348 ns/op (366 B) | 306 ns/op (341 B) | 670 ns/op (803 B) |
| **Full Message Decode Speed** | **654 ns/op** | 877 ns/op | 791 ns/op | 1,701 ns/op |
| **Read a Single Nested Field** | **120 ns/op** | - | 591 ns/op | 1,495 ns/op |
| **Selective Single-Field Mutation** | **436 ns/op** | - | 960 ns/op | 2,375 ns/op |
| **Large Embedded Message Encode** | 5,240 ns/op (39,256 B) | 5,327 ns/op (39,256 B) | **4,203 ns/op** (39,227 B) | 53,581 ns/op (39,681 B) |
| **Embedded Message Growth** | **3,640 ns/op** | - | 5,967 ns/op | 36,900 ns/op |

---

## License

Released under the [MIT License](./LICENSE).
