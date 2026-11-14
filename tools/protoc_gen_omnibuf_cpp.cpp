#include <unistd.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <sstream>
#include <stdexcept>
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

struct MessageRecord {
  std::string full_proto_name;
  std::string cpp_qualified;     // e.g. ::omnibuf::test::EdgeShowcase::InnerPayload
  std::string cpp_local_path;    // relative to file namespace, e.g. EdgeShowcase::InnerPayload
  std::string struct_ident;      // e.g. InnerPayload
  std::string opaque_qualified;  // e.g. ::omnibuf::test::OpaqueEdgeShowcase_InnerPayload
  std::string opaque_ident;      // e.g. OpaqueEdgeShowcase_InnerPayload
  const DescriptorProto* desc = nullptr;
  bool is_proto2 = false;
};

struct TypeCatalog {
  std::unordered_map<std::string, MapEntryInfo> map_entries;
  std::unordered_map<std::string, std::string> enum_cpp_names;
  std::unordered_map<std::string, MessageRecord> messages;
};

std::string trim_leading_dot(std::string_view s) {
  if (!s.empty() && s.front() == '.') {
    s.remove_prefix(1);
  }
  return std::string(s);
}

bool is_cpp_keyword(std::string_view name) {
  static const std::unordered_set<std::string_view> kKeywords = {
      "alignas",       "alignof",      "and",          "and_eq",     "asm",
      "auto",          "bitand",       "bitor",        "bool",       "break",
      "case",          "catch",        "char",         "char8_t",    "char16_t",
      "char32_t",      "class",        "compl",        "concept",    "const",
      "consteval",     "constexpr",    "constinit",    "const_cast", "continue",
      "co_await",      "co_return",    "co_yield",     "decltype",   "default",
      "delete",        "do",           "double",       "dynamic_cast", "else",
      "enum",          "explicit",     "export",       "extern",     "false",
      "float",         "for",          "friend",       "goto",       "if",
      "inline",        "int",          "long",         "mutable",    "namespace",
      "new",           "noexcept",     "not",          "not_eq",     "nullptr",
      "operator",      "or",           "or_eq",        "private",    "protected",
      "public",        "register",     "reinterpret_cast", "requires", "return",
      "short",         "signed",       "sizeof",       "static",     "static_assert",
      "static_cast",   "struct",       "switch",       "template",   "this",
      "thread_local",  "throw",        "true",         "try",        "typedef",
      "typeid",        "typename",     "union",        "unsigned",   "using",
      "virtual",       "void",         "volatile",     "wchar_t",    "while",
      "xor",           "xor_eq",       "NULL",         "stdin",      "stdout",
      "stderr",        "errno"};
  return kKeywords.contains(name);
}

std::string sanitize_basic_ident(std::string_view raw) {
  if (raw.empty()) return "_";
  std::string out;
  out.reserve(raw.size() + 2);
  for (char c : raw) {
    const unsigned char uc = static_cast<unsigned char>(c);
    if (std::isalnum(uc) || c == '_') {
      out.push_back(c);
    } else {
      out.push_back('_');
    }
  }
  if (std::isdigit(static_cast<unsigned char>(out.front()))) {
    out.insert(out.begin(), '_');
  }
  if (is_cpp_keyword(out)) {
    out.push_back('_');
  }
  return out;
}

std::string sanitize_field_ident(std::string_view raw,
                                 const std::unordered_set<std::string>& local_type_names) {
  static const std::unordered_set<std::string_view> kReservedStructMethods = {
      "clear",
      "schema_meta",
      "try_marshal_omnibuf_fast",
      "marshal_omnibuf_fast",
      "marshal_omnibuf_append",
      "try_marshal_omnibuf",
      "marshal_omnibuf",
      "unmarshal_omnibuf",
      "decode_omnibuf",
      "decode_omnibuf_raw",
  };
  std::string out = sanitize_basic_ident(raw);
  while (kReservedStructMethods.contains(out) || local_type_names.contains(out)) {
    out.push_back('_');
  }
  return out;
}

std::string to_pascal_case(std::string_view s) {
  std::string out;
  out.reserve(s.size() + 2);
  bool cap_next = true;
  for (char c : s) {
    const unsigned char uc = static_cast<unsigned char>(c);
    if (!std::isalnum(uc)) {
      cap_next = true;
      continue;
    }
    if (cap_next) {
      out.push_back(static_cast<char>(std::toupper(uc)));
      cap_next = false;
    } else {
      out.push_back(c);
    }
  }
  if (out.empty()) out = "Value";
  if (std::isdigit(static_cast<unsigned char>(out.front()))) {
    out.insert(out.begin(), 'F');
  }
  return out;
}

std::string proto_package_to_cpp_ns(std::string_view pkg) {
  if (pkg.empty()) return "";
  std::string out;
  size_t start = 0;
  while (start <= pkg.size()) {
    const size_t dot = pkg.find('.', start);
    const std::string_view seg =
        (dot == std::string_view::npos) ? pkg.substr(start) : pkg.substr(start, dot - start);
    if (!seg.empty()) {
      if (!out.empty()) out += "::";
      out += sanitize_basic_ident(seg);
    }
    if (dot == std::string_view::npos) break;
    start = dot + 1;
  }
  return out;
}

std::string header_output_path(std::string_view proto_path) {
  if (proto_path.size() >= 6 && proto_path.substr(proto_path.size() - 6) == ".proto") {
    proto_path.remove_suffix(6);
  }
  return std::string(proto_path) + ".omnibuf.pb.hpp";
}

bool is_valid_map_key_type(FieldType t) {
  switch (t) {
    case FieldType::Int32:
    case FieldType::Int64:
    case FieldType::Uint32:
    case FieldType::Uint64:
    case FieldType::Sint32:
    case FieldType::Sint64:
    case FieldType::Fixed32:
    case FieldType::Fixed64:
    case FieldType::Sfixed32:
    case FieldType::Sfixed64:
    case FieldType::Bool:
    case FieldType::String:
      return true;
    default:
      return false;
  }
}

bool is_supported_field_type(FieldType t) {
  switch (t) {
    case FieldType::Double:
    case FieldType::Float:
    case FieldType::Int64:
    case FieldType::Uint64:
    case FieldType::Int32:
    case FieldType::Fixed64:
    case FieldType::Fixed32:
    case FieldType::Bool:
    case FieldType::String:
    case FieldType::Message:
    case FieldType::Bytes:
    case FieldType::Uint32:
    case FieldType::Enum:
    case FieldType::Sfixed32:
    case FieldType::Sfixed64:
    case FieldType::Sint32:
    case FieldType::Sint64:
      return true;
    default:
      return false;
  }
}

std::optional<std::string> validate_message_tree(const std::string& prefix,
                                                 const DescriptorProto& d,
                                                 bool is_proto2) {
  const std::string full = prefix.empty() ? d.name : (prefix + "." + d.name);
  if (d.has_extensions) {
    return "omnibuf: proto2 extensions are not supported in message " + full;
  }
  if (d.map_entry) {
    const FieldDescriptorProto* key_f = nullptr;
    const FieldDescriptorProto* val_f = nullptr;
    for (const auto& f : d.fields) {
      if (f.number == 1) key_f = &f;
      if (f.number == 2) val_f = &f;
    }
    if (!key_f || !val_f || !key_f->has_type || !val_f->has_type) {
      return "omnibuf: malformed map_entry descriptor " + full;
    }
    if (!is_valid_map_key_type(key_f->type)) {
      return "omnibuf: invalid map key type in " + full;
    }
    if (val_f->type == FieldType::Group) {
      return "omnibuf: proto2 groups (TYPE_GROUP) are not supported in " + full;
    }
    if (!is_supported_field_type(val_f->type)) {
      return "omnibuf: unsupported map value type in " + full;
    }
    return std::nullopt;
  }

  for (const auto& f : d.fields) {
    const std::string ffull = full + "." + f.name;
    if (!f.extendee.empty()) {
      return "omnibuf: field extensions are not supported: " + ffull;
    }
    if (f.number == 0 || f.number > omnibuf::MAX_FIELD_NUMBER) {
      return "omnibuf: field number out of OmniBuf range [1..16777215]: " + ffull;
    }
    if (!f.has_type || f.type == FieldType::Unknown) {
      return "omnibuf: unknown or missing descriptor field type: " + ffull;
    }
    if (f.type == FieldType::Group) {
      return "omnibuf: proto2 groups (TYPE_GROUP) are not supported: " + ffull;
    }
    if (!is_supported_field_type(f.type)) {
      return "omnibuf: unsupported descriptor field type: " + ffull;
    }
    if (f.label == FieldLabel::Unknown) {
      return "omnibuf: unknown descriptor field label: " + ffull;
    }
    if (f.label == FieldLabel::Required) {
      return "omnibuf: proto2 required fields (LABEL_REQUIRED) are not supported: " + ffull;
    }
    if (is_proto2 && !f.default_value.empty()) {
      return "omnibuf: proto2 custom default values are not supported: " + ffull;
    }
  }
  for (const auto& nested : d.nested_types) {
    if (auto err = validate_message_tree(full, nested, is_proto2)) {
      return err;
    }
  }
  return std::nullopt;
}

std::optional<std::string> validate_target_file(const FileDescriptorProto& file) {
  if (file.has_extensions) {
    return "omnibuf: proto2 extensions are not supported in " + file.name;
  }
  const bool is_proto2 = file.syntax.empty() || file.syntax == "proto2";
  for (const auto& m : file.message_types) {
    if (auto err = validate_message_tree(file.package, m, is_proto2)) {
      return err;
    }
  }
  return std::nullopt;
}

void index_descriptor_tree(const std::string& proto_prefix,
                           const std::string& cpp_ns_prefix,
                           const std::string& local_cpp_prefix,
                           const std::string& opaque_chain_prefix,
                           const DescriptorProto& d,
                           bool is_proto2,
                           TypeCatalog& catalog) {
  const std::string full_proto =
      proto_prefix.empty() ? d.name : (proto_prefix + "." + d.name);
  if (d.map_entry) {
    MapEntryInfo info;
    for (const auto& f : d.fields) {
      if (f.number == 1) info.key_type = f.type;
      if (f.number == 2) {
        info.val_type = f.type;
        info.val_type_name = trim_leading_dot(f.type_name);
      }
    }
    catalog.map_entries[full_proto] = std::move(info);
    return;
  }

  const std::string s_name = sanitize_basic_ident(d.name);
  const std::string local_path =
      local_cpp_prefix.empty() ? s_name : (local_cpp_prefix + "::" + s_name);
  const std::string cpp_qual =
      cpp_ns_prefix.empty() ? ("::" + local_path) : ("::" + cpp_ns_prefix + "::" + local_path);
  const std::string opaque_chain =
      opaque_chain_prefix.empty() ? s_name : (opaque_chain_prefix + "_" + s_name);
  const std::string opaque_ident = "Opaque" + opaque_chain;
  const std::string opaque_qual =
      cpp_ns_prefix.empty() ? ("::" + opaque_ident) : ("::" + cpp_ns_prefix + "::" + opaque_ident);

  catalog.messages[full_proto] = MessageRecord{
      .full_proto_name = full_proto,
      .cpp_qualified = cpp_qual,
      .cpp_local_path = local_path,
      .struct_ident = s_name,
      .opaque_qualified = opaque_qual,
      .opaque_ident = opaque_ident,
      .desc = &d,
      .is_proto2 = is_proto2,
  };

  for (const auto& e : d.enum_types) {
    const std::string e_proto = full_proto + "." + e.name;
    const std::string e_cpp = cpp_qual + "::" + sanitize_basic_ident(e.name);
    catalog.enum_cpp_names[e_proto] = e_cpp;
  }
  for (const auto& nested : d.nested_types) {
    index_descriptor_tree(full_proto, cpp_ns_prefix, local_path, opaque_chain, nested, is_proto2,
                          catalog);
  }
}

TypeCatalog build_catalog(const CodeGeneratorRequest& req) {
  TypeCatalog catalog;
  for (const auto& file : req.proto_files) {
    const std::string cpp_ns = proto_package_to_cpp_ns(file.package);
    const bool is_proto2 = file.syntax.empty() || file.syntax == "proto2";
    for (const auto& e : file.enum_types) {
      const std::string e_proto =
          file.package.empty() ? e.name : (file.package + "." + e.name);
      const std::string e_ident = sanitize_basic_ident(e.name);
      catalog.enum_cpp_names[e_proto] =
          cpp_ns.empty() ? ("::" + e_ident) : ("::" + cpp_ns + "::" + e_ident);
    }
    for (const auto& m : file.message_types) {
      index_descriptor_tree(file.package, cpp_ns, "", "", m, is_proto2, catalog);
    }
  }
  return catalog;
}

std::string resolve_cpp_message(std::string_view type_name, const TypeCatalog& catalog) {
  const std::string clean = trim_leading_dot(type_name);
  auto it = catalog.messages.find(clean);
  if (it != catalog.messages.end()) return it->second.cpp_qualified;
  return "::" + proto_package_to_cpp_ns(clean);
}

std::string resolve_cpp_opaque(std::string_view type_name, const TypeCatalog& catalog) {
  const std::string clean = trim_leading_dot(type_name);
  auto it = catalog.messages.find(clean);
  if (it != catalog.messages.end()) return it->second.opaque_qualified;
  const size_t dot = clean.rfind('.');
  if (dot == std::string::npos) return "::Opaque" + sanitize_basic_ident(clean);
  return "::" + proto_package_to_cpp_ns(clean.substr(0, dot)) + "::Opaque" +
         sanitize_basic_ident(clean.substr(dot + 1));
}

std::string resolve_cpp_enum(std::string_view type_name, const TypeCatalog& catalog) {
  const std::string clean = trim_leading_dot(type_name);
  auto it = catalog.enum_cpp_names.find(clean);
  if (it != catalog.enum_cpp_names.end()) return it->second;
  return "::" + proto_package_to_cpp_ns(clean);
}

std::string scalar_cpp_type_for_kind(FieldType type,
                                     std::string_view type_name,
                                     const TypeCatalog& catalog) {
  switch (type) {
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
      return resolve_cpp_enum(type_name, catalog);
    case FieldType::Message:
      return resolve_cpp_message(type_name, catalog);
    default:
      return "void";
  }
}

std::string scalar_cpp_type(const FieldDescriptorProto& f, const TypeCatalog& catalog) {
  return scalar_cpp_type_for_kind(f.type, f.type_name, catalog);
}

std::string scalar_default_literal(const FieldDescriptorProto& f, const TypeCatalog& catalog) {
  switch (f.type) {
    case FieldType::Bool:
      return "false";
    case FieldType::Float:
      return "0.0f";
    case FieldType::Double:
      return "0.0";
    case FieldType::Enum:
      return "static_cast<" + resolve_cpp_enum(f.type_name, catalog) + ">(0)";
    case FieldType::String:
    case FieldType::Bytes:
      return "{}";
    default:
      return "0";
  }
}

