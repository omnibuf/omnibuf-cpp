#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

#include "omnibuf/decoder.hpp"
#include "omnibuf/encoder.hpp"
#include "omnibuf/field0.hpp"
#include "omnibuf/struct_codec.hpp"
#include "omnibuf/utf8.hpp"
#include "omnibuf/varint.hpp"

namespace omnibuf::proto {

// Standard google.protobuf.FieldDescriptorProto.Type numbers (1..18).
enum class FieldType : uint32_t {
  Double = 1,
  Float = 2,
  Int64 = 3,
  Uint64 = 4,
  Int32 = 5,
  Fixed64 = 6,
  Fixed32 = 7,
  Bool = 8,
  String = 9,
  Group = 10,
  Message = 11,
  Bytes = 12,
  Uint32 = 13,
  Enum = 14,
  Sfixed32 = 15,
  Sfixed64 = 16,
  Sint32 = 17,
  Sint64 = 18,
};

enum class FieldLabel : uint32_t {
  Optional = 1,
  Required = 2,
  Repeated = 3,
};

struct FieldDescriptorProto {
  std::string name;
  uint32_t number = 0;
  FieldLabel label = FieldLabel::Optional;
  FieldType type = FieldType::Uint32;
  std::string type_name;
  std::string json_name;
  std::optional<uint32_t> oneof_index;
  bool proto3_optional = false;
};

struct EnumValueDescriptorProto {
  std::string name;
  int32_t number = 0;
};

struct EnumDescriptorProto {
  std::string name;
  std::vector<EnumValueDescriptorProto> values;
};

struct OneofDescriptorProto {
  std::string name;
};

struct DescriptorProto {
  std::string name;
  std::vector<FieldDescriptorProto> fields;
  std::vector<DescriptorProto> nested_types;
  std::vector<EnumDescriptorProto> enum_types;
  std::vector<OneofDescriptorProto> oneof_decls;
  bool map_entry = false;
};

struct FileDescriptorProto {
  std::string name;
  std::string package;
  std::vector<std::string> dependencies;
  std::vector<DescriptorProto> message_types;
  std::vector<EnumDescriptorProto> enum_types;
  std::string syntax;
};

struct FileDescriptorSet {
  std::vector<FileDescriptorProto> files;
};

struct CodeGeneratorRequest {
  std::vector<std::string> files_to_generate;
  std::string parameter;
  std::vector<FileDescriptorProto> proto_files;
};

struct GeneratedFile {
  std::string name;
  std::string content;
};

namespace detail {

struct PbVarint {
  uint64_t val = 0;
  size_t next_pos = 0;
};

[[nodiscard]] inline Result<PbVarint> read_pb_varint(std::span<const uint8_t> b, size_t pos) {
  uint64_t result = 0;
  uint32_t shift = 0;
  while (pos < b.size()) {
    if (shift >= 64) return Error{"omnibuf::proto: protobuf varint overflow"};
    const uint8_t byte = b[pos++];
    if (shift == 63 && byte > 1) return Error{"omnibuf::proto: protobuf varint overflow"};
    result |= static_cast<uint64_t>(byte & 0x7Fu) << shift;
    if ((byte & 0x80u) == 0) return PbVarint{result, pos};
    shift += 7;
  }
  return Error{"omnibuf::proto: unexpected EOF in protobuf varint"};
}

[[nodiscard]] inline Result<SliceResult> read_pb_bytes(std::span<const uint8_t> b, size_t pos) {
  auto len_res = read_pb_varint(b, pos);
  if (!len_res) return Error{len_res.error()};
  const uint64_t len = len_res.value().val;
  const size_t p2 = len_res.value().next_pos;
  if (len > static_cast<uint64_t>(b.size() - p2)) {
    return Error{"omnibuf::proto: truncated protobuf length-delimited field"};
  }
  const size_t n = static_cast<size_t>(len);
  return SliceResult{b.subspan(p2, n), p2 + n};
}

[[nodiscard]] inline Result<size_t> skip_pb_field(std::span<const uint8_t> b,
                                                  size_t pos,
                                                  uint32_t wt) {
  switch (wt) {
    case 0: {
      auto r = read_pb_varint(b, pos);
      if (!r) return Error{r.error()};
      return r.value().next_pos;
    }
    case 1:
      if (b.size() - pos < 8) return Error{"omnibuf::proto: truncated 64-bit protobuf field"};
      return pos + 8;
    case 2: {
      auto r = read_pb_bytes(b, pos);
      if (!r) return Error{r.error()};
      return r.value().next_pos;
    }
    case 5:
      if (b.size() - pos < 4) return Error{"omnibuf::proto: truncated 32-bit protobuf field"};
      return pos + 4;
    default:
      return Error{"omnibuf::proto: unsupported protobuf wire type"};
  }
}

inline void write_pb_varint(std::vector<uint8_t>& out, uint64_t v) {
  while (v >= 0x80u) {
    out.push_back(static_cast<uint8_t>((v & 0x7Fu) | 0x80u));
    v >>= 7;
  }
  out.push_back(static_cast<uint8_t>(v));
}

inline void write_pb_string_field(std::vector<uint8_t>& out,
                                  uint32_t field_num,
                                  std::string_view s) {
  write_pb_varint(out, (static_cast<uint64_t>(field_num) << 3) | 2u);
  write_pb_varint(out, s.size());
  out.insert(out.end(), s.begin(), s.end());
}

[[nodiscard]] inline Result<FieldDescriptorProto> parse_field_descriptor(
    std::span<const uint8_t> b) {
  FieldDescriptorProto out;
  size_t pos = 0;
  while (pos < b.size()) {
    auto tag_res = read_pb_varint(b, pos);
    if (!tag_res) return Error{tag_res.error()};
    pos = tag_res.value().next_pos;
    const uint32_t fnum = static_cast<uint32_t>(tag_res.value().val >> 3);
    const uint32_t wt = static_cast<uint32_t>(tag_res.value().val & 7u);
    if (wt == 2 && (fnum == 1 || fnum == 6 || fnum == 10)) {
      auto s_res = read_pb_bytes(b, pos);
      if (!s_res) return Error{s_res.error()};
      pos = s_res.value().next_pos;
      const auto sp = s_res.value().slice;
      std::string str(reinterpret_cast<const char*>(sp.data()), sp.size());
      if (fnum == 1) out.name = std::move(str);
      else if (fnum == 6) out.type_name = std::move(str);
      else out.json_name = std::move(str);
    } else if (wt == 0 && (fnum == 3 || fnum == 4 || fnum == 5 || fnum == 9 || fnum == 17)) {
      auto v_res = read_pb_varint(b, pos);
      if (!v_res) return Error{v_res.error()};
      pos = v_res.value().next_pos;
      const uint64_t val = v_res.value().val;
      if (fnum == 3) out.number = static_cast<uint32_t>(val);
      else if (fnum == 4) out.label = static_cast<FieldLabel>(val);
      else if (fnum == 5) out.type = static_cast<FieldType>(val);
      else if (fnum == 9) out.oneof_index = static_cast<uint32_t>(val);
      else out.proto3_optional = (val != 0);
    } else {
      auto sk = skip_pb_field(b, pos, wt);
      if (!sk) return Error{sk.error()};
      pos = sk.value();
    }
  }
  return out;
}

[[nodiscard]] inline Result<EnumDescriptorProto> parse_enum_descriptor(
    std::span<const uint8_t> b) {
  EnumDescriptorProto out;
  size_t pos = 0;
  while (pos < b.size()) {
    auto tag_res = read_pb_varint(b, pos);
    if (!tag_res) return Error{tag_res.error()};
    pos = tag_res.value().next_pos;
    const uint32_t fnum = static_cast<uint32_t>(tag_res.value().val >> 3);
    const uint32_t wt = static_cast<uint32_t>(tag_res.value().val & 7u);
    if (wt == 2 && fnum == 1) {
      auto s_res = read_pb_bytes(b, pos);
      if (!s_res) return Error{s_res.error()};
      pos = s_res.value().next_pos;
      const auto sp = s_res.value().slice;
      out.name.assign(reinterpret_cast<const char*>(sp.data()), sp.size());
    } else if (wt == 2 && fnum == 2) {
      auto s_res = read_pb_bytes(b, pos);
      if (!s_res) return Error{s_res.error()};
      pos = s_res.value().next_pos;
      const auto sub = s_res.value().slice;
      EnumValueDescriptorProto ev;
      size_t p2 = 0;
      while (p2 < sub.size()) {
        auto t2 = read_pb_varint(sub, p2);
        if (!t2) return Error{t2.error()};
        p2 = t2.value().next_pos;
        const uint32_t fn2 = static_cast<uint32_t>(t2.value().val >> 3);
        const uint32_t wt2 = static_cast<uint32_t>(t2.value().val & 7u);
        if (wt2 == 2 && fn2 == 1) {
          auto sr = read_pb_bytes(sub, p2);
          if (!sr) return Error{sr.error()};
          p2 = sr.value().next_pos;
          ev.name.assign(reinterpret_cast<const char*>(sr.value().slice.data()),
                         sr.value().slice.size());
        } else if (wt2 == 0 && fn2 == 2) {
          auto vr = read_pb_varint(sub, p2);
          if (!vr) return Error{vr.error()};
          p2 = vr.value().next_pos;
          ev.number = static_cast<int32_t>(static_cast<uint32_t>(vr.value().val));
        } else {
          auto sk = skip_pb_field(sub, p2, wt2);
          if (!sk) return Error{sk.error()};
          p2 = sk.value();
        }
      }
      out.values.push_back(std::move(ev));
    } else {
      auto sk = skip_pb_field(b, pos, wt);
      if (!sk) return Error{sk.error()};
      pos = sk.value();
    }
  }
  return out;
}

[[nodiscard]] inline Result<DescriptorProto> parse_descriptor_proto(std::span<const uint8_t> b,
                                                                    int depth = 0) {
  if (depth > MAX_CONTAINER_DEPTH) {
    return Error{"omnibuf::proto: descriptor nesting depth exceeds MAX_CONTAINER_DEPTH"};
  }
  DescriptorProto out;
  size_t pos = 0;
  while (pos < b.size()) {
    auto tag_res = read_pb_varint(b, pos);
    if (!tag_res) return Error{tag_res.error()};
    pos = tag_res.value().next_pos;
    const uint32_t fnum = static_cast<uint32_t>(tag_res.value().val >> 3);
    const uint32_t wt = static_cast<uint32_t>(tag_res.value().val & 7u);
    if (wt == 2 && (fnum == 1 || fnum == 2 || fnum == 3 || fnum == 4 || fnum == 7 || fnum == 8)) {
      auto s_res = read_pb_bytes(b, pos);
      if (!s_res) return Error{s_res.error()};
      pos = s_res.value().next_pos;
      const auto sp = s_res.value().slice;
      if (fnum == 1) {
        out.name.assign(reinterpret_cast<const char*>(sp.data()), sp.size());
      } else if (fnum == 2) {
        auto fd = parse_field_descriptor(sp);
        if (!fd) return Error{fd.error()};
        out.fields.push_back(std::move(fd.value()));
      } else if (fnum == 3) {
        auto nd = parse_descriptor_proto(sp, depth + 1);
        if (!nd) return Error{nd.error()};
        out.nested_types.push_back(std::move(nd.value()));
      } else if (fnum == 4) {
        auto ed = parse_enum_descriptor(sp);
        if (!ed) return Error{ed.error()};
        out.enum_types.push_back(std::move(ed.value()));
      } else if (fnum == 7) {
        // MessageOptions: field #7 is bool map_entry
        size_t op = 0;
        while (op < sp.size()) {
          auto ot = read_pb_varint(sp, op);
          if (!ot) return Error{ot.error()};
          op = ot.value().next_pos;
          const uint32_t ofn = static_cast<uint32_t>(ot.value().val >> 3);
          const uint32_t owt = static_cast<uint32_t>(ot.value().val & 7u);
          if (owt == 0 && ofn == 7) {
            auto ov = read_pb_varint(sp, op);
            if (!ov) return Error{ov.error()};
            op = ov.value().next_pos;
            out.map_entry = (ov.value().val != 0);
          } else {
            auto osk = skip_pb_field(sp, op, owt);
            if (!osk) return Error{osk.error()};
            op = osk.value();
          }
        }
      } else if (fnum == 8) {
        OneofDescriptorProto od;
        size_t op = 0;
        while (op < sp.size()) {
          auto ot = read_pb_varint(sp, op);
          if (!ot) return Error{ot.error()};
          op = ot.value().next_pos;
          const uint32_t ofn = static_cast<uint32_t>(ot.value().val >> 3);
          const uint32_t owt = static_cast<uint32_t>(ot.value().val & 7u);
          if (owt == 2 && ofn == 1) {
            auto os = read_pb_bytes(sp, op);
            if (!os) return Error{os.error()};
            op = os.value().next_pos;
            od.name.assign(reinterpret_cast<const char*>(os.value().slice.data()),
                           os.value().slice.size());
          } else {
            auto osk = skip_pb_field(sp, op, owt);
            if (!osk) return Error{osk.error()};
            op = osk.value();
          }
        }
        out.oneof_decls.push_back(std::move(od));
      }
    } else {
      auto sk = skip_pb_field(b, pos, wt);
      if (!sk) return Error{sk.error()};
      pos = sk.value();
    }
  }
  return out;
}

}  // namespace detail

[[nodiscard]] inline Result<FileDescriptorProto> parse_file_descriptor_proto(
    std::span<const uint8_t> b) {
  FileDescriptorProto out;
  size_t pos = 0;
  while (pos < b.size()) {
    auto tag_res = detail::read_pb_varint(b, pos);
    if (!tag_res) return Error{tag_res.error()};
    pos = tag_res.value().next_pos;
    const uint32_t fnum = static_cast<uint32_t>(tag_res.value().val >> 3);
    const uint32_t wt = static_cast<uint32_t>(tag_res.value().val & 7u);
    if (wt == 2 && (fnum == 1 || fnum == 2 || fnum == 3 || fnum == 4 || fnum == 5 || fnum == 12)) {
      auto s_res = detail::read_pb_bytes(b, pos);
      if (!s_res) return Error{s_res.error()};
      pos = s_res.value().next_pos;
      const auto sp = s_res.value().slice;
      if (fnum == 1) {
        out.name.assign(reinterpret_cast<const char*>(sp.data()), sp.size());
      } else if (fnum == 2) {
        out.package.assign(reinterpret_cast<const char*>(sp.data()), sp.size());
      } else if (fnum == 3) {
        out.dependencies.emplace_back(reinterpret_cast<const char*>(sp.data()), sp.size());
      } else if (fnum == 4) {
        auto md = detail::parse_descriptor_proto(sp, 0);
        if (!md) return Error{md.error()};
        out.message_types.push_back(std::move(md.value()));
      } else if (fnum == 5) {
        auto ed = detail::parse_enum_descriptor(sp);
        if (!ed) return Error{ed.error()};
        out.enum_types.push_back(std::move(ed.value()));
      } else if (fnum == 12) {
        out.syntax.assign(reinterpret_cast<const char*>(sp.data()), sp.size());
      }
    } else {
      auto sk = detail::skip_pb_field(b, pos, wt);
      if (!sk) return Error{sk.error()};
      pos = sk.value();
    }
  }
  return out;
}

[[nodiscard]] inline Result<FileDescriptorSet> parse_file_descriptor_set(
    std::span<const uint8_t> buf) {
  FileDescriptorSet fds;
  size_t pos = 0;
  while (pos < buf.size()) {
    auto tag_res = detail::read_pb_varint(buf, pos);
    if (!tag_res) return Error{tag_res.error()};
    pos = tag_res.value().next_pos;
    const uint32_t fnum = static_cast<uint32_t>(tag_res.value().val >> 3);
    const uint32_t wt = static_cast<uint32_t>(tag_res.value().val & 7u);
    if (wt == 2 && fnum == 1) {
      auto s_res = detail::read_pb_bytes(buf, pos);
      if (!s_res) return Error{s_res.error()};
      pos = s_res.value().next_pos;
      auto fd = parse_file_descriptor_proto(s_res.value().slice);
      if (!fd) return Error{fd.error()};
      fds.files.push_back(std::move(fd.value()));
    } else {
      auto sk = detail::skip_pb_field(buf, pos, wt);
      if (!sk) return Error{sk.error()};
      pos = sk.value();
    }
  }
  return fds;
}

[[nodiscard]] inline Result<CodeGeneratorRequest> parse_code_generator_request(
    std::span<const uint8_t> buf) {
  CodeGeneratorRequest req;
  size_t pos = 0;
  while (pos < buf.size()) {
    auto tag_res = detail::read_pb_varint(buf, pos);
    if (!tag_res) return Error{tag_res.error()};
    pos = tag_res.value().next_pos;
    const uint32_t fnum = static_cast<uint32_t>(tag_res.value().val >> 3);
    const uint32_t wt = static_cast<uint32_t>(tag_res.value().val & 7u);
    if (wt == 2 && (fnum == 1 || fnum == 2 || fnum == 15)) {
      auto s_res = detail::read_pb_bytes(buf, pos);
      if (!s_res) return Error{s_res.error()};
      pos = s_res.value().next_pos;
      const auto sp = s_res.value().slice;
      if (fnum == 1) {
        req.files_to_generate.emplace_back(reinterpret_cast<const char*>(sp.data()), sp.size());
      } else if (fnum == 2) {
        req.parameter.assign(reinterpret_cast<const char*>(sp.data()), sp.size());
      } else {
        auto fd = parse_file_descriptor_proto(sp);
        if (!fd) return Error{fd.error()};
        req.proto_files.push_back(std::move(fd.value()));
      }
    } else {
      auto sk = detail::skip_pb_field(buf, pos, wt);
      if (!sk) return Error{sk.error()};
      pos = sk.value();
    }
  }
  return req;
}

[[nodiscard]] inline std::vector<uint8_t> encode_code_generator_response(
    std::span<const GeneratedFile> files,
    std::string_view error_msg = {}) {
  std::vector<uint8_t> out;
  if (!error_msg.empty()) {
    detail::write_pb_string_field(out, 1, error_msg);
    return out;
  }
  // supported_features = FEATURE_PROTO3_OPTIONAL (1), field #2, varint
  detail::write_pb_varint(out, (2ULL << 3) | 0u);
  detail::write_pb_varint(out, 1ULL);

  for (const auto& f : files) {
    std::vector<uint8_t> file_buf;
    file_buf.reserve(f.name.size() + f.content.size() + 32);
    detail::write_pb_string_field(file_buf, 1, f.name);
    detail::write_pb_string_field(file_buf, 15, f.content);
    detail::write_pb_varint(out, (15ULL << 3) | 2u);
    detail::write_pb_varint(out, file_buf.size());
    out.insert(out.end(), file_buf.begin(), file_buf.end());
  }
  return out;
}

// ============================================================================
// Descriptor-Driven DynamicMessage & OmniBuf Serialization (TIER_PROTOBUF_TYPES)
// ============================================================================

struct MessageDescriptor;

struct FieldDesc {
  uint32_t number = 0;
  std::string name;
  FieldType type = FieldType::Uint32;
  bool is_repeated = false;
  bool is_map = false;
  bool has_explicit_presence = false;
  std::optional<uint32_t> real_oneof_index;
  FieldType map_key_type = FieldType::String;
  FieldType map_val_type = FieldType::Uint64;
  const MessageDescriptor* message_desc = nullptr;
};

struct MessageDescriptor {
  std::string full_name;
  std::string short_name;
  std::vector<FieldDesc> fields;
  std::unordered_map<uint32_t, size_t> field_index_by_number;
};

[[nodiscard]] inline Result<uint8_t> proto_type_to_wire_type(FieldType t) {
  switch (t) {
    case FieldType::Uint32:
    case FieldType::Uint64:
      return WireUint;
    case FieldType::Enum:
    case FieldType::Int32:
    case FieldType::Sint32:
    case FieldType::Int64:
    case FieldType::Sint64:
      return WireSint;
    case FieldType::Bool:
      return WireBoolFalse;
    case FieldType::Fixed32:
      return WireFixed32;
    case FieldType::Sfixed32:
      return WireSfixed32;
    case FieldType::Float:
      return WireFloat32;
    case FieldType::Fixed64:
      return WireFixed64;
    case FieldType::Sfixed64:
      return WireSfixed64;
    case FieldType::Double:
      return WireFloat64;
    case FieldType::String:
      return WireString;
    case FieldType::Bytes:
      return WireBytes;
    case FieldType::Message:
      return WireSubMsg;
    default:
      return Error{"omnibuf::proto: unsupported FieldDescriptorProto.Type"};
  }
}

class DynamicMessage;

using ScalarValue = std::variant<bool,
                                 uint32_t,
                                 uint64_t,
                                 int32_t,
                                 int64_t,
                                 float,
                                 double,
                                 std::string,
                                 std::vector<uint8_t>>;

struct MapKey {
  std::variant<bool, uint64_t, int64_t, std::string> key;

