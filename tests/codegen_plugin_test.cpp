#ifndef _WIN32
#include <sys/wait.h>
#include <unistd.h>
#endif

#include <cctype>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "omnibuf/omnibuf.hpp"
#include "test_harness.hpp"

namespace {

// Minimal binary Protobuf CodeGeneratorRequest / CodeGeneratorResponse helpers
// for invoking protoc-gen-omnibuf-cpp via POSIX pipes.
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

void test_codegen_high_field_number_uses_compact_dirty_bitset() {
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
  file.package = "test";
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

void test_codegen_same_package_import_boxes_recursive_message() {
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
  EXPECT_MSG(
      hdr.find("std::optional<::pkg::Node> next") == std::string::npos &&
          hdr.find("std::optional<Node> next") == std::string::npos,
      "b.omnibuf.pb.hpp emitted uncompilable std::optional<Node> for incomplete recursive Node");
}

void test_codegen_escapes_colliding_and_reserved_identifiers() {
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
  EXPECT_MSG(
      hdr.find("std::string value_schema_meta{};") == std::string::npos,
      "field 'value_schema_meta' was not escaped and collides with value_schema_meta() method");
  EXPECT_MSG(hdr.find("__") == std::string::npos,
             "generated header contains C++ reserved double-underscore '__' identifier");
}

}  // namespace

int main() {
  RUN_CASE(test_codegen_high_field_number_uses_compact_dirty_bitset);
  RUN_CASE(test_codegen_same_package_import_boxes_recursive_message);
  RUN_CASE(test_codegen_escapes_colliding_and_reserved_identifiers);

  std::printf("\nCodegen Plugin Test Summary: %d passed, %d failed\n", omnibuf_test::g_passed,
              omnibuf_test::g_failed);
  return omnibuf_test::g_failed == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