std::string wire_const_expr(FieldType t) {
  switch (t) {
    case FieldType::Uint32:
    case FieldType::Uint64:
      return "::omnibuf::WireUint";
    case FieldType::Enum:
    case FieldType::Int32:
    case FieldType::Sint32:
    case FieldType::Int64:
    case FieldType::Sint64:
      return "::omnibuf::WireSint";
    case FieldType::Bool:
      return "::omnibuf::WireBoolFalse";
    case FieldType::Fixed32:
      return "::omnibuf::WireFixed32";
    case FieldType::Sfixed32:
      return "::omnibuf::WireSfixed32";
    case FieldType::Float:
      return "::omnibuf::WireFloat32";
    case FieldType::Fixed64:
      return "::omnibuf::WireFixed64";
    case FieldType::Sfixed64:
      return "::omnibuf::WireSfixed64";
    case FieldType::Double:
      return "::omnibuf::WireFloat64";
    case FieldType::String:
      return "::omnibuf::WireString";
    case FieldType::Bytes:
      return "::omnibuf::WireBytes";
    case FieldType::Message:
      return "::omnibuf::WireSubMsg";
    default:
      return "::omnibuf::WireUint";
  }
}

uint8_t scalar_wire_const(FieldType t) {
  auto res = omnibuf::proto::proto_type_to_wire_type(t);
  return res ? res.value() : omnibuf::WireUint;
}

bool is_real_oneof_field(const FieldDescriptorProto& f) noexcept {
  return !f.proto3_optional && f.oneof_index.has_value();
}

bool is_explicit_optional_scalar(const FieldDescriptorProto& f, bool is_proto2) noexcept {
  if (f.label == FieldLabel::Repeated || f.type == FieldType::Message) return false;
  if (is_real_oneof_field(f)) return false;
  return f.proto3_optional || (is_proto2 && f.label == FieldLabel::Optional);
}

std::string map_cpp_type(const MapEntryInfo& info, const TypeCatalog& catalog) {
  const std::string k_cpp = scalar_cpp_type_for_kind(info.key_type, "", catalog);
  const std::string v_cpp = scalar_cpp_type_for_kind(info.val_type, info.val_type_name, catalog);
  return "std::map<" + k_cpp + ", " + v_cpp + ">";
}

struct RealOneofInfo {
  uint32_t oneof_index = 0;
  std::string raw_name;
  std::string enum_name;       // e.g. ActionCase
  std::string member_var;      // e.g. oneof_action_case_
  std::string case_fn_name;    // e.g. action_case
  std::string clear_fn_name;   // e.g. clear_action
  std::vector<size_t> field_indices;
};

struct MessagePlan {
  const MessageRecord* rec = nullptr;
  std::unordered_set<std::string> local_type_names;
  std::vector<std::string> field_idents;
  std::vector<bool> field_uses_box;  // for singular submessages (including oneof message branches)
  std::vector<RealOneofInfo> real_oneofs;
  std::unordered_map<uint32_t, size_t> oneof_idx_to_plan;
};

void collect_proto_refs(const DescriptorProto& d,
                        const TypeCatalog& catalog,
                        std::unordered_set<std::string>& refs) {
  for (const auto& f : d.fields) {
    if (f.type != FieldType::Message && f.type != FieldType::Enum) continue;
    const std::string target = trim_leading_dot(f.type_name);
    auto map_it = catalog.map_entries.find(target);
    if (map_it != catalog.map_entries.end()) {
      if (map_it->second.val_type == FieldType::Message ||
          map_it->second.val_type == FieldType::Enum) {
        refs.insert(map_it->second.val_type_name);
      }
    } else {
      refs.insert(target);
    }
  }
  for (const auto& nested : d.nested_types) {
    if (!nested.map_entry) {
      collect_proto_refs(nested, catalog, refs);
    }
  }
}

std::vector<const DescriptorProto*> topo_sort_siblings(
    const std::string& scope_prefix,
    const std::vector<DescriptorProto>& siblings,
    const TypeCatalog& catalog) {
  std::vector<const DescriptorProto*> items;
  std::unordered_map<std::string, size_t> full_to_idx;
  for (const auto& m : siblings) {
    if (m.map_entry) continue;
    const std::string full = scope_prefix.empty() ? m.name : (scope_prefix + "." + m.name);
    full_to_idx[full] = items.size();
    items.push_back(&m);
  }
  const size_t n = items.size();
  if (n <= 1) return items;

  std::vector<std::vector<size_t>> adj(n);
  std::vector<size_t> indeg(n, 0);

  for (size_t i = 0; i < n; ++i) {
    std::unordered_set<std::string> refs;
    collect_proto_refs(*items[i], catalog, refs);
    std::unordered_set<size_t> deps;
    for (const auto& ref : refs) {
      for (const auto& [sib_full, sib_idx] : full_to_idx) {
        if (sib_idx == i) continue;
        if (ref == sib_full ||
            (ref.size() > sib_full.size() && ref.rfind(sib_full + ".", 0) == 0)) {
          deps.insert(sib_idx);
        }
      }
    }
    for (size_t dep_idx : deps) {
      adj[dep_idx].push_back(i);
      ++indeg[i];
    }
  }

  std::vector<const DescriptorProto*> ordered;
  ordered.reserve(n);
  std::vector<bool> placed(n, false);
  while (ordered.size() < n) {
    size_t pick = n;
    for (size_t i = 0; i < n; ++i) {
      if (!placed[i] && indeg[i] == 0) {
        pick = i;
        break;
      }
    }
    if (pick == n) {
      for (size_t i = 0; i < n; ++i) {
        if (!placed[i]) {
          pick = i;
          break;
        }
      }
    }
    placed[pick] = true;
    ordered.push_back(items[pick]);
    for (size_t nxt : adj[pick]) {
      if (indeg[nxt] > 0) --indeg[nxt];
    }
  }
  return ordered;
}

MessagePlan build_message_plan(const MessageRecord& rec,
                               const TypeCatalog& catalog,
                               const std::unordered_set<std::string>& defined_messages) {
  MessagePlan plan;
  plan.rec = &rec;
  const DescriptorProto& msg = *rec.desc;

  plan.local_type_names.insert(rec.struct_ident);
  for (const auto& e : msg.enum_types) {
    plan.local_type_names.insert(sanitize_basic_ident(e.name));
  }
  for (const auto& nested : msg.nested_types) {
    if (!nested.map_entry) {
      plan.local_type_names.insert(sanitize_basic_ident(nested.name));
    }
  }

  // First reserve oneof enum names so field idents never collide with them either.
  std::unordered_map<uint32_t, std::vector<size_t>> oneof_fields;
  for (size_t i = 0; i < msg.fields.size(); ++i) {
    const auto& f = msg.fields[i];
    if (is_real_oneof_field(f)) {
      oneof_fields[*f.oneof_index].push_back(i);
    }
  }

  for (uint32_t oi = 0; oi < msg.oneof_decls.size(); ++oi) {
    auto it = oneof_fields.find(oi);
    if (it == oneof_fields.end()) continue;
    RealOneofInfo info;
    info.oneof_index = oi;
    info.raw_name = msg.oneof_decls[oi].name;
    info.enum_name = to_pascal_case(info.raw_name) + "Case";
    while (plan.local_type_names.contains(info.enum_name)) {
      info.enum_name += "_";
    }
    plan.local_type_names.insert(info.enum_name);
    info.member_var = "oneof_" + sanitize_basic_ident(info.raw_name) + "_case_";
    info.case_fn_name = sanitize_basic_ident(info.raw_name) + "_case";
    info.clear_fn_name = "clear_" + sanitize_basic_ident(info.raw_name);
    info.field_indices = std::move(it->second);
    plan.oneof_idx_to_plan[oi] = plan.real_oneofs.size();
    plan.real_oneofs.push_back(std::move(info));
  }

  plan.field_idents.resize(msg.fields.size());
  plan.field_uses_box.assign(msg.fields.size(), false);
  std::unordered_set<std::string> used_members = plan.local_type_names;
  for (const auto& ro : plan.real_oneofs) {
    used_members.insert(ro.member_var);
    used_members.insert(ro.case_fn_name);
    used_members.insert(ro.clear_fn_name);
  }

  for (size_t i = 0; i < msg.fields.size(); ++i) {
    const auto& f = msg.fields[i];
    std::string ident = sanitize_field_ident(f.name, used_members);
    used_members.insert(ident);
    plan.field_idents[i] = std::move(ident);

    if (f.type == FieldType::Message && f.label != FieldLabel::Repeated) {
      const std::string target = trim_leading_dot(f.type_name);
      if (!catalog.map_entries.contains(target)) {
        // Use Box<SubT> whenever SubT is not yet completely defined (self or mutual recursion).
        if (!defined_messages.contains(target)) {
          plan.field_uses_box[i] = true;
        }
      }
    }
  }

  return plan;
}

void emit_enum(std::ostringstream& out, const EnumDescriptorProto& e, std::string_view indent) {
  out << indent << "enum class " << sanitize_basic_ident(e.name) << " : int32_t {\n";
  for (const auto& v : e.values) {
    out << indent << "  " << sanitize_basic_ident(v.name) << " = " << v.number << ",\n";
  }
  out << indent << "};\n\n";
}

void emit_try_encode_scalar_value_fast(std::ostringstream& out,
                                       const FieldDescriptorProto& f,
                                       std::string_view val_expr,
                                       std::string_view w_var,
                                       std::string_view indent) {
  const uint32_t fnum = f.number;
  switch (f.type) {
    case FieldType::Uint32:
    case FieldType::Uint64:
      out << indent << w_var << ".write_tag(" << fnum << "u, ::omnibuf::WireUint);\n";
      out << indent << w_var << ".write_varint(static_cast<uint64_t>(" << val_expr << "));\n";
      break;
    case FieldType::Int32:
    case FieldType::Sint32:
    case FieldType::Int64:
    case FieldType::Sint64:
      out << indent << w_var << ".write_tag(" << fnum << "u, ::omnibuf::WireSint);\n";
      out << indent << w_var << ".write_varint(::omnibuf::encode_zigzag(static_cast<int64_t>("
          << val_expr << ")));\n";
      break;
    case FieldType::Enum:
      out << indent << w_var << ".write_tag(" << fnum << "u, ::omnibuf::WireSint);\n";
      out << indent << w_var
          << ".write_varint(::omnibuf::encode_zigzag(static_cast<int64_t>(static_cast<int32_t>("
          << val_expr << "))));\n";
      break;
    case FieldType::Bool:
      out << indent << w_var << ".write_tag(" << fnum << "u, (" << val_expr
          << ") ? ::omnibuf::WireBoolTrue : ::omnibuf::WireBoolFalse);\n";
      break;
    case FieldType::Fixed32:
      out << indent << w_var << ".write_tag(" << fnum << "u, ::omnibuf::WireFixed32);\n";
      out << indent << w_var << ".write_u32_raw(" << val_expr << ");\n";
      break;
    case FieldType::Sfixed32:
      out << indent << w_var << ".write_tag(" << fnum << "u, ::omnibuf::WireSfixed32);\n";
      out << indent << w_var << ".write_u32_raw(static_cast<uint32_t>(" << val_expr << "));\n";
      break;
    case FieldType::Float:
      out << indent << w_var << ".write_tag(" << fnum << "u, ::omnibuf::WireFloat32);\n";
      out << indent << w_var << ".write_u32_raw(::omnibuf::float32_to_bits(" << val_expr << "));\n";
      break;
    case FieldType::Fixed64:
      out << indent << w_var << ".write_tag(" << fnum << "u, ::omnibuf::WireFixed64);\n";
      out << indent << w_var << ".write_u64_raw(" << val_expr << ");\n";
      break;
    case FieldType::Sfixed64:
      out << indent << w_var << ".write_tag(" << fnum << "u, ::omnibuf::WireSfixed64);\n";
      out << indent << w_var << ".write_u64_raw(static_cast<uint64_t>(" << val_expr << "));\n";
      break;
    case FieldType::Double:
      out << indent << w_var << ".write_tag(" << fnum << "u, ::omnibuf::WireFloat64);\n";
      out << indent << w_var << ".write_u64_raw(::omnibuf::float64_to_bits(" << val_expr << "));\n";
      break;
    case FieldType::String:
      out << indent << "if (!::omnibuf::is_valid_utf8(" << val_expr << ")) [[unlikely]] {\n";
      out << indent << "  return \"omnibuf: invalid UTF-8 in WireString\";\n";
      out << indent << "}\n";
      out << indent << w_var << ".write_tag(" << fnum << "u, ::omnibuf::WireString);\n";
      out << indent << w_var << ".write_varint((" << val_expr << ").size());\n";
      out << indent << w_var << ".write_bytes((" << val_expr << ").data(), (" << val_expr
          << ").size());\n";
      break;
    case FieldType::Bytes:
      out << indent << w_var << ".write_tag(" << fnum << "u, ::omnibuf::WireBytes);\n";
      out << indent << w_var << ".write_varint((" << val_expr << ").size());\n";
      out << indent << w_var << ".write_bytes((" << val_expr << ").data(), (" << val_expr
          << ").size());\n";
      break;
    default:
      break;
  }
}