  bool operator<(const MapKey& other) const noexcept { return key < other.key; }
  bool operator==(const MapKey& other) const noexcept { return key == other.key; }
};

class DynamicMessage {
 public:
  struct FieldValue {
    bool present = false;
    ScalarValue scalar;
    std::unique_ptr<DynamicMessage> submsg;
    std::vector<ScalarValue> scalar_list;
    std::vector<std::unique_ptr<DynamicMessage>> submsg_list;
    std::map<MapKey, ScalarValue> scalar_map;
    std::map<MapKey, std::unique_ptr<DynamicMessage>> submsg_map;
  };

  void clear() { fields_.clear(); }

  std::map<uint32_t, FieldValue>& mutable_fields() noexcept { return fields_; }
  const std::map<uint32_t, FieldValue>& fields() const noexcept { return fields_; }

  static ScalarValue default_scalar(FieldType t) {
    switch (t) {
      case FieldType::Bool:
        return false;
      case FieldType::Uint32:
      case FieldType::Fixed32:
        return uint32_t{0};
      case FieldType::Uint64:
      case FieldType::Fixed64:
        return uint64_t{0};
      case FieldType::Int32:
      case FieldType::Sint32:
      case FieldType::Sfixed32:
      case FieldType::Enum:
        return int32_t{0};
      case FieldType::Int64:
      case FieldType::Sint64:
      case FieldType::Sfixed64:
        return int64_t{0};
      case FieldType::Float:
        return 0.0f;
      case FieldType::Double:
        return 0.0;
      case FieldType::String:
        return std::string{};
      case FieldType::Bytes:
        return std::vector<uint8_t>{};
      default:
        return uint32_t{0};
    }
  }

