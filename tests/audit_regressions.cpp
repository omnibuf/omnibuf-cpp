#ifndef _WIN32
#include <sys/wait.h>
#include <unistd.h>
#endif

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <new>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "omnibuf/bench/peer.omnibuf.pb.hpp"
#include "omnibuf/conformance/conformance.omnibuf.pb.hpp"
#include "omnibuf/omnibuf.hpp"
#include "omnibuf/test/edge.omnibuf.pb.hpp"

#if !defined(__SANITIZE_ADDRESS__) && \
    !(defined(__has_feature) && __has_feature(address_sanitizer))
#define OMNIBUF_TRACK_HEAP 1
#else
#define OMNIBUF_TRACK_HEAP 0
#endif

#if OMNIBUF_TRACK_HEAP
namespace {

std::atomic<std::ptrdiff_t> g_live_heap_bytes{0};

struct AllocPrefix {
  void* base;
  std::size_t size;
  std::size_t magic;
};

constexpr std::size_t kAllocMagic = 0x4F4D4E4942554631ULL;

void* tracked_alloc_aligned(std::size_t size, std::size_t alignment) noexcept {
  const std::size_t align =
      std::max<std::size_t>(alignment, alignof(std::max_align_t));
  if ((align & (align - 1)) != 0) {
    return nullptr;
  }
  const std::size_t actual = (size == 0) ? 1 : size;
  if (actual > SIZE_MAX - sizeof(AllocPrefix) - align) {
    return nullptr;
  }
  void* raw = std::malloc(sizeof(AllocPrefix) + align + actual);
  if (raw == nullptr) {
    return nullptr;
  }
  const uintptr_t min_user = reinterpret_cast<uintptr_t>(raw) + sizeof(AllocPrefix);
  const uintptr_t user_addr = (min_user + (align - 1)) & ~(static_cast<uintptr_t>(align) - 1);
  auto* hdr = reinterpret_cast<AllocPrefix*>(user_addr) - 1;
  hdr->base = raw;
  hdr->size = actual;
  hdr->magic = kAllocMagic;
  g_live_heap_bytes.fetch_add(static_cast<std::ptrdiff_t>(actual), std::memory_order_relaxed);
  return reinterpret_cast<void*>(user_addr);
}

void* tracked_alloc(std::size_t size) noexcept {
  return tracked_alloc_aligned(size, alignof(std::max_align_t));
}

void tracked_free(void* ptr) noexcept {
  if (ptr == nullptr) {
    return;
  }
  auto* hdr = reinterpret_cast<AllocPrefix*>(ptr) - 1;
  if (hdr->magic == kAllocMagic) {
    hdr->magic = 0;
    g_live_heap_bytes.fetch_sub(static_cast<std::ptrdiff_t>(hdr->size), std::memory_order_relaxed);
    std::free(hdr->base);
  }
}

}  // namespace

void* operator new(std::size_t size) {
  if (void* p = tracked_alloc(size)) {
    return p;
  }
  throw std::bad_alloc();
}

void* operator new(std::size_t size, const std::nothrow_t&) noexcept {
  return tracked_alloc(size);
}

void* operator new[](std::size_t size) {
  if (void* p = tracked_alloc(size)) {
    return p;
  }
  throw std::bad_alloc();
}

void* operator new[](std::size_t size, const std::nothrow_t&) noexcept {
  return tracked_alloc(size);
}

void* operator new(std::size_t size, std::align_val_t al) {
  if (void* p = tracked_alloc_aligned(size, static_cast<std::size_t>(al))) {
    return p;
  }
  throw std::bad_alloc();
}

void* operator new(std::size_t size, std::align_val_t al, const std::nothrow_t&) noexcept {
  return tracked_alloc_aligned(size, static_cast<std::size_t>(al));
}

void* operator new[](std::size_t size, std::align_val_t al) {
  if (void* p = tracked_alloc_aligned(size, static_cast<std::size_t>(al))) {
    return p;
  }
  throw std::bad_alloc();
}

void* operator new[](std::size_t size, std::align_val_t al, const std::nothrow_t&) noexcept {
  return tracked_alloc_aligned(size, static_cast<std::size_t>(al));
}