void emit_try_encode_field_fast(std::ostringstream& out,
                                const FieldDescriptorProto& f,
                                std::string_view member_expr,
                                bool is_optional_scalar,
                                std::string_view oneof_guard_expr,
                                const TypeCatalog& catalog,
                                std::string_view w_var,
                                std::string_view indent) {
  const uint32_t fnum = f.number;
  const bool is_repeated = (f.label == FieldLabel::Repeated);

  if (!oneof_guard_expr.empty()) {
    out << indent << "if (" << oneof_guard_expr << ") {\n";
    const std::string inner_indent = std::string(indent) + "  ";
    if (f.type == FieldType::Message) {
      out << inner_indent << "if (" << member_expr << ".has_value()) {\n";
      out << inner_indent << "  " << w_var << ".write_tag(" << fnum
          << "u, ::omnibuf::WireSubMsg);\n";
      out << inner_indent << "  const size_t len_pos = " << w_var << ".reserve_len_2b();\n";
      out << inner_indent << "  if (const char* err = " << member_expr
          << "->try_marshal_omnibuf_fast(" << w_var << ")) [[unlikely]] return err;\n";
      out << inner_indent << "  " << w_var << ".backpatch_len(len_pos);\n";
      out << inner_indent << "}\n";
    } else {
      emit_try_encode_scalar_value_fast(out, f, member_expr, w_var, inner_indent);
    }
    out << indent << "}\n";
    return;
  }

  if (f.type == FieldType::Message) {
    const std::string target = trim_leading_dot(f.type_name);
    auto map_it = catalog.map_entries.find(target);
    if (map_it != catalog.map_entries.end()) {
      const auto& mi = map_it->second;
      out << indent << w_var << ".write_tag(" << fnum << "u, ::omnibuf::WireMap);\n";
      if (mi.val_type == FieldType::Message) {
        out << indent << "if (const char* err = ::omnibuf::try_encode_submsg_map<"
            << wire_const_expr(mi.key_type) << ">(" << w_var << ", " << member_expr
            << ", [](::omnibuf::FastWriter& sw, const auto& sv) {\n";
        out << indent << "  return sv.try_marshal_omnibuf_fast(sw);\n";
        out << indent << "})) [[unlikely]] return err;\n";
      } else {
        out << indent << "if (const char* err = ::omnibuf::try_encode_scalar_map<"
            << wire_const_expr(mi.key_type) << ", " << wire_const_expr(mi.val_type) << ">(" << w_var
            << ", " << member_expr << ")) [[unlikely]] return err;\n";
      }
      return;
    }
    if (is_repeated) {
      out << indent << "{\n";
      out << indent << "  " << w_var << ".write_tag(" << fnum << "u, ::omnibuf::WireArray);\n";
      out << indent << "  const size_t len_pos = " << w_var << ".reserve_len_2b();\n";
      out << indent << "  " << w_var << ".write_byte(::omnibuf::WireSubMsg);\n";
      out << indent << "  " << w_var << ".write_varint(" << member_expr << ".size());\n";
      out << indent << "  for (const auto& elem : " << member_expr << ") {\n";
      out << indent << "    const size_t elem_pos = " << w_var << ".reserve_len_2b();\n";
      out << indent
          << "    if (const char* err = elem.try_marshal_omnibuf_fast(" << w_var
          << ")) [[unlikely]] return err;\n";
      out << indent << "    " << w_var << ".backpatch_len(elem_pos);\n";
      out << indent << "  }\n";
      out << indent << "  " << w_var << ".backpatch_len(len_pos);\n";
      out << indent << "}\n";
      return;
    }
    out << indent << "if (" << member_expr << ".has_value()) {\n";
    out << indent << "  " << w_var << ".write_tag(" << fnum << "u, ::omnibuf::WireSubMsg);\n";
    out << indent << "  const size_t len_pos = " << w_var << ".reserve_len_2b();\n";
    out << indent << "  if (const char* err = " << member_expr << "->try_marshal_omnibuf_fast("
        << w_var << ")) [[unlikely]] return err;\n";
    out << indent << "  " << w_var << ".backpatch_len(len_pos);\n";
    out << indent << "}\n";
    return;
  }

  if (is_repeated) {
    out << indent << w_var << ".write_tag(" << fnum << "u, ::omnibuf::WireArray);\n";
    switch (f.type) {
      case FieldType::String:
        out << indent << "if (const char* err = ::omnibuf::try_encode_string_arr(" << w_var << ", "
            << member_expr << ")) [[unlikely]] return err;\n";
        break;
      case FieldType::Bytes:
        out << indent << "::omnibuf::encode_bytes_arr(" << w_var << ", " << member_expr << ");\n";
        break;
      case FieldType::Bool:
        out << indent << "::omnibuf::encode_bool_arr(" << w_var << ", " << member_expr << ");\n";
        break;
      case FieldType::Uint32:
      case FieldType::Uint64:
        out << indent << "::omnibuf::encode_uint_arr(" << w_var << ", std::span<const "
            << scalar_cpp_type(f, catalog) << ">(" << member_expr << "));\n";
        break;
      case FieldType::Int32:
      case FieldType::Sint32:
      case FieldType::Int64:
      case FieldType::Sint64:
        out << indent << "::omnibuf::encode_sint_arr(" << w_var << ", std::span<const "
            << scalar_cpp_type(f, catalog) << ">(" << member_expr << "));\n";
        break;
      case FieldType::Fixed32:
        out << indent << "::omnibuf::encode_fixed32_arr(" << w_var
            << ", std::span<const uint32_t>(" << member_expr << "), ::omnibuf::WireFixed32);\n";
        break;
      case FieldType::Sfixed32:
        out << indent << "::omnibuf::encode_fixed32_arr(" << w_var
            << ", std::span<const int32_t>(" << member_expr << "), ::omnibuf::WireSfixed32);\n";
        break;
      case FieldType::Fixed64:
        out << indent << "::omnibuf::encode_fixed64_arr(" << w_var
            << ", std::span<const uint64_t>(" << member_expr << "), ::omnibuf::WireFixed64);\n";
        break;
      case FieldType::Sfixed64:
        out << indent << "::omnibuf::encode_fixed64_arr(" << w_var
            << ", std::span<const int64_t>(" << member_expr << "), ::omnibuf::WireSfixed64);\n";
        break;
      case FieldType::Float:
        out << indent << "::omnibuf::encode_float32_arr(" << w_var << ", " << member_expr << ");\n";
        break;
      case FieldType::Double:
        out << indent << "::omnibuf::encode_float64_arr(" << w_var << ", " << member_expr << ");\n";
        break;
      case FieldType::Enum:
        out << indent << "{\n";
        out << indent << "  const size_t len_pos = " << w_var << ".reserve_len_2b();\n";
        out << indent << "  " << w_var << ".write_byte(::omnibuf::WireSint);\n";
        out << indent << "  " << w_var << ".write_varint(" << member_expr << ".size());\n";
        out << indent << "  for (auto ev : " << member_expr << ") {\n";
        out << indent << "    " << w_var
            << ".write_varint(::omnibuf::encode_zigzag(static_cast<int64_t>(static_cast<int32_t>(ev))));\n";
        out << indent << "  }\n";
        out << indent << "  " << w_var << ".backpatch_len(len_pos);\n";
        out << indent << "}\n";
        break;
      default:
        break;
    }
    return;
  }

  if (is_optional_scalar) {
    out << indent << "if (" << member_expr << ".has_value()) {\n";
    emit_try_encode_scalar_value_fast(out, f, "*" + std::string(member_expr), w_var,
                                      std::string(indent) + "  ");
    out << indent << "}\n";
    return;
  }

  emit_try_encode_scalar_value_fast(out, f, member_expr, w_var, indent);
}

void emit_clear_single_member(std::ostringstream& out,
                              const FieldDescriptorProto& f,
                              std::string_view ident,
                              bool is_optional_scalar,
                              const TypeCatalog& catalog,
                              std::string_view indent) {
  const bool is_repeated = (f.label == FieldLabel::Repeated);
  if (f.type == FieldType::Message) {
    const std::string target = trim_leading_dot(f.type_name);
    if (catalog.map_entries.contains(target) || is_repeated) {
      out << indent << ident << ".clear();\n";
    } else {
      out << indent << ident << ".reset();\n";
    }
  } else if (is_optional_scalar) {
    out << indent << ident << ".reset();\n";
  } else if (is_repeated || f.type == FieldType::String || f.type == FieldType::Bytes) {
    out << indent << ident << ".clear();\n";
  } else {
    out << indent << ident << " = " << scalar_default_literal(f, catalog) << ";\n";
  }
}

void emit_struct_declaration_recursive(std::ostringstream& out,
                                       const std::string& proto_prefix,
                                       const DescriptorProto& msg,
                                       const TypeCatalog& catalog,
                                       std::unordered_set<std::string>& defined_messages,
                                       std::vector<std::string>& pending_defined,
                                       std::vector<MessagePlan>& ordered_plans,
                                       std::string_view indent) {
  const std::string full_proto =
      proto_prefix.empty() ? msg.name : (proto_prefix + "." + msg.name);
  const MessageRecord& rec = catalog.messages.at(full_proto);

  out << indent << "struct " << rec.struct_ident << " {\n";
  const std::string inner = std::string(indent) + "  ";

  for (const auto& e : msg.enum_types) {
    emit_enum(out, e, inner);
  }

  const auto sorted_nested = topo_sort_siblings(full_proto, msg.nested_types, catalog);
  if (!sorted_nested.empty()) {
    for (const DescriptorProto* nm : sorted_nested) {
      out << inner << "struct " << sanitize_basic_ident(nm->name) << ";\n";
    }
    out << "\n";
    for (const DescriptorProto* nm : sorted_nested) {
      emit_struct_declaration_recursive(out, full_proto, *nm, catalog, defined_messages,
                                        pending_defined, ordered_plans, inner);
    }
  }

  MessagePlan plan = build_message_plan(rec, catalog, defined_messages);

  // Emit real oneof case enums
  for (const auto& ro : plan.real_oneofs) {
    out << inner << "enum class " << ro.enum_name << " : uint32_t {\n";
    out << inner << "  kNotSet = 0,\n";
    for (size_t fi : ro.field_indices) {
      out << inner << "  k" << to_pascal_case(msg.fields[fi].name) << " = "
          << msg.fields[fi].number << "u,\n";
    }
    out << inner << "};\n\n";
  }

  // Emit field members
  for (size_t i = 0; i < msg.fields.size(); ++i) {
    const auto& f = msg.fields[i];
    const std::string& ident = plan.field_idents[i];
    const bool is_repeated = (f.label == FieldLabel::Repeated);
    const bool is_opt_scalar = is_explicit_optional_scalar(f, rec.is_proto2);

    if (f.type == FieldType::Message) {
      const std::string target = trim_leading_dot(f.type_name);
      auto map_it = catalog.map_entries.find(target);
      if (map_it != catalog.map_entries.end()) {
        out << inner << map_cpp_type(map_it->second, catalog) << " " << ident << "{};\n";
      } else if (is_repeated) {
        out << inner << "std::vector<" << resolve_cpp_message(f.type_name, catalog) << "> " << ident
            << "{};\n";
      } else if (plan.field_uses_box[i]) {
        out << inner << "::omnibuf::Box<" << resolve_cpp_message(f.type_name, catalog) << "> "
            << ident << "{};\n";
      } else {
        out << inner << "std::optional<" << resolve_cpp_message(f.type_name, catalog) << "> "
            << ident << "{};\n";
      }
    } else if (is_repeated) {
      out << inner << "std::vector<" << scalar_cpp_type(f, catalog) << "> " << ident << "{};\n";
    } else if (is_opt_scalar) {
      out << inner << "std::optional<" << scalar_cpp_type(f, catalog) << "> " << ident << "{};\n";
    } else if (f.type == FieldType::String || f.type == FieldType::Bytes) {
      out << inner << scalar_cpp_type(f, catalog) << " " << ident << "{};\n";
    } else {
      out << inner << scalar_cpp_type(f, catalog) << " " << ident << " = "
          << scalar_default_literal(f, catalog) << ";\n";
    }
  }

  for (const auto& ro : plan.real_oneofs) {
    out << inner << ro.enum_name << " " << ro.member_var << " = " << ro.enum_name << "::kNotSet;\n";
  }
  if (!msg.fields.empty() || !plan.real_oneofs.empty()) {
    out << "\n";
  }

  // Real oneof ergonomic helpers
  for (const auto& ro : plan.real_oneofs) {
    out << inner << "[[nodiscard]] " << ro.enum_name << " " << ro.case_fn_name
        << "() const noexcept { return " << ro.member_var << "; }\n";
    out << inner << "void " << ro.clear_fn_name << "();\n";
    for (size_t fi : ro.field_indices) {
      const auto& f = msg.fields[fi];
      const std::string case_val = ro.enum_name + "::k" + to_pascal_case(f.name);
      const std::string fn_suf = sanitize_basic_ident(f.name);
      out << inner << "[[nodiscard]] bool has_" << f.name << "() const noexcept { return "
          << ro.member_var << " == " << case_val << "; }\n";
      if (f.type == FieldType::Message) {
        out << inner << "void set_" << f.name << "(" << resolve_cpp_message(f.type_name, catalog)
            << " v);\n";
      } else {
        out << inner << "void set_" << f.name << "(" << scalar_cpp_type(f, catalog) << " v);\n";
      }
      (void)fn_suf;
    }
    out << "\n";
  }

  out << inner << "bool operator==(const " << rec.struct_ident << "& rhs) const;\n";
  out << inner << "bool operator!=(const " << rec.struct_ident
      << "& rhs) const { return !(*this == rhs); }\n\n";
  out << inner << "void clear();\n\n";
  out << inner
      << "[[nodiscard]] static std::shared_ptr<::omnibuf::SchemaMeta> schema_meta(int schema_depth = 1);\n";
  out << inner
      << "[[nodiscard]] static std::shared_ptr<::omnibuf::SchemaMeta> schema_meta(int schema_depth, std::set<const void*>& active, bool* cyclic = nullptr);\n";
  out << inner
      << "[[nodiscard]] std::shared_ptr<::omnibuf::SchemaMeta> value_schema_meta(int schema_depth = 1) const;\n\n";
  out << inner
      << "[[nodiscard]] const char* try_marshal_omnibuf_fast(::omnibuf::FastWriter& w) const;\n";
  out << inner << "void marshal_omnibuf_fast(::omnibuf::FastWriter& w) const;\n";
  out << inner << "void marshal_omnibuf_append(std::vector<uint8_t>& dst) const;\n";
  out << inner
      << "[[nodiscard]] ::omnibuf::Result<std::vector<uint8_t>> try_marshal_omnibuf(::omnibuf::MarshalOptions opts = {}) const;\n";
  out << inner
      << "[[nodiscard]] std::vector<uint8_t> marshal_omnibuf(::omnibuf::MarshalOptions opts = {}) const;\n\n";
  out << inner
      << "[[nodiscard]] ::omnibuf::Result<void> unmarshal_omnibuf(std::span<const uint8_t> data, ::omnibuf::DecodeOptions opts = {});\n";
  out << inner
      << "[[nodiscard]] ::omnibuf::Result<void> decode_omnibuf(std::span<const uint8_t> data, int depth, bool merge, bool is_fresh = false, ::omnibuf::DecodeOptions opts = {});\n";
  out << inner
      << "[[nodiscard]] const char* decode_omnibuf_raw(std::span<const uint8_t> data, int depth, bool merge, bool is_fresh, ::omnibuf::DecodeState& state);\n";
  out << inner
      << "[[nodiscard]] const char* decode_omnibuf_raw(std::span<const uint8_t> data, int depth, bool merge, bool is_fresh = false);\n";
  out << indent << "};\n\n";

  pending_defined.push_back(full_proto);
  if (indent.empty()) {
    for (const auto& name : pending_defined) {
      defined_messages.insert(name);
    }
    pending_defined.clear();
  }
  ordered_plans.push_back(std::move(plan));
}