 private:
  std::map<uint32_t, FieldValue> fields_;
};

class DescriptorPool {
 public:
  Result<void> add_file_descriptor_set(const FileDescriptorSet& fds) {
    struct RawEntry {
      DescriptorProto proto;
      std::string syntax;
    };
    std::unordered_map<std::string, RawEntry> raw_map;

    auto register_rec = [&](auto& self,
                            const std::string& prefix,
                            const DescriptorProto& d,
                            const std::string& syntax) -> void {
      const std::string full = prefix.empty() ? d.name : (prefix + "." + d.name);
      raw_map[full] = RawEntry{d, syntax};
      if (!d.map_entry && messages_.find(full) == messages_.end()) {
        auto md = std::make_unique<MessageDescriptor>();
        md->full_name = full;
        md->short_name = d.name;
        messages_[full] = std::move(md);
      }
      for (const auto& nested : d.nested_types) {
        self(self, full, nested, syntax);
      }
    };

    for (const auto& file : fds.files) {
      for (const auto& msg : file.message_types) {
        register_rec(register_rec, file.package, msg, file.syntax);
      }
    }

    auto clean_name = [](std::string_view tn) -> std::string {
      if (!tn.empty() && tn.front() == '.') tn.remove_prefix(1);
      return std::string(tn);
    };

    for (auto& [full, md_ptr] : messages_) {
      auto raw_it = raw_map.find(full);
      if (raw_it == raw_map.end() || !md_ptr->fields.empty()) continue;
      const auto& dp = raw_it->second.proto;
      const bool is_proto2 = (raw_it->second.syntax == "proto2" || raw_it->second.syntax.empty());

      for (const auto& f : dp.fields) {
        if (f.type == FieldType::Group) {
          return Error{"omnibuf::proto: TYPE_GROUP is not supported"};
        }
        FieldDesc fd;
        fd.number = f.number;
        fd.name = f.name;
        fd.type = f.type;
        fd.is_repeated = (f.label == FieldLabel::Repeated);
        const bool has_real_oneof =
            f.oneof_index.has_value() && !f.proto3_optional && *f.oneof_index < dp.oneof_decls.size();
        if (has_real_oneof) {
          fd.real_oneof_index = *f.oneof_index;
        }
        fd.has_explicit_presence =
            has_real_oneof ||
            (!fd.is_repeated &&
             (f.type == FieldType::Message || f.proto3_optional || is_proto2));

        if (f.type == FieldType::Message) {
          const std::string target_name = clean_name(f.type_name);
          auto target_raw = raw_map.find(target_name);
          if (target_raw != raw_map.end() && target_raw->second.proto.map_entry) {
            fd.is_map = true;
            fd.is_repeated = false;
            fd.has_explicit_presence = false;
            for (const auto& mf : target_raw->second.proto.fields) {
              if (mf.number == 1) fd.map_key_type = mf.type;
              if (mf.number == 2) {
                fd.map_val_type = mf.type;
                if (mf.type == FieldType::Message) {
                  auto vit = messages_.find(clean_name(mf.type_name));
                  if (vit == messages_.end()) {
                    return Error{"omnibuf::proto: missing map value message descriptor"};
                  }
                  fd.message_desc = vit->second.get();
                }
              }
            }
          } else {
            auto mit = messages_.find(target_name);
            if (mit == messages_.end()) {
              return Error{"omnibuf::proto: missing sub-message descriptor: " + target_name};
            }
            fd.message_desc = mit->second.get();
          }
        }
        md_ptr->field_index_by_number[fd.number] = md_ptr->fields.size();
        md_ptr->fields.push_back(std::move(fd));
      }
    }
    return Result<void>::success();
  }

