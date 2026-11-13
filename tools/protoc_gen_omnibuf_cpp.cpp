#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <map>
#include <set>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "omnibuf/descriptor.hpp"
#include "omnibuf/varint.hpp"

namespace {

using omnibuf::proto::CodeGeneratorRequest;
using omnibuf::proto::DescriptorProto;
using omnibuf::proto::EnumDescriptorProto;
using omnibuf::proto::FieldDescriptorProto;
using omnibuf::proto::FieldLabel;
using omnibuf::proto::FieldType;
using omnibuf::proto::FileDescriptorProto;
using omnibuf::proto::GeneratedFile;

struct MapEntryInfo {
  FieldType key_type = FieldType::String;
  FieldType val_type = FieldType::Uint64;
  std::string val_type_name;
};

struct TypeCatalog {
  std::unordered_map<std::string, MapEntryInfo> map_entries;
  std::unordered_set<std::string> enum_names;
  std::unordered_set<std::string> message_names;
};

std::string trim_leading_dot(std::string_view s) {
  if (!s.empty() && s.front() == '.') {
    s.remove_prefix(1);
  }
  return std::string(s);
}

std::string proto_package_to_cpp_ns(std::string_view pkg) {
  std::string out;
  out.reserve(pkg.size() + 8);
  for (char c : pkg) {
    if (c == '.') {
      out += "::";
    } else {
      out.push_back(c);
    }
  }
  return out;
}

std::string qualified_cpp_type(std::string_view proto_type_name) {
  const std::string clean = trim_leading_dot(proto_type_name);
  return "::" + proto_package_to_cpp_ns(clean);
}

std::string qualified_opaque_cpp_type(std::string_view proto_type_name) {
  const std::string clean = trim_leading_dot(proto_type_name);
  const size_t dot = clean.rfind('.');
  if (dot == std::string::npos) {
    return "::Opaque" + clean;
  }
  const std::string pkg = clean.substr(0, dot);
  const std::string short_name = clean.substr(dot + 1);
  return "::" + proto_package_to_cpp_ns(pkg) + "::Opaque" + short_name;
}

std::string header_output_path(std::string_view proto_path) {
  if (proto_path.size() >= 6 && proto_path.substr(proto_path.size() - 6) == ".proto") {
    proto_path.remove_suffix(6);
  }
  return std::string(proto_path) + ".omnibuf.pb.hpp";
}

std::string sanitize_ident(std::string_view name) {
  static const std::unordered_set<std::string_view> kKeywords = {
      "alignas", "alignof", "and", "and_eq", "asm", "auto", "bitand", "bitor", "bool",
      "break", "case", "catch", "char", "char8_t", "char16_t", "char32_t", "class",
      "compl", "concept", "const", "consteval", "constexpr", "constinit", "const_cast",
      "continue", "co_await", "co_return", "co_yield", "decltype", "default", "delete",
      "do", "double", "dynamic_cast", "else", "enum", "explicit", "export", "extern",
      "false", "float", "for", "friend", "goto", "if", "inline", "int", "long",
      "mutable", "namespace", "new", "noexcept", "not", "not_eq", "nullptr", "operator",
      "or", "or_eq", "private", "protected", "public", "register", "reinterpret_cast",
      "requires", "return", "short", "signed", "sizeof", "static", "static_assert",
      "static_cast", "struct", "switch", "template", "this", "thread_local", "throw",
      "true", "try", "typedef", "typeid", "typename", "union", "unsigned", "using",
      "virtual", "void", "volatile", "wchar_t", "while", "xor", "xor_eq"};
  if (kKeywords.contains(name)) {
    return std::string(name) + "_";
  }
  return std::string(name);
}

void index_descriptor_tree(const std::string& prefix,
                           const DescriptorProto& d,
                           TypeCatalog& catalog) {
  const std::string full = prefix.empty() ? d.name : (prefix + "." + d.name);
  if (d.map_entry) {
    MapEntryInfo info;
    for (const auto& f : d.fields) {
      if (f.number == 1) info.key_type = f.type;
      if (f.number == 2) {
        info.val_type = f.type;
        info.val_type_name = f.type_name;
      }
    }
    catalog.map_entries[full] = std::move(info);
  } else {
    catalog.message_names.insert(full);
  }
  for (const auto& e : d.enum_types) {
    catalog.enum_names.insert(full + "." + e.name);
  }
  for (const auto& nested : d.nested_types) {
    index_descriptor_tree(full, nested, catalog);
  }
}

TypeCatalog build_catalog(const CodeGeneratorRequest& req) {
  TypeCatalog catalog;
  for (const auto& file : req.proto_files) {
    for (const auto& e : file.enum_types) {
      const std::string full = file.package.empty() ? e.name : (file.package + "." + e.name);
      catalog.enum_names.insert(full);
    }
    for (const auto& m : file.message_types) {
      index_descriptor_tree(file.package, m, catalog);
    }
  }
  return catalog;
}

std::string scalar_cpp_type(const FieldDescriptorProto& f) {
  switch (f.type) {
    case FieldType::Uint32:
    case FieldType::Fixed32:
      return "uint32_t";
    case FieldType::Uint64:
    case FieldType::Fixed64:
      return "uint64_t";
    case FieldType::Int32:
    case FieldType::Sint32:
    case FieldType::Sfixed32:
      return "int32_t";
    case FieldType::Int64:
    case FieldType::Sint64:
    case FieldType::Sfixed64:
      return "int64_t";
    case FieldType::Float:
      return "float";
    case FieldType::Double:
      return "double";
    case FieldType::Bool:
      return "bool";
    case FieldType::String:
      return "std::string";
    case FieldType::Bytes:
      return "std::vector<uint8_t>";
    case FieldType::Enum:
      return qualified_cpp_type(f.type_name);
    case FieldType::Message:
      return qualified_cpp_type(f.type_name);
    default:
      return "uint32_t";
  }
}

std::string scalar_default_literal(const FieldDescriptorProto& f) {
  switch (f.type) {
    case FieldType::Bool:
      return "false";
    case FieldType::Float:
      return "0.0f";
    case FieldType::Double:
      return "0.0";
    case FieldType::Enum:
      return "static_cast<" + qualified_cpp_type(f.type_name) + ">(0)";
    case FieldType::String:
    case FieldType::Bytes:
      return "{}";
    default:
      return "0";
  }
}

uint8_t scalar_wire_const(FieldType t) {
  auto res = omnibuf::proto::proto_type_to_wire_type(t);
  return res ? res.value() : omnibuf::WireUint;
}

void emit_enum(std::ostringstream& out, const EnumDescriptorProto& e) {
  out << "enum class " << e.name << " : int32_t {\n";
  for (const auto& v : e.values) {
    out << "  " << v.name << " = " << v.number << ",\n";
  }
  out << "};\n\n";
}

void emit_encode_single_field(std::ostringstream& out,
                              const FieldDescriptorProto& f,
                              const TypeCatalog& catalog,
                              std::string_view expr_prefix,
                              std::string_view buf_var,
                              std::string_view indent) {
  const std::string member = std::string(expr_prefix) + sanitize_ident(f.name);
  const uint32_t fnum = f.number;
  const bool is_repeated = (f.label == FieldLabel::Repeated);

  if (f.type == FieldType::Message) {
    const std::string target = trim_leading_dot(f.type_name);
    auto map_it = catalog.map_entries.find(target);
    if (map_it != catalog.map_entries.end()) {
      out << indent << "::omnibuf::append_tag(" << buf_var << ", " << fnum
          << ", ::omnibuf::WireMap);\n";
      out << indent << "::omnibuf::encode_map_string_uint(" << buf_var << ", " << member << ");\n";
      return;
    }
    if (is_repeated) {
      out << indent << "{\n";
      out << indent << "  ::omnibuf::append_tag(" << buf_var << ", " << fnum
          << ", ::omnibuf::WireArray);\n";
      out << indent << "  const size_t len_pos = " << buf_var << ".size();\n";
      out << indent << "  " << buf_var << ".push_back(0);\n";
      out << indent << "  " << buf_var << ".push_back(0);\n";
      out << indent << "  " << buf_var << ".push_back(::omnibuf::WireSubMsg);\n";
      out << indent << "  ::omnibuf::append_varint(" << buf_var << ", " << member << ".size());\n";
      out << indent << "  for (const auto& elem : " << member << ") {\n";
      out << indent << "    const size_t elem_pos = " << buf_var << ".size();\n";
      out << indent << "    " << buf_var << ".push_back(0);\n";
      out << indent << "    " << buf_var << ".push_back(0);\n";
      out << indent << "    elem.marshal_omnibuf_append(" << buf_var << ");\n";
      out << indent << "    ::omnibuf::backpatch_container_len(" << buf_var << ", elem_pos);\n";
      out << indent << "  }\n";
      out << indent << "  ::omnibuf::backpatch_container_len(" << buf_var << ", len_pos);\n";
      out << indent << "}\n";
      return;
    }
    out << indent << "if (" << member << ".has_value()) {\n";
    out << indent << "  ::omnibuf::append_tag(" << buf_var << ", " << fnum
        << ", ::omnibuf::WireSubMsg);\n";
    out << indent << "  const size_t len_pos = " << buf_var << ".size();\n";
    out << indent << "  " << buf_var << ".push_back(0);\n";
    out << indent << "  " << buf_var << ".push_back(0);\n";
    out << indent << "  " << member << "->marshal_omnibuf_append(" << buf_var << ");\n";
    out << indent << "  ::omnibuf::backpatch_container_len(" << buf_var << ", len_pos);\n";
    out << indent << "}\n";
    return;
  }

  if (is_repeated) {
    out << indent << "::omnibuf::append_tag(" << buf_var << ", " << fnum
        << ", ::omnibuf::WireArray);\n";
    switch (f.type) {
      case FieldType::String:
        out << indent << "::omnibuf::encode_string_arr(" << buf_var << ", " << member << ");\n";
        break;
      case FieldType::Bytes:
        out << indent << "::omnibuf::encode_bytes_arr(" << buf_var << ", " << member << ");\n";
        break;
      case FieldType::Bool:
        out << indent << "::omnibuf::encode_bool_arr(" << buf_var << ", " << member << ");\n";
        break;
      case FieldType::Uint32:
      case FieldType::Uint64:
        out << indent << "::omnibuf::encode_uint_arr(" << buf_var << ", std::span<const "
            << scalar_cpp_type(f) << ">(" << member << "));\n";
        break;
      case FieldType::Int32:
      case FieldType::Sint32:
      case FieldType::Int64:
      case FieldType::Sint64:
        out << indent << "::omnibuf::encode_sint_arr(" << buf_var << ", std::span<const "
            << scalar_cpp_type(f) << ">(" << member << "));\n";
        break;
      case FieldType::Fixed32:
        out << indent << "::omnibuf::encode_fixed32_arr(" << buf_var
            << ", std::span<const uint32_t>(" << member << "), ::omnibuf::WireFixed32);\n";
        break;
      case FieldType::Sfixed32:
        out << indent << "::omnibuf::encode_fixed32_arr(" << buf_var
            << ", std::span<const int32_t>(" << member << "), ::omnibuf::WireSfixed32);\n";
        break;
      case FieldType::Fixed64:
        out << indent << "::omnibuf::encode_fixed64_arr(" << buf_var
            << ", std::span<const uint64_t>(" << member << "), ::omnibuf::WireFixed64);\n";
        break;
      case FieldType::Sfixed64:
        out << indent << "::omnibuf::encode_fixed64_arr(" << buf_var
            << ", std::span<const int64_t>(" << member << "), ::omnibuf::WireSfixed64);\n";
        break;
      case FieldType::Float:
        out << indent << "::omnibuf::encode_float32_arr(" << buf_var << ", " << member << ");\n";
        break;
      case FieldType::Double:
        out << indent << "::omnibuf::encode_float64_arr(" << buf_var << ", " << member << ");\n";
        break;
      case FieldType::Enum:
        out << indent << "{\n";
        out << indent << "  const size_t len_pos = " << buf_var << ".size();\n";
        out << indent << "  " << buf_var << ".push_back(0);\n";
        out << indent << "  " << buf_var << ".push_back(0);\n";
        out << indent << "  " << buf_var << ".push_back(::omnibuf::WireSint);\n";
        out << indent << "  ::omnibuf::append_varint(" << buf_var << ", " << member << ".size());\n";
        out << indent << "  for (auto ev : " << member << ") {\n";
        out << indent << "    ::omnibuf::append_varint(" << buf_var
            << ", ::omnibuf::encode_zigzag(static_cast<int64_t>(static_cast<int32_t>(ev))));\n";
        out << indent << "  }\n";
        out << indent << "  ::omnibuf::backpatch_container_len(" << buf_var << ", len_pos);\n";
        out << indent << "}\n";
        break;
      default:
        break;
    }
    return;
  }

  switch (f.type) {
    case FieldType::Uint32:
    case FieldType::Uint64:
      out << indent << "::omnibuf::append_tag(" << buf_var << ", " << fnum
          << ", ::omnibuf::WireUint);\n";
      out << indent << "::omnibuf::append_varint(" << buf_var << ", static_cast<uint64_t>("
          << member << "));\n";
      break;
    case FieldType::Int32:
    case FieldType::Sint32:
    case FieldType::Int64:
    case FieldType::Sint64:
      out << indent << "::omnibuf::append_tag(" << buf_var << ", " << fnum
          << ", ::omnibuf::WireSint);\n";
      out << indent << "::omnibuf::append_varint(" << buf_var
          << ", ::omnibuf::encode_zigzag(static_cast<int64_t>(" << member << ")));\n";
      break;
    case FieldType::Enum:
      out << indent << "::omnibuf::append_tag(" << buf_var << ", " << fnum
          << ", ::omnibuf::WireSint);\n";
      out << indent << "::omnibuf::append_varint(" << buf_var
          << ", ::omnibuf::encode_zigzag(static_cast<int64_t>(static_cast<int32_t>(" << member
          << "))));\n";
      break;
    case FieldType::Bool:
      out << indent << "::omnibuf::append_tag(" << buf_var << ", " << fnum << ", " << member
          << " ? ::omnibuf::WireBoolTrue : ::omnibuf::WireBoolFalse);\n";
      break;
    case FieldType::Fixed32:
      out << indent << "::omnibuf::append_tag(" << buf_var << ", " << fnum
          << ", ::omnibuf::WireFixed32);\n";
      out << indent << "::omnibuf::append_u32_le(" << buf_var << ", " << member << ");\n";
      break;
    case FieldType::Sfixed32:
      out << indent << "::omnibuf::append_tag(" << buf_var << ", " << fnum
          << ", ::omnibuf::WireSfixed32);\n";
      out << indent << "::omnibuf::append_u32_le(" << buf_var << ", static_cast<uint32_t>("
          << member << "));\n";
      break;
    case FieldType::Float:
      out << indent << "::omnibuf::append_tag(" << buf_var << ", " << fnum
          << ", ::omnibuf::WireFloat32);\n";
      out << indent << "::omnibuf::append_u32_le(" << buf_var << ", ::omnibuf::float32_to_bits("
          << member << "));\n";
      break;
    case FieldType::Fixed64:
      out << indent << "::omnibuf::append_tag(" << buf_var << ", " << fnum
          << ", ::omnibuf::WireFixed64);\n";
      out << indent << "::omnibuf::append_u64_le(" << buf_var << ", " << member << ");\n";
      break;
    case FieldType::Sfixed64:
      out << indent << "::omnibuf::append_tag(" << buf_var << ", " << fnum
          << ", ::omnibuf::WireSfixed64);\n";
      out << indent << "::omnibuf::append_u64_le(" << buf_var << ", static_cast<uint64_t>("
          << member << "));\n";
      break;
    case FieldType::Double:
      out << indent << "::omnibuf::append_tag(" << buf_var << ", " << fnum
          << ", ::omnibuf::WireFloat64);\n";
      out << indent << "::omnibuf::append_u64_le(" << buf_var << ", ::omnibuf::float64_to_bits("
          << member << "));\n";
      break;
    case FieldType::String:
      out << indent << "::omnibuf::append_tag(" << buf_var << ", " << fnum
          << ", ::omnibuf::WireString);\n";
      out << indent << "::omnibuf::append_varint(" << buf_var << ", " << member << ".size());\n";
      out << indent << buf_var << ".insert(" << buf_var << ".end(), " << member << ".begin(), "
          << member << ".end());\n";
      break;
    case FieldType::Bytes:
      out << indent << "::omnibuf::append_tag(" << buf_var << ", " << fnum
          << ", ::omnibuf::WireBytes);\n";
      out << indent << "::omnibuf::append_varint(" << buf_var << ", " << member << ".size());\n";
      out << indent << buf_var << ".insert(" << buf_var << ".end(), " << member << ".begin(), "
          << member << ".end());\n";
      break;
    default:
      break;
  }
}

void emit_message_struct(std::ostringstream& out,
                         const DescriptorProto& msg,
                         const TypeCatalog& catalog) {
  out << "struct " << msg.name << " {\n";

  // Field members
  for (const auto& f : msg.fields) {
    const std::string ident = sanitize_ident(f.name);
    const bool is_repeated = (f.label == FieldLabel::Repeated);
    if (f.type == FieldType::Message) {
      const std::string target = trim_leading_dot(f.type_name);
      if (catalog.map_entries.contains(target)) {
        out << "  std::map<std::string, uint64_t> " << ident << "{};\n";
      } else if (is_repeated) {
        out << "  std::vector<" << qualified_cpp_type(f.type_name) << "> " << ident << "{};\n";
      } else {
        out << "  std::optional<" << qualified_cpp_type(f.type_name) << "> " << ident << "{};\n";
      }
    } else if (is_repeated) {
      out << "  std::vector<" << scalar_cpp_type(f) << "> " << ident << "{};\n";
    } else if (f.type == FieldType::String || f.type == FieldType::Bytes) {
      out << "  " << scalar_cpp_type(f) << " " << ident << "{};\n";
    } else {
      out << "  " << scalar_cpp_type(f) << " " << ident << " = " << scalar_default_literal(f)
          << ";\n";
    }
  }
  out << "\n";
  out << "  bool operator==(const " << msg.name << "&) const = default;\n\n";

  // Reset helper
  out << "  void clear() {\n";
  for (const auto& f : msg.fields) {
    const std::string ident = sanitize_ident(f.name);
    const bool is_repeated = (f.label == FieldLabel::Repeated);
    if (f.type == FieldType::Message) {
      const std::string target = trim_leading_dot(f.type_name);
      if (catalog.map_entries.contains(target) || is_repeated) {
        out << "    " << ident << ".clear();\n";
      } else {
        out << "    " << ident << ".reset();\n";
      }
    } else if (is_repeated || f.type == FieldType::String || f.type == FieldType::Bytes) {
      out << "    " << ident << ".clear();\n";
    } else {
      out << "    " << ident << " = " << scalar_default_literal(f) << ";\n";
    }
  }
  out << "  }\n\n";

  // SchemaMeta builder for SelfDescribing Field #0
  out << "  [[nodiscard]] static std::shared_ptr<::omnibuf::SchemaMeta> schema_meta(int depth = 0) {\n";
  out << "    if (depth > ::omnibuf::MAX_CONTAINER_DEPTH) return nullptr;\n";
  out << "    auto meta = std::make_shared<::omnibuf::SchemaMeta>();\n";
  out << "    meta->type_name = \"" << msg.name << "\";\n";
  for (const auto& f : msg.fields) {
    out << "    meta->fields[" << f.number << "u] = \"" << f.name << "\";\n";
    if (f.type == FieldType::Message) {
      const std::string target = trim_leading_dot(f.type_name);
      auto map_it = catalog.map_entries.find(target);
      if (map_it != catalog.map_entries.end()) {
        if (map_it->second.val_type == FieldType::Message) {
          out << "    if (auto c = " << qualified_cpp_type(map_it->second.val_type_name)
              << "::schema_meta(depth + 2)) meta->children[" << f.number
              << "u] = std::move(c);\n";
        }
      } else if (f.label == FieldLabel::Repeated) {
        out << "    if (auto c = " << qualified_cpp_type(f.type_name)
            << "::schema_meta(depth + 2)) meta->children[" << f.number << "u] = std::move(c);\n";
      } else {
        out << "    if (auto c = " << qualified_cpp_type(f.type_name)
            << "::schema_meta(depth + 1)) meta->children[" << f.number << "u] = std::move(c);\n";
      }
    }
  }
  out << "    return meta;\n";
  out << "  }\n\n";

  // marshal_omnibuf_append & marshal_omnibuf
  out << "  void marshal_omnibuf_append(std::vector<uint8_t>& dst) const {\n";
  for (const auto& f : msg.fields) {
    emit_encode_single_field(out, f, catalog, "", "dst", "    ");
  }
  out << "  }\n\n";

  out << "  [[nodiscard]] std::vector<uint8_t> marshal_omnibuf(::omnibuf::MarshalOptions opts = {}) const {\n";
  out << "    std::vector<uint8_t> dst;\n";
  out << "    dst.reserve(512);\n";
  out << "    if (opts.self_describing) {\n";
  out << "      if (auto meta = schema_meta(0)) {\n";
  out << "        ::omnibuf::append_field0_schema_meta(dst, *meta);\n";
  out << "      }\n";
  out << "    }\n";
  out << "    marshal_omnibuf_append(dst);\n";
  out << "    return dst;\n";
  out << "  }\n\n";

  // unmarshal_omnibuf & decode_omnibuf
  out << "  [[nodiscard]] ::omnibuf::Result<void> unmarshal_omnibuf(std::span<const uint8_t> data) {\n";
  out << "    return decode_omnibuf(data, 0, false, false);\n";
  out << "  }\n\n";

  out << "  [[nodiscard]] ::omnibuf::Result<void> decode_omnibuf(std::span<const uint8_t> data,\n";
  out << "                                                        int depth,\n";
  out << "                                                        bool merge,\n";
  out << "                                                        bool is_fresh = false) {\n";
  out << "    if (depth > ::omnibuf::MAX_CONTAINER_DEPTH) {\n";
  out << "      return ::omnibuf::Error{\"omnibuf: max nesting depth exceeded\"};\n";
  out << "    }\n";
  out << "    if (!merge && !is_fresh) {\n";
  out << "      clear();\n";
  out << "    }\n";
  out << "    uint64_t seen_mask = 0;\n";
  out << "    (void)seen_mask;\n";
  out << "    const size_t n = data.size();\n";
  out << "    size_t pos = 0;\n";
  out << "    while (pos < n) {\n";
  out << "      uint64_t tag = 0;\n";
  out << "      const uint8_t b0 = data[pos];\n";
  out << "      if ((b0 & 1u) != 0) {\n";
  out << "        tag = static_cast<uint64_t>(b0 >> 1);\n";
  out << "        ++pos;\n";
  out << "      } else if ((b0 & 2u) != 0 && pos + 2 <= n) {\n";
  out << "        tag = static_cast<uint64_t>(::omnibuf::read_u16_le(data.data() + pos) >> 2);\n";
  out << "        pos += 2;\n";
  out << "      } else {\n";
  out << "        const auto [t64, tn] = ::omnibuf::decode_varint(data.subspan(pos));\n";
  out << "        if (tn == 0 || (t64 >> 4) > ::omnibuf::MAX_FIELD_NUMBER) {\n";
  out << "          return ::omnibuf::Error{\"omnibuf: invalid tag\"};\n";
  out << "        }\n";
  out << "        tag = t64;\n";
  out << "        pos += tn;\n";
  out << "      }\n";
  out << "      switch (tag) {\n";
  out << "        case ::omnibuf::WireSubMsg: {\n";
  out << "          auto f0 = ::omnibuf::validate_field0(data, pos, ::omnibuf::WireSubMsg, depth);\n";
  out << "          if (!f0) return ::omnibuf::Error{f0.error()};\n";
  out << "          pos = f0.value();\n";
  out << "          break;\n";
  out << "        }\n";

  for (size_t idx = 0; idx < msg.fields.size(); ++idx) {
    const auto& f = msg.fields[idx];
    const std::string ident = sanitize_ident(f.name);
    const uint64_t fnum = f.number;
    const bool is_repeated = (f.label == FieldLabel::Repeated);
    const uint64_t bit = (idx < 64) ? (uint64_t{1} << idx) : 0;

    if (f.type == FieldType::Message) {
      const std::string target = trim_leading_dot(f.type_name);
      if (catalog.map_entries.contains(target)) {
        const uint64_t case_tag = (fnum << 4) | omnibuf::WireMap;
        out << "        case " << case_tag << "ULL: {\n";
        out << "          if (depth + 1 > ::omnibuf::MAX_CONTAINER_DEPTH) {\n";
        out << "            return ::omnibuf::Error{\"omnibuf: max nesting depth exceeded\"};\n";
        out << "          }\n";
        out << "          auto mr = ::omnibuf::read_len_slice(data, pos, 3);\n";
        out << "          if (!mr) return ::omnibuf::Error{mr.error()};\n";
        out << "          if (!::omnibuf::decode_map_string_uint_body(" << ident
            << ", mr.value().slice)) {\n";
        out << "            return ::omnibuf::Error{\"omnibuf: invalid map body\"};\n";
        out << "          }\n";
        out << "          pos = mr.value().next_pos;\n";
        out << "          break;\n";
        out << "        }\n";
        continue;
      }
      if (is_repeated) {
        const uint64_t case_tag = (fnum << 4) | omnibuf::WireArray;
        out << "        case " << case_tag << "ULL: {\n";
        out << "          if (depth + 1 > ::omnibuf::MAX_CONTAINER_DEPTH) {\n";
        out << "            return ::omnibuf::Error{\"omnibuf: max nesting depth exceeded\"};\n";
        out << "          }\n";
        out << "          auto ar = ::omnibuf::read_len_slice(data, pos, 2);\n";
        out << "          if (!ar) return ::omnibuf::Error{ar.error()};\n";
        out << "          const auto body = ar.value().slice;\n";
        out << "          const auto hdr = ::omnibuf::parse_array_header(body);\n";
        out << "          if (!hdr.ok || hdr.elem_wt != ::omnibuf::WireSubMsg) {\n";
        out << "            return ::omnibuf::Error{\"omnibuf: invalid submsg array header\"};\n";
        out << "          }\n";
        out << "          " << ident << ".reserve(::omnibuf::grow_capacity(" << ident
            << ".capacity(), " << ident << ".size() + hdr.count));\n";
        out << "          size_t p = hdr.pos;\n";
        out << "          for (size_t i = 0; i < hdr.count; ++i) {\n";
        out << "            auto es = ::omnibuf::read_len_slice(body, p, 0);\n";
        out << "            if (!es) return ::omnibuf::Error{es.error()};\n";
        out << "            " << qualified_cpp_type(f.type_name) << " elem{};\n";
        out << "            auto dr = elem.decode_omnibuf(es.value().slice, depth + 2, false, true);\n";
        out << "            if (!dr) return dr;\n";
        out << "            " << ident << ".push_back(std::move(elem));\n";
        out << "            p = es.value().next_pos;\n";
        out << "          }\n";
        out << "          if (p != body.size()) {\n";
        out << "            return ::omnibuf::Error{\"omnibuf: trailing bytes in submsg array\"};\n";
        out << "          }\n";
        out << "          pos = ar.value().next_pos;\n";
        out << "          break;\n";
        out << "        }\n";
        continue;
      }
      const uint64_t case_tag = (fnum << 4) | omnibuf::WireSubMsg;
      out << "        case " << case_tag << "ULL: {\n";
      out << "          auto sr = ::omnibuf::read_len_slice(data, pos, 0);\n";
      out << "          if (!sr) return ::omnibuf::Error{sr.error()};\n";
      out << "          const bool merge_sub = merge || ((seen_mask & " << bit << "ULL) != 0);\n";
      out << "          bool fresh_sub = false;\n";
      out << "          if (!" << ident << ".has_value() || !merge_sub) {\n";
      out << "            " << ident << ".emplace();\n";
      out << "            fresh_sub = true;\n";
      out << "          }\n";
      if (bit != 0) {
        out << "          seen_mask |= " << bit << "ULL;\n";
      }
      out << "          auto dr = " << ident
          << "->decode_omnibuf(sr.value().slice, depth + 1, merge_sub, fresh_sub);\n";
      out << "          if (!dr) return dr;\n";
      out << "          pos = sr.value().next_pos;\n";
      out << "          break;\n";
      out << "        }\n";
      continue;
    }

    if (is_repeated) {
      const uint64_t case_tag = (fnum << 4) | omnibuf::WireArray;
      out << "        case " << case_tag << "ULL: {\n";
      out << "          if (depth + 1 > ::omnibuf::MAX_CONTAINER_DEPTH) {\n";
      out << "            return ::omnibuf::Error{\"omnibuf: max nesting depth exceeded\"};\n";
      out << "          }\n";
      out << "          auto ar = ::omnibuf::read_len_slice(data, pos, 2);\n";
      out << "          if (!ar) return ::omnibuf::Error{ar.error()};\n";
      switch (f.type) {
        case FieldType::String:
          out << "          if (!::omnibuf::append_string_arr_body(" << ident
              << ", ar.value().slice)) return ::omnibuf::Error{\"omnibuf: invalid string array\"};\n";
          break;
        case FieldType::Bytes:
          out << "          if (!::omnibuf::append_bytes_arr_body(" << ident
              << ", ar.value().slice)) return ::omnibuf::Error{\"omnibuf: invalid bytes array\"};\n";
          break;
        case FieldType::Bool:
          out << "          if (!::omnibuf::append_bool_arr_body(" << ident
              << ", ar.value().slice)) return ::omnibuf::Error{\"omnibuf: invalid bool array\"};\n";
          break;
        case FieldType::Uint32:
        case FieldType::Uint64:
          out << "          if (!::omnibuf::append_uint_arr_body(" << ident
              << ", ar.value().slice)) return ::omnibuf::Error{\"omnibuf: invalid uint array\"};\n";
          break;
        case FieldType::Int32:
        case FieldType::Sint32:
        case FieldType::Int64:
        case FieldType::Sint64:
          out << "          if (!::omnibuf::append_sint_arr_body(" << ident
              << ", ar.value().slice)) return ::omnibuf::Error{\"omnibuf: invalid sint array\"};\n";
          break;
        case FieldType::Fixed32:
          out << "          if (!::omnibuf::append_fixed32_arr_body(" << ident
              << ", ar.value().slice, ::omnibuf::WireFixed32)) return ::omnibuf::Error{\"omnibuf: invalid fixed32 array\"};\n";
          break;
        case FieldType::Sfixed32:
          out << "          if (!::omnibuf::append_fixed32_arr_body(" << ident
              << ", ar.value().slice, ::omnibuf::WireSfixed32)) return ::omnibuf::Error{\"omnibuf: invalid sfixed32 array\"};\n";
          break;
        case FieldType::Float:
          out << "          if (!::omnibuf::append_fixed32_arr_body(" << ident
              << ", ar.value().slice, ::omnibuf::WireFloat32)) return ::omnibuf::Error{\"omnibuf: invalid float32 array\"};\n";
          break;
        case FieldType::Fixed64:
          out << "          if (!::omnibuf::append_fixed64_arr_body(" << ident
              << ", ar.value().slice, ::omnibuf::WireFixed64)) return ::omnibuf::Error{\"omnibuf: invalid fixed64 array\"};\n";
          break;
        case FieldType::Sfixed64:
          out << "          if (!::omnibuf::append_fixed64_arr_body(" << ident
              << ", ar.value().slice, ::omnibuf::WireSfixed64)) return ::omnibuf::Error{\"omnibuf: invalid sfixed64 array\"};\n";
          break;
        case FieldType::Double:
          out << "          if (!::omnibuf::append_fixed64_arr_body(" << ident
              << ", ar.value().slice, ::omnibuf::WireFloat64)) return ::omnibuf::Error{\"omnibuf: invalid float64 array\"};\n";
          break;
        case FieldType::Enum:
          out << "          {\n";
          out << "            std::vector<int32_t> tmp;\n";
          out << "            if (!::omnibuf::append_sint_arr_body(tmp, ar.value().slice)) {\n";
          out << "              return ::omnibuf::Error{\"omnibuf: invalid enum array\"};\n";
          out << "            }\n";
          out << "            for (int32_t ev : tmp) {\n";
          out << "              " << ident << ".push_back(static_cast<" << scalar_cpp_type(f)
              << ">(ev));\n";
          out << "            }\n";
          out << "          }\n";
          break;
        default:
          break;
      }
      out << "          pos = ar.value().next_pos;\n";
      out << "          break;\n";
      out << "        }\n";
      continue;
    }

    if (f.type == FieldType::Bool) {
      const uint64_t tag_false = (fnum << 4) | omnibuf::WireBoolFalse;
      const uint64_t tag_true = (fnum << 4) | omnibuf::WireBoolTrue;
      out << "        case " << tag_false << "ULL:\n";
      out << "          " << ident << " = false;\n";
      out << "          break;\n";
      out << "        case " << tag_true << "ULL:\n";
      out << "          " << ident << " = true;\n";
      out << "          break;\n";
      continue;
    }

    const uint8_t wt = scalar_wire_const(f.type);
    const uint64_t case_tag = (fnum << 4) | wt;
    out << "        case " << case_tag << "ULL: {\n";
    switch (f.type) {
      case FieldType::Uint32:
      case FieldType::Uint64:
        out << "          const auto [v, vn] = ::omnibuf::decode_varint(data.subspan(pos));\n";
        out << "          if (vn == 0) return ::omnibuf::Error{\"omnibuf: truncated uint\"};\n";
        out << "          pos += vn;\n";
        out << "          " << ident << " = static_cast<" << scalar_cpp_type(f) << ">(v);\n";
        break;
      case FieldType::Int32:
      case FieldType::Sint32:
      case FieldType::Int64:
      case FieldType::Sint64:
      case FieldType::Enum:
        out << "          const auto [v, vn] = ::omnibuf::decode_varint(data.subspan(pos));\n";
        out << "          if (vn == 0) return ::omnibuf::Error{\"omnibuf: truncated sint\"};\n";
        out << "          pos += vn;\n";
        if (f.type == FieldType::Enum) {
          out << "          " << ident << " = static_cast<" << scalar_cpp_type(f)
              << ">(static_cast<int32_t>(::omnibuf::decode_zigzag(v)));\n";
        } else {
          out << "          " << ident << " = static_cast<" << scalar_cpp_type(f)
              << ">(::omnibuf::decode_zigzag(v));\n";
        }
        break;
      case FieldType::Fixed32:
      case FieldType::Sfixed32:
      case FieldType::Float:
        out << "          if (n - pos < 4) return ::omnibuf::Error{\"omnibuf: truncated 32-bit scalar\"};\n";
        out << "          const uint32_t bits = ::omnibuf::read_u32_le(data.data() + pos);\n";
        out << "          pos += 4;\n";
        if (f.type == FieldType::Float) {
          out << "          " << ident << " = ::omnibuf::bits_to_float32(bits);\n";
        } else {
          out << "          " << ident << " = static_cast<" << scalar_cpp_type(f) << ">(bits);\n";
        }
        break;
      case FieldType::Fixed64:
      case FieldType::Sfixed64:
      case FieldType::Double:
        out << "          if (n - pos < 8) return ::omnibuf::Error{\"omnibuf: truncated 64-bit scalar\"};\n";
        out << "          const uint64_t bits = ::omnibuf::read_u64_le(data.data() + pos);\n";
        out << "          pos += 8;\n";
        if (f.type == FieldType::Double) {
          out << "          " << ident << " = ::omnibuf::bits_to_float64(bits);\n";
        } else {
          out << "          " << ident << " = static_cast<" << scalar_cpp_type(f) << ">(bits);\n";
        }
        break;
      case FieldType::String:
        out << "          auto sr = ::omnibuf::read_str_slice(data, pos);\n";
        out << "          if (!sr) return ::omnibuf::Error{sr.error()};\n";
        out << "          const auto sp = sr.value().slice;\n";
        out << "          " << ident << ".assign(reinterpret_cast<const char*>(sp.data()), sp.size());\n";
        out << "          pos = sr.value().next_pos;\n";
        break;
      case FieldType::Bytes:
        out << "          auto br = ::omnibuf::read_len_slice(data, pos, 0);\n";
        out << "          if (!br) return ::omnibuf::Error{br.error()};\n";
        out << "          const auto sp = br.value().slice;\n";
        out << "          " << ident << ".assign(sp.begin(), sp.end());\n";
        out << "          pos = br.value().next_pos;\n";
        break;
      default:
        break;
    }
    out << "          break;\n";
    out << "        }\n";
  }

  out << "        default: {\n";
  out << "          const uint32_t fnum = static_cast<uint32_t>(tag >> 4);\n";
  out << "          const uint8_t wt = static_cast<uint8_t>(tag & 0x0Fu);\n";
  out << "          if (fnum == 0) {\n";
  out << "            return ::omnibuf::Error{\"omnibuf: invalid field number 0\"};\n";
  out << "          }\n";
  out << "          auto sk = ::omnibuf::validate_value(data, pos, wt, depth);\n";
  out << "          if (!sk) return ::omnibuf::Error{sk.error()};\n";
  out << "          pos = sk.value();\n";
  out << "          break;\n";
  out << "        }\n";
  out << "      }\n";
  out << "    }\n";
  out << "    return ::omnibuf::Result<void>::success();\n";
  out << "  }\n";
  out << "};\n\n";
}

void emit_opaque_class(std::ostringstream& out,
                       const DescriptorProto& msg,
                       const TypeCatalog& catalog) {
  const std::string cls = "Opaque" + msg.name;
  uint16_t max_fnum = 0;
  for (const auto& f : msg.fields) {
    if (f.number < 64 && f.number > max_fnum) {
      max_fnum = static_cast<uint16_t>(f.number);
    }
  }

  auto is_heap_dirty_field = [](const FieldDescriptorProto& f) noexcept {
    return f.label == FieldLabel::Repeated || f.type == FieldType::Message ||
           f.type == FieldType::String || f.type == FieldType::Bytes;
  };

  out << "class " << cls << " {\n";
  out << " public:\n";
  out << "  " << cls << "() = default;\n";
  out << "  explicit " << cls << "(std::span<const uint8_t> raw) noexcept : raw_view_(raw) {}\n";
  out << "  explicit " << cls << "(std::vector<uint8_t> owned)\n";
  out << "      : owned_raw_(std::make_unique<std::vector<uint8_t>>(std::move(owned))) {}\n";
  out << "  " << cls << "(const " << cls << "& other)\n";
  out << "      : raw_view_(other.raw_view_),\n";
  out << "        owned_raw_(other.owned_raw_ ? std::make_unique<std::vector<uint8_t>>(*other.owned_raw_)\n";
  out << "                                    : nullptr),\n";
  out << "        dirty_(other.dirty_),\n";
  out << "        dirty_scalars_(other.dirty_scalars_),\n";
  out << "        dirty_heap_(other.dirty_heap_ ? std::make_unique<DirtyHeap>(*other.dirty_heap_)\n";
  out << "                                      : nullptr) {}\n";
  out << "  " << cls << "& operator=(const " << cls << "& other) {\n";
  out << "    if (this != &other) {\n";
  out << "      *this = " << cls << "(other);\n";
  out << "    }\n";
  out << "    return *this;\n";
  out << "  }\n";
  out << "  " << cls << "(" << cls << "&&) noexcept = default;\n";
  out << "  " << cls << "& operator=(" << cls << "&&) noexcept = default;\n\n";

  out << "  void unmarshal(std::span<const uint8_t> raw) noexcept {\n";
  out << "    owned_raw_.reset();\n";
  out << "    raw_view_ = raw;\n";
  out << "    dirty_ = 0;\n";
  out << "  }\n\n";

  out << "  [[nodiscard]] std::span<const uint8_t> raw() const noexcept {\n";
  out << "    return owned_raw_ ? std::span<const uint8_t>(*owned_raw_) : raw_view_;\n";
  out << "  }\n\n";

  for (const auto& f : msg.fields) {
    if (f.number >= 64) continue;
    const std::string ident = sanitize_ident(f.name);
    const std::string dirty_var = "f_" + f.name + "_";
    const std::string heap_ref = "dirty_heap_->" + dirty_var;
    const std::string heap_mut = "ensure_dirty_heap()." + dirty_var;
    const std::string scalar_ref = "dirty_scalars_." + dirty_var;
    const uint32_t fnum = f.number;
    const std::string mask_expr = "(uint64_t{1} << " + std::to_string(fnum) + "u)";
    const bool is_repeated = (f.label == FieldLabel::Repeated);

    if (f.type == FieldType::Message) {
      const std::string target = trim_leading_dot(f.type_name);
      if (catalog.map_entries.contains(target)) {
        out << "  [[nodiscard]] std::pair<uint64_t, bool> lookup_" << f.name
            << "(std::string_view key) const {\n";
        out << "    if ((dirty_ & " << mask_expr << ") != 0) {\n";
        out << "      auto it = " << heap_ref << ".find(std::string(key));\n";
        out << "      if (it == " << heap_ref << ".end()) return {0, false};\n";
        out << "      return {it->second, true};\n";
        out << "    }\n";
        out << "    return ::omnibuf::lookup_map_string_uint(raw(), " << fnum << "u, key);\n";
        out << "  }\n";

        out << "  [[nodiscard]] std::map<std::string, uint64_t> get_" << f.name << "() const {\n";
        out << "    if ((dirty_ & " << mask_expr << ") != 0) return " << heap_ref << ";\n";
        out << "    return ::omnibuf::read_map_string_uint(raw(), " << fnum << "u).first;\n";
        out << "  }\n";

        out << "  void set_" << f.name << "(std::map<std::string, uint64_t> v) {\n";
        out << "    " << heap_mut << " = std::move(v);\n";
        out << "    dirty_ |= " << mask_expr << ";\n";
        out << "  }\n\n";
        continue;
      }

      const std::string sub_cpp = qualified_cpp_type(f.type_name);
      const std::string sub_opaque = qualified_opaque_cpp_type(f.type_name);

      if (is_repeated) {
        out << "  [[nodiscard]] size_t " << f.name << "_len() const noexcept {\n";
        out << "    if ((dirty_ & " << mask_expr << ") != 0) return " << heap_ref << ".size();\n";
        out << "    return ::omnibuf::submsg_arr_len(raw(), " << fnum << "u);\n";
        out << "  }\n";

        out << "  [[nodiscard]] " << sub_opaque << " get_" << f.name << "(size_t idx) const {\n";
        out << "    if ((dirty_ & " << mask_expr << ") != 0) {\n";
        out << "      if (idx < " << heap_ref << ".size()) {\n";
        out << "        return " << sub_opaque << "(" << heap_ref << "[idx].marshal_omnibuf());\n";
        out << "      }\n";
        out << "      return " << sub_opaque << "{};\n";
        out << "    }\n";
        out << "    const auto [elem, ok] = ::omnibuf::read_submsg_arr_elem(raw(), " << fnum
            << "u, idx);\n";
        out << "    return ok ? " << sub_opaque << "(elem) : " << sub_opaque << "{};\n";
        out << "  }\n";

        out << "  void set_" << f.name << "(std::vector<" << sub_cpp << "> v) {\n";
        out << "    " << heap_mut << " = std::move(v);\n";
        out << "    dirty_ |= " << mask_expr << ";\n";
        out << "  }\n\n";
        continue;
      }

      // Singular sub-message: zero-allocation fast path when a single WireSubMsg segment is present
      out << "  [[nodiscard]] " << sub_opaque << " get_" << f.name << "() const {\n";
      out << "    if ((dirty_ & " << mask_expr << ") != 0) {\n";
      out << "      if (" << heap_ref << ".has_value()) {\n";
      out << "        return " << sub_opaque << "(" << heap_ref << "->marshal_omnibuf());\n";
      out << "      }\n";
      out << "      return " << sub_opaque << "{};\n";
      out << "    }\n";
      out << "    const auto pkt = raw();\n";
      out << "    const auto first = ::omnibuf::next_container_field(pkt, 0, " << fnum
          << "u, ::omnibuf::WireSubMsg, 0);\n";
      out << "    if (!first.valid || !first.matched) return " << sub_opaque << "{};\n";
      out << "    const auto second = ::omnibuf::next_container_field(pkt, first.next_pos, "
          << fnum << "u, ::omnibuf::WireSubMsg, 0);\n";
      out << "    if (!second.valid) return " << sub_opaque << "{};\n";
      out << "    if (!second.matched) return " << sub_opaque << "(first.body);\n";
      out << "    auto [merged, ok] = ::omnibuf::read_submsg(pkt, " << fnum << "u);\n";
      out << "    return ok ? " << sub_opaque << "(std::move(merged)) : " << sub_opaque << "{};\n";
      out << "  }\n";

      out << "  void set_" << f.name << "(std::optional<" << sub_cpp << "> v) {\n";
      out << "    " << heap_mut << " = std::move(v);\n";
      out << "    dirty_ |= " << mask_expr << ";\n";
      out << "  }\n\n";
      continue;
    }

    if (is_repeated) {
      const uint8_t elem_wt = scalar_wire_const(f.type);
      out << "  [[nodiscard]] size_t " << f.name << "_len() const noexcept {\n";
      out << "    if ((dirty_ & " << mask_expr << ") != 0) return " << heap_ref << ".size();\n";
      out << "    return ::omnibuf::counted_arr_len(raw(), " << fnum << "u, "
          << static_cast<uint32_t>(elem_wt) << "u);\n";
      out << "  }\n";

      out << "  [[nodiscard]] std::vector<" << scalar_cpp_type(f) << "> get_" << f.name
          << "() const {\n";
      out << "    if ((dirty_ & " << mask_expr << ") != 0) return " << heap_ref << ";\n";
      out << "    std::vector<" << scalar_cpp_type(f) << "> dst;\n";
      switch (f.type) {
        case FieldType::String:
          out << "    (void)::omnibuf::read_arrays_into(dst, raw(), " << fnum
              << "u, ::omnibuf::append_string_arr_body);\n";
          break;
        case FieldType::Bytes:
          out << "    (void)::omnibuf::read_arrays_into(dst, raw(), " << fnum
              << "u, ::omnibuf::append_bytes_arr_body);\n";
          break;
        case FieldType::Bool:
          out << "    (void)::omnibuf::read_arrays_into(dst, raw(), " << fnum
              << "u, ::omnibuf::append_bool_arr_body);\n";
          break;
        case FieldType::Uint32:
        case FieldType::Uint64:
          out << "    (void)::omnibuf::read_arrays_into(dst, raw(), " << fnum
              << "u, [](auto& v, auto b) { return ::omnibuf::append_uint_arr_body(v, b); });\n";
          break;
        case FieldType::Int32:
        case FieldType::Sint32:
        case FieldType::Int64:
        case FieldType::Sint64:
          out << "    (void)::omnibuf::read_arrays_into(dst, raw(), " << fnum
              << "u, [](auto& v, auto b) { return ::omnibuf::append_sint_arr_body(v, b); });\n";
          break;
        case FieldType::Fixed32:
        case FieldType::Sfixed32:
        case FieldType::Float:
          out << "    (void)::omnibuf::read_arrays_into(dst, raw(), " << fnum
              << "u, [](auto& v, auto b) { return ::omnibuf::append_fixed32_arr_body(v, b, "
              << static_cast<uint32_t>(elem_wt) << "u); });\n";
          break;
        case FieldType::Fixed64:
        case FieldType::Sfixed64:
        case FieldType::Double:
          out << "    (void)::omnibuf::read_arrays_into(dst, raw(), " << fnum
              << "u, [](auto& v, auto b) { return ::omnibuf::append_fixed64_arr_body(v, b, "
              << static_cast<uint32_t>(elem_wt) << "u); });\n";
          break;
        default:
          break;
      }
      out << "    return dst;\n";
      out << "  }\n";

      out << "  void set_" << f.name << "(std::vector<" << scalar_cpp_type(f) << "> v) {\n";
      out << "    " << heap_mut << " = std::move(v);\n";
      out << "    dirty_ |= " << mask_expr << ";\n";
      out << "  }\n\n";
      continue;
    }

    const std::string cpp_t = scalar_cpp_type(f);
    if (f.type == FieldType::String) {
      out << "  [[nodiscard]] std::string_view get_" << f.name << "_view() const noexcept {\n";
      out << "    if ((dirty_ & " << mask_expr << ") != 0) return " << heap_ref << ";\n";
      out << "    return ::omnibuf::read_string_view(raw(), " << fnum << "u).first;\n";
      out << "  }\n";
      out << "  [[nodiscard]] std::string get_" << f.name << "() const {\n";
      out << "    return std::string(get_" << f.name << "_view());\n";
      out << "  }\n";
      out << "  void set_" << f.name << "(std::string v) {\n";
      out << "    " << heap_mut << " = std::move(v);\n";
      out << "    dirty_ |= " << mask_expr << ";\n";
      out << "  }\n\n";
      continue;
    }
    if (f.type == FieldType::Bytes) {
      out << "  [[nodiscard]] std::span<const uint8_t> get_" << f.name
          << "_view() const noexcept {\n";
      out << "    if ((dirty_ & " << mask_expr << ") != 0) return " << heap_ref << ";\n";
      out << "    return ::omnibuf::read_bytes_view(raw(), " << fnum << "u).first;\n";
      out << "  }\n";
      out << "  [[nodiscard]] std::vector<uint8_t> get_" << f.name << "() const {\n";
      out << "    const auto sp = get_" << f.name << "_view();\n";
      out << "    return std::vector<uint8_t>(sp.begin(), sp.end());\n";
      out << "  }\n";
      out << "  void set_" << f.name << "(std::vector<uint8_t> v) {\n";
      out << "    " << heap_mut << " = std::move(v);\n";
      out << "    dirty_ |= " << mask_expr << ";\n";
      out << "  }\n\n";
      continue;
    }

    out << "  [[nodiscard]] " << cpp_t << " get_" << f.name << "() const noexcept {\n";
    out << "    if ((dirty_ & " << mask_expr << ") != 0) return " << scalar_ref << ";\n";
    switch (f.type) {
      case FieldType::Uint32:
      case FieldType::Uint64:
        out << "    return static_cast<" << cpp_t << ">(::omnibuf::read_uint(raw(), " << fnum
            << "u).first);\n";
        break;
      case FieldType::Int32:
      case FieldType::Sint32:
      case FieldType::Int64:
      case FieldType::Sint64:
        out << "    return static_cast<" << cpp_t << ">(::omnibuf::read_sint(raw(), " << fnum
            << "u).first);\n";
        break;
      case FieldType::Enum:
        out << "    return static_cast<" << cpp_t
            << ">(static_cast<int32_t>(::omnibuf::read_sint(raw(), " << fnum << "u).first));\n";
        break;
      case FieldType::Bool:
        out << "    return ::omnibuf::read_bool(raw(), " << fnum << "u).first;\n";
        break;
      case FieldType::Fixed32:
        out << "    return ::omnibuf::read_fixed32(raw(), " << fnum << "u).first;\n";
        break;
      case FieldType::Sfixed32:
        out << "    return ::omnibuf::read_sfixed32(raw(), " << fnum << "u).first;\n";
        break;
      case FieldType::Float:
        out << "    return ::omnibuf::read_float32(raw(), " << fnum << "u).first;\n";
        break;
      case FieldType::Fixed64:
        out << "    return ::omnibuf::read_fixed64(raw(), " << fnum << "u).first;\n";
        break;
      case FieldType::Sfixed64:
        out << "    return ::omnibuf::read_sfixed64(raw(), " << fnum << "u).first;\n";
        break;
      case FieldType::Double:
        out << "    return ::omnibuf::read_float64(raw(), " << fnum << "u).first;\n";
        break;
      default:
        out << "    return " << scalar_default_literal(f) << ";\n";
        break;
    }
    out << "  }\n";

    out << "  void set_" << f.name << "(" << cpp_t << " v) noexcept {\n";
    out << "    " << scalar_ref << " = v;\n";
    out << "    dirty_ |= " << mask_expr << ";\n";
    out << "  }\n\n";
    (void)ident;
  }

  // marshal_append & marshal
  out << "  void marshal_into(std::vector<uint8_t>& dst) const {\n";
  out << "    dst.clear();\n";
  out << "    if (dirty_ == 0) {\n";
  out << "      ::omnibuf::copy_packet_append(dst, raw());\n";
  out << "      return;\n";
  out << "    }\n";
  out << "    ::omnibuf::splice_fields_into(dst, raw(), dirty_, " << max_fnum
      << "u, [this](std::vector<uint8_t>& b, uint16_t fnum) {\n";
  out << "      (void)this;\n";
  out << "      (void)b;\n";
  out << "      switch (fnum) {\n";
  for (const auto& f : msg.fields) {
    if (f.number >= 64) continue;
    out << "        case " << f.number << "u:\n";
    FieldDescriptorProto renamed = f;
    renamed.name = "f_" + f.name + "_";
    const std::string prefix =
        is_heap_dirty_field(f) ? "this->dirty_heap_->" : "this->dirty_scalars_.";
    emit_encode_single_field(out, renamed, catalog, prefix, "b", "          ");
    out << "          break;\n";
  }
  out << "        default:\n";
  out << "          break;\n";
  out << "      }\n";
  out << "    });\n";
  out << "  }\n\n";

  out << "  [[nodiscard]] std::vector<uint8_t> marshal_append(std::vector<uint8_t> dst = {}) const {\n";
  out << "    marshal_into(dst);\n";
  out << "    return dst;\n";
  out << "  }\n\n";

  out << "  [[nodiscard]] std::vector<uint8_t> marshal() const {\n";
  out << "    return marshal_append();\n";
  out << "  }\n\n";

  out << " private:\n";
  out << "  struct DirtyScalars {\n";
  for (const auto& f : msg.fields) {
    if (f.number >= 64 || is_heap_dirty_field(f)) continue;
    const std::string dirty_var = "f_" + f.name + "_";
    out << "    " << scalar_cpp_type(f) << " " << dirty_var << " = " << scalar_default_literal(f)
        << ";\n";
  }
  out << "  };\n\n";

  out << "  struct DirtyHeap {\n";
  for (const auto& f : msg.fields) {
    if (f.number >= 64 || !is_heap_dirty_field(f)) continue;
    const std::string dirty_var = "f_" + f.name + "_";
    const bool is_repeated = (f.label == FieldLabel::Repeated);
    if (f.type == FieldType::Message) {
      const std::string target = trim_leading_dot(f.type_name);
      if (catalog.map_entries.contains(target)) {
        out << "    std::map<std::string, uint64_t> " << dirty_var << "{};\n";
      } else if (is_repeated) {
        out << "    std::vector<" << qualified_cpp_type(f.type_name) << "> " << dirty_var << "{};\n";
      } else {
        out << "    std::optional<" << qualified_cpp_type(f.type_name) << "> " << dirty_var
            << "{};\n";
      }
    } else if (is_repeated) {
      out << "    std::vector<" << scalar_cpp_type(f) << "> " << dirty_var << "{};\n";
    } else {
      out << "    " << scalar_cpp_type(f) << " " << dirty_var << "{};\n";
    }
  }
  out << "  };\n\n";

  out << "  DirtyHeap& ensure_dirty_heap() {\n";
  out << "    if (!dirty_heap_) {\n";
  out << "      dirty_heap_ = std::make_unique<DirtyHeap>();\n";
  out << "    }\n";
  out << "    return *dirty_heap_;\n";
  out << "  }\n\n";

  out << "  std::span<const uint8_t> raw_view_{};\n";
  out << "  std::unique_ptr<std::vector<uint8_t>> owned_raw_{};\n";
  out << "  uint64_t dirty_ = 0;\n";
  out << "  DirtyScalars dirty_scalars_{};\n";
  out << "  std::unique_ptr<DirtyHeap> dirty_heap_{};\n";
  out << "};\n\n";
}

GeneratedFile generate_header_for_file(const FileDescriptorProto& file,
                                       const TypeCatalog& catalog) {
  std::ostringstream out;
  out << "// Code generated by protoc-gen-omnibuf-cpp. DO NOT EDIT.\n";
  out << "// source: " << file.name << "\n\n";
  out << "#pragma once\n\n";
  out << "#include <cstddef>\n";
  out << "#include <cstdint>\n";
  out << "#include <map>\n";
  out << "#include <memory>\n";
  out << "#include <optional>\n";
  out << "#include <span>\n";
  out << "#include <string>\n";
  out << "#include <string_view>\n";
  out << "#include <utility>\n";
  out << "#include <vector>\n\n";
  out << "#include \"omnibuf/decoder.hpp\"\n";
  out << "#include \"omnibuf/encoder.hpp\"\n";
  out << "#include \"omnibuf/field0.hpp\"\n";
  out << "#include \"omnibuf/opaque.hpp\"\n";
  out << "#include \"omnibuf/struct_codec.hpp\"\n";
  out << "#include \"omnibuf/varint.hpp\"\n";

  for (const auto& dep : file.dependencies) {
    if (dep.rfind("google/protobuf/", 0) == 0) continue;
    out << "#include \"" << header_output_path(dep) << "\"\n";
  }
  out << "\n";

  const std::string cpp_ns = proto_package_to_cpp_ns(file.package);
  if (!cpp_ns.empty()) {
    out << "namespace " << cpp_ns << " {\n\n";
  }

  for (const auto& e : file.enum_types) {
    emit_enum(out, e);
  }

  for (const auto& m : file.message_types) {
    if (m.map_entry) continue;
    out << "struct " << m.name << ";\n";
    out << "class Opaque" << m.name << ";\n";
  }
  out << "\n";

  for (const auto& m : file.message_types) {
    if (m.map_entry) continue;
    emit_message_struct(out, m, catalog);
    emit_opaque_class(out, m, catalog);
  }

  if (!cpp_ns.empty()) {
    out << "}  // namespace " << cpp_ns << "\n";
  }

  return GeneratedFile{
      .name = header_output_path(file.name),
      .content = out.str(),
  };
}

std::vector<uint8_t> read_all_stdin() {
  std::vector<uint8_t> buf;
  uint8_t chunk[16384];
  while (true) {
    const ssize_t n = ::read(STDIN_FILENO, chunk, sizeof(chunk));
    if (n <= 0) break;
    buf.insert(buf.end(), chunk, chunk + n);
  }
  return buf;
}

bool write_all_stdout(std::span<const uint8_t> data) {
  size_t written = 0;
  while (written < data.size()) {
    const ssize_t n = ::write(STDOUT_FILENO, data.data() + written, data.size() - written);
    if (n <= 0) return false;
    written += static_cast<size_t>(n);
  }
  return true;
}

}  // namespace

int main() {
  const std::vector<uint8_t> input = read_all_stdin();
  auto req_res = omnibuf::proto::parse_code_generator_request(input);
  if (!req_res) {
    const auto err_bytes =
        omnibuf::proto::encode_code_generator_response({}, req_res.error());
    (void)write_all_stdout(err_bytes);
    return 0;
  }

  const auto& req = req_res.value();
  const TypeCatalog catalog = build_catalog(req);
  const std::unordered_set<std::string> targets(req.files_to_generate.begin(),
                                                req.files_to_generate.end());

  std::vector<GeneratedFile> outputs;
  outputs.reserve(req.files_to_generate.size());
  for (const auto& file : req.proto_files) {
    if (!targets.contains(file.name)) continue;
    outputs.push_back(generate_header_for_file(file, catalog));
  }

  const auto resp_bytes = omnibuf::proto::encode_code_generator_response(outputs);
  if (!write_all_stdout(resp_bytes)) {
    return 1;
  }
  return 0;
}