void emit_struct_methods(std::ostringstream& out,
                         const MessagePlan& plan,
                         const TypeCatalog& catalog) {
  const MessageRecord& rec = *plan.rec;
  const DescriptorProto& msg = *rec.desc;
  const std::string& qname = rec.cpp_local_path;

  // Oneof helpers
  for (const auto& ro : plan.real_oneofs) {
    out << "inline void " << qname << "::" << ro.clear_fn_name << "() {\n";
    for (size_t fi : ro.field_indices) {
      emit_clear_single_member(out, msg.fields[fi], plan.field_idents[fi], false, catalog, "  ");
    }
    out << "  " << ro.member_var << " = " << ro.enum_name << "::kNotSet;\n";
    out << "}\n\n";

    for (size_t fi : ro.field_indices) {
      const auto& f = msg.fields[fi];
      const std::string case_val = ro.enum_name + "::k" + to_pascal_case(f.name);
      if (f.type == FieldType::Message) {
        out << "inline void " << qname << "::set_" << f.name << "("
            << resolve_cpp_message(f.type_name, catalog) << " v) {\n";
        out << "  if (" << ro.member_var << " != " << case_val << ") " << ro.clear_fn_name
            << "();\n";
        out << "  " << plan.field_idents[fi] << " = std::move(v);\n";
        out << "  " << ro.member_var << " = " << case_val << ";\n";
        out << "}\n\n";
      } else {
        out << "inline void " << qname << "::set_" << f.name << "(" << scalar_cpp_type(f, catalog)
            << " v) {\n";
        out << "  if (" << ro.member_var << " != " << case_val << ") " << ro.clear_fn_name
            << "();\n";
        out << "  " << plan.field_idents[fi] << " = std::move(v);\n";
        out << "  " << ro.member_var << " = " << case_val << ";\n";
        out << "}\n\n";
      }
    }
  }

  // operator==
  out << "inline bool " << qname << "::operator==(const " << qname << "& rhs) const {\n";
  out << "  (void)rhs;\n";
  for (size_t i = 0; i < msg.fields.size(); ++i) {
    out << "  if (!(" << plan.field_idents[i] << " == rhs." << plan.field_idents[i]
        << ")) return false;\n";
  }
  for (const auto& ro : plan.real_oneofs) {
    out << "  if (" << ro.member_var << " != rhs." << ro.member_var << ") return false;\n";
  }
  out << "  return true;\n";
  out << "}\n\n";

  // clear()
  out << "inline void " << qname << "::clear() {\n";
  for (size_t i = 0; i < msg.fields.size(); ++i) {
    const auto& f = msg.fields[i];
    const bool is_opt_scalar = is_explicit_optional_scalar(f, rec.is_proto2);
    emit_clear_single_member(out, f, plan.field_idents[i], is_opt_scalar, catalog, "  ");
  }
  for (const auto& ro : plan.real_oneofs) {
    out << "  " << ro.member_var << " = " << ro.enum_name << "::kNotSet;\n";
  }
  out << "}\n\n";

  // Cycle-safe schema_meta + value_schema_meta for recursive messages
  out << "inline std::shared_ptr<::omnibuf::SchemaMeta> " << qname
      << "::schema_meta(int schema_depth, std::set<const void*>& active, bool* cyclic) {\n";
  out << "  static const int kTypeToken = 0;\n";
  out << "  if (schema_depth + 1 > ::omnibuf::MAX_CONTAINER_DEPTH) {\n";
  out << "    return nullptr;\n";
  out << "  }\n";
  out << "  if (active.contains(&kTypeToken)) {\n";
  out << "    if (cyclic != nullptr) *cyclic = true;\n";
  out << "    return nullptr;\n";
  out << "  }\n";
  out << "  active.insert(&kTypeToken);\n";
  out << "  auto meta = std::make_shared<::omnibuf::SchemaMeta>();\n";
  out << "  meta->type_name = \"" << msg.name << "\";\n";
  for (const auto& f : msg.fields) {
    out << "  meta->fields[" << f.number << "u] = \"" << f.name << "\";\n";
    if (f.type == FieldType::Message) {
      const std::string target = trim_leading_dot(f.type_name);
      auto map_it = catalog.map_entries.find(target);
      if (map_it != catalog.map_entries.end()) {
        if (map_it->second.val_type == FieldType::Message) {
          out << "  if (schema_depth + 3 <= ::omnibuf::MAX_CONTAINER_DEPTH) {\n";
          out << "    if (auto c = " << resolve_cpp_message(map_it->second.val_type_name, catalog)
              << "::schema_meta(schema_depth + 2, active, cyclic)) meta->children[" << f.number
              << "u] = std::move(c);\n";
          out << "  }\n";
        }
      } else {
        out << "  if (schema_depth + 3 <= ::omnibuf::MAX_CONTAINER_DEPTH) {\n";
        out << "    if (auto c = " << resolve_cpp_message(f.type_name, catalog)
            << "::schema_meta(schema_depth + 2, active, cyclic)) meta->children[" << f.number
            << "u] = std::move(c);\n";
        out << "  }\n";
      }
    }
  }
  out << "  active.erase(&kTypeToken);\n";
  out << "  return meta;\n";
  out << "}\n\n";

  out << "inline std::shared_ptr<::omnibuf::SchemaMeta> " << qname
      << "::schema_meta(int schema_depth) {\n";
  out << "  std::set<const void*> active;\n";
  out << "  return schema_meta(schema_depth, active, nullptr);\n";
  out << "}\n\n";

  out << "inline std::shared_ptr<::omnibuf::SchemaMeta> " << qname
      << "::value_schema_meta(int schema_depth) const {\n";
  out << "  if (schema_depth + 1 > ::omnibuf::MAX_CONTAINER_DEPTH) return nullptr;\n";
  out << "  auto meta = std::make_shared<::omnibuf::SchemaMeta>();\n";
  out << "  meta->type_name = \"" << msg.name << "\";\n";
  for (size_t i = 0; i < msg.fields.size(); ++i) {
    const auto& f = msg.fields[i];
    const std::string& ident = plan.field_idents[i];
    out << "  meta->fields[" << f.number << "u] = \"" << f.name << "\";\n";
    if (f.type == FieldType::Message) {
      const std::string target = trim_leading_dot(f.type_name);
      auto map_it = catalog.map_entries.find(target);
      out << "  if (schema_depth + 3 <= ::omnibuf::MAX_CONTAINER_DEPTH) {\n";
      if (map_it != catalog.map_entries.end()) {
        if (map_it->second.val_type == FieldType::Message) {
          out << "    std::shared_ptr<::omnibuf::SchemaMeta> child;\n";
          out << "    for (const auto& [_, sv] : " << ident << ") {\n";
          out << "      child = ::omnibuf::merge_schema_meta(child, sv.value_schema_meta(schema_depth + 2));\n";
          out << "    }\n";
          out << "    if (child) meta->children[" << f.number << "u] = std::move(child);\n";
        }
      } else if (f.label == FieldLabel::Repeated) {
        out << "    std::shared_ptr<::omnibuf::SchemaMeta> child;\n";
        out << "    for (const auto& elem : " << ident << ") {\n";
        out << "      child = ::omnibuf::merge_schema_meta(child, elem.value_schema_meta(schema_depth + 2));\n";
        out << "    }\n";
        out << "    if (child) meta->children[" << f.number << "u] = std::move(child);\n";
      } else if (is_real_oneof_field(f)) {
        const auto& ro = plan.real_oneofs[plan.oneof_idx_to_plan.at(*f.oneof_index)];
        const std::string case_val = ro.enum_name + "::k" + to_pascal_case(f.name);
        out << "    if (" << ro.member_var << " == " << case_val << " && " << ident
            << ".has_value()) {\n";
        out << "      if (auto c = " << ident
            << "->value_schema_meta(schema_depth + 2)) meta->children[" << f.number
            << "u] = std::move(c);\n";
        out << "    }\n";
      } else {
        out << "    if (" << ident << ".has_value()) {\n";
        out << "      if (auto c = " << ident
            << "->value_schema_meta(schema_depth + 2)) meta->children[" << f.number
            << "u] = std::move(c);\n";
        out << "    }\n";
      }
      out << "  }\n";
    }
  }
  out << "  return meta;\n";
  out << "}\n\n";

  // try_marshal_omnibuf_fast & marshal wrappers
  out << "inline const char* " << qname
      << "::try_marshal_omnibuf_fast(::omnibuf::FastWriter& w) const {\n";
  out << "  (void)w;\n";
  for (size_t i = 0; i < msg.fields.size(); ++i) {
    const auto& f = msg.fields[i];
    const bool is_opt_scalar = is_explicit_optional_scalar(f, rec.is_proto2);
    std::string oneof_guard;
    if (is_real_oneof_field(f)) {
      const auto& ro = plan.real_oneofs[plan.oneof_idx_to_plan.at(*f.oneof_index)];
      oneof_guard = ro.member_var + " == " + ro.enum_name + "::k" + to_pascal_case(f.name);
    }
    emit_try_encode_field_fast(out, f, plan.field_idents[i], is_opt_scalar, oneof_guard, catalog,
                               "w", "  ");
  }
  out << "  return nullptr;\n";
  out << "}\n\n";

  out << "inline void " << qname << "::marshal_omnibuf_fast(::omnibuf::FastWriter& w) const {\n";
  out << "  if (const char* err = try_marshal_omnibuf_fast(w)) [[unlikely]] {\n";
  out << "    throw std::invalid_argument(err);\n";
  out << "  }\n";
  out << "}\n\n";

  out << "inline void " << qname << "::marshal_omnibuf_append(std::vector<uint8_t>& dst) const {\n";
  out << "  ::omnibuf::FastWriter w(dst, 512);\n";
  out << "  marshal_omnibuf_fast(w);\n";
  out << "  w.finish();\n";
  out << "}\n\n";

  out << "inline ::omnibuf::Result<std::vector<uint8_t>> " << qname
      << "::try_marshal_omnibuf(::omnibuf::MarshalOptions opts) const {\n";
  out << "  std::vector<uint8_t> dst;\n";
  out << "  if (opts.self_describing) {\n";
  out << "    dst.reserve(512);\n";
  out << "    std::set<const void*> active;\n";
  out << "    bool cyclic = false;\n";
  out << "    auto meta = schema_meta(1, active, &cyclic);\n";
  out << "    if (cyclic) {\n";
  out << "      meta = value_schema_meta(1);\n";
  out << "    }\n";
  out << "    if (meta) {\n";
  out << "      ::omnibuf::append_field0_schema_meta(dst, *meta);\n";
  out << "    }\n";
  out << "  }\n";
  out << "  ::omnibuf::FastWriter w(dst, 512);\n";
  out << "  if (const char* err = try_marshal_omnibuf_fast(w)) [[unlikely]] {\n";
  out << "    return ::omnibuf::Error{err};\n";
  out << "  }\n";
  out << "  w.finish();\n";
  out << "  return dst;\n";
  out << "}\n\n";

  out << "inline std::vector<uint8_t> " << qname
      << "::marshal_omnibuf(::omnibuf::MarshalOptions opts) const {\n";
  out << "  auto res = try_marshal_omnibuf(opts);\n";
  out << "  if (!res) [[unlikely]] throw std::invalid_argument(res.error());\n";
  out << "  return std::move(res.value());\n";
  out << "}\n\n";

  // unmarshal_omnibuf & decode_omnibuf
  out << "inline ::omnibuf::Result<void> " << qname
      << "::unmarshal_omnibuf(std::span<const uint8_t> data, ::omnibuf::DecodeOptions opts) {\n";
  out << "  if (data.size() > opts.max_message_bytes) [[unlikely]] {\n";
  out << "    return ::omnibuf::Error{\"omnibuf: message exceeds max_message_bytes\"};\n";
  out << "  }\n";
  out << "  ::omnibuf::DecodeState state{opts, 0};\n";
  out << "  const char* err = decode_omnibuf_raw(data, 0, false, false, state);\n";
  out << "  if (err != nullptr) [[unlikely]] return ::omnibuf::Error{err};\n";
  out << "  return ::omnibuf::Result<void>::success();\n";
  out << "}\n\n";

  out << "inline ::omnibuf::Result<void> " << qname
      << "::decode_omnibuf(std::span<const uint8_t> data, int depth, bool merge, bool is_fresh, ::omnibuf::DecodeOptions opts) {\n";
  out << "  if (data.size() > opts.max_message_bytes) [[unlikely]] {\n";
  out << "    return ::omnibuf::Error{\"omnibuf: message exceeds max_message_bytes\"};\n";
  out << "  }\n";
  out << "  ::omnibuf::DecodeState state{opts, 0};\n";
  out << "  const char* err = decode_omnibuf_raw(data, depth, merge, is_fresh, state);\n";
  out << "  if (err != nullptr) [[unlikely]] return ::omnibuf::Error{err};\n";
  out << "  return ::omnibuf::Result<void>::success();\n";
  out << "}\n\n";

  out << "inline const char* " << qname
      << "::decode_omnibuf_raw(std::span<const uint8_t> data, int depth, bool merge, bool is_fresh) {\n";
  out << "  ::omnibuf::DecodeState state{};\n";
  out << "  return decode_omnibuf_raw(data, depth, merge, is_fresh, state);\n";
  out << "}\n\n";

  const size_t seen_words = std::max<size_t>(1, (msg.fields.size() + 63) / 64);

  out << "inline const char* " << qname
      << "::decode_omnibuf_raw(std::span<const uint8_t> data, int depth, bool merge, bool is_fresh, ::omnibuf::DecodeState& state) {\n";
  out << "  if (depth > state.opts.max_depth) [[unlikely]] {\n";
  out << "    return \"omnibuf: max nesting depth exceeded\";\n";
  out << "  }\n";
  out << "  if (!merge && !is_fresh) {\n";
  out << "    clear();\n";
  out << "  }\n";
  out << "  std::array<uint64_t, " << seen_words << "> seen_words{};\n";
  out << "  (void)seen_words;\n";
  out << "  const size_t n = data.size();\n";
  out << "  const uint8_t* base = data.data();\n";
  out << "  size_t pos = 0;\n";
  out << "  while (pos < n) {\n";
  out << "    uint64_t tag = 0;\n";
  out << "    const uint8_t b0 = base[pos];\n";
  out << "    if ((b0 & 1u) != 0) [[likely]] {\n";
  out << "      tag = static_cast<uint64_t>(b0 >> 1);\n";
  out << "      ++pos;\n";
  out << "    } else if ((b0 & 2u) != 0 && pos + 2 <= n) {\n";
  out << "      tag = static_cast<uint64_t>(::omnibuf::read_u16_le(base + pos) >> 2);\n";
  out << "      pos += 2;\n";
  out << "    } else {\n";
  out << "      const auto [t64, tn] = ::omnibuf::decode_varint(data.subspan(pos));\n";
  out << "      if (tn == 0 || (t64 >> 4) > ::omnibuf::MAX_FIELD_NUMBER) [[unlikely]] {\n";
  out << "        return \"omnibuf: invalid tag\";\n";
  out << "      }\n";
  out << "      tag = t64;\n";
  out << "      pos += tn;\n";
  out << "    }\n";
  out << "    switch (tag) {\n";
  out << "      case ::omnibuf::WireSubMsg: {\n";
  out << "        auto f0 = ::omnibuf::validate_field0(data, pos, ::omnibuf::WireSubMsg, depth, state);\n";
  out << "        if (!f0) return \"omnibuf: invalid Field #0\";\n";
  out << "        pos = f0.value();\n";
  out << "        break;\n";
  out << "      }\n";

  auto emit_oneof_activate = [&](size_t field_idx, std::string_view ind) {
    const auto& f = msg.fields[field_idx];
    if (!is_real_oneof_field(f)) return;
    const auto& ro = plan.real_oneofs[plan.oneof_idx_to_plan.at(*f.oneof_index)];
    const std::string case_val = ro.enum_name + "::k" + to_pascal_case(f.name);
    out << ind << "if (" << ro.member_var << " != " << case_val << ") {\n";
    for (size_t sib_fi : ro.field_indices) {
      if (sib_fi == field_idx) continue;
      emit_clear_single_member(out, msg.fields[sib_fi], plan.field_idents[sib_fi], false, catalog,
                               std::string(ind) + "  ");
      const size_t sw = sib_fi >> 6;
      const uint64_t sb = uint64_t{1} << (sib_fi & 63u);
      out << ind << "  seen_words[" << sw << "] &= ~" << sb << "ULL;\n";
    }
    out << ind << "  " << ro.member_var << " = " << case_val << ";\n";
    out << ind << "}\n";
  };

  for (size_t idx = 0; idx < msg.fields.size(); ++idx) {
    const auto& f = msg.fields[idx];
    const std::string& ident = plan.field_idents[idx];
    const uint64_t fnum = f.number;
    const bool is_repeated = (f.label == FieldLabel::Repeated);
    const size_t word_idx = idx >> 6;
    const uint64_t bit_mask = uint64_t{1} << (idx & 63u);

    if (f.type == FieldType::Message) {
      const std::string target = trim_leading_dot(f.type_name);
      auto map_it = catalog.map_entries.find(target);
      if (map_it != catalog.map_entries.end()) {
        const auto& mi = map_it->second;
        const uint64_t case_tag = (fnum << 4) | omnibuf::WireMap;
        out << "      case " << case_tag << "ULL: {\n";
        out << "        if (depth + 1 > state.opts.max_depth) [[unlikely]] {\n";
        out << "          return \"omnibuf: max nesting depth exceeded\";\n";
        out << "        }\n";
        out << "        const auto mr = ::omnibuf::read_len_slice_fast(data, pos, 3);\n";
        out << "        if (!mr) [[unlikely]] return mr.err;\n";
        if (mi.val_type == FieldType::Message) {
          out << "        if (!::omnibuf::decode_submsg_map_body<" << wire_const_expr(mi.key_type)
              << ">(" << ident << ", mr.span(), [&](std::span<const uint8_t> sb, auto& sv) {\n";
          out << "          return sv.decode_omnibuf_raw(sb, depth + 2, false, true, state) == nullptr;\n";
          out << "        }, &state)) [[unlikely]] {\n";
          out << "          return \"omnibuf: invalid map body\";\n";
          out << "        }\n";
        } else {
          out << "        if (!::omnibuf::decode_scalar_map_body<" << wire_const_expr(mi.key_type)
              << ", " << wire_const_expr(mi.val_type) << ">(" << ident
              << ", mr.span(), &state)) [[unlikely]] {\n";
          out << "          return \"omnibuf: invalid map body\";\n";
          out << "        }\n";
        }
        out << "        pos = mr.next_pos;\n";
        out << "        break;\n";
        out << "      }\n";
        continue;
      }

      if (is_repeated) {
        const uint64_t case_tag = (fnum << 4) | omnibuf::WireArray;
        out << "      case " << case_tag << "ULL: {\n";
        out << "        if (depth + 1 > state.opts.max_depth) [[unlikely]] {\n";
        out << "          return \"omnibuf: max nesting depth exceeded\";\n";
        out << "        }\n";
        out << "        const auto ar = ::omnibuf::read_len_slice_fast(data, pos, 2);\n";
        out << "        if (!ar) [[unlikely]] return ar.err;\n";
        out << "        const auto body = ar.span();\n";
        out << "        const auto hdr = ::omnibuf::parse_array_header(body);\n";
        out << "        if (!hdr.ok || hdr.elem_wt != ::omnibuf::WireSubMsg) [[unlikely]] {\n";
        out << "          return \"omnibuf: invalid submsg array header\";\n";
        out << "        }\n";
        out << "        if (!state.charge_elements(hdr.count)) [[unlikely]] {\n";
        out << "          return \"omnibuf: max decoded elements exceeded\";\n";
        out << "        }\n";
        out << "        " << ident << ".reserve(::omnibuf::bounded_reserve_target(" << ident
            << ".capacity(), " << ident << ".size(), hdr.count));\n";
        out << "        size_t p = hdr.pos;\n";
        out << "        for (size_t i = 0; i < hdr.count; ++i) {\n";
        out << "          const auto es = ::omnibuf::read_len_slice_fast(body, p, 0);\n";
        out << "          if (!es) [[unlikely]] return es.err;\n";
        out << "          auto& elem = " << ident << ".emplace_back();\n";
        out << "          const char* dr = elem.decode_omnibuf_raw(es.span(), depth + 2, false, true, state);\n";
        out << "          if (dr != nullptr) [[unlikely]] return dr;\n";
        out << "          p = es.next_pos;\n";
        out << "        }\n";
        out << "        if (p != body.size()) [[unlikely]] {\n";
        out << "          return \"omnibuf: trailing bytes in submsg array\";\n";
        out << "        }\n";
        out << "        pos = ar.next_pos;\n";
        out << "        break;\n";
        out << "      }\n";
        continue;
      }

      const uint64_t case_tag = (fnum << 4) | omnibuf::WireSubMsg;
      out << "      case " << case_tag << "ULL: {\n";
      out << "        const auto sr = ::omnibuf::read_len_slice_fast(data, pos, 0);\n";
      out << "        if (!sr) [[unlikely]] return sr.err;\n";
      emit_oneof_activate(idx, "        ");
      out << "        const bool merge_sub = merge || ((seen_words[" << word_idx << "] & "
          << bit_mask << "ULL) != 0);\n";
      out << "        bool fresh_sub = false;\n";
      out << "        if (!" << ident << ".has_value() || !merge_sub) {\n";
      out << "          " << ident << ".emplace();\n";
      out << "          fresh_sub = true;\n";
      out << "        }\n";
      out << "        seen_words[" << word_idx << "] |= " << bit_mask << "ULL;\n";
      out << "        const char* dr = " << ident
          << "->decode_omnibuf_raw(sr.span(), depth + 1, merge_sub, fresh_sub, state);\n";
      out << "        if (dr != nullptr) [[unlikely]] return dr;\n";
      out << "        pos = sr.next_pos;\n";
      out << "        break;\n";
      out << "      }\n";
      continue;
    }

    if (is_repeated) {
      const uint64_t case_tag = (fnum << 4) | omnibuf::WireArray;
      out << "      case " << case_tag << "ULL: {\n";
      out << "        if (depth + 1 > state.opts.max_depth) [[unlikely]] {\n";
      out << "          return \"omnibuf: max nesting depth exceeded\";\n";
      out << "        }\n";
      out << "        const auto ar = ::omnibuf::read_len_slice_fast(data, pos, 2);\n";
      out << "        if (!ar) [[unlikely]] return ar.err;\n";
      switch (f.type) {
        case FieldType::String:
          out << "        if (!::omnibuf::append_string_arr_body(" << ident
              << ", ar.span(), &state)) [[unlikely]] return \"omnibuf: invalid string array\";\n";
          break;
        case FieldType::Bytes:
          out << "        if (!::omnibuf::append_bytes_arr_body(" << ident
              << ", ar.span(), &state)) [[unlikely]] return \"omnibuf: invalid bytes array\";\n";
          break;
        case FieldType::Bool:
          out << "        if (!::omnibuf::append_bool_arr_body(" << ident
              << ", ar.span(), &state)) [[unlikely]] return \"omnibuf: invalid bool array\";\n";
          break;
        case FieldType::Uint32:
        case FieldType::Uint64:
          out << "        if (!::omnibuf::append_uint_arr_body(" << ident
              << ", ar.span(), &state)) [[unlikely]] return \"omnibuf: invalid uint array\";\n";
          break;
        case FieldType::Int32:
        case FieldType::Sint32:
        case FieldType::Int64:
        case FieldType::Sint64:
          out << "        if (!::omnibuf::append_sint_arr_body(" << ident
              << ", ar.span(), &state)) [[unlikely]] return \"omnibuf: invalid sint array\";\n";
          break;
        case FieldType::Fixed32:
          out << "        if (!::omnibuf::append_fixed32_arr_body(" << ident
              << ", ar.span(), ::omnibuf::WireFixed32, &state)) [[unlikely]] return \"omnibuf: invalid fixed32 array\";\n";
          break;
        case FieldType::Sfixed32:
          out << "        if (!::omnibuf::append_fixed32_arr_body(" << ident
              << ", ar.span(), ::omnibuf::WireSfixed32, &state)) [[unlikely]] return \"omnibuf: invalid sfixed32 array\";\n";
          break;
        case FieldType::Float:
          out << "        if (!::omnibuf::append_fixed32_arr_body(" << ident
              << ", ar.span(), ::omnibuf::WireFloat32, &state)) [[unlikely]] return \"omnibuf: invalid float32 array\";\n";
          break;
        case FieldType::Fixed64:
          out << "        if (!::omnibuf::append_fixed64_arr_body(" << ident
              << ", ar.span(), ::omnibuf::WireFixed64, &state)) [[unlikely]] return \"omnibuf: invalid fixed64 array\";\n";
          break;
        case FieldType::Sfixed64:
          out << "        if (!::omnibuf::append_fixed64_arr_body(" << ident
              << ", ar.span(), ::omnibuf::WireSfixed64, &state)) [[unlikely]] return \"omnibuf: invalid sfixed64 array\";\n";
          break;
        case FieldType::Double:
          out << "        if (!::omnibuf::append_fixed64_arr_body(" << ident
              << ", ar.span(), ::omnibuf::WireFloat64, &state)) [[unlikely]] return \"omnibuf: invalid float64 array\";\n";
          break;
        case FieldType::Enum:
          out << "        {\n";
          out << "          std::vector<int32_t> tmp;\n";
          out << "          if (!::omnibuf::append_sint_arr_body(tmp, ar.span(), &state)) [[unlikely]] {\n";
          out << "            return \"omnibuf: invalid enum array\";\n";
          out << "          }\n";
          out << "          for (int32_t ev : tmp) {\n";
          out << "            " << ident << ".push_back(static_cast<" << scalar_cpp_type(f, catalog)
              << ">(ev));\n";
          out << "          }\n";
          out << "        }\n";
          break;
        default:
          break;
      }
      out << "        pos = ar.next_pos;\n";
      out << "        break;\n";
      out << "      }\n";
      continue;
    }

    const bool is_opt_scalar = is_explicit_optional_scalar(f, rec.is_proto2);
    const std::string cpp_t = scalar_cpp_type(f, catalog);

    if (f.type == FieldType::Bool) {
      const uint64_t tag_false = (fnum << 4) | omnibuf::WireBoolFalse;
      const uint64_t tag_true = (fnum << 4) | omnibuf::WireBoolTrue;
      out << "      case " << tag_false << "ULL: {\n";
      emit_oneof_activate(idx, "        ");
      out << "        " << ident << " = false;\n";
      out << "        break;\n";
      out << "      }\n";
      out << "      case " << tag_true << "ULL: {\n";
      emit_oneof_activate(idx, "        ");
      out << "        " << ident << " = true;\n";
      out << "        break;\n";
      out << "      }\n";
      continue;
    }

    const uint8_t wt = scalar_wire_const(f.type);
    const uint64_t case_tag = (fnum << 4) | wt;
    out << "      case " << case_tag << "ULL: {\n";
    switch (f.type) {
      case FieldType::Uint32:
      case FieldType::Uint64:
        out << "        " << cpp_t << " parsed{};\n";
        out << "        if (pos < n && (base[pos] & 1u) != 0) [[likely]] {\n";
        out << "          parsed = static_cast<" << cpp_t << ">(base[pos] >> 1);\n";
        out << "          ++pos;\n";
        out << "        } else {\n";
        out << "          const auto [v, vn] = ::omnibuf::decode_varint(data.subspan(pos));\n";
        out << "          if (vn == 0) [[unlikely]] return \"omnibuf: truncated uint\";\n";
        out << "          pos += vn;\n";
        out << "          parsed = static_cast<" << cpp_t << ">(v);\n";
        out << "        }\n";
        emit_oneof_activate(idx, "        ");
        out << "        " << ident << " = parsed;\n";
        break;
      case FieldType::Int32:
      case FieldType::Sint32:
      case FieldType::Int64:
      case FieldType::Sint64:
      case FieldType::Enum:
        out << "        uint64_t v = 0;\n";
        out << "        if (pos < n && (base[pos] & 1u) != 0) [[likely]] {\n";
        out << "          v = static_cast<uint64_t>(base[pos] >> 1);\n";
        out << "          ++pos;\n";
        out << "        } else {\n";
        out << "          const auto [v64, vn] = ::omnibuf::decode_varint(data.subspan(pos));\n";
        out << "          if (vn == 0) [[unlikely]] return \"omnibuf: truncated sint\";\n";
        out << "          v = v64;\n";
        out << "          pos += vn;\n";
        out << "        }\n";
        emit_oneof_activate(idx, "        ");
        if (f.type == FieldType::Enum) {
          out << "        " << ident << " = static_cast<" << cpp_t
              << ">(static_cast<int32_t>(::omnibuf::decode_zigzag(v)));\n";
        } else {
          out << "        " << ident << " = static_cast<" << cpp_t
              << ">(::omnibuf::decode_zigzag(v));\n";
        }
        break;
      case FieldType::Fixed32:
      case FieldType::Sfixed32:
      case FieldType::Float:
        out << "        if (n - pos < 4) [[unlikely]] return \"omnibuf: truncated 32-bit scalar\";\n";
        out << "        const uint32_t bits = ::omnibuf::read_u32_le(base + pos);\n";
        out << "        pos += 4;\n";
        emit_oneof_activate(idx, "        ");
        if (f.type == FieldType::Float) {
          out << "        " << ident << " = ::omnibuf::bits_to_float32(bits);\n";
        } else {
          out << "        " << ident << " = static_cast<" << cpp_t << ">(bits);\n";
        }
        break;
      case FieldType::Fixed64:
      case FieldType::Sfixed64:
      case FieldType::Double:
        out << "        if (n - pos < 8) [[unlikely]] return \"omnibuf: truncated 64-bit scalar\";\n";
        out << "        const uint64_t bits = ::omnibuf::read_u64_le(base + pos);\n";
        out << "        pos += 8;\n";
        emit_oneof_activate(idx, "        ");
        if (f.type == FieldType::Double) {
          out << "        " << ident << " = ::omnibuf::bits_to_float64(bits);\n";
        } else {
          out << "        " << ident << " = static_cast<" << cpp_t << ">(bits);\n";
        }
        break;
      case FieldType::String:
        out << "        const auto sr = ::omnibuf::read_str_slice_fast(data, pos);\n";
        out << "        if (!sr) [[unlikely]] return sr.err;\n";
        emit_oneof_activate(idx, "        ");
        if (is_opt_scalar) {
          out << "        " << ident
              << ".emplace(reinterpret_cast<const char*>(sr.ptr), sr.len);\n";
        } else {
          out << "        " << ident
              << ".assign(reinterpret_cast<const char*>(sr.ptr), sr.len);\n";
        }
        out << "        pos = sr.next_pos;\n";
        break;
      case FieldType::Bytes:
        out << "        const auto br = ::omnibuf::read_len_slice_fast(data, pos, 0);\n";
        out << "        if (!br) [[unlikely]] return br.err;\n";
        emit_oneof_activate(idx, "        ");
        if (is_opt_scalar) {
          out << "        " << ident << ".emplace(br.ptr, br.ptr + br.len);\n";
        } else {
          out << "        " << ident << ".assign(br.ptr, br.ptr + br.len);\n";
        }
        out << "        pos = br.next_pos;\n";
        break;
      default:
        break;
    }
    out << "        break;\n";
    out << "      }\n";
  }

  out << "      default: {\n";
  out << "        const uint32_t fnum = static_cast<uint32_t>(tag >> 4);\n";
  out << "        const uint8_t wt = static_cast<uint8_t>(tag & 0x0Fu);\n";
  out << "        if (fnum == 0) [[unlikely]] {\n";
  out << "          return \"omnibuf: invalid field number 0\";\n";
  out << "        }\n";
  out << "        auto sk = ::omnibuf::validate_value(data, pos, wt, depth, state);\n";
  out << "        if (!sk) [[unlikely]] return \"omnibuf: invalid unknown field\";\n";
  out << "        pos = sk.value();\n";
  out << "        break;\n";
  out << "      }\n";
  out << "    }\n";
  out << "  }\n";
  out << "  return nullptr;\n";
  out << "}\n\n";
}

