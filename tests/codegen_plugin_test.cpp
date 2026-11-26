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
#include <optional>
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

std::vector<uint8_t> encode_pb_features(const omnibuf::proto::EditionFeatures& f) {
  std::vector<uint8_t> b;
  if (f.presence != 0) pb_write_varint_field(b, 1, f.presence);
  if (f.enum_type != 0) pb_write_varint_field(b, 2, f.enum_type);
  if (f.repeated_field_encoding != 0) pb_write_varint_field(b, 3, f.repeated_field_encoding);
  if (f.utf8 != 0) pb_write_varint_field(b, 4, f.utf8);
  if (f.message_encoding != 0) pb_write_varint_field(b, 5, f.message_encoding);
  if (f.json_format != 0) pb_write_varint_field(b, 6, f.json_format);
  return b;
}

std::vector<uint8_t> encode_pb_field_desc(const omnibuf::proto::FieldDescriptorProto& f) {
  std::vector<uint8_t> b;
  omnibuf::proto::detail::write_pb_string_field(b, 1, f.name);
  if (!f.extendee.empty()) {
    omnibuf::proto::detail::write_pb_string_field(b, 2, f.extendee);
  }
  pb_write_varint_field(b, 3, f.number);
  pb_write_varint_field(b, 4, static_cast<uint64_t>(f.label));
  pb_write_varint_field(b, 5, static_cast<uint64_t>(f.type));
  if (!f.type_name.empty()) {
    omnibuf::proto::detail::write_pb_string_field(b, 6, f.type_name);
  }
  if (!f.default_value.empty()) {
    omnibuf::proto::detail::write_pb_string_field(b, 7, f.default_value);
  }
  const auto feat = encode_pb_features(f.features);
  if (!feat.empty()) {
    std::vector<uint8_t> opts;
    pb_write_bytes_field(opts, 21, feat);
    pb_write_bytes_field(b, 8, opts);
  }
  if (f.oneof_index.has_value()) {
    pb_write_varint_field(b, 9, *f.oneof_index);
  }
  if (f.proto3_optional) {
    pb_write_varint_field(b, 17, 1);
  }
  return b;
}