void operator delete(void* ptr) noexcept { tracked_free(ptr); }
void operator delete(void* ptr, std::size_t) noexcept { tracked_free(ptr); }
void operator delete(void* ptr, const std::nothrow_t&) noexcept { tracked_free(ptr); }
void operator delete[](void* ptr) noexcept { tracked_free(ptr); }
void operator delete[](void* ptr, std::size_t) noexcept { tracked_free(ptr); }
void operator delete[](void* ptr, const std::nothrow_t&) noexcept { tracked_free(ptr); }
void operator delete(void* ptr, std::align_val_t) noexcept { tracked_free(ptr); }
void operator delete(void* ptr, std::size_t, std::align_val_t) noexcept { tracked_free(ptr); }
void operator delete(void* ptr, std::align_val_t, const std::nothrow_t&) noexcept {
  tracked_free(ptr);
}
void operator delete[](void* ptr, std::align_val_t) noexcept { tracked_free(ptr); }
void operator delete[](void* ptr, std::size_t, std::align_val_t) noexcept { tracked_free(ptr); }
void operator delete[](void* ptr, std::align_val_t, const std::nothrow_t&) noexcept {
  tracked_free(ptr);
}
#endif

namespace {

int g_passed = 0;
int g_failed = 0;
int g_case_failures = 0;
volatile uintptr_t g_volatile_sink = 0;

#define EXPECT(cond)                                                               \
  do {                                                                             \
    if (!(cond)) {                                                                 \
      std::fprintf(stderr, "  FAIL [%s:%d]: %s\n", __FILE__, __LINE__, #cond);     \
      ++g_case_failures;                                                           \
    }                                                                              \
  } while (0)

#define EXPECT_MSG(cond, msg)                                                      \
  do {                                                                             \
    if (!(cond)) {                                                                 \
      std::fprintf(stderr, "  FAIL [%s:%d]: %s (%s)\n", __FILE__, __LINE__, #cond, \
                   (msg));                                                         \
      ++g_case_failures;                                                           \
    }                                                                              \
  } while (0)

#define RUN_CASE(fn)                              \
  do {                                            \
    g_case_failures = 0;                          \
    fn();                                         \
    if (g_case_failures == 0) {                   \
      ++g_passed;                                 \
      std::printf("PASS %s\n", #fn);              \
    } else {                                      \
      ++g_failed;                                 \
      std::printf("FAIL %s\n", #fn);              \
    }                                             \
  } while (0)

// StructTraits test types for CPP-4 & CPP-5.
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

}  // namespace

template <>
struct omnibuf::StructTraits<DepthStructNode> {
  static constexpr std::string_view type_name = "omnibuf.audit.DepthStructNode";
  static constexpr auto fields() {
    using T = DepthStructNode;
    return std::make_tuple(omnibuf::field<1, &T::id>("id"), omnibuf::field<2, &T::next>("next"));
  }
};

template <>
struct omnibuf::StructTraits<UnscopedEnumHolder> {
  static constexpr std::string_view type_name = "omnibuf.audit.UnscopedEnumHolder";
  static constexpr auto fields() {
    using T = UnscopedEnumHolder;
    return std::make_tuple(omnibuf::field<1, &T::state>("state"));
  }
};

template <>
struct omnibuf::StructTraits<SignedFixedEnumHolder> {
  static constexpr std::string_view type_name = "omnibuf.audit.SignedFixedEnumHolder";
  static constexpr auto fields() {
    using T = SignedFixedEnumHolder;
    return std::make_tuple(
        omnibuf::field<1, &T::val, omnibuf::WireOverride::Fixed32>("val"));
  }
};

namespace {

// Minimal binary Protobuf CodeGeneratorRequest / CodeGeneratorResponse helpers
// for invoking protoc-gen-omnibuf-cpp in-process via POSIX pipes.
void pb_write_bytes_field(std::vector<uint8_t>& out,
                          uint32_t fnum,
                          std::span<const uint8_t> payload) {
  omnibuf::proto::detail::write_pb_varint(out, (static_cast<uint64_t>(fnum) << 3) | 2u);
  omnibuf::proto::detail::write_pb_varint(out, payload.size());
  out.insert(out.end(), payload.begin(), payload.end());
}

void pb_write_varint_field(std::vector<uint8_t>& out, uint32_t fnum, uint64_t val) {
  omnibuf::proto::detail::write_pb_varint(out, (static_cast<uint64_t>(fnum) << 3) | 0u);
  omnibuf::proto::detail::write_pb_varint(out, val);
}

std::vector<uint8_t> encode_pb_field_desc(const omnibuf::proto::FieldDescriptorProto& f) {
  std::vector<uint8_t> b;
  omnibuf::proto::detail::write_pb_string_field(b, 1, f.name);
  pb_write_varint_field(b, 3, f.number);
  pb_write_varint_field(b, 4, static_cast<uint64_t>(f.label));
  pb_write_varint_field(b, 5, static_cast<uint64_t>(f.type));
  if (!f.type_name.empty()) {
    omnibuf::proto::detail::write_pb_string_field(b, 6, f.type_name);
  }
  if (f.oneof_index.has_value()) {
    pb_write_varint_field(b, 9, *f.oneof_index);
  }
  if (f.proto3_optional) {
    pb_write_varint_field(b, 17, 1);
  }
  return b;
}

std::vector<uint8_t> encode_pb_msg_desc(const omnibuf::proto::DescriptorProto& m) {
  std::vector<uint8_t> b;
  omnibuf::proto::detail::write_pb_string_field(b, 1, m.name);
  for (const auto& f : m.fields) {
    const auto fb = encode_pb_field_desc(f);
    pb_write_bytes_field(b, 2, fb);
  }
  for (const auto& nested : m.nested_types) {
    const auto nb = encode_pb_msg_desc(nested);
    pb_write_bytes_field(b, 3, nb);
  }
  for (const auto& od : m.oneof_decls) {
    std::vector<uint8_t> ob;
    omnibuf::proto::detail::write_pb_string_field(ob, 1, od.name);
    pb_write_bytes_field(b, 8, ob);
  }
  return b;
}

std::vector<uint8_t> encode_pb_file_desc(const omnibuf::proto::FileDescriptorProto& file) {
  std::vector<uint8_t> b;
  omnibuf::proto::detail::write_pb_string_field(b, 1, file.name);
  if (!file.package.empty()) {
    omnibuf::proto::detail::write_pb_string_field(b, 2, file.package);
  }
  for (const auto& dep : file.dependencies) {
    omnibuf::proto::detail::write_pb_string_field(b, 3, dep);
  }
  for (const auto& m : file.message_types) {
    const auto mb = encode_pb_msg_desc(m);
    pb_write_bytes_field(b, 4, mb);
  }
  if (!file.syntax.empty()) {
    omnibuf::proto::detail::write_pb_string_field(b, 12, file.syntax);
  }
  return b;
}

std::vector<uint8_t> encode_pb_codegen_request(const omnibuf::proto::CodeGeneratorRequest& req) {
  std::vector<uint8_t> b;
  for (const auto& target : req.files_to_generate) {
    omnibuf::proto::detail::write_pb_string_field(b, 1, target);
  }
  for (const auto& file : req.proto_files) {
    const auto fb = encode_pb_file_desc(file);
    pb_write_bytes_field(b, 15, fb);
  }
  return b;
}

struct PluginResponse {
  bool exited_cleanly = false;
  std::string error{};
  std::map<std::string, std::string> files{};
};

PluginResponse invoke_codegen_plugin(const omnibuf::proto::CodeGeneratorRequest& req) {
  PluginResponse out;
#ifdef _WIN32
  (void)req;
  out.error = "POSIX pipe execution required";
  return out;
#else
  const std::vector<uint8_t> req_bytes = encode_pb_codegen_request(req);

  const char* plugin_path =
#ifdef OMNIBUF_PLUGIN_BIN
      OMNIBUF_PLUGIN_BIN;
#else
      "./build/protoc-gen-omnibuf-cpp";
#endif
  if (::access(plugin_path, X_OK) != 0) {
    if (::access("./build/protoc-gen-omnibuf-cpp", X_OK) == 0) {
      plugin_path = "./build/protoc-gen-omnibuf-cpp";
    } else if (::access("./protoc-gen-omnibuf-cpp", X_OK) == 0) {
      plugin_path = "./protoc-gen-omnibuf-cpp";
    }
  }

  int in_pipe[2] = {-1, -1};
  int out_pipe[2] = {-1, -1};
  if (::pipe(in_pipe) != 0 || ::pipe(out_pipe) != 0) {
    out.error = "pipe() failed";
    return out;
  }

  const pid_t pid = ::fork();
  if (pid < 0) {
    ::close(in_pipe[0]);
    ::close(in_pipe[1]);
    ::close(out_pipe[0]);
    ::close(out_pipe[1]);
    out.error = "fork() failed";
    return out;
  }

  if (pid == 0) {
    ::close(in_pipe[1]);
    ::close(out_pipe[0]);
    (void)::dup2(in_pipe[0], STDIN_FILENO);
    (void)::dup2(out_pipe[1], STDOUT_FILENO);
    ::close(in_pipe[0]);
    ::close(out_pipe[1]);
    char* const argv[] = {const_cast<char*>(plugin_path), nullptr};
    ::execv(plugin_path, argv);
    ::_exit(127);
  }

  ::close(in_pipe[0]);
  ::close(out_pipe[1]);

  size_t written = 0;
  while (written < req_bytes.size()) {
    const ssize_t n =
        ::write(in_pipe[1], req_bytes.data() + written, req_bytes.size() - written);
    if (n < 0) {
      if (errno == EINTR) continue;
      break;
    }
    written += static_cast<size_t>(n);
  }
  ::close(in_pipe[1]);

  std::vector<uint8_t> resp_bytes;
  uint8_t chunk[8192];
  while (true) {
    const ssize_t n = ::read(out_pipe[0], chunk, sizeof(chunk));
    if (n < 0) {
      if (errno == EINTR) continue;
      break;
    }
    if (n == 0) break;
    resp_bytes.insert(resp_bytes.end(), chunk, chunk + n);
  }
  ::close(out_pipe[0]);

  int status = 0;
  (void)::waitpid(pid, &status, 0);
  out.exited_cleanly = WIFEXITED(status) && (WEXITSTATUS(status) == 0);

  size_t pos = 0;
  const std::span<const uint8_t> sp(resp_bytes);
  while (pos < sp.size()) {
    auto tag = omnibuf::proto::detail::read_pb_varint(sp, pos);
    if (!tag) break;
    pos = tag.value().next_pos;
    const uint32_t fn = static_cast<uint32_t>(tag.value().val >> 3);
    const uint32_t wt = static_cast<uint32_t>(tag.value().val & 7u);
    if (wt == 2 && fn == 1) {
      auto s = omnibuf::proto::detail::read_pb_bytes(sp, pos);
      if (!s) break;
      pos = s.value().next_pos;
      out.error.assign(reinterpret_cast<const char*>(s.value().slice.data()),
                       s.value().slice.size());
    } else if (wt == 2 && fn == 15) {
      auto f = omnibuf::proto::detail::read_pb_bytes(sp, pos);
      if (!f) break;
      pos = f.value().next_pos;
      const auto fsp = f.value().slice;
      std::string fname;
      std::string fcontent;
      size_t fp = 0;
      while (fp < fsp.size()) {
        auto ft = omnibuf::proto::detail::read_pb_varint(fsp, fp);
        if (!ft) break;
        fp = ft.value().next_pos;
        const uint32_t ffn = static_cast<uint32_t>(ft.value().val >> 3);
        const uint32_t fwt = static_cast<uint32_t>(ft.value().val & 7u);
        if (fwt == 2 && (ffn == 1 || ffn == 15)) {
          auto fs = omnibuf::proto::detail::read_pb_bytes(fsp, fp);
          if (!fs) break;
          fp = fs.value().next_pos;
          std::string str(reinterpret_cast<const char*>(fs.value().slice.data()),
                          fs.value().slice.size());
          if (ffn == 1) {
            fname = std::move(str);
          } else {
            fcontent = std::move(str);
          }
        } else {
          auto sk = omnibuf::proto::detail::skip_pb_field(fsp, fp, fwt);
          if (!sk) break;
          fp = sk.value();
        }
      }
      out.files[std::move(fname)] = std::move(fcontent);
    } else {
      auto sk = omnibuf::proto::detail::skip_pb_field(sp, pos, wt);
      if (!sk) break;
      pos = sk.value();
    }
  }
  return out;
#endif
}

void scrub_freed_heap_slots(size_t target_size, uint8_t fill_byte) {
  std::vector<std::vector<uint8_t>> blocks;
  blocks.reserve(64);
  for (int i = 0; i < 32; ++i) {
    blocks.emplace_back(target_size, fill_byte);
    g_volatile_sink ^= reinterpret_cast<uintptr_t>(blocks.back().data()) ^ blocks.back()[0];
  }
  for (size_t delta : {size_t{8}, size_t{16}, size_t{32}, size_t{48}, size_t{64}}) {
    blocks.emplace_back(target_size + delta, fill_byte);
    g_volatile_sink ^= reinterpret_cast<uintptr_t>(blocks.back().data()) ^ blocks.back()[0];
    if (target_size > delta) {
      blocks.emplace_back(target_size - delta, fill_byte);
      g_volatile_sink ^= reinterpret_cast<uintptr_t>(blocks.back().data()) ^ blocks.back()[0];
    }
  }
}

// ---------------------------------------------------------------------------
// CPP-1: Chained Opaque Heap UAF (multi-segment merge & dirty submsg temporary)
// ---------------------------------------------------------------------------
void test_cpp1_chained_opaque_heap_uaf() {
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
  // Under ASan, isolate the UAF in a child process so ASan's abort is recorded as FAIL
  // without terminating the remaining regression test cases.
  const pid_t pid = ::fork();
  if (pid == 0) {
    omnibuf::bench::OpaquePeerUpdate view(pkt);
    std::string_view city = view.get_location().get_city_view();
    scrub_freed_heap_slots(merged_sz, static_cast<uint8_t>('X'));
    const bool ok1 = (city == kExpectedCity);

    view.set_location(omnibuf::bench::Location{.city = std::string(kExpectedDirtyCity)});
    std::string_view dirty_city = view.get_location().get_city_view();
    scrub_freed_heap_slots(merged_sz + 8, static_cast<uint8_t>('Y'));
    const bool ok2 = (dirty_city == kExpectedDirtyCity);
    ::_exit((ok1 && ok2) ? 0 : 1);
  }
  int status = 0;
  (void)::waitpid(pid, &status, 0);
  EXPECT(WIFEXITED(status) && WEXITSTATUS(status) == 0);
#else
  omnibuf::bench::OpaquePeerUpdate view(pkt);
  std::string_view city = view.get_location().get_city_view();
  scrub_freed_heap_slots(merged_sz, static_cast<uint8_t>('X'));
  EXPECT_MSG(city == kExpectedCity, "multi-segment view.get_location().get_city_view() dangled");

  const omnibuf::bench::Location dirty_loc{.city = std::string(kExpectedDirtyCity)};
  const size_t dirty_loc_sz = dirty_loc.marshal_omnibuf().size();
  view.set_location(dirty_loc);
  std::string_view dirty_city = view.get_location().get_city_view();
  scrub_freed_heap_slots(dirty_loc_sz, static_cast<uint8_t>('Y'));
  EXPECT_MSG(dirty_city == kExpectedDirtyCity,
             "dirty submsg view.get_location().get_city_view() dangled");
#endif
}

// ---------------------------------------------------------------------------
// CPP-2: 64 MiB Opaque Stack Frame on High Tag Number (536870911)
// ---------------------------------------------------------------------------
void test_cpp2_opaque_64mib_stack_frame() {
  omnibuf::proto::FieldDescriptorProto f;
  f.name = "x";
  f.number = 536870911u;
  f.label = omnibuf::proto::FieldLabel::Optional;
  f.type = omnibuf::proto::FieldType::Uint32;
  f.has_type = true;

  omnibuf::proto::DescriptorProto msg;
  msg.name = "HighTag";
  msg.fields.push_back(std::move(f));

  omnibuf::proto::FileDescriptorProto file;
  file.name = "high_tag.proto";
  file.package = "audit";
  file.syntax = "proto3";
  file.message_types.push_back(std::move(msg));

  omnibuf::proto::CodeGeneratorRequest req;
  req.files_to_generate = {"high_tag.proto"};
  req.proto_files.push_back(std::move(file));

  const PluginResponse resp = invoke_codegen_plugin(req);
  EXPECT(resp.exited_cleanly);
  EXPECT_MSG(resp.error.empty(), resp.error.c_str());

  const auto it = resp.files.find("high_tag.omnibuf.pb.hpp");
  EXPECT(it != resp.files.end());
  if (it == resp.files.end()) return;

  const std::string& hdr = it->second;
  EXPECT(hdr.find("std::array<uint64_t, 8388608>") == std::string::npos);
  EXPECT(hdr.find("std::array<uint64_t, 8388609>") == std::string::npos);

  // Also reject any inline std::array<uint64_t, N> in OpaqueHighTag exceeding 1024 words (8 KiB).
  const size_t opaque_pos = hdr.find("class OpaqueHighTag {");
  EXPECT(opaque_pos != std::string::npos);
  if (opaque_pos != std::string::npos) {
    const std::string_view needle = "std::array<uint64_t, ";
    const size_t arr_pos = hdr.find(needle, opaque_pos);
    if (arr_pos != std::string::npos) {
      const size_t num_start = arr_pos + needle.size();
      size_t num_end = num_start;
      while (num_end < hdr.size() && std::isdigit(static_cast<unsigned char>(hdr[num_end]))) {
        ++num_end;
      }
      const uint64_t words = std::strtoull(hdr.substr(num_start, num_end - num_start).c_str(),
                                           nullptr, 10);
      EXPECT_MSG(words <= 1024, "OpaqueHighTag emitted huge inline std::array<uint64_t, N>");
    }
  }
}

// ---------------------------------------------------------------------------
// CPP-3: Opaque::marshal_append Prefix Wipe & Self-Aliased marshal_into
// ---------------------------------------------------------------------------
void test_cpp3_opaque_marshal_append_prefix_wipe() {
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
    scrub_freed_heap_slots(old_cap, static_cast<uint8_t>('Z'));
    EXPECT_MSG(alias_view.get_router_id_view() == "rtr-01",
               "self-aliased marshal_into left raw_view_ dangling into old dst buffer");
  }
}

// ---------------------------------------------------------------------------
// CPP-4: Missing MAX_CONTAINER_DEPTH = 32 Enforcement on Encode
// ---------------------------------------------------------------------------
void test_cpp4_encode_missing_max_container_depth() {
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

// ---------------------------------------------------------------------------
// CPP-5: StructTraits Unscoped enum & std::is_signed_v<Enum> Wire Type Bugs
// ---------------------------------------------------------------------------
void test_cpp5_struct_traits_enum_wire_types() {
  // 1. Unscoped enum with non-negative enumerators must still encode as WireSint (1), NOT WireUint (0).
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

  // 2. Scoped signed enum (int32_t) with WireOverride::Fixed32 must encode as WireSfixed32 (6), NOT WireFixed32 (4).
  {
    SignedFixedEnumHolder h{.val = SignedFixedEnum::NEG};
    const auto res = omnibuf::marshal(h);
    EXPECT(res.ok());
    if (res.ok() && !res.value().empty()) {
      const auto [tag, tn] = omnibuf::decode_varint(res.value());
      EXPECT(tn > 0);
      const uint8_t wt = static_cast<uint8_t>(tag & 0x0Fu);
      EXPECT_MSG(wt == omnibuf::WireSfixed32,
                 "SignedFixedEnum with Fixed32 override marshaled as WireFixed32(4) instead of WireSfixed32(6)");
    }
  }
}

// ---------------------------------------------------------------------------
// CPP-6: protoc-gen-omnibuf-cpp Same-Package defined_messages Premature Population
// ---------------------------------------------------------------------------
void test_cpp6_codegen_same_pkg_defined_messages() {
  // a.proto is an imported non-target file in package "pkg".
  omnibuf::proto::FileDescriptorProto file_a;
  file_a.name = "a.proto";
  file_a.package = "pkg";
  file_a.syntax = "proto3";
  {
    omnibuf::proto::DescriptorProto helper;
    helper.name = "Helper";
    omnibuf::proto::FieldDescriptorProto f;
    f.name = "id";
    f.number = 1;
    f.label = omnibuf::proto::FieldLabel::Optional;
    f.type = omnibuf::proto::FieldType::Uint32;
    f.has_type = true;
    helper.fields.push_back(std::move(f));
    file_a.message_types.push_back(std::move(helper));
  }

  // b.proto is in the SAME package "pkg", imports a.proto, and defines recursive message Node.
  omnibuf::proto::FileDescriptorProto file_b;
  file_b.name = "b.proto";
  file_b.package = "pkg";
  file_b.syntax = "proto3";
  file_b.dependencies = {"a.proto"};
  {
    omnibuf::proto::DescriptorProto node;
    node.name = "Node";
    omnibuf::proto::FieldDescriptorProto f1;
    f1.name = "next";
    f1.number = 1;
    f1.label = omnibuf::proto::FieldLabel::Optional;
    f1.type = omnibuf::proto::FieldType::Message;
    f1.has_type = true;
    f1.type_name = ".pkg.Node";
    node.fields.push_back(std::move(f1));
    file_b.message_types.push_back(std::move(node));
  }

  omnibuf::proto::CodeGeneratorRequest req;
  req.files_to_generate = {"b.proto"};
  req.proto_files = {std::move(file_a), std::move(file_b)};

  const PluginResponse resp = invoke_codegen_plugin(req);
  EXPECT(resp.exited_cleanly);
  EXPECT_MSG(resp.error.empty(), resp.error.c_str());

  const auto it = resp.files.find("b.omnibuf.pb.hpp");
  EXPECT(it != resp.files.end());
  if (it == resp.files.end()) return;

  const std::string& hdr = it->second;
  EXPECT_MSG(hdr.find("::omnibuf::Box<::pkg::Node> next") != std::string::npos ||
                 hdr.find("::omnibuf::Box<Node> next") != std::string::npos,
             "b.omnibuf.pb.hpp did not emit ::omnibuf::Box for recursive Node.next");
  EXPECT_MSG(hdr.find("std::optional<::pkg::Node> next") == std::string::npos &&
                 hdr.find("std::optional<Node> next") == std::string::npos,
             "b.omnibuf.pb.hpp emitted uncompilable std::optional<Node> for incomplete recursive Node");
}

// ---------------------------------------------------------------------------
// CPP-9: protoc-gen-omnibuf-cpp Identifier Collisions & Reserved Double Underscore
// ---------------------------------------------------------------------------
void test_cpp9_codegen_identifier_collisions() {
  omnibuf::proto::FileDescriptorProto file_b;
  file_b.name = "b.proto";
  file_b.package = "pkg";
  file_b.syntax = "proto3";
  {
    omnibuf::proto::DescriptorProto node;
    node.name = "Node";
    node.oneof_decls.push_back(omnibuf::proto::OneofDescriptorProto{.name = "class"});

    omnibuf::proto::FieldDescriptorProto f2;
    f2.name = "value_schema_meta";
    f2.number = 2;
    f2.label = omnibuf::proto::FieldLabel::Optional;
    f2.type = omnibuf::proto::FieldType::String;
    f2.has_type = true;
    node.fields.push_back(std::move(f2));

    omnibuf::proto::FieldDescriptorProto f3;
    f3.name = "x";
    f3.number = 3;
    f3.label = omnibuf::proto::FieldLabel::Optional;
    f3.type = omnibuf::proto::FieldType::Uint32;
    f3.has_type = true;
    f3.oneof_index = 0;
    node.fields.push_back(std::move(f3));

    file_b.message_types.push_back(std::move(node));
  }

  omnibuf::proto::CodeGeneratorRequest req;
  req.files_to_generate = {"b.proto"};
  req.proto_files = {std::move(file_b)};

  const PluginResponse resp = invoke_codegen_plugin(req);
  EXPECT(resp.exited_cleanly);
  EXPECT_MSG(resp.error.empty(), resp.error.c_str());

  const auto it = resp.files.find("b.omnibuf.pb.hpp");
  EXPECT(it != resp.files.end());
  if (it == resp.files.end()) return;

  const std::string& hdr = it->second;
  EXPECT_MSG(hdr.find("std::string value_schema_meta{};") == std::string::npos,
             "field 'value_schema_meta' was not escaped and collides with value_schema_meta() method");
  EXPECT_MSG(hdr.find("__") == std::string::npos,
             "generated header contains C++ reserved double-underscore '__' identifier");
}

// ---------------------------------------------------------------------------
// CPP-10: Opaque oneof Mutual Exclusion & unmarshal Dirty Retention
// ---------------------------------------------------------------------------
void test_cpp10_opaque_oneof_mutual_exclusion_and_unmarshal_dirty() {
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
  EXPECT_MSG(op_concat.get_action_code() == 0,
             "OpaqueEdgeShowcase::get_action_code() returned superseded oneof branch 99 instead of 0");
  const auto try_code = op_concat.try_get_action_code();
  EXPECT(try_code.ok() && try_code.value() == 0);
  EXPECT(op_concat.get_action_name_view() == "go");

  // 2. Opaque::unmarshal(clean_pkt) after dirty mutation must release DirtyHeap / reset active oneof state.
  const std::vector<uint8_t> clean_pkt = es2.marshal_omnibuf();
  OpaqueEdgeShowcase op_reuse(clean_pkt);
#if OMNIBUF_TRACK_HEAP
  const std::ptrdiff_t baseline_bytes = g_live_heap_bytes.load(std::memory_order_relaxed);
#endif
  op_reuse.set_action_name(std::string(65536, 'D'));
#if OMNIBUF_TRACK_HEAP
  const std::ptrdiff_t dirty_bytes = g_live_heap_bytes.load(std::memory_order_relaxed);
  EXPECT(dirty_bytes - baseline_bytes >= 65536);
#endif
  op_reuse.unmarshal(concat);
  EXPECT_MSG(op_reuse.get_action_code() == 0,
             "OpaqueEdgeShowcase::unmarshal(concat) failed oneof mutual exclusion");
#if OMNIBUF_TRACK_HEAP
  const std::ptrdiff_t after_unmarshal_bytes = g_live_heap_bytes.load(std::memory_order_relaxed);
  EXPECT_MSG(after_unmarshal_bytes - baseline_bytes < 4096,
             "OpaqueEdgeShowcase::unmarshal retained 64 KiB DirtyHeap allocation");
#endif
}

// ---------------------------------------------------------------------------
// CPP-11: Opaque try_get_* Skipping Corrupt Field #0 Without validate_field0
// ---------------------------------------------------------------------------
void test_cpp11_opaque_try_get_skips_corrupt_field0() {
  omnibuf::SchemaMeta bad_meta;
  bad_meta.type_name = "PeerUpdate";
  bad_meta.fields[1] = std::string("\xC3\x28", 2);  // invalid UTF-8 2-byte sequence

  std::vector<uint8_t> pkt = omnibuf::encode_field0_schema_meta(bad_meta);
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

// ---------------------------------------------------------------------------
// CPP-12: DecodeBlind max_total_elements Depth Multiplication
// ---------------------------------------------------------------------------
void test_cpp12_decode_blind_max_total_elements_depth_mult() {
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

  // decode_blind must also succeed under max_total_elements = 1500 instead of charging 21 * 1000 = 21,000.
  const auto blind_res = omnibuf::decode_blind(pkt, {.max_total_elements = 1500});
  EXPECT_MSG(blind_res.ok(), blind_res.error().c_str());
}

// ---------------------------------------------------------------------------
// CPP-13: FastWriter Exception Zero-Tail in marshal_omnibuf_append
// ---------------------------------------------------------------------------
void test_cpp13_fast_writer_exception_zero_tail() {
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

// ---------------------------------------------------------------------------
// CPP-14: Reused unmarshal_omnibuf must not exponentially grow vector capacity
// ---------------------------------------------------------------------------
void test_cpp14_reused_unmarshal_no_capacity_explosion() {
  omnibuf::bench::PeerUpdate src;
  src.asn = 64512;
  src.router_id = "edge-rtr-fra01.core.backbone.net";
  src.tags = {"tier1", "ebgp", "eu-central", "ixp-de-cix"};
  src.prefixes = {167772160u, 172032000u, 3232235520u};
  src.neighbors = {
      omnibuf::bench::Neighbor{.peer_asn = 3356, .router = "ae0.fra1", .latency_us = 850, .established = true},
      omnibuf::bench::Neighbor{.peer_asn = 1299, .router = "ffm-b5", .latency_us = 920, .established = true},
  };
  const std::vector<uint8_t> wire = src.marshal_omnibuf();

  omnibuf::bench::PeerUpdate reused;
  for (int i = 0; i < 1000; ++i) {
    const auto res = reused.unmarshal_omnibuf(wire);
    EXPECT_MSG(res.ok(), res.error().c_str());
  }
  EXPECT(reused == src);
  EXPECT_MSG(reused.tags.capacity() <= 64, "tags capacity grew unbounded across reused unmarshal");
  EXPECT_MSG(reused.prefixes.capacity() <= 64, "prefixes capacity grew unbounded across reused unmarshal");
  EXPECT_MSG(reused.neighbors.capacity() <= 64, "neighbors capacity grew unbounded across reused unmarshal");
}

}  // namespace

int main() {
  RUN_CASE(test_cpp1_chained_opaque_heap_uaf);
  RUN_CASE(test_cpp2_opaque_64mib_stack_frame);
  RUN_CASE(test_cpp3_opaque_marshal_append_prefix_wipe);
  RUN_CASE(test_cpp4_encode_missing_max_container_depth);
  RUN_CASE(test_cpp5_struct_traits_enum_wire_types);
  RUN_CASE(test_cpp6_codegen_same_pkg_defined_messages);
  RUN_CASE(test_cpp9_codegen_identifier_collisions);
  RUN_CASE(test_cpp10_opaque_oneof_mutual_exclusion_and_unmarshal_dirty);
  RUN_CASE(test_cpp11_opaque_try_get_skips_corrupt_field0);
  RUN_CASE(test_cpp12_decode_blind_max_total_elements_depth_mult);
  RUN_CASE(test_cpp13_fast_writer_exception_zero_tail);
  RUN_CASE(test_cpp14_reused_unmarshal_no_capacity_explosion);

  std::printf("\nAudit Regression Summary: %d passed, %d failed\n", g_passed, g_failed);
  return g_failed == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