void emit_opaque_class(std::ostringstream& cls_out,
                       std::ostringstream& out_of_line_out,
                       const MessagePlan& plan,
                       const TypeCatalog& catalog) {
  const MessageRecord& rec = *plan.rec;
  const DescriptorProto& msg = *rec.desc;
  const std::string& cls = rec.opaque_ident;

  uint32_t max_fnum = 0;
  for (const auto& f : msg.fields) {
    if (f.number > max_fnum) max_fnum = f.number;
  }
  const size_t dirty_words = std::max<size_t>(1, static_cast<size_t>(max_fnum / 64) + 1);

  auto is_heap_dirty_field = [&](size_t idx) noexcept {
    const auto& f = msg.fields[idx];
    return f.label == FieldLabel::Repeated || f.type == FieldType::Message ||
           f.type == FieldType::String || f.type == FieldType::Bytes ||
           is_explicit_optional_scalar(f, rec.is_proto2);
  };

  cls_out << "class " << cls << " {\n";
  cls_out << " public:\n";
  cls_out << "  " << cls << "() = default;\n";
  cls_out << "  explicit " << cls << "(std::span<const uint8_t> raw) noexcept : raw_view_(raw) {}\n";
  cls_out << "  explicit " << cls << "(std::vector<uint8_t> owned)\n";
  cls_out << "      : owned_raw_(std::make_unique<std::vector<uint8_t>>(std::move(owned))) {}\n";
  cls_out << "  " << cls << "(const " << cls << "& other)\n";
  cls_out << "      : raw_view_(other.raw_view_),\n";
  cls_out << "        owned_raw_(other.owned_raw_ ? std::make_unique<std::vector<uint8_t>>(*other.owned_raw_)\n";
  cls_out << "                                    : nullptr),\n";
  cls_out << "        dirty_(other.dirty_),\n";
  cls_out << "        dirty_scalars_(other.dirty_scalars_),\n";
  cls_out << "        dirty_heap_(other.dirty_heap_ ? std::make_unique<DirtyHeap>(*other.dirty_heap_)\n";
  cls_out << "                                      : nullptr) {}\n";
  cls_out << "  " << cls << "& operator=(const " << cls << "& other) {\n";
  cls_out << "    if (this != &other) {\n";
  cls_out << "      *this = " << cls << "(other);\n";
  cls_out << "    }\n";
  cls_out << "    return *this;\n";
  cls_out << "  }\n";
  cls_out << "  " << cls << "(" << cls << "&&) noexcept = default;\n";
  cls_out << "  " << cls << "& operator=(" << cls << "&&) noexcept = default;\n\n";

  cls_out << "  void unmarshal(std::span<const uint8_t> raw) noexcept {\n";
  cls_out << "    owned_raw_.reset();\n";
  cls_out << "    raw_view_ = raw;\n";
  cls_out << "    dirty_.fill(0);\n";
  cls_out << "  }\n\n";

  cls_out << "  [[nodiscard]] std::span<const uint8_t> raw() const noexcept {\n";
  cls_out << "    return owned_raw_ ? std::span<const uint8_t>(*owned_raw_) : raw_view_;\n";
  cls_out << "  }\n\n";

  cls_out << "  [[nodiscard]] ::omnibuf::Result<void> validate(::omnibuf::DecodeOptions opts = {}) const {\n";
  cls_out << "    const auto pkt = raw();\n";
  cls_out << "    if (pkt.size() > opts.max_message_bytes) {\n";
  cls_out << "      return ::omnibuf::Error{\"omnibuf: message exceeds max_message_bytes\"};\n";
  cls_out << "    }\n";
  cls_out << "    ::omnibuf::DecodeState state{opts, 0};\n";
  cls_out << "    return ::omnibuf::validate_message_bytes(pkt, 0, state);\n";
  cls_out << "  }\n\n";

  for (size_t idx = 0; idx < msg.fields.size(); ++idx) {
    const auto& f = msg.fields[idx];
    const std::string dirty_var = "f_" + f.name + "_";
    const std::string heap_ref = "dirty_heap_->" + dirty_var;
    const std::string heap_mut = "ensure_dirty_heap()." + dirty_var;
    const std::string scalar_ref = "dirty_scalars_." + dirty_var;
    const uint32_t fnum = f.number;
    const bool is_repeated = (f.label == FieldLabel::Repeated);
    const bool is_opt_scalar = is_explicit_optional_scalar(f, rec.is_proto2);
    const bool is_oneof = is_real_oneof_field(f);

    auto emit_dirty_oneof_siblings = [&](std::ostringstream& os, std::string_view ind) {
      if (!is_oneof) return;
      const auto& ro = plan.real_oneofs[plan.oneof_idx_to_plan.at(*f.oneof_index)];
      for (size_t sib_fi : ro.field_indices) {
        if (sib_fi == idx) continue;
        const auto& sf = msg.fields[sib_fi];
        os << ind << "mark_dirty(" << sf.number << "u);\n";
      }
      os << ind << "dirty_scalars_.oneof_" << ro.oneof_index << "_active_ = " << fnum << "u;\n";
    };

    if (f.type == FieldType::Message) {
      const std::string target = trim_leading_dot(f.type_name);
      auto map_it = catalog.map_entries.find(target);
      if (map_it != catalog.map_entries.end()) {
        const auto& mi = map_it->second;
        const std::string m_type = map_cpp_type(mi, catalog);
        const std::string k_cpp = scalar_cpp_type_for_kind(mi.key_type, "", catalog);
        const std::string k_param =
            (mi.key_type == FieldType::String) ? "std::string_view" : k_cpp;

        if (mi.val_type == FieldType::Message) {
          const std::string v_opaque = resolve_cpp_opaque(mi.val_type_name, catalog);
          cls_out << "  [[nodiscard]] std::pair<" << v_opaque << ", bool> lookup_" << f.name << "("
                  << k_param << " key) const;\n";
          out_of_line_out << "inline std::pair<" << v_opaque << ", bool> " << cls << "::lookup_"
                          << f.name << "(" << k_param << " key) const {\n";
          out_of_line_out << "  if (is_dirty(" << fnum << "u)) {\n";
          if (mi.key_type == FieldType::String) {
            out_of_line_out << "    auto it = " << heap_ref << ".find(std::string(key));\n";
          } else {
            out_of_line_out << "    auto it = " << heap_ref << ".find(key);\n";
          }
          out_of_line_out << "    if (it == " << heap_ref << ".end()) return {" << v_opaque
                          << "{}, false};\n";
          out_of_line_out << "    return {" << v_opaque << "(it->second.marshal_omnibuf()), true};\n";
          out_of_line_out << "  }\n";
          out_of_line_out << "  const auto [sl, ok] = ::omnibuf::lookup_submsg_map_slice<"
                          << wire_const_expr(mi.key_type) << ", " << k_cpp << ">(raw(), " << fnum
                          << "u, key);\n";
          out_of_line_out << "  return ok ? std::pair<" << v_opaque << ", bool>{" << v_opaque
                          << "(sl), true} : std::pair<" << v_opaque << ", bool>{" << v_opaque
                          << "{}, false};\n";
          out_of_line_out << "}\n\n";

          cls_out << "  [[nodiscard]] " << m_type << " get_" << f.name << "() const {\n";
          cls_out << "    if (is_dirty(" << fnum << "u)) return " << heap_ref << ";\n";
          cls_out << "    return ::omnibuf::read_submsg_map<" << wire_const_expr(mi.key_type)
                  << ", " << m_type << ">(raw(), " << fnum
                  << "u, [](std::span<const uint8_t> sb, auto& sv) {\n";
          cls_out << "      return sv.unmarshal_omnibuf(sb).ok();\n";
          cls_out << "    }).first;\n";
          cls_out << "  }\n";

          cls_out << "  [[nodiscard]] ::omnibuf::Result<" << m_type << "> try_get_" << f.name
                  << "() const {\n";
          cls_out << "    if (is_dirty(" << fnum << "u)) return " << heap_ref << ";\n";
          cls_out << "    return ::omnibuf::try_read_submsg_map<" << wire_const_expr(mi.key_type)
                  << ", " << m_type << ">(raw(), " << fnum
                  << "u, [](std::span<const uint8_t> sb, auto& sv) {\n";
          cls_out << "      return sv.unmarshal_omnibuf(sb).ok();\n";
          cls_out << "    });\n";
          cls_out << "  }\n";
        } else {
          const std::string v_cpp =
              scalar_cpp_type_for_kind(mi.val_type, mi.val_type_name, catalog);
          cls_out << "  [[nodiscard]] std::pair<" << v_cpp << ", bool> lookup_" << f.name << "("
                  << k_param << " key) const {\n";
          cls_out << "    if (is_dirty(" << fnum << "u)) {\n";
          if (mi.key_type == FieldType::String) {
            cls_out << "      auto it = " << heap_ref << ".find(std::string(key));\n";
          } else {
            cls_out << "      auto it = " << heap_ref << ".find(key);\n";
          }
          cls_out << "      if (it == " << heap_ref << ".end()) return {" << v_cpp
                  << "{}, false};\n";
          cls_out << "      return {it->second, true};\n";
          cls_out << "    }\n";
          cls_out << "    return ::omnibuf::lookup_scalar_map<" << wire_const_expr(mi.key_type)
                  << ", " << wire_const_expr(mi.val_type) << ", " << k_cpp << ", " << v_cpp
                  << ">(raw(), " << fnum << "u, key);\n";
          cls_out << "  }\n";

          cls_out << "  [[nodiscard]] " << m_type << " get_" << f.name << "() const {\n";
          cls_out << "    if (is_dirty(" << fnum << "u)) return " << heap_ref << ";\n";
          cls_out << "    return ::omnibuf::read_scalar_map<" << wire_const_expr(mi.key_type)
                  << ", " << wire_const_expr(mi.val_type) << ", " << m_type << ">(raw(), " << fnum
                  << "u).first;\n";
          cls_out << "  }\n";

          cls_out << "  [[nodiscard]] ::omnibuf::Result<" << m_type << "> try_get_" << f.name
                  << "() const {\n";
          cls_out << "    if (is_dirty(" << fnum << "u)) return " << heap_ref << ";\n";
          cls_out << "    return ::omnibuf::try_read_scalar_map<" << wire_const_expr(mi.key_type)
                  << ", " << wire_const_expr(mi.val_type) << ", " << m_type << ">(raw(), " << fnum
                  << "u);\n";
          cls_out << "  }\n";
        }

        cls_out << "  void set_" << f.name << "(" << m_type << " v) {\n";
        cls_out << "    " << heap_mut << " = std::move(v);\n";
        cls_out << "    mark_dirty(" << fnum << "u);\n";
        cls_out << "  }\n\n";
        continue;
      }

      const std::string sub_cpp = resolve_cpp_message(f.type_name, catalog);
      const std::string sub_opaque = resolve_cpp_opaque(f.type_name, catalog);

      if (is_repeated) {
        cls_out << "  [[nodiscard]] size_t " << f.name << "_len() const noexcept {\n";
        cls_out << "    if (is_dirty(" << fnum << "u)) return " << heap_ref << ".size();\n";
        cls_out << "    return ::omnibuf::submsg_arr_len(raw(), " << fnum << "u);\n";
        cls_out << "  }\n";

        cls_out << "  [[nodiscard]] " << sub_opaque << " get_" << f.name << "(size_t idx) const;\n";
        out_of_line_out << "inline " << sub_opaque << " " << cls << "::get_" << f.name
                        << "(size_t idx) const {\n";
        out_of_line_out << "  if (is_dirty(" << fnum << "u)) {\n";
        out_of_line_out << "    if (idx < " << heap_ref << ".size()) {\n";
        out_of_line_out << "      return " << sub_opaque << "(" << heap_ref
                        << "[idx].marshal_omnibuf());\n";
        out_of_line_out << "    }\n";
        out_of_line_out << "    return " << sub_opaque << "{};\n";
        out_of_line_out << "  }\n";
        out_of_line_out << "  const auto [elem, ok] = ::omnibuf::read_submsg_arr_elem(raw(), "
                        << fnum << "u, idx);\n";
        out_of_line_out << "  return ok ? " << sub_opaque << "(elem) : " << sub_opaque << "{};\n";
        out_of_line_out << "}\n\n";

        cls_out << "  void set_" << f.name << "(std::vector<" << sub_cpp << "> v) {\n";
        cls_out << "    " << heap_mut << " = std::move(v);\n";
        cls_out << "    mark_dirty(" << fnum << "u);\n";
        cls_out << "  }\n\n";
        continue;
      }

      cls_out << "  [[nodiscard]] " << sub_opaque << " get_" << f.name << "() const;\n";
      cls_out << "  [[nodiscard]] ::omnibuf::Result<std::optional<" << sub_opaque << ">> try_get_"
              << f.name << "() const;\n";

      out_of_line_out << "inline " << sub_opaque << " " << cls << "::get_" << f.name
                      << "() const {\n";
      out_of_line_out << "  if (is_dirty(" << fnum << "u)) {\n";
      if (is_oneof) {
        const auto& ro = plan.real_oneofs[plan.oneof_idx_to_plan.at(*f.oneof_index)];
        out_of_line_out << "    if (dirty_scalars_.oneof_" << ro.oneof_index
                        << "_active_ != " << fnum << "u) return " << sub_opaque << "{};\n";
      }
      out_of_line_out << "    if (" << heap_ref << ".has_value()) {\n";
      out_of_line_out << "      return " << sub_opaque << "(" << heap_ref
                      << "->marshal_omnibuf());\n";
      out_of_line_out << "    }\n";
      out_of_line_out << "    return " << sub_opaque << "{};\n";
      out_of_line_out << "  }\n";
      out_of_line_out << "  const auto pkt = raw();\n";
      out_of_line_out << "  const auto first = ::omnibuf::next_container_field(pkt, 0, " << fnum
                      << "u, ::omnibuf::WireSubMsg, 0);\n";
      out_of_line_out << "  if (!first.valid || !first.matched) return " << sub_opaque << "{};\n";
      out_of_line_out
          << "  const auto second = ::omnibuf::next_container_field(pkt, first.next_pos, " << fnum
          << "u, ::omnibuf::WireSubMsg, 0);\n";
      out_of_line_out << "  if (!second.valid) return " << sub_opaque << "{};\n";
      out_of_line_out << "  if (!second.matched) return " << sub_opaque << "(first.body);\n";
      out_of_line_out << "  auto [merged, ok] = ::omnibuf::read_submsg(pkt, " << fnum << "u);\n";
      out_of_line_out << "  return ok ? " << sub_opaque << "(std::move(merged)) : " << sub_opaque
                      << "{};\n";
      out_of_line_out << "}\n\n";

      out_of_line_out << "inline ::omnibuf::Result<std::optional<" << sub_opaque << ">> " << cls
                      << "::try_get_" << f.name << "() const {\n";
      out_of_line_out << "  if (is_dirty(" << fnum << "u)) {\n";
      if (is_oneof) {
        const auto& ro = plan.real_oneofs[plan.oneof_idx_to_plan.at(*f.oneof_index)];
        out_of_line_out << "    if (dirty_scalars_.oneof_" << ro.oneof_index
                        << "_active_ != " << fnum << "u) return std::optional<" << sub_opaque
                        << ">{std::nullopt};\n";
      }
      out_of_line_out << "    if (" << heap_ref << ".has_value()) {\n";
      out_of_line_out << "      auto mb = " << heap_ref << "->try_marshal_omnibuf();\n";
      out_of_line_out << "      if (!mb) return ::omnibuf::Error{mb.error()};\n";
      out_of_line_out << "      return std::optional<" << sub_opaque << ">{" << sub_opaque
                      << "(std::move(mb.value()))};\n";
      out_of_line_out << "    }\n";
      out_of_line_out << "    return std::optional<" << sub_opaque << ">{std::nullopt};\n";
      out_of_line_out << "  }\n";
      out_of_line_out << "  auto sr = ::omnibuf::try_read_submsg(raw(), " << fnum << "u);\n";
      out_of_line_out << "  if (!sr) return ::omnibuf::Error{sr.error()};\n";
      out_of_line_out << "  if (!sr.value().has_value()) return std::optional<" << sub_opaque
                      << ">{std::nullopt};\n";
      out_of_line_out << "  return std::optional<" << sub_opaque << ">{" << sub_opaque
                      << "(std::move(*sr.value()))};\n";
      out_of_line_out << "}\n\n";

      const std::string param_box =
          plan.field_uses_box[idx] ? ("::omnibuf::Box<" + sub_cpp + ">")
                                   : ("std::optional<" + sub_cpp + ">");
      cls_out << "  void set_" << f.name << "(" << param_box << " v) {\n";
      emit_dirty_oneof_siblings(cls_out, "    ");
      if (is_oneof) {
        const auto& ro = plan.real_oneofs[plan.oneof_idx_to_plan.at(*f.oneof_index)];
        cls_out << "    if (!v.has_value()) dirty_scalars_.oneof_" << ro.oneof_index
                << "_active_ = 0u;\n";
      }
      cls_out << "    " << heap_mut << " = std::move(v);\n";
      cls_out << "    mark_dirty(" << fnum << "u);\n";
      cls_out << "  }\n\n";
      continue;
    }

    if (is_repeated) {
      const uint8_t elem_wt = scalar_wire_const(f.type);
      const std::string cpp_t = scalar_cpp_type(f, catalog);
      cls_out << "  [[nodiscard]] size_t " << f.name << "_len() const noexcept {\n";
      cls_out << "    if (is_dirty(" << fnum << "u)) return " << heap_ref << ".size();\n";
      cls_out << "    return ::omnibuf::counted_arr_len(raw(), " << fnum << "u, "
              << static_cast<uint32_t>(elem_wt) << "u);\n";
      cls_out << "  }\n";

      auto emit_arr_lambda = [&](std::ostringstream& os) {
        switch (f.type) {
          case FieldType::String:
            os << "[](auto& v, auto b) { return ::omnibuf::append_string_arr_body(v, b); }";
            break;
          case FieldType::Bytes:
            os << "[](auto& v, auto b) { return ::omnibuf::append_bytes_arr_body(v, b); }";
            break;
          case FieldType::Bool:
            os << "[](auto& v, auto b) { return ::omnibuf::append_bool_arr_body(v, b); }";
            break;
          case FieldType::Uint32:
          case FieldType::Uint64:
            os << "[](auto& v, auto b) { return ::omnibuf::append_uint_arr_body(v, b); }";
            break;
          case FieldType::Int32:
          case FieldType::Sint32:
          case FieldType::Int64:
          case FieldType::Sint64:
            os << "[](auto& v, auto b) { return ::omnibuf::append_sint_arr_body(v, b); }";
            break;
          case FieldType::Enum:
            os << "[](auto& v, auto b) {\n"
               << "      std::vector<int32_t> tmp;\n"
               << "      if (!::omnibuf::append_sint_arr_body(tmp, b)) return false;\n"
               << "      for (int32_t ev : tmp) v.push_back(static_cast<" << cpp_t << ">(ev));\n"
               << "      return true;\n"
               << "    }";
            break;
          case FieldType::Fixed32:
          case FieldType::Sfixed32:
          case FieldType::Float:
            os << "[](auto& v, auto b) { return ::omnibuf::append_fixed32_arr_body(v, b, "
               << static_cast<uint32_t>(elem_wt) << "u); }";
            break;
          case FieldType::Fixed64:
          case FieldType::Sfixed64:
          case FieldType::Double:
          default:
            os << "[](auto& v, auto b) { return ::omnibuf::append_fixed64_arr_body(v, b, "
               << static_cast<uint32_t>(elem_wt) << "u); }";
            break;
        }
      };

      cls_out << "  [[nodiscard]] std::vector<" << cpp_t << "> get_" << f.name << "() const {\n";
      cls_out << "    if (is_dirty(" << fnum << "u)) return " << heap_ref << ";\n";
      cls_out << "    std::vector<" << cpp_t << "> dst;\n";
      cls_out << "    (void)::omnibuf::read_arrays_into(dst, raw(), " << fnum << "u, ";
      emit_arr_lambda(cls_out);
      cls_out << ");\n";
      cls_out << "    return dst;\n";
      cls_out << "  }\n";

      cls_out << "  [[nodiscard]] ::omnibuf::Result<std::vector<" << cpp_t << ">> try_get_"
              << f.name << "() const {\n";
      cls_out << "    if (is_dirty(" << fnum << "u)) return " << heap_ref << ";\n";
      cls_out << "    return ::omnibuf::try_read_arrays<std::vector<" << cpp_t << ">>(raw(), "
              << fnum << "u, ";
      emit_arr_lambda(cls_out);
      cls_out << ");\n";
      cls_out << "  }\n";

      cls_out << "  void set_" << f.name << "(std::vector<" << cpp_t << "> v) {\n";
      cls_out << "    " << heap_mut << " = std::move(v);\n";
      cls_out << "    mark_dirty(" << fnum << "u);\n";
      cls_out << "  }\n\n";
      continue;
    }

    const std::string cpp_t = scalar_cpp_type(f, catalog);

    auto emit_try_read_scalar_expr = [&](std::ostringstream& os) {
      switch (f.type) {
        case FieldType::Uint32:
        case FieldType::Uint64:
          os << "::omnibuf::try_read_uint(raw(), " << fnum << "u)";
          break;
        case FieldType::Int32:
        case FieldType::Sint32:
        case FieldType::Int64:
        case FieldType::Sint64:
        case FieldType::Enum:
          os << "::omnibuf::try_read_sint(raw(), " << fnum << "u)";
          break;
        case FieldType::Bool:
          os << "::omnibuf::try_read_bool(raw(), " << fnum << "u)";
          break;
        case FieldType::Fixed32:
          os << "::omnibuf::try_read_fixed32(raw(), " << fnum << "u)";
          break;
        case FieldType::Sfixed32:
          os << "::omnibuf::try_read_sfixed32(raw(), " << fnum << "u)";
          break;
        case FieldType::Float:
          os << "::omnibuf::try_read_float32(raw(), " << fnum << "u)";
          break;
        case FieldType::Fixed64:
          os << "::omnibuf::try_read_fixed64(raw(), " << fnum << "u)";
          break;
        case FieldType::Sfixed64:
          os << "::omnibuf::try_read_sfixed64(raw(), " << fnum << "u)";
          break;
        case FieldType::Double:
          os << "::omnibuf::try_read_float64(raw(), " << fnum << "u)";
          break;
        case FieldType::String:
          os << "::omnibuf::try_read_string_view(raw(), " << fnum << "u)";
          break;
        case FieldType::Bytes:
        default:
          os << "::omnibuf::try_read_bytes_view(raw(), " << fnum << "u)";
          break;
      }
    };

    if (is_opt_scalar) {
      cls_out << "  [[nodiscard]] ::omnibuf::Result<std::optional<" << cpp_t << ">> try_get_"
              << f.name << "() const {\n";
      cls_out << "    if (is_dirty(" << fnum << "u)) return " << heap_ref << ";\n";
      cls_out << "    auto r = ";
      emit_try_read_scalar_expr(cls_out);
      cls_out << ";\n";
      cls_out << "    if (!r) return ::omnibuf::Error{r.error()};\n";
      cls_out << "    if (!r.value().has_value()) return std::optional<" << cpp_t
              << ">{std::nullopt};\n";
      if (f.type == FieldType::String) {
        cls_out << "    return std::optional<" << cpp_t << ">{std::string(*r.value())};\n";
      } else if (f.type == FieldType::Bytes) {
        cls_out << "    return std::optional<" << cpp_t
                << ">{std::vector<uint8_t>(r.value()->begin(), r.value()->end())};\n";
      } else if (f.type == FieldType::Enum) {
        cls_out << "    return std::optional<" << cpp_t << ">{static_cast<" << cpp_t
                << ">(static_cast<int32_t>(*r.value()))};\n";
      } else {
        cls_out << "    return std::optional<" << cpp_t << ">{static_cast<" << cpp_t
                << ">(*r.value())};\n";
      }
      cls_out << "  }\n";

      cls_out << "  [[nodiscard]] std::optional<" << cpp_t << "> get_" << f.name << "() const {\n";
      cls_out << "    auto r = try_get_" << f.name << "();\n";
      cls_out << "    return r ? std::move(r.value()) : std::optional<" << cpp_t
              << ">{std::nullopt};\n";
      cls_out << "  }\n";

      cls_out << "  void set_" << f.name << "(std::optional<" << cpp_t << "> v) {\n";
      cls_out << "    " << heap_mut << " = std::move(v);\n";
      cls_out << "    mark_dirty(" << fnum << "u);\n";
      cls_out << "  }\n\n";
      continue;
    }

    if (f.type == FieldType::String) {
      cls_out << "  [[nodiscard]] std::string_view get_" << f.name << "_view() const noexcept {\n";
      cls_out << "    if (is_dirty(" << fnum << "u)) {\n";
      if (is_oneof) {
        const auto& ro = plan.real_oneofs[plan.oneof_idx_to_plan.at(*f.oneof_index)];
        cls_out << "      if (dirty_scalars_.oneof_" << ro.oneof_index << "_active_ != " << fnum
                << "u) return {};\n";
      }
      cls_out << "      return " << heap_ref << ";\n";
      cls_out << "    }\n";
      cls_out << "    return ::omnibuf::read_string_view(raw(), " << fnum << "u).first;\n";
      cls_out << "  }\n";
      cls_out << "  [[nodiscard]] std::string get_" << f.name << "() const {\n";
      cls_out << "    return std::string(get_" << f.name << "_view());\n";
      cls_out << "  }\n";
      cls_out << "  [[nodiscard]] ::omnibuf::Result<std::string_view> try_get_" << f.name
              << "_view() const {\n";
      cls_out << "    if (is_dirty(" << fnum << "u)) {\n";
      if (is_oneof) {
        const auto& ro = plan.real_oneofs[plan.oneof_idx_to_plan.at(*f.oneof_index)];
        cls_out << "      if (dirty_scalars_.oneof_" << ro.oneof_index << "_active_ != " << fnum
                << "u) return std::string_view{};\n";
      }
      cls_out << "      if (!::omnibuf::is_valid_utf8(" << heap_ref
              << ")) return ::omnibuf::Error{\"omnibuf: invalid UTF-8 in WireString\"};\n";
      cls_out << "      return std::string_view(" << heap_ref << ");\n";
      cls_out << "    }\n";
      cls_out << "    auto r = ::omnibuf::try_read_string_view(raw(), " << fnum << "u);\n";
      cls_out << "    if (!r) return ::omnibuf::Error{r.error()};\n";
      cls_out << "    return r.value().value_or(std::string_view{});\n";
      cls_out << "  }\n";
      cls_out << "  [[nodiscard]] ::omnibuf::Result<std::string> try_get_" << f.name
              << "() const {\n";
      cls_out << "    auto r = try_get_" << f.name << "_view();\n";
      cls_out << "    if (!r) return ::omnibuf::Error{r.error()};\n";
      cls_out << "    return std::string(r.value());\n";
      cls_out << "  }\n";
      cls_out << "  void set_" << f.name << "(std::string v) {\n";
      emit_dirty_oneof_siblings(cls_out, "    ");
      cls_out << "    " << heap_mut << " = std::move(v);\n";
      cls_out << "    mark_dirty(" << fnum << "u);\n";
      cls_out << "  }\n\n";
      continue;
    }

    if (f.type == FieldType::Bytes) {
      cls_out << "  [[nodiscard]] std::span<const uint8_t> get_" << f.name
              << "_view() const noexcept {\n";
      cls_out << "    if (is_dirty(" << fnum << "u)) {\n";
      if (is_oneof) {
        const auto& ro = plan.real_oneofs[plan.oneof_idx_to_plan.at(*f.oneof_index)];
        cls_out << "      if (dirty_scalars_.oneof_" << ro.oneof_index << "_active_ != " << fnum
                << "u) return {};\n";
      }
      cls_out << "      return " << heap_ref << ";\n";
      cls_out << "    }\n";
      cls_out << "    return ::omnibuf::read_bytes_view(raw(), " << fnum << "u).first;\n";
      cls_out << "  }\n";
      cls_out << "  [[nodiscard]] std::vector<uint8_t> get_" << f.name << "() const {\n";
      cls_out << "    const auto sp = get_" << f.name << "_view();\n";
      cls_out << "    return std::vector<uint8_t>(sp.begin(), sp.end());\n";
      cls_out << "  }\n";
      cls_out << "  [[nodiscard]] ::omnibuf::Result<std::vector<uint8_t>> try_get_" << f.name
              << "() const {\n";
      cls_out << "    if (is_dirty(" << fnum << "u)) {\n";
      if (is_oneof) {
        const auto& ro = plan.real_oneofs[plan.oneof_idx_to_plan.at(*f.oneof_index)];
        cls_out << "      if (dirty_scalars_.oneof_" << ro.oneof_index << "_active_ != " << fnum
                << "u) return std::vector<uint8_t>{};\n";
      }
      cls_out << "      return " << heap_ref << ";\n";
      cls_out << "    }\n";
      cls_out << "    auto r = ::omnibuf::try_read_bytes_view(raw(), " << fnum << "u);\n";
      cls_out << "    if (!r) return ::omnibuf::Error{r.error()};\n";
      cls_out << "    if (!r.value().has_value()) return std::vector<uint8_t>{};\n";
      cls_out << "    return std::vector<uint8_t>(r.value()->begin(), r.value()->end());\n";
      cls_out << "  }\n";
      cls_out << "  void set_" << f.name << "(std::vector<uint8_t> v) {\n";
      emit_dirty_oneof_siblings(cls_out, "    ");
      cls_out << "    " << heap_mut << " = std::move(v);\n";
      cls_out << "    mark_dirty(" << fnum << "u);\n";
      cls_out << "  }\n\n";
      continue;
    }

    cls_out << "  [[nodiscard]] " << cpp_t << " get_" << f.name << "() const noexcept {\n";
    cls_out << "    if (is_dirty(" << fnum << "u)) {\n";
    if (is_oneof) {
      const auto& ro = plan.real_oneofs[plan.oneof_idx_to_plan.at(*f.oneof_index)];
      cls_out << "      if (dirty_scalars_.oneof_" << ro.oneof_index << "_active_ != " << fnum
              << "u) return " << scalar_default_literal(f, catalog) << ";\n";
    }
    cls_out << "      return " << scalar_ref << ";\n";
    cls_out << "    }\n";
    switch (f.type) {
      case FieldType::Uint32:
      case FieldType::Uint64:
        cls_out << "    return static_cast<" << cpp_t << ">(::omnibuf::read_uint(raw(), " << fnum
                << "u).first);\n";
        break;
      case FieldType::Int32:
      case FieldType::Sint32:
      case FieldType::Int64:
      case FieldType::Sint64:
        cls_out << "    return static_cast<" << cpp_t << ">(::omnibuf::read_sint(raw(), " << fnum
                << "u).first);\n";
        break;
      case FieldType::Enum:
        cls_out << "    return static_cast<" << cpp_t
                << ">(static_cast<int32_t>(::omnibuf::read_sint(raw(), " << fnum
                << "u).first));\n";
        break;
      case FieldType::Bool:
        cls_out << "    return ::omnibuf::read_bool(raw(), " << fnum << "u).first;\n";
        break;
      case FieldType::Fixed32:
        cls_out << "    return ::omnibuf::read_fixed32(raw(), " << fnum << "u).first;\n";
        break;
      case FieldType::Sfixed32:
        cls_out << "    return ::omnibuf::read_sfixed32(raw(), " << fnum << "u).first;\n";
        break;
      case FieldType::Float:
        cls_out << "    return ::omnibuf::read_float32(raw(), " << fnum << "u).first;\n";
        break;
      case FieldType::Fixed64:
        cls_out << "    return ::omnibuf::read_fixed64(raw(), " << fnum << "u).first;\n";
        break;
      case FieldType::Sfixed64:
        cls_out << "    return ::omnibuf::read_sfixed64(raw(), " << fnum << "u).first;\n";
        break;
      case FieldType::Double:
        cls_out << "    return ::omnibuf::read_float64(raw(), " << fnum << "u).first;\n";
        break;
      default:
        cls_out << "    return " << scalar_default_literal(f, catalog) << ";\n";
        break;
    }
    cls_out << "  }\n";

    cls_out << "  [[nodiscard]] ::omnibuf::Result<" << cpp_t << "> try_get_" << f.name
            << "() const {\n";
    cls_out << "    if (is_dirty(" << fnum << "u)) {\n";
    if (is_oneof) {
      const auto& ro = plan.real_oneofs[plan.oneof_idx_to_plan.at(*f.oneof_index)];
      cls_out << "      if (dirty_scalars_.oneof_" << ro.oneof_index << "_active_ != " << fnum
              << "u) return " << scalar_default_literal(f, catalog) << ";\n";
    }
    cls_out << "      return " << scalar_ref << ";\n";
    cls_out << "    }\n";
    cls_out << "    auto r = ";
    emit_try_read_scalar_expr(cls_out);
    cls_out << ";\n";
    cls_out << "    if (!r) return ::omnibuf::Error{r.error()};\n";
    cls_out << "    if (!r.value().has_value()) return " << scalar_default_literal(f, catalog)
            << ";\n";
    if (f.type == FieldType::Enum) {
      cls_out << "    return static_cast<" << cpp_t << ">(static_cast<int32_t>(*r.value()));\n";
    } else {
      cls_out << "    return static_cast<" << cpp_t << ">(*r.value());\n";
    }
    cls_out << "  }\n";

    cls_out << "  void set_" << f.name << "(" << cpp_t << " v) noexcept {\n";
    emit_dirty_oneof_siblings(cls_out, "    ");
    cls_out << "    " << scalar_ref << " = v;\n";
    cls_out << "    mark_dirty(" << fnum << "u);\n";
    cls_out << "  }\n\n";
  }

  // try_marshal_into, marshal_into, try_marshal, marshal_append, marshal
  cls_out << "  [[nodiscard]] ::omnibuf::Result<void> try_marshal_into(std::vector<uint8_t>& dst) const {\n";
  cls_out << "    dst.clear();\n";
  cls_out << "    if (!any_dirty()) {\n";
  cls_out << "      auto vr = validate();\n";
  cls_out << "      if (!vr) return vr;\n";
  cls_out << "      ::omnibuf::copy_packet_append(dst, raw());\n";
  cls_out << "      return ::omnibuf::Result<void>::success();\n";
  cls_out << "    }\n";
  cls_out << "    return ::omnibuf::try_splice_fields_into(\n";
  cls_out << "        dst, raw(), std::span<const uint64_t>(dirty_), " << max_fnum << "u,\n";
  cls_out << "        [this](std::vector<uint8_t>& b, uint32_t fnum) -> const char* {\n";
  cls_out << "          return this->append_dirty_field_(b, fnum);\n";
  cls_out << "        });\n";
  cls_out << "  }\n\n";

  cls_out << "  void marshal_into(std::vector<uint8_t>& dst) const {\n";
  cls_out << "    dst.clear();\n";
  cls_out << "    if (!any_dirty()) {\n";
  cls_out << "      ::omnibuf::copy_packet_append(dst, raw());\n";
  cls_out << "      return;\n";
  cls_out << "    }\n";
  cls_out << "    ::omnibuf::splice_fields_into(\n";
  cls_out << "        dst, raw(), std::span<const uint64_t>(dirty_), " << max_fnum << "u,\n";
  cls_out << "        [this](std::vector<uint8_t>& b, uint32_t fnum) -> const char* {\n";
  cls_out << "          return this->append_dirty_field_(b, fnum);\n";
  cls_out << "        });\n";
  cls_out << "  }\n\n";

  cls_out << "  [[nodiscard]] ::omnibuf::Result<std::vector<uint8_t>> try_marshal() const {\n";
  cls_out << "    std::vector<uint8_t> dst;\n";
  cls_out << "    auto res = try_marshal_into(dst);\n";
  cls_out << "    if (!res) return ::omnibuf::Error{res.error()};\n";
  cls_out << "    return dst;\n";
  cls_out << "  }\n\n";

  cls_out << "  [[nodiscard]] std::vector<uint8_t> marshal_append(std::vector<uint8_t> dst = {}) const {\n";
  cls_out << "    marshal_into(dst);\n";
  cls_out << "    return dst;\n";
  cls_out << "  }\n\n";

  cls_out << "  [[nodiscard]] std::vector<uint8_t> marshal() const {\n";
  cls_out << "    return marshal_append();\n";
  cls_out << "  }\n\n";

  cls_out << " private:\n";
  cls_out << "  [[nodiscard]] const char* append_dirty_field_(std::vector<uint8_t>& b, uint32_t fnum) const;\n\n";

  out_of_line_out << "inline const char* " << cls
                  << "::append_dirty_field_(std::vector<uint8_t>& b, uint32_t fnum) const {\n";
  out_of_line_out << "  (void)this;\n";
  out_of_line_out << "  ::omnibuf::FastWriter w(b, 128);\n";
  out_of_line_out << "  switch (fnum) {\n";
  for (size_t idx = 0; idx < msg.fields.size(); ++idx) {
    const auto& f = msg.fields[idx];
    const std::string dirty_var = "f_" + f.name + "_";
    const bool use_heap = is_heap_dirty_field(idx);
    const std::string member_expr =
        use_heap ? ("dirty_heap_->" + dirty_var) : ("dirty_scalars_." + dirty_var);
    const bool is_opt_scalar = is_explicit_optional_scalar(f, rec.is_proto2);
    std::string oneof_guard;
    if (is_real_oneof_field(f)) {
      const auto& ro = plan.real_oneofs[plan.oneof_idx_to_plan.at(*f.oneof_index)];
      oneof_guard = "dirty_scalars_.oneof_" + std::to_string(ro.oneof_index) + "_active_ == " +
                    std::to_string(f.number) + "u";
    }
    out_of_line_out << "    case " << f.number << "u: {\n";
    if (use_heap) {
      out_of_line_out << "      if (!dirty_heap_) break;\n";
    }
    emit_try_encode_field_fast(out_of_line_out, f, member_expr, is_opt_scalar, oneof_guard, catalog,
                               "w", "      ");
    out_of_line_out << "      break;\n";
    out_of_line_out << "    }\n";
  }
  out_of_line_out << "    default:\n";
  out_of_line_out << "      break;\n";
  out_of_line_out << "  }\n";
  out_of_line_out << "  w.finish();\n";
  out_of_line_out << "  return nullptr;\n";
  out_of_line_out << "}\n\n";

  cls_out << "  [[nodiscard]] bool is_dirty(uint32_t fnum) const noexcept {\n";
  cls_out << "    const size_t w = fnum >> 6;\n";
  cls_out << "    return w < dirty_.size() && ((dirty_[w] >> (fnum & 63u)) & 1u) != 0;\n";
  cls_out << "  }\n";
  cls_out << "  void mark_dirty(uint32_t fnum) noexcept {\n";
  cls_out << "    const size_t w = fnum >> 6;\n";
  cls_out << "    if (w < dirty_.size()) dirty_[w] |= (uint64_t{1} << (fnum & 63u));\n";
  cls_out << "  }\n";
  cls_out << "  [[nodiscard]] bool any_dirty() const noexcept {\n";
  cls_out << "    for (uint64_t w : dirty_) {\n";
  cls_out << "      if (w != 0) return true;\n";
  cls_out << "    }\n";
  cls_out << "    return false;\n";
  cls_out << "  }\n\n";

  cls_out << "  struct DirtyScalars {\n";
  for (size_t idx = 0; idx < msg.fields.size(); ++idx) {
    if (is_heap_dirty_field(idx)) continue;
    const auto& f = msg.fields[idx];
    cls_out << "    " << scalar_cpp_type(f, catalog) << " f_" << f.name << "_ = "
            << scalar_default_literal(f, catalog) << ";\n";
  }
  for (const auto& ro : plan.real_oneofs) {
    cls_out << "    uint32_t oneof_" << ro.oneof_index << "_active_ = 0u;\n";
  }
  cls_out << "  };\n\n";

  cls_out << "  struct DirtyHeap {\n";
  for (size_t idx = 0; idx < msg.fields.size(); ++idx) {
    if (!is_heap_dirty_field(idx)) continue;
    const auto& f = msg.fields[idx];
    const std::string dirty_var = "f_" + f.name + "_";
    const bool is_repeated = (f.label == FieldLabel::Repeated);
    const bool is_opt_scalar = is_explicit_optional_scalar(f, rec.is_proto2);
    if (f.type == FieldType::Message) {
      const std::string target = trim_leading_dot(f.type_name);
      auto map_it = catalog.map_entries.find(target);
      if (map_it != catalog.map_entries.end()) {
        cls_out << "    " << map_cpp_type(map_it->second, catalog) << " " << dirty_var << "{};\n";
      } else if (is_repeated) {
        cls_out << "    std::vector<" << resolve_cpp_message(f.type_name, catalog) << "> "
                << dirty_var << "{};\n";
      } else if (plan.field_uses_box[idx]) {
        cls_out << "    ::omnibuf::Box<" << resolve_cpp_message(f.type_name, catalog) << "> "
                << dirty_var << "{};\n";
      } else {
        cls_out << "    std::optional<" << resolve_cpp_message(f.type_name, catalog) << "> "
                << dirty_var << "{};\n";
      }
    } else if (is_repeated) {
      cls_out << "    std::vector<" << scalar_cpp_type(f, catalog) << "> " << dirty_var << "{};\n";
    } else if (is_opt_scalar) {
      cls_out << "    std::optional<" << scalar_cpp_type(f, catalog) << "> " << dirty_var
              << "{};\n";
    } else {
      cls_out << "    " << scalar_cpp_type(f, catalog) << " " << dirty_var << "{};\n";
    }
  }
  cls_out << "  };\n\n";

  cls_out << "  DirtyHeap& ensure_dirty_heap() {\n";
  cls_out << "    if (!dirty_heap_) {\n";
  cls_out << "      dirty_heap_ = std::make_unique<DirtyHeap>();\n";
  cls_out << "    }\n";
  cls_out << "    return *dirty_heap_;\n";
  cls_out << "  }\n\n";

  cls_out << "  std::span<const uint8_t> raw_view_{};\n";
  cls_out << "  std::unique_ptr<std::vector<uint8_t>> owned_raw_{};\n";
  cls_out << "  std::array<uint64_t, " << dirty_words << "> dirty_{};\n";
  cls_out << "  DirtyScalars dirty_scalars_{};\n";
  cls_out << "  std::unique_ptr<DirtyHeap> dirty_heap_{};\n";
  cls_out << "};\n\n";
}