std::vector<uint8_t> encode_pb_enum_desc(const omnibuf::proto::EnumDescriptorProto& e) {
  std::vector<uint8_t> b;
  omnibuf::proto::detail::write_pb_string_field(b, 1, e.name);
  for (const auto& ev : e.values) {
    std::vector<uint8_t> eb;
    omnibuf::proto::detail::write_pb_string_field(eb, 1, ev.name);
    pb_write_varint_field(eb, 2, static_cast<uint64_t>(static_cast<uint32_t>(ev.number)));
    pb_write_bytes_field(b, 2, eb);
  }
  const auto feat = encode_pb_features(e.features);
  if (!feat.empty()) {
    std::vector<uint8_t> opts;
    pb_write_bytes_field(opts, 7, feat);
    pb_write_bytes_field(b, 3, opts);
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
  for (const auto& e : m.enum_types) {
    const auto eb = encode_pb_enum_desc(e);
    pb_write_bytes_field(b, 4, eb);
  }
  if (m.has_extensions) {
    std::vector<uint8_t> er;
    pb_write_varint_field(er, 1, 100);
    pb_write_varint_field(er, 2, 200);
    pb_write_bytes_field(b, 5, er);
  }
  {
    std::vector<uint8_t> opts;
    if (m.map_entry) {
      pb_write_varint_field(opts, 7, 1);
    }
    const auto feat = encode_pb_features(m.features);
    if (!feat.empty()) {
      pb_write_bytes_field(opts, 12, feat);
    }
    if (!opts.empty()) {
      pb_write_bytes_field(b, 7, opts);
    }
  }
  for (const auto& od : m.oneof_decls) {
    std::vector<uint8_t> ob;
    omnibuf::proto::detail::write_pb_string_field(ob, 1, od.name);
    const auto ofeat = encode_pb_features(od.features);
    if (!ofeat.empty()) {
      std::vector<uint8_t> oopts;
      pb_write_bytes_field(oopts, 1, ofeat);
      pb_write_bytes_field(ob, 2, oopts);
    }
    pb_write_bytes_field(b, 8, ob);
  }
  return b;
}

std::vector<uint8_t> encode_pb_file_desc(
    const omnibuf::proto::FileDescriptorProto& file,
    const std::vector<omnibuf::proto::FieldDescriptorProto>& file_extensions = {}) {
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
  for (const auto& e : file.enum_types) {
    const auto eb = encode_pb_enum_desc(e);
    pb_write_bytes_field(b, 5, eb);
  }
  for (const auto& ext : file_extensions) {
    const auto eb = encode_pb_field_desc(ext);
    pb_write_bytes_field(b, 7, eb);
  }
  if (file.has_extensions && file_extensions.empty()) {
    omnibuf::proto::FieldDescriptorProto dummy_ext;
    dummy_ext.name = "ext_x";
    dummy_ext.extendee = ".pkg.Base";
    dummy_ext.number = 100;
    dummy_ext.label = omnibuf::proto::FieldLabel::Optional;
    dummy_ext.type = omnibuf::proto::FieldType::Int32;
    dummy_ext.has_type = true;
    const auto eb = encode_pb_field_desc(dummy_ext);
    pb_write_bytes_field(b, 7, eb);
  }
  const auto feat = encode_pb_features(file.features);
  if (!feat.empty()) {
    std::vector<uint8_t> opts;
    pb_write_bytes_field(opts, 50, feat);
    pb_write_bytes_field(b, 8, opts);
  }
  if (!file.syntax.empty()) {
    omnibuf::proto::detail::write_pb_string_field(b, 12, file.syntax);
  }
  if (file.edition != 0) {
    pb_write_varint_field(b, 14, file.edition);
  }
  return b;
}

std::vector<uint8_t> encode_pb_codegen_request(
    const omnibuf::proto::CodeGeneratorRequest& req,
    const std::map<std::string, std::vector<omnibuf::proto::FieldDescriptorProto>>&
        file_extensions = {}) {
  std::vector<uint8_t> b;
  for (const auto& target : req.files_to_generate) {
    omnibuf::proto::detail::write_pb_string_field(b, 1, target);
  }
  for (const auto& file : req.proto_files) {
    auto it = file_extensions.find(file.name);
    const auto fb = (it != file_extensions.end()) ? encode_pb_file_desc(file, it->second)
                                                  : encode_pb_file_desc(file);
    pb_write_bytes_field(b, 15, fb);
  }
  return b;
}

struct PluginResponse {
  bool exited_cleanly = false;
  std::string error{};
  uint64_t supported_features = 0;
  int32_t minimum_edition = 0;
  int32_t maximum_edition = 0;
  std::map<std::string, std::string> files{};
};

PluginResponse invoke_codegen_plugin_raw(std::span<const uint8_t> req_bytes) {
  PluginResponse out;
#ifdef _WIN32
  (void)req_bytes;
  out.error = "POSIX pipe execution required";
  return out;
#else
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
    } else if (wt == 0 && (fn == 2 || fn == 3 || fn == 4)) {
      auto v = omnibuf::proto::detail::read_pb_varint(sp, pos);
      if (!v) break;
      pos = v.value().next_pos;
      if (fn == 2) {
        out.supported_features = v.value().val;
      } else if (fn == 3) {
        out.minimum_edition = static_cast<int32_t>(v.value().val);
      } else {
        out.maximum_edition = static_cast<int32_t>(v.value().val);
      }
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

PluginResponse invoke_codegen_plugin(
    const omnibuf::proto::CodeGeneratorRequest& req,
    const std::map<std::string, std::vector<omnibuf::proto::FieldDescriptorProto>>&
        file_extensions = {}) {
  const auto req_bytes = encode_pb_codegen_request(req, file_extensions);
  return invoke_codegen_plugin_raw(req_bytes);
}

omnibuf::proto::FieldDescriptorProto make_field(std::string name,
                                                uint32_t number,
                                                omnibuf::proto::FieldType type,
                                                omnibuf::proto::FieldLabel label =
                                                    omnibuf::proto::FieldLabel::Optional,
                                                std::string type_name = {}) {
  omnibuf::proto::FieldDescriptorProto f;
  f.name = std::move(name);
  f.number = number;
  f.label = label;
  f.type = type;
  f.has_type = true;
  f.type_name = std::move(type_name);
  return f;
}

omnibuf::proto::EnumDescriptorProto make_enum(std::string name,
                                              std::vector<std::pair<std::string, int32_t>> vals,
                                              omnibuf::proto::EditionFeatures features = {}) {
  omnibuf::proto::EnumDescriptorProto e;
  e.name = std::move(name);
  e.features = features;
  for (auto& [vname, vnum] : vals) {
    e.values.push_back(omnibuf::proto::EnumValueDescriptorProto{std::move(vname), vnum});
  }
  return e;
}

void test_codegen_high_field_number_uses_compact_dirty_bitset() {
  omnibuf::proto::FieldDescriptorProto f =
      make_field("x", 536870911u, omnibuf::proto::FieldType::Uint32);

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
  EXPECT(resp.supported_features == 3u);
  EXPECT(resp.minimum_edition == 1000);
  EXPECT(resp.maximum_edition == 1002);

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
  omnibuf::proto::FileDescriptorProto file_a;
  file_a.name = "a.proto";
  file_a.package = "pkg";
  file_a.syntax = "proto3";
  {
    omnibuf::proto::DescriptorProto helper;
    helper.name = "Helper";
    helper.fields.push_back(make_field("id", 1, omnibuf::proto::FieldType::Uint32));
    file_a.message_types.push_back(std::move(helper));
  }

  omnibuf::proto::FileDescriptorProto file_b;
  file_b.name = "b.proto";
  file_b.package = "pkg";
  file_b.syntax = "proto3";
  file_b.dependencies = {"a.proto"};
  {
    omnibuf::proto::DescriptorProto node;
    node.name = "Node";
    node.fields.push_back(make_field("next", 1, omnibuf::proto::FieldType::Message,
                                     omnibuf::proto::FieldLabel::Optional, ".pkg.Node"));
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
    node.fields.push_back(make_field("value_schema_meta", 2, omnibuf::proto::FieldType::String));

    auto f3 = make_field("x", 3, omnibuf::proto::FieldType::Uint32);
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

void test_codegen_editions_feature_inheritance_and_group_normalization() {
  for (uint32_t ed : {1000u, 1001u, 1002u}) {
    omnibuf::proto::FileDescriptorProto file;
    file.name = "ed.proto";
    file.package = "edtest";
    file.syntax = "editions";
    file.edition = ed;
    // File sets IMPLICIT presence, EXPANDED repeated, LEGACY_BEST_EFFORT json, DELIMITED message.
    file.features = {.presence = 2,
                     .enum_type = 1,
                     .repeated_field_encoding = 2,
                     .utf8 = 2,
                     .message_encoding = 2,
                     .json_format = 2};

    omnibuf::proto::DescriptorProto child;
    child.name = "Child";
    child.fields.push_back(make_field("id", 1, omnibuf::proto::FieldType::Int32));
    file.message_types.push_back(std::move(child));

    omnibuf::proto::DescriptorProto outer;
    outer.name = "Outer";
    // Message overrides presence to EXPLICIT (1).
    outer.features.presence = 1;

    // Inherits EXPLICIT from Outer -> std::optional<int32_t>.
    outer.fields.push_back(make_field("from_msg_explicit", 1, omnibuf::proto::FieldType::Int32));

    // Field overrides back to IMPLICIT (2) -> plain int32_t.
    auto f_imp = make_field("field_implicit", 2, omnibuf::proto::FieldType::Int32);
    f_imp.features.presence = 2;
    outer.fields.push_back(std::move(f_imp));

    // Legacy Group field normalized to Message (WireSubMsg / std::optional<Child>).
    auto f_grp = make_field("delim_group", 3, omnibuf::proto::FieldType::Group,
                            omnibuf::proto::FieldLabel::Optional, ".edtest.Child");
    outer.fields.push_back(std::move(f_grp));

    // Oneof inherits UTF-8 VERIFY (override from file NONE would also work, checked below).
    outer.oneof_decls.push_back(omnibuf::proto::OneofDescriptorProto{
        .name = "pick", .features = {.utf8 = 2}});
    auto f_oneof_str = make_field("pick_str", 4, omnibuf::proto::FieldType::String);
    f_oneof_str.oneof_index = 0;
    outer.fields.push_back(std::move(f_oneof_str));

    // Nested message inherits EXPLICIT from Outer.
    omnibuf::proto::DescriptorProto nested;
    nested.name = "Nested";
    nested.fields.push_back(make_field("nested_explicit", 1, omnibuf::proto::FieldType::Bool));
    outer.nested_types.push_back(std::move(nested));

    file.message_types.push_back(std::move(outer));

    omnibuf::proto::CodeGeneratorRequest req;
    req.files_to_generate = {"ed.proto"};
    req.proto_files.push_back(std::move(file));

    const PluginResponse resp = invoke_codegen_plugin(req);
    EXPECT(resp.exited_cleanly);
    EXPECT_MSG(resp.error.empty(), resp.error.c_str());
    EXPECT(resp.supported_features == 3u);
    EXPECT(resp.minimum_edition == 1000);
    EXPECT(resp.maximum_edition == 1002);

    const auto it = resp.files.find("ed.omnibuf.pb.hpp");
    EXPECT(it != resp.files.end());
    if (it == resp.files.end()) continue;
    const std::string& hdr = it->second;
    EXPECT(hdr.find("std::optional<int32_t> from_msg_explicit{};") != std::string::npos);
    EXPECT(hdr.find("int32_t field_implicit = 0;") != std::string::npos);
    EXPECT(hdr.find("std::optional<::edtest::Child> delim_group{};") != std::string::npos);
    EXPECT(hdr.find("std::optional<bool> nested_explicit{};") != std::string::npos);
  }
}

void test_codegen_rejects_unsupported_editions() {
  for (uint32_t bad_ed : {0u, 999u, 1003u, 9999u}) {
    omnibuf::proto::FileDescriptorProto file;
    file.name = "bad_ed.proto";
    file.package = "bad";
    file.syntax = "editions";
    file.edition = bad_ed;
    omnibuf::proto::DescriptorProto msg;
    msg.name = "M";
    msg.fields.push_back(make_field("x", 1, omnibuf::proto::FieldType::Int32));
    file.message_types.push_back(std::move(msg));

    omnibuf::proto::CodeGeneratorRequest req;
    req.files_to_generate = {"bad_ed.proto"};
    req.proto_files.push_back(std::move(file));

    const PluginResponse resp = invoke_codegen_plugin(req);
    EXPECT(resp.exited_cleanly);
    EXPECT(resp.supported_features == 3u);
    EXPECT(resp.minimum_edition == 1000);
    EXPECT(resp.maximum_edition == 1002);
    EXPECT_MSG(resp.error.find("unsupported protobuf edition") != std::string::npos,
               resp.error.c_str());
  }
}

void test_codegen_rejects_editions_closed_enums_declared_and_cross_file() {
  // 1. Top-level CLOSED enum in Edition 2023.
  {
    omnibuf::proto::FileDescriptorProto file;
    file.name = "closed_top.proto";
    file.package = "bad";
    file.syntax = "editions";
    file.edition = 1000;
    file.enum_types.push_back(
        make_enum("ClosedE", {{"CLOSED_E_ZERO", 0}}, {.enum_type = 2}));
    omnibuf::proto::CodeGeneratorRequest req;
    req.files_to_generate = {"closed_top.proto"};
    req.proto_files.push_back(std::move(file));
    const PluginResponse resp = invoke_codegen_plugin(req);
    EXPECT(resp.exited_cleanly);
    EXPECT(resp.error.find("CLOSED") != std::string::npos);
  }

  // 2. Nested CLOSED enum inherited from file features.enum_type = CLOSED.
  {
    omnibuf::proto::FileDescriptorProto file;
    file.name = "closed_nested.proto";
    file.package = "bad";
    file.syntax = "editions";
    file.edition = 1002;
    file.features.enum_type = 2;
    omnibuf::proto::DescriptorProto msg;
    msg.name = "Holder";
    msg.enum_types.push_back(make_enum("NestedClosed", {{"ZERO", 0}}));
    file.message_types.push_back(std::move(msg));

    omnibuf::proto::CodeGeneratorRequest req;
    req.files_to_generate = {"closed_nested.proto"};
    req.proto_files.push_back(std::move(file));
    const PluginResponse resp = invoke_codegen_plugin(req);
    EXPECT(resp.exited_cleanly);
    EXPECT(resp.error.find("CLOSED") != std::string::npos);
  }

  // 3. Cross-file imported proto2 closed enum referenced by singular field and map value.
  for (bool via_map : {false, true}) {
    omnibuf::proto::FileDescriptorProto p2_dep;
    p2_dep.name = "legacy_p2.proto";
    p2_dep.package = "legacy";
    p2_dep.syntax = "proto2";
    p2_dep.enum_types.push_back(make_enum("LegacyClosed", {{"ZERO", 0}, {"ONE", 1}}));

    omnibuf::proto::FileDescriptorProto ed_file;
    ed_file.name = "use_p2.proto";
    ed_file.package = "ed";
    ed_file.syntax = "editions";
    ed_file.edition = 1001;
    ed_file.dependencies = {"legacy_p2.proto"};

    omnibuf::proto::DescriptorProto msg;
    msg.name = "UsesClosed";
    if (!via_map) {
      msg.fields.push_back(make_field("e", 1, omnibuf::proto::FieldType::Enum,
                                      omnibuf::proto::FieldLabel::Optional,
                                      ".legacy.LegacyClosed"));
    } else {
      msg.fields.push_back(make_field("m", 1, omnibuf::proto::FieldType::Message,
                                      omnibuf::proto::FieldLabel::Repeated,
                                      ".ed.UsesClosed.MEntry"));
      omnibuf::proto::DescriptorProto entry;
      entry.name = "MEntry";
      entry.map_entry = true;
      entry.fields.push_back(make_field("key", 1, omnibuf::proto::FieldType::String));
      entry.fields.push_back(make_field("value", 2, omnibuf::proto::FieldType::Enum,
                                        omnibuf::proto::FieldLabel::Optional,
                                        ".legacy.LegacyClosed"));
      msg.nested_types.push_back(std::move(entry));
    }
    ed_file.message_types.push_back(std::move(msg));

    omnibuf::proto::CodeGeneratorRequest req;
    req.files_to_generate = {"use_p2.proto"};
    req.proto_files = {std::move(p2_dep), std::move(ed_file)};
    const PluginResponse resp = invoke_codegen_plugin(req);
    EXPECT(resp.exited_cleanly);
    EXPECT_MSG(resp.error.find("CLOSED") != std::string::npos, resp.error.c_str());
  }

  // 4. Cross-file imported proto3 open enum IS allowed in Editions.
  {
    omnibuf::proto::FileDescriptorProto p3_dep;
    p3_dep.name = "open_p3.proto";
    p3_dep.package = "p3";
    p3_dep.syntax = "proto3";
    p3_dep.enum_types.push_back(make_enum("OpenE", {{"OPEN_E_ZERO", 0}, {"OPEN_E_ONE", 1}}));

    omnibuf::proto::FileDescriptorProto ed_file;
    ed_file.name = "use_p3.proto";
    ed_file.package = "ed";
    ed_file.syntax = "editions";
    ed_file.edition = 1002;
    ed_file.dependencies = {"open_p3.proto"};

    omnibuf::proto::DescriptorProto msg;
    msg.name = "UsesOpen";
    msg.fields.push_back(make_field("e", 1, omnibuf::proto::FieldType::Enum,
                                    omnibuf::proto::FieldLabel::Optional, ".p3.OpenE"));
    ed_file.message_types.push_back(std::move(msg));

    omnibuf::proto::CodeGeneratorRequest req;
    req.files_to_generate = {"use_p3.proto"};
    req.proto_files = {std::move(p3_dep), std::move(ed_file)};
    const PluginResponse resp = invoke_codegen_plugin(req);
    EXPECT(resp.exited_cleanly);
    EXPECT_MSG(resp.error.empty(), resp.error.c_str());
  }
}

void test_codegen_rejects_editions_legacy_required_and_utf8_none() {
  // 1. LEGACY_REQUIRED on field.
  {
    omnibuf::proto::FileDescriptorProto file;
    file.name = "req.proto";
    file.package = "bad";
    file.syntax = "editions";
    file.edition = 1000;
    omnibuf::proto::DescriptorProto msg;
    msg.name = "ReqMsg";
    auto f = make_field("x", 1, omnibuf::proto::FieldType::Int32);
    f.features.presence = 3;
    msg.fields.push_back(std::move(f));
    file.message_types.push_back(std::move(msg));

    omnibuf::proto::CodeGeneratorRequest req;
    req.files_to_generate = {"req.proto"};
    req.proto_files.push_back(std::move(file));
    const PluginResponse resp = invoke_codegen_plugin(req);
    EXPECT(resp.exited_cleanly);
    EXPECT(resp.error.find("LEGACY_REQUIRED") != std::string::npos);
  }

  // 2. utf8_validation = NONE on singular string, repeated string, oneof string, map key, map val.
  for (int mode = 0; mode < 5; ++mode) {
    omnibuf::proto::FileDescriptorProto file;
    file.name = "utf8.proto";
    file.package = "bad";
    file.syntax = "editions";
    file.edition = 1002;
    omnibuf::proto::DescriptorProto msg;
    msg.name = "Utf8Msg";

    if (mode == 0) {
      auto f = make_field("s", 1, omnibuf::proto::FieldType::String);
      f.features.utf8 = 3;
      msg.fields.push_back(std::move(f));
    } else if (mode == 1) {
      auto f = make_field("rs", 1, omnibuf::proto::FieldType::String,
                          omnibuf::proto::FieldLabel::Repeated);
      f.features.utf8 = 3;
      msg.fields.push_back(std::move(f));
    } else if (mode == 2) {
      msg.oneof_decls.push_back(
          omnibuf::proto::OneofDescriptorProto{.name = "o", .features = {.utf8 = 3}});
      auto f = make_field("os", 1, omnibuf::proto::FieldType::String);
      f.oneof_index = 0;
      msg.fields.push_back(std::move(f));
    } else if (mode == 3) {
      // utf8_validation = NONE placed directly on map field (not duplicated on entry).
      auto f = make_field("kv", 1, omnibuf::proto::FieldType::Message,
                          omnibuf::proto::FieldLabel::Repeated, ".bad.Utf8Msg.KvEntry");
      f.features.utf8 = 3;
      msg.fields.push_back(std::move(f));
      omnibuf::proto::DescriptorProto entry;
      entry.name = "KvEntry";
      entry.map_entry = true;
      entry.fields.push_back(make_field("key", 1, omnibuf::proto::FieldType::String));
      entry.fields.push_back(make_field("value", 2, omnibuf::proto::FieldType::Int32));
      msg.nested_types.push_back(std::move(entry));
    } else {
      // utf8_validation = NONE inherited from file into map<int32, string>.
      file.features.utf8 = 3;
      msg.fields.push_back(make_field("kv", 1, omnibuf::proto::FieldType::Message,
                                      omnibuf::proto::FieldLabel::Repeated,
                                      ".bad.Utf8Msg.KvEntry"));
      omnibuf::proto::DescriptorProto entry;
      entry.name = "KvEntry";
      entry.map_entry = true;
      entry.fields.push_back(make_field("key", 1, omnibuf::proto::FieldType::Int32));
      entry.fields.push_back(make_field("value", 2, omnibuf::proto::FieldType::String));
      msg.nested_types.push_back(std::move(entry));
    }

    file.message_types.push_back(std::move(msg));
    omnibuf::proto::CodeGeneratorRequest req;
    req.files_to_generate = {"utf8.proto"};
    req.proto_files.push_back(std::move(file));
    const PluginResponse resp = invoke_codegen_plugin(req);
    EXPECT(resp.exited_cleanly);
    EXPECT_MSG(resp.error.find("UTF-8 NONE") != std::string::npos, resp.error.c_str());
  }
}

void test_codegen_editions_defaults_and_extensions() {
  // 1. Non-zero custom defaults rejected across types (including cross-file enum).
  struct DefCase {
    omnibuf::proto::FieldType type;
    std::string type_name;
    std::string def_val;
    bool expect_ok;
  };
  const std::vector<DefCase> cases = {
      {omnibuf::proto::FieldType::Int32, "", "0", true},
      {omnibuf::proto::FieldType::Int32, "", "7", false},
      {omnibuf::proto::FieldType::Bool, "", "false", true},
      {omnibuf::proto::FieldType::Bool, "", "true", false},
      {omnibuf::proto::FieldType::Double, "", "0.0", true},
      {omnibuf::proto::FieldType::Double, "", "-0.0", false},
      {omnibuf::proto::FieldType::Double, "", "1.25", false},
      {omnibuf::proto::FieldType::String, "", "abc", false},
      {omnibuf::proto::FieldType::Bytes, "", "ab", false},
      {omnibuf::proto::FieldType::Enum, ".imp.ImpStatus", "IMP_ZERO", true},
      {omnibuf::proto::FieldType::Enum, ".imp.ImpStatus", "IMP_ONE", false},
  };

  for (const auto& c : cases) {
    omnibuf::proto::FileDescriptorProto dep;
    dep.name = "imp.proto";
    dep.package = "imp";
    dep.syntax = "editions";
    dep.edition = 1002;
    dep.enum_types.push_back(make_enum("ImpStatus", {{"IMP_ZERO", 0}, {"IMP_ONE", 1}}));

    omnibuf::proto::FileDescriptorProto file;
    file.name = "defs.proto";
    file.package = "ed";
    file.syntax = "editions";
    file.edition = 1002;
    file.dependencies = {"imp.proto"};
    omnibuf::proto::DescriptorProto msg;
    msg.name = "DefMsg";
    auto f = make_field("v", 1, c.type, omnibuf::proto::FieldLabel::Optional, c.type_name);
    f.default_value = c.def_val;
    msg.fields.push_back(std::move(f));
    file.message_types.push_back(std::move(msg));

    omnibuf::proto::CodeGeneratorRequest req;
    req.files_to_generate = {"defs.proto"};
    req.proto_files = {std::move(dep), std::move(file)};
    const PluginResponse resp = invoke_codegen_plugin(req);
    EXPECT(resp.exited_cleanly);
    if (c.expect_ok) {
      EXPECT_MSG(resp.error.empty(), resp.error.c_str());
    } else {
      EXPECT_MSG(resp.error.find("custom default values") != std::string::npos, resp.error.c_str());
    }
  }

  // 2. Option extension in Editions is allowed; non-option extension is rejected.
  {
    omnibuf::proto::FileDescriptorProto file;
    file.name = "opt_ok.proto";
    file.package = "ed";
    file.syntax = "editions";
    file.edition = 1001;
    omnibuf::proto::DescriptorProto msg;
    msg.name = "M";
    msg.fields.push_back(make_field("a", 1, omnibuf::proto::FieldType::Int32));
    file.message_types.push_back(std::move(msg));

    auto opt_ext = make_field("my_opt", 50001, omnibuf::proto::FieldType::String);
    opt_ext.extendee = ".google.protobuf.MessageOptions";

    omnibuf::proto::CodeGeneratorRequest req;
    req.files_to_generate = {"opt_ok.proto"};
    req.proto_files.push_back(std::move(file));
    const PluginResponse resp = invoke_codegen_plugin(req, {{"opt_ok.proto", {opt_ext}}});
    EXPECT(resp.exited_cleanly);
    EXPECT_MSG(resp.error.empty(), resp.error.c_str());
  }

  {
    omnibuf::proto::FileDescriptorProto file;
    file.name = "non_opt.proto";
    file.package = "ed";
    file.syntax = "editions";
    file.edition = 1001;
    omnibuf::proto::DescriptorProto msg;
    msg.name = "M";
    msg.has_extensions = true;
    msg.fields.push_back(make_field("a", 1, omnibuf::proto::FieldType::Int32));
    file.message_types.push_back(std::move(msg));

    omnibuf::proto::CodeGeneratorRequest req;
    req.files_to_generate = {"non_opt.proto"};
    req.proto_files.push_back(std::move(file));
    const PluginResponse resp = invoke_codegen_plugin(req);
    EXPECT(resp.exited_cleanly);
    EXPECT_MSG(resp.error.find("extensions") != std::string::npos, resp.error.c_str());
  }
}

}  // namespace

int main() {
  RUN_CASE(test_codegen_high_field_number_uses_compact_dirty_bitset);
  RUN_CASE(test_codegen_same_package_import_boxes_recursive_message);
  RUN_CASE(test_codegen_escapes_colliding_and_reserved_identifiers);
  RUN_CASE(test_codegen_editions_feature_inheritance_and_group_normalization);
  RUN_CASE(test_codegen_rejects_unsupported_editions);
  RUN_CASE(test_codegen_rejects_editions_closed_enums_declared_and_cross_file);
  RUN_CASE(test_codegen_rejects_editions_legacy_required_and_utf8_none);
  RUN_CASE(test_codegen_editions_defaults_and_extensions);

  std::printf("\nCodegen Plugin Test Summary: %d passed, %d failed\n", omnibuf_test::g_passed,
              omnibuf_test::g_failed);
  return omnibuf_test::g_failed == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
