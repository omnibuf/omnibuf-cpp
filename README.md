# OmniBuf for C++20

**[Website](https://omnibuf.com)** · **[How It Works](https://omnibuf.com/how-it-works)** · **[Go Implementation](https://github.com/omnibuf/omnibuf-go)** · **[TypeScript Implementation](https://github.com/omnibuf/omnibuf-es)** · **[Wire Format Specification](https://omnibuf.com/spec)**

Self-contained C++20 header library (`include/omnibuf/`) and `protoc-gen-omnibuf-cpp` code generator for the **OmniBuf** binary wire format. Zero external C++ library dependencies.

1. **Single-instruction Prefix-Varint integers:** `std::countr_zero` decodes variable-length integers in one unaligned load and shift.
2. **Single-pass forward serialization:** Nested sub-messages, typed arrays, and maps reserve a 2-byte Prefix-Varint length slot and backpatch in place after writing payload bytes.
3. **Zero-allocation Opaque views and splicing:** `Opaque*` wrappers jump across field headers via `locate_field`, return `std::string_view` slices directly into packet memory, and splice single-field edits via `splice_fields`.
4. **Schema-free wire inspection:** Explicit 4-bit wire types (`0..15`) disambiguate UTF-8 strings, raw bytes, sub-messages, homogeneous arrays, and typed maps on the wire. Optional Field `#0` carries human-readable field names inline.

---

## Requirements and Build

`omnibuf-cpp` targets standard **C++20** (`<bit>`, `<span>`, `<string_view>`, `<charconv>`, `<optional>`) on Clang or GCC, paired with `cmake`, `buf`, and `just` pinned in `mise.toml`.

```bash
mise trust
mise install

just build
just test
just conformance
just bench
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

`protoc-gen-omnibuf-cpp` emits value-type C++20 structs alongside `Opaque<Message>` zero-decode wrappers for every `.proto` message:

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

// Zero-allocation nested lookup directly over raw bytes
omnibuf::bench::OpaquePeerUpdate view(packet);
std::string_view city = view.get_location().get_city_view(); // "Frankfurt"

// Zero-decode field splice: copies unmodified spans verbatim
view.set_sequence(9876543211ULL);
std::vector<uint8_t> updated_packet = view.marshal();
```

### 2. Native C++20 Structs

Specialize `omnibuf::StructTraits<T>` to serialize plain C++20 structs directly. Optional fields (`std::optional<T>`), recursive pointers (`std::unique_ptr<T>`), vectors, maps, enums, and wire overrides (`WireOverride::Sint`, `Fixed32`, `Fixed64`) all map cleanly:

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

`omnibuf::DescriptorPool` loads binary `FileDescriptorSet` images produced by `buf build -o schemas.fds.pb --exclude-source-info`. `DynamicMessage` transcodes between standard Protobuf binary wire payloads and OmniBuf packets at runtime:

```cpp
omnibuf::DescriptorPool pool;
auto loaded = pool.load_file_descriptor_set(fds_bytes);

const omnibuf::MessageDesc* desc = pool.find_message("omnibuf.bench.PeerUpdate");
auto msg = omnibuf::proto::from_binary(*desc, protobuf_wire_bytes);
auto omnibuf_bytes = omnibuf::marshal_dynamic(*desc, msg.value());
```

### 4. Schema-Free Blind Inspection

`omnibuf::decode_blind` and `omnibuf::decode_blind_json` inspect arbitrary OmniBuf packets directly from their 4-bit wire tags, resolving human-readable keys automatically whenever Self-Describing Field `#0` metadata is present:

```cpp
auto json = omnibuf::decode_blind_json(encoded.value());
// {"asn":64512,"router_id":"edge-rtr-fra01","active":true,"rssi_dbm":-62,"tags":["tier1","ebgp"]}
```

---

## Cross-Language Conformance

`conformance-cpp` implements the 4-byte little-endian length-prefixed stdin/stdout conformance protocol across all four tiers and passes all **584 / 584** cases from the canonical runner:

```text
OmniBuf Conformance Suite (./build/conformance-cpp)
  TIER_PLUGIN:              205 passed
  TIER_PROTOBUF_TYPES:      123 passed
  TIER_ANNOTATED_STRUCT:    128 passed
  TIER_BLIND:               128 passed
  Total: 584 passed, 0 skipped, 0 failed (out of 584 cases)
```

---

## Benchmarks

Measured by `./build/omnibuf_bench` (`just bench`, `-O3 -DNDEBUG`, Apple Clang 21.0.0 C++20 on Apple M1 Pro, `darwin/arm64`) on the 11-field `SamplePeerUpdate` workload (`OmniBuf = 366 B`, `Protobuf = 341 B`, `JSON = 803 B`). Raw results are stored in [`testdata/benchmarks.json`](testdata/benchmarks.json).

| Category | OmniBuf Generated | OmniBuf C++ Struct | Protobuf LEB128 | JSON |
| :--- | ---: | ---: | ---: | ---: |
| **Full Encode** | **370 ns/op** (366 B) | 465 ns/op (366 B) | 403 ns/op (341 B) | 1,693 ns/op (803 B) |
| **Full Decode** | **509 ns/op** | 1,153 ns/op | 562 ns/op | 1,622 ns/op |
| **Nested Lookup (`peer.location.city`)** | **118 ns/op** | - | 562 ns/op | 1,571 ns/op |
| **Single-Field Splice (`peer.sequence`)** | **153 ns/op** | - | 974 ns/op | 3,341 ns/op |

---

## License

Released under the [MIT License](./LICENSE).