GeneratedFile generate_header_for_file(const FileDescriptorProto& file,
                                       const TypeCatalog& catalog,
                                       std::unordered_set<std::string>& defined_messages) {
  std::ostringstream out;
  out << "// Code generated by protoc-gen-omnibuf-cpp. DO NOT EDIT.\n";
  out << "// source: " << file.name << "\n\n";
  out << "#pragma once\n\n";
  out << "#include <array>\n";
  out << "#include <cstddef>\n";
  out << "#include <cstdint>\n";
  out << "#include <map>\n";
  out << "#include <memory>\n";
  out << "#include <optional>\n";
  out << "#include <set>\n";
  out << "#include <span>\n";
  out << "#include <stdexcept>\n";
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
    emit_enum(out, e, "");
  }

  const auto sorted_top = topo_sort_siblings(file.package, file.message_types, catalog);
  for (const DescriptorProto* m : sorted_top) {
    out << "struct " << sanitize_basic_ident(m->name) << ";\n";
  }
  if (!sorted_top.empty()) {
    out << "\n";
  }

  std::vector<MessagePlan> ordered_plans;
  std::vector<std::string> pending_defined;
  for (const DescriptorProto* m : sorted_top) {
    emit_struct_declaration_recursive(out, file.package, *m, catalog, defined_messages,
                                      pending_defined, ordered_plans, "");
  }

  for (const auto& plan : ordered_plans) {
    emit_struct_methods(out, plan, catalog);
  }

  for (const auto& plan : ordered_plans) {
    out << "class " << plan.rec->opaque_ident << ";\n";
  }
  if (!ordered_plans.empty()) {
    out << "\n";
  }

  std::ostringstream opaque_classes_out;
  std::ostringstream opaque_methods_out;
  for (const auto& plan : ordered_plans) {
    emit_opaque_class(opaque_classes_out, opaque_methods_out, plan, catalog);
  }
  out << opaque_classes_out.str();
  out << opaque_methods_out.str();

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
    if (n < 0) {
      if (errno == EINTR) continue;
      break;
    }
    if (n == 0) break;
    buf.insert(buf.end(), chunk, chunk + n);
  }
  return buf;
}