  Result<void> add_file_descriptor_set_bytes(std::span<const uint8_t> bytes) {
    auto fds = parse_file_descriptor_set(bytes);
    if (!fds) return Error{fds.error()};
    return add_file_descriptor_set(fds.value());
  }

  Result<void> load_file(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
      return Error{"omnibuf::proto: failed to open descriptor file: " + path};
    }
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)),
                               std::istreambuf_iterator<char>());
    return add_file_descriptor_set_bytes(bytes);
  }

  [[nodiscard]] const MessageDescriptor* find_message(std::string_view full_name) const noexcept {
    if (!full_name.empty() && full_name.front() == '.') full_name.remove_prefix(1);
    auto it = messages_.find(std::string(full_name));
    return (it != messages_.end()) ? it->second.get() : nullptr;
  }

 private:
  std::unordered_map<std::string, std::unique_ptr<MessageDescriptor>> messages_;
};

namespace detail {

inline std::shared_ptr<SchemaMeta> build_proto_schema_meta(const MessageDescriptor* md,
                                                           int depth,
                                                           std::set<std::string_view>& active) {
  if (md == nullptr || depth > MAX_CONTAINER_DEPTH || active.contains(md->full_name)) {
    return nullptr;
  }
  active.insert(md->full_name);
  auto meta = std::make_shared<SchemaMeta>();
  meta->type_name = md->short_name;
  for (const auto& fd : md->fields) {
    meta->fields[fd.number] = fd.name;
    if (fd.message_desc != nullptr) {
      const int child_depth = (fd.is_repeated || fd.is_map) ? (depth + 2) : (depth + 1);
      if (auto child = build_proto_schema_meta(fd.message_desc, child_depth, active)) {
        meta->children[fd.number] = std::move(child);
      }
    }
  }
  active.erase(md->full_name);
  return meta;
}

[[nodiscard]] inline Result<void> append_proto_scalar(std::vector<uint8_t>& dst,
                                                      FieldType t,
                                                      const ScalarValue& val) {
  switch (t) {
    case FieldType::Bool:
      dst.push_back(std::get<bool>(val) ? uint8_t{1} : uint8_t{0});
      return Result<void>::success();
    case FieldType::Uint32:
      append_varint(dst, std::get<uint32_t>(val));
      return Result<void>::success();
    case FieldType::Uint64:
      append_varint(dst, std::get<uint64_t>(val));
      return Result<void>::success();
    case FieldType::Int32:
    case FieldType::Sint32:
    case FieldType::Enum:
      append_varint(dst, encode_zigzag(std::get<int32_t>(val)));
      return Result<void>::success();
    case FieldType::Int64:
    case FieldType::Sint64:
      append_varint(dst, encode_zigzag(std::get<int64_t>(val)));
      return Result<void>::success();
    case FieldType::Fixed32:
      append_u32_le(dst, std::get<uint32_t>(val));
      return Result<void>::success();
    case FieldType::Sfixed32:
      append_u32_le(dst, static_cast<uint32_t>(std::get<int32_t>(val)));
      return Result<void>::success();
    case FieldType::Float:
      append_u32_le(dst, float32_to_bits(std::get<float>(val)));
      return Result<void>::success();
    case FieldType::Fixed64:
      append_u64_le(dst, std::get<uint64_t>(val));
      return Result<void>::success();
    case FieldType::Sfixed64:
      append_u64_le(dst, static_cast<uint64_t>(std::get<int64_t>(val)));
      return Result<void>::success();
    case FieldType::Double:
      append_u64_le(dst, float64_to_bits(std::get<double>(val)));
      return Result<void>::success();
    case FieldType::String: {
      const auto& s = std::get<std::string>(val);
      if (!is_valid_utf8(s)) {
        return Error{"omnibuf::proto: string contains invalid UTF-8"};
      }
      append_varint(dst, s.size());
      dst.insert(dst.end(), s.begin(), s.end());
      return Result<void>::success();
    }
    case FieldType::Bytes: {
      const auto& b = std::get<std::vector<uint8_t>>(val);
      append_varint(dst, b.size());
      dst.insert(dst.end(), b.begin(), b.end());
      return Result<void>::success();
    }
    default:
      return Error{"omnibuf::proto: unsupported scalar type"};
  }
}

[[nodiscard]] inline Result<void> append_proto_map_key(std::vector<uint8_t>& dst,
                                                       FieldType kt,
                                                       const MapKey& k) {
  switch (kt) {
    case FieldType::Bool:
      dst.push_back(std::get<bool>(k.key) ? uint8_t{1} : uint8_t{0});
      return Result<void>::success();
    case FieldType::Uint32:
    case FieldType::Uint64:
      append_varint(dst, std::get<uint64_t>(k.key));
      return Result<void>::success();
    case FieldType::Int32:
    case FieldType::Sint32:
    case FieldType::Int64:
    case FieldType::Sint64:
      append_varint(dst, encode_zigzag(std::get<int64_t>(k.key)));
      return Result<void>::success();
    case FieldType::Fixed32:
      append_u32_le(dst, static_cast<uint32_t>(std::get<uint64_t>(k.key)));
      return Result<void>::success();
    case FieldType::Sfixed32:
      append_u32_le(dst, static_cast<uint32_t>(static_cast<int32_t>(std::get<int64_t>(k.key))));
      return Result<void>::success();
    case FieldType::Fixed64:
      append_u64_le(dst, std::get<uint64_t>(k.key));
      return Result<void>::success();
    case FieldType::Sfixed64:
      append_u64_le(dst, static_cast<uint64_t>(std::get<int64_t>(k.key)));
      return Result<void>::success();
    case FieldType::String: {
      const auto& s = std::get<std::string>(k.key);
      if (!is_valid_utf8(s)) return Error{"omnibuf::proto: map key invalid UTF-8"};
      append_varint(dst, s.size());
      dst.insert(dst.end(), s.begin(), s.end());
      return Result<void>::success();
    }
    default:
      return Error{"omnibuf::proto: unsupported map key type"};
  }
}

struct DecodedScalar {
  ScalarValue val;
  size_t next_pos = 0;
};

[[nodiscard]] inline Result<DecodedScalar> decode_proto_scalar(std::span<const uint8_t> data,
                                                               size_t pos,
                                                               FieldType t) {
  switch (t) {
    case FieldType::Bool: {
      if (pos >= data.size() || data[pos] > 1u) return Error{"omnibuf::proto: invalid bool"};
      return DecodedScalar{data[pos] == 1u, pos + 1};
    }
    case FieldType::Uint32: {
      const auto [v, vn] = decode_varint(data.subspan(pos));
      if (vn == 0) return Error{"omnibuf::proto: truncated uint32"};
      return DecodedScalar{static_cast<uint32_t>(v), pos + vn};
    }
    case FieldType::Uint64: {
      const auto [v, vn] = decode_varint(data.subspan(pos));
      if (vn == 0) return Error{"omnibuf::proto: truncated uint64"};
      return DecodedScalar{v, pos + vn};
    }
    case FieldType::Int32:
    case FieldType::Sint32:
    case FieldType::Enum: {
      const auto [v, vn] = decode_varint(data.subspan(pos));
      if (vn == 0) return Error{"omnibuf::proto: truncated sint32"};
      return DecodedScalar{static_cast<int32_t>(decode_zigzag(v)), pos + vn};
    }
    case FieldType::Int64:
    case FieldType::Sint64: {
      const auto [v, vn] = decode_varint(data.subspan(pos));
      if (vn == 0) return Error{"omnibuf::proto: truncated sint64"};
      return DecodedScalar{decode_zigzag(v), pos + vn};
    }
    case FieldType::Fixed32:
      if (data.size() - pos < 4) return Error{"omnibuf::proto: truncated fixed32"};
      return DecodedScalar{read_u32_le(data.data() + pos), pos + 4};
    case FieldType::Sfixed32:
      if (data.size() - pos < 4) return Error{"omnibuf::proto: truncated sfixed32"};
      return DecodedScalar{static_cast<int32_t>(read_u32_le(data.data() + pos)), pos + 4};
    case FieldType::Float:
      if (data.size() - pos < 4) return Error{"omnibuf::proto: truncated float32"};
      return DecodedScalar{bits_to_float32(read_u32_le(data.data() + pos)), pos + 4};
    case FieldType::Fixed64:
      if (data.size() - pos < 8) return Error{"omnibuf::proto: truncated fixed64"};
      return DecodedScalar{read_u64_le(data.data() + pos), pos + 8};
    case FieldType::Sfixed64:
      if (data.size() - pos < 8) return Error{"omnibuf::proto: truncated sfixed64"};
      return DecodedScalar{static_cast<int64_t>(read_u64_le(data.data() + pos)), pos + 8};
    case FieldType::Double:
      if (data.size() - pos < 8) return Error{"omnibuf::proto: truncated float64"};
      return DecodedScalar{bits_to_float64(read_u64_le(data.data() + pos)), pos + 8};
    case FieldType::String: {
      auto s_res = read_str_slice(data, pos);
      if (!s_res) return Error{s_res.error()};
      const auto sp = s_res.value().slice;
      return DecodedScalar{std::string(reinterpret_cast<const char*>(sp.data()), sp.size()),
                           s_res.value().next_pos};
    }
    case FieldType::Bytes: {
      auto b_res = read_len_slice(data, pos, 0);
      if (!b_res) return Error{b_res.error()};
      const auto sp = b_res.value().slice;
      return DecodedScalar{std::vector<uint8_t>(sp.begin(), sp.end()), b_res.value().next_pos};
    }
    default:
      return Error{"omnibuf::proto: unsupported scalar type"};
  }
}

struct DecodedMapKey {
  MapKey key;
  size_t next_pos = 0;
};

[[nodiscard]] inline Result<DecodedMapKey> decode_proto_map_key(std::span<const uint8_t> data,
                                                                size_t pos,
                                                                FieldType kt) {
  auto sc = decode_proto_scalar(data, pos, kt);
  if (!sc) return Error{sc.error()};
  MapKey mk;
  switch (kt) {
    case FieldType::Bool:
      mk.key = std::get<bool>(sc.value().val);
      break;
    case FieldType::Uint32:
    case FieldType::Fixed32:
      mk.key = static_cast<uint64_t>(std::get<uint32_t>(sc.value().val));
      break;
    case FieldType::Uint64:
    case FieldType::Fixed64:
      mk.key = std::get<uint64_t>(sc.value().val);
      break;
    case FieldType::Int32:
    case FieldType::Sint32:
    case FieldType::Sfixed32:
      mk.key = static_cast<int64_t>(std::get<int32_t>(sc.value().val));
      break;
    case FieldType::Int64:
    case FieldType::Sint64:
    case FieldType::Sfixed64:
      mk.key = std::get<int64_t>(sc.value().val);
      break;
    case FieldType::String:
      mk.key = std::move(std::get<std::string>(sc.value().val));
      break;
    default:
      return Error{"omnibuf::proto: invalid map key type"};
  }
  return DecodedMapKey{std::move(mk), sc.value().next_pos};
}

[[nodiscard]] inline Result<void> encode_dynamic_msg(std::vector<uint8_t>& dst,
                                                     const MessageDescriptor& md,
                                                     const DynamicMessage& msg) {
  const auto& fv_map = msg.fields();
  for (const auto& fd : md.fields) {
    auto it = fv_map.find(fd.number);
    const bool has = (it != fv_map.end() && it->second.present);
    if (fd.has_explicit_presence && !has) {
      continue;
    }

    if (fd.is_map) {
      auto key_wt = proto_type_to_wire_type(fd.map_key_type);
      auto val_wt = proto_type_to_wire_type(fd.map_val_type);
      if (!key_wt || !val_wt) return Error{"omnibuf::proto: invalid map wire type"};
      append_tag(dst, fd.number, WireMap);
      const size_t len_pos = dst.size();
      dst.push_back(0);
      dst.push_back(0);
      dst.push_back(key_wt.value());
      dst.push_back(val_wt.value());
      if (!has) {
        append_varint(dst, 0);
      } else if (fd.map_val_type == FieldType::Message) {
        const auto& mp = it->second.submsg_map;
        append_varint(dst, mp.size());
        for (const auto& [k, v_ptr] : mp) {
          auto kr = append_proto_map_key(dst, fd.map_key_type, k);
          if (!kr) return kr;
          const size_t v_pos = dst.size();
          dst.push_back(0);
          dst.push_back(0);
          if (v_ptr) {
            auto vr = encode_dynamic_msg(dst, *fd.message_desc, *v_ptr);
            if (!vr) return vr;
          }
          backpatch_container_len(dst, v_pos);
        }
      } else {
        const auto& mp = it->second.scalar_map;
        append_varint(dst, mp.size());
        for (const auto& [k, v] : mp) {
          auto kr = append_proto_map_key(dst, fd.map_key_type, k);
          if (!kr) return kr;
          auto vr = append_proto_scalar(dst, fd.map_val_type, v);
          if (!vr) return vr;
        }
      }
      backpatch_container_len(dst, len_pos);
      continue;
    }

    if (fd.is_repeated) {
      auto elem_wt = proto_type_to_wire_type(fd.type);
      if (!elem_wt) return Error{elem_wt.error()};
      append_tag(dst, fd.number, WireArray);
      const size_t len_pos = dst.size();
      dst.push_back(0);
      dst.push_back(0);
      dst.push_back(elem_wt.value());
      if (!has) {
        append_varint(dst, 0);
      } else if (fd.type == FieldType::Message) {
        const auto& list = it->second.submsg_list;
        append_varint(dst, list.size());
        for (const auto& elem_ptr : list) {
          const size_t e_pos = dst.size();
          dst.push_back(0);
          dst.push_back(0);
          if (elem_ptr) {
            auto er = encode_dynamic_msg(dst, *fd.message_desc, *elem_ptr);
            if (!er) return er;
          }
          backpatch_container_len(dst, e_pos);
        }
      } else {
        const auto& list = it->second.scalar_list;
        append_varint(dst, list.size());
        for (const auto& elem : list) {
          auto er = append_proto_scalar(dst, fd.type, elem);
          if (!er) return er;
        }
      }
      backpatch_container_len(dst, len_pos);
      continue;
    }

    if (fd.type == FieldType::Message) {
      if (!has || !it->second.submsg) continue;
      append_tag(dst, fd.number, WireSubMsg);
      const size_t len_pos = dst.size();
      dst.push_back(0);
      dst.push_back(0);
      auto sr = encode_dynamic_msg(dst, *fd.message_desc, *it->second.submsg);
      if (!sr) return sr;
      backpatch_container_len(dst, len_pos);
      continue;
    }

    const ScalarValue default_val = DynamicMessage::default_scalar(fd.type);
    const ScalarValue& val = has ? it->second.scalar : default_val;
    if (fd.type == FieldType::Bool) {
      append_tag(dst, fd.number, std::get<bool>(val) ? WireBoolTrue : WireBoolFalse);
    } else {
      auto wt = proto_type_to_wire_type(fd.type);
      if (!wt) return Error{wt.error()};
      append_tag(dst, fd.number, wt.value());
      auto sr = append_proto_scalar(dst, fd.type, val);
      if (!sr) return sr;
    }
  }
  return Result<void>::success();
}

[[nodiscard]] inline Result<void> decode_dynamic_msg(const MessageDescriptor& md,
                                                     std::span<const uint8_t> data,
                                                     DynamicMessage& out,
                                                     int depth,
                                                     bool merge) {
  if (depth > MAX_CONTAINER_DEPTH) {
    return Error{"omnibuf::proto: nesting depth exceeds MAX_CONTAINER_DEPTH"};
  }
  if (!merge) {
    out.clear();
  }
  std::set<uint32_t> seen;
  size_t pos = 0;
  const size_t n = data.size();

  while (pos < n) {
    const auto [tag, tn] = decode_varint(data.subspan(pos));
    if (tn == 0 || (tag >> 4) > MAX_FIELD_NUMBER) {
      return Error{"omnibuf::proto: invalid or truncated tag"};
    }
    pos += tn;
    const uint32_t fnum = static_cast<uint32_t>(tag >> 4);
    const uint8_t wt = static_cast<uint8_t>(tag & 0x0Fu);

    if (fnum == 0) {
      auto f0 = validate_field0(data, pos, wt, depth);
      if (!f0) return Error{f0.error()};
      pos = f0.value();
      continue;
    }

    auto idx_it = md.field_index_by_number.find(fnum);
    if (idx_it == md.field_index_by_number.end()) {
      auto sk = validate_value(data, pos, wt, depth);
      if (!sk) return Error{sk.error()};
      pos = sk.value();
      continue;
    }

    const auto& fd = md.fields[idx_it->second];
    const bool already_seen = merge || seen.contains(fnum);

    if (fd.real_oneof_index.has_value()) {
      for (const auto& sibling : md.fields) {
        if (sibling.real_oneof_index == fd.real_oneof_index && sibling.number != fd.number) {
          out.mutable_fields().erase(sibling.number);
        }
      }
    }

    if (fd.is_map) {
      if (wt != WireMap) {
        auto sk = validate_value(data, pos, wt, depth);
        if (!sk) return Error{sk.error()};
        pos = sk.value();
        continue;
      }
      if (depth + 1 > MAX_CONTAINER_DEPTH) {
        return Error{"omnibuf::proto: map nesting depth exceeds MAX_CONTAINER_DEPTH"};
      }
      auto body_res = read_len_slice(data, pos, 3);
      if (!body_res) return Error{body_res.error()};
      const auto body = body_res.value().slice;
      const auto hdr = parse_map_header(body);
      if (!hdr.ok) return Error{"omnibuf::proto: invalid WireMap header"};
      auto exp_k = proto_type_to_wire_type(fd.map_key_type);
      auto exp_v = proto_type_to_wire_type(fd.map_val_type);
      if (!exp_k || !exp_v || hdr.key_wt != exp_k.value() || hdr.val_wt != exp_v.value()) {
        return Error{"omnibuf::proto: WireMap wire type mismatch"};
      }
      auto& fv = out.mutable_fields()[fnum];
      fv.present = true;
      if (!already_seen) {
        fv.scalar_map.clear();
        fv.submsg_map.clear();
      }
      seen.insert(fnum);
      size_t p = hdr.pos;
      for (size_t i = 0; i < hdr.count; ++i) {
        auto kr = decode_proto_map_key(body, p, fd.map_key_type);
        if (!kr) return Error{kr.error()};
        p = kr.value().next_pos;
        if (fd.map_val_type == FieldType::Message) {
          auto vs = read_len_slice(body, p, 0);
          if (!vs) return Error{vs.error()};
          auto sub = std::make_unique<DynamicMessage>();
          auto dr = decode_dynamic_msg(*fd.message_desc, vs.value().slice, *sub, depth + 2, false);
          if (!dr) return Error{dr.error()};
          fv.submsg_map[std::move(kr.value().key)] = std::move(sub);
          p = vs.value().next_pos;
        } else {
          auto vr = decode_proto_scalar(body, p, fd.map_val_type);
          if (!vr) return Error{vr.error()};
          fv.scalar_map[std::move(kr.value().key)] = std::move(vr.value().val);
          p = vr.value().next_pos;
        }
      }
      if (p != body.size()) return Error{"omnibuf::proto: WireMap trailing bytes"};
      pos = body_res.value().next_pos;
      continue;
    }

    if (fd.is_repeated) {
      if (wt != WireArray) {
        auto sk = validate_value(data, pos, wt, depth);
        if (!sk) return Error{sk.error()};
        pos = sk.value();
        continue;
      }
      if (depth + 1 > MAX_CONTAINER_DEPTH) {
        return Error{"omnibuf::proto: array nesting depth exceeds MAX_CONTAINER_DEPTH"};
      }
      auto body_res = read_len_slice(data, pos, 2);
      if (!body_res) return Error{body_res.error()};
      const auto body = body_res.value().slice;
      const auto hdr = parse_array_header(body);
      if (!hdr.ok) return Error{"omnibuf::proto: invalid WireArray header"};
      auto exp_elem = proto_type_to_wire_type(fd.type);
      if (!exp_elem || hdr.elem_wt != exp_elem.value()) {
        return Error{"omnibuf::proto: WireArray element type mismatch"};
      }
      auto& fv = out.mutable_fields()[fnum];
      fv.present = true;
      if (!already_seen) {
        fv.scalar_list.clear();
        fv.submsg_list.clear();
      }
      seen.insert(fnum);
      size_t p = hdr.pos;
      if (fd.type == FieldType::Message) {
        fv.submsg_list.reserve(grow_capacity(fv.submsg_list.capacity(), fv.submsg_list.size() + hdr.count));
        for (size_t i = 0; i < hdr.count; ++i) {
          auto es = read_len_slice(body, p, 0);
          if (!es) return Error{es.error()};
          auto sub = std::make_unique<DynamicMessage>();
          auto dr = decode_dynamic_msg(*fd.message_desc, es.value().slice, *sub, depth + 2, false);
          if (!dr) return Error{dr.error()};
          fv.submsg_list.push_back(std::move(sub));
          p = es.value().next_pos;
        }
      } else {
        fv.scalar_list.reserve(grow_capacity(fv.scalar_list.capacity(), fv.scalar_list.size() + hdr.count));
        for (size_t i = 0; i < hdr.count; ++i) {
          auto er = decode_proto_scalar(body, p, fd.type);
          if (!er) return Error{er.error()};
          fv.scalar_list.push_back(std::move(er.value().val));
          p = er.value().next_pos;
        }
      }
      if (p != body.size()) return Error{"omnibuf::proto: WireArray trailing bytes"};
      pos = body_res.value().next_pos;
      continue;
    }

    if (fd.type == FieldType::Message) {
      if (wt != WireSubMsg) {
        auto sk = validate_value(data, pos, wt, depth);
        if (!sk) return Error{sk.error()};
        pos = sk.value();
        continue;
      }
      auto sub_res = read_len_slice(data, pos, 0);
      if (!sub_res) return Error{sub_res.error()};
      auto& fv = out.mutable_fields()[fnum];
      fv.present = true;
      if (!fv.submsg || !already_seen) {
        fv.submsg = std::make_unique<DynamicMessage>();
      }
      seen.insert(fnum);
      auto dr = decode_dynamic_msg(
          *fd.message_desc, sub_res.value().slice, *fv.submsg, depth + 1, already_seen);
      if (!dr) return Error{dr.error()};
      pos = sub_res.value().next_pos;
      continue;
    }

    if (fd.type == FieldType::Bool) {
      if (wt != WireBoolFalse && wt != WireBoolTrue) {
        auto sk = validate_value(data, pos, wt, depth);
        if (!sk) return Error{sk.error()};
        pos = sk.value();
        continue;
      }
      auto& fv = out.mutable_fields()[fnum];
      fv.present = true;
      fv.scalar = (wt == WireBoolTrue);
      seen.insert(fnum);
      continue;
    }

    auto exp_wt = proto_type_to_wire_type(fd.type);
    if (!exp_wt || wt != exp_wt.value()) {
      auto sk = validate_value(data, pos, wt, depth);
      if (!sk) return Error{sk.error()};
      pos = sk.value();
      continue;
    }
    auto sr = decode_proto_scalar(data, pos, fd.type);
    if (!sr) return Error{sr.error()};
    auto& fv = out.mutable_fields()[fnum];
    fv.present = true;
    fv.scalar = std::move(sr.value().val);
    seen.insert(fnum);
    pos = sr.value().next_pos;
  }

  return Result<void>::success();
}

}  // namespace detail

[[nodiscard]] inline Result<void> from_binary(const MessageDescriptor& desc,
                                              std::span<const uint8_t> data,
                                              DynamicMessage& out) {
  return detail::decode_dynamic_msg(desc, data, out, 0, false);
}

[[nodiscard]] inline Result<std::vector<uint8_t>> to_binary(const MessageDescriptor& desc,
                                                            const DynamicMessage& msg,
                                                            MarshalOptions opts = {}) {
  std::vector<uint8_t> dst;
  dst.reserve(256);
  if (opts.self_describing) {
    std::set<std::string_view> active;
    if (auto meta = detail::build_proto_schema_meta(&desc, 0, active)) {
      append_field0_schema_meta(dst, *meta);
    }
  }
  auto res = detail::encode_dynamic_msg(dst, desc, msg);
  if (!res) return Error{res.error()};
  return dst;
}

}  // namespace omnibuf::proto