bool write_all_stdout(std::span<const uint8_t> data) {
  size_t written = 0;
  while (written < data.size()) {
    const ssize_t n = ::write(STDOUT_FILENO, data.data() + written, data.size() - written);
    if (n < 0) {
      if (errno == EINTR) continue;
      return false;
    }
    if (n == 0) return false;
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
  const std::unordered_set<std::string> targets(req.files_to_generate.begin(),
                                                req.files_to_generate.end());

  for (const auto& file : req.proto_files) {
    if (!targets.contains(file.name)) continue;
    if (auto err = validate_target_file(file)) {
      const auto err_bytes = omnibuf::proto::encode_code_generator_response({}, *err);
      (void)write_all_stdout(err_bytes);
      return 0;
    }
  }

  const TypeCatalog catalog = build_catalog(req);

  // Messages from imported non-target files are already defined via #include headers.
  std::unordered_set<std::string> defined_messages;
  for (const auto& file : req.proto_files) {
    if (targets.contains(file.name)) continue;
    for (const auto& [full_name, _] : catalog.messages) {
      if (file.package.empty() || full_name.rfind(file.package + ".", 0) == 0) {
        defined_messages.insert(full_name);
      }
    }
  }

  std::vector<GeneratedFile> outputs;
  outputs.reserve(req.files_to_generate.size());
  for (const auto& file : req.proto_files) {
    if (!targets.contains(file.name)) continue;
    std::unordered_set<std::string> file_defined = defined_messages;
    outputs.push_back(generate_header_for_file(file, catalog, file_defined));
    defined_messages.insert(file_defined.begin(), file_defined.end());
  }

  const auto resp_bytes = omnibuf::proto::encode_code_generator_response(outputs);
  if (!write_all_stdout(resp_bytes)) {
    return 1;
  }
  return 0;
}
