#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

#include "omnibuf/decoder.hpp"
#include "omnibuf/encoder.hpp"
#include "omnibuf/utf8.hpp"
#include "omnibuf/varint.hpp"

namespace omnibuf {

// Inline Field #0 schema metadata (Tag = 0x19, WireSubMsg = 12, omnibuf.v1.SchemaMeta).
struct SchemaMeta {
  std::string type_name;
  std::map<uint32_t, std::string> fields;
  std::map<uint32_t, std::shared_ptr<SchemaMeta>> children;
};

namespace detail {

inline std::shared_ptr<SchemaMeta> merge_schema_meta_impl(const std::shared_ptr<SchemaMeta>& base,
                                                          const std::shared_ptr<SchemaMeta>& next,
                                                          int depth,
                                                          std::set<const SchemaMeta*>& active) {
  if (!next || depth > MAX_CONTAINER_DEPTH) return base;
  if (!active.insert(next.get()).second) {
    return base;
  }
  const bool inserted_base = base ? active.insert(base.get()).second : false;
  auto out = std::make_shared<SchemaMeta>();
  if (base) {
    out->type_name = base->type_name;
    out->fields = base->fields;
    if (inserted_base && depth + 2 <= MAX_CONTAINER_DEPTH) {
      for (const auto& [k, c] : base->children) {
        if (c) out->children[k] = merge_schema_meta_impl(nullptr, c, depth + 2, active);
      }
    }
  }
  if (!next->type_name.empty()) {
    out->type_name = next->type_name;
  }
  for (const auto& [k, name] : next->fields) {
    out->fields[k] = name;
  }
  if (depth + 2 <= MAX_CONTAINER_DEPTH) {
    for (const auto& [k, c] : next->children) {
      if (!c) continue;
      auto it = out->children.find(k);
      if (it != out->children.end()) {
        it->second = merge_schema_meta_impl(it->second, c, depth + 2, active);
      } else {
        out->children[k] = merge_schema_meta_impl(nullptr, c, depth + 2, active);
      }
    }
  }
  if (inserted_base) active.erase(base.get());
  active.erase(next.get());
  return out;
}

inline void merge_schema_meta_in_place_impl(SchemaMeta& dst,
                                            SchemaMeta& next,
                                            int depth,
                                            std::set<const SchemaMeta*>& active) {
  if (&dst == &next || depth > MAX_CONTAINER_DEPTH) return;
  if (!active.insert(&next).second) {
    return;
  }
  struct ActiveGuard {
    std::set<const SchemaMeta*>& set;
    const SchemaMeta* ptr;
    ~ActiveGuard() { set.erase(ptr); }
  } next_guard{active, &next};

  if (!next.type_name.empty()) {
    dst.type_name = std::move(next.type_name);
  }
  for (auto& [k, name] : next.fields) {
    dst.fields[k] = std::move(name);
  }
  if (depth + 2 <= MAX_CONTAINER_DEPTH) {
    for (auto& [k, c] : next.children) {
      if (!c) continue;
      auto it = dst.children.find(k);
      if (it != dst.children.end() && it->second) {
        merge_schema_meta_in_place_impl(*it->second, *c, depth + 2, active);
      } else {
        dst.children[k] = std::move(c);
      }
    }
  }
}

inline void append_schema_meta_body_impl(std::vector<uint8_t>& dst,
                                         const SchemaMeta& meta,
                                         int depth,
                                         std::set<const SchemaMeta*>& active) {
  if (depth > MAX_CONTAINER_DEPTH) {
    throw std::invalid_argument("omnibuf: SchemaMeta nesting depth exceeds MAX_CONTAINER_DEPTH");
  }
  if (!active.insert(&meta).second) {
    throw std::invalid_argument("omnibuf: cyclic SchemaMeta graph");
  }
  struct ActiveGuard {
    std::set<const SchemaMeta*>& set;
    const SchemaMeta* ptr;
    ~ActiveGuard() { set.erase(ptr); }
  } guard{active, &meta};

  // Field #1: type_name (WireString = 10 -> Tag (1 << 4) | 10 = 26 -> 0x35)
  if (!meta.type_name.empty()) {
    if (!is_valid_utf8(meta.type_name)) {
      throw std::invalid_argument("omnibuf: invalid UTF-8 in SchemaMeta type_name");
    }
    dst.push_back(0x35u);
    append_varint(dst, meta.type_name.size());
    dst.insert(dst.end(), meta.type_name.begin(), meta.type_name.end());
  }
  // Field #2: fields map<uint32, string> (WireMap = 14 -> Tag (2 << 4) | 14 = 46 -> 0x5D)
  if (!meta.fields.empty()) {
    if (depth + 1 > MAX_CONTAINER_DEPTH) {
      throw std::invalid_argument("omnibuf: SchemaMeta fields map exceeds MAX_CONTAINER_DEPTH");
    }
    dst.push_back(0x5Du);
    const size_t len_pos = dst.size();
    dst.push_back(0);
    dst.push_back(0);
    dst.push_back(WireUint);
    dst.push_back(WireString);
    append_varint(dst, meta.fields.size());
    for (const auto& [k, name] : meta.fields) {
      if (k == 0 || k > MAX_FIELD_NUMBER) {
        throw std::invalid_argument("omnibuf: SchemaMeta field number out of range");
      }
      if (!is_valid_utf8(name)) {
        throw std::invalid_argument("omnibuf: invalid UTF-8 in SchemaMeta field name");
      }
      append_varint(dst, k);
      append_varint(dst, name.size());
      dst.insert(dst.end(), name.begin(), name.end());
    }
    backpatch_container_len(dst, len_pos);
  }
  // Field #3: children map<uint32, SchemaMeta> (WireMap = 14 -> Tag (3 << 4) | 14 = 62 -> 0x7D)
  if (!meta.children.empty()) {
    if (depth + 1 > MAX_CONTAINER_DEPTH) {
      throw std::invalid_argument("omnibuf: SchemaMeta children map exceeds MAX_CONTAINER_DEPTH");
    }
    dst.push_back(0x7Du);
    const size_t len_pos = dst.size();
    dst.push_back(0);
    dst.push_back(0);
    dst.push_back(WireUint);
    dst.push_back(WireSubMsg);
    append_varint(dst, meta.children.size());
    for (const auto& [k, child] : meta.children) {
      if (k == 0 || k > MAX_FIELD_NUMBER) {
        throw std::invalid_argument("omnibuf: SchemaMeta child field number out of range");
      }
      append_varint(dst, k);
      const size_t c_len_pos = dst.size();
      dst.push_back(0);
      dst.push_back(0);
      if (child) {
        append_schema_meta_body_impl(dst, *child, depth + 2, active);
      }
      backpatch_container_len(dst, c_len_pos);
    }
    backpatch_container_len(dst, len_pos);
  }
}

}  // namespace detail

inline std::shared_ptr<SchemaMeta> merge_schema_meta(const std::shared_ptr<SchemaMeta>& base,
                                                     const std::shared_ptr<SchemaMeta>& next) {
  std::set<const SchemaMeta*> active;
  return detail::merge_schema_meta_impl(base, next, 1, active);
}

inline void merge_schema_meta_in_place(std::shared_ptr<SchemaMeta>& dst,
                                       std::shared_ptr<SchemaMeta> next) {
  if (!next) return;
  if (!dst) {
    dst = std::move(next);
    return;
  }
  if (dst.get() == next.get()) return;
  std::set<const SchemaMeta*> active;
  detail::merge_schema_meta_in_place_impl(*dst, *next, 1, active);
}

inline void append_schema_meta_body(std::vector<uint8_t>& dst, const SchemaMeta& meta) {
  std::set<const SchemaMeta*> active;
  detail::append_schema_meta_body_impl(dst, meta, 1, active);
}

inline void append_field0_schema_meta(std::vector<uint8_t>& dst, const SchemaMeta& meta) {
  const size_t orig_size = dst.size();
  try {
    // Tag = (0 << 4) | WireSubMsg(12) = 12 -> (12 << 1) | 1 = 0x19
    dst.push_back(0x19u);
    const size_t len_pos = dst.size();
    dst.push_back(0);
    dst.push_back(0);
    append_schema_meta_body(dst, meta);
    backpatch_container_len(dst, len_pos);
  } catch (...) {
    dst.resize(orig_size);
    throw;
  }
}

[[nodiscard]] inline Result<void> try_append_field0_schema_meta(std::vector<uint8_t>& dst,
                                                                const SchemaMeta& meta) {
  try {
    append_field0_schema_meta(dst, meta);
    return Result<void>::success();
  } catch (const std::invalid_argument& e) {
    return Error{e.what()};
  }
}

[[nodiscard]] inline std::vector<uint8_t> encode_field0_schema_meta(const SchemaMeta& meta) {
  std::vector<uint8_t> out;
  out.reserve(128);
  append_field0_schema_meta(out, meta);
  return out;
}

[[nodiscard]] inline Result<std::shared_ptr<SchemaMeta>> decode_schema_meta(
    std::span<const uint8_t> data,
    int depth,
    DecodeState& state) {
  if (depth > state.opts.max_depth) {
    return Error{"omnibuf: SchemaMeta nesting depth exceeds max_depth"};
  }
  if (data.size() > state.opts.max_message_bytes) {
    return Error{"omnibuf: SchemaMeta payload exceeds max_message_bytes"};
  }
  auto meta = std::make_shared<SchemaMeta>();
  size_t pos = 0;
  const size_t n = data.size();

  while (pos < n) {
    const auto [tag, tn] = decode_varint(data.subspan(pos));
    if (tn == 0 || (tag >> 4) > MAX_FIELD_NUMBER) {
      return Error{"omnibuf: invalid or truncated tag in Field #0 SchemaMeta"};
    }
    pos += tn;
    const uint32_t fnum = static_cast<uint32_t>(tag >> 4);
    const uint8_t wt = static_cast<uint8_t>(tag & 0x0Fu);

    switch (fnum) {
      case 1: {
        if (wt != WireString) {
          return Error{"omnibuf: SchemaMeta field #1 (type_name) must be WireString"};
        }
        auto s_res = read_str_slice(data, pos);
        if (!s_res) return Error{s_res.error()};
        const auto sp = s_res.value().slice;
        meta->type_name.assign(reinterpret_cast<const char*>(sp.data()), sp.size());
        pos = s_res.value().next_pos;
        break;
      }
      case 2: {
        if (wt != WireMap) {
          return Error{"omnibuf: SchemaMeta field #2 (fields) must be WireMap"};
        }
        if (depth + 1 > state.opts.max_depth) {
          return Error{"omnibuf: SchemaMeta fields map exceeds max_depth"};
        }
        auto body_res = read_len_slice(data, pos, 3);
        if (!body_res) return Error{body_res.error()};
        pos = body_res.value().next_pos;
        const auto body = body_res.value().slice;
        const auto hdr = parse_map_header(body);
        if (!hdr.ok || hdr.key_wt != WireUint || hdr.val_wt != WireString) {
          return Error{"omnibuf: invalid SchemaMeta fields map header"};
        }
        if (!state.charge_elements(hdr.count)) {
          return Error{"omnibuf: SchemaMeta fields map exceeds max_total_elements"};
        }
        size_t p = hdr.pos;
        for (size_t i = 0; i < hdr.count; ++i) {
          const auto [k64, kn] = decode_varint(body.subspan(p));
          if (kn == 0 || k64 == 0 || k64 > MAX_FIELD_NUMBER) {
            return Error{"omnibuf: invalid SchemaMeta field number"};
          }
          p += kn;
          auto name_res = read_str_slice(body, p);
          if (!name_res) return Error{name_res.error()};
          const auto sp = name_res.value().slice;
          meta->fields[static_cast<uint32_t>(k64)] =
              std::string(reinterpret_cast<const char*>(sp.data()), sp.size());
          p = name_res.value().next_pos;
        }
        if (p != body.size()) {
          return Error{"omnibuf: SchemaMeta fields map trailing bytes"};
        }
        break;
      }
      case 3: {
        if (wt != WireMap) {
          return Error{"omnibuf: SchemaMeta field #3 (children) must be WireMap"};
        }
        if (depth + 1 > state.opts.max_depth) {
          return Error{"omnibuf: SchemaMeta children map exceeds max_depth"};
        }
        auto body_res = read_len_slice(data, pos, 3);
        if (!body_res) return Error{body_res.error()};
        pos = body_res.value().next_pos;
        const auto body = body_res.value().slice;
        const auto hdr = parse_map_header(body);
        if (!hdr.ok || hdr.key_wt != WireUint || hdr.val_wt != WireSubMsg) {
          return Error{"omnibuf: invalid SchemaMeta children map header"};
        }
        if (!state.charge_elements(hdr.count)) {
          return Error{"omnibuf: SchemaMeta children map exceeds max_total_elements"};
        }
        std::map<uint32_t, std::shared_ptr<SchemaMeta>> map_children;
        size_t p = hdr.pos;
        for (size_t i = 0; i < hdr.count; ++i) {
          const auto [k64, kn] = decode_varint(body.subspan(p));
          if (kn == 0 || k64 == 0 || k64 > MAX_FIELD_NUMBER) {
            return Error{"omnibuf: invalid SchemaMeta child field number"};
          }
          p += kn;
          auto child_slice = read_len_slice(body, p, 0);
          if (!child_slice) return Error{child_slice.error()};
          auto child_meta = decode_schema_meta(child_slice.value().slice, depth + 2, state);
          if (!child_meta) return Error{child_meta.error()};
          // Duplicate keys within a single WireMap replace the earlier entry.
          map_children[static_cast<uint32_t>(k64)] = std::move(child_meta.value());
          p = child_slice.value().next_pos;
        }
        if (p != body.size()) {
          return Error{"omnibuf: SchemaMeta children map trailing bytes"};
        }
        for (auto& [key_num, child_meta] : map_children) {
          auto it = meta->children.find(key_num);
          if (it != meta->children.end()) {
            merge_schema_meta_in_place(it->second, std::move(child_meta));
          } else {
            meta->children.emplace(key_num, std::move(child_meta));
          }
        }
        break;
      }
      default: {
        if (fnum == 0) {
          return Error{"omnibuf: invalid field #0 inside SchemaMeta"};
        }
        auto skip_res = validate_value(data, pos, wt, depth, state);
        if (!skip_res) return Error{skip_res.error()};
        pos = skip_res.value();
        break;
      }
    }
  }
  return meta;
}

[[nodiscard]] inline Result<std::shared_ptr<SchemaMeta>> decode_schema_meta(
    std::span<const uint8_t> data,
    int depth = 0) {
  DecodeState state{};
  return decode_schema_meta(data, depth, state);
}

[[nodiscard]] inline Result<void> validate_field0_slice(std::span<const uint8_t> data,
                                                        int depth,
                                                        DecodeState& state) {
  if (depth > state.opts.max_depth) {
    return Error{"omnibuf: SchemaMeta nesting depth exceeds max_depth"};
  }
  if (data.size() > state.opts.max_message_bytes) {
    return Error{"omnibuf: SchemaMeta payload exceeds max_message_bytes"};
  }
  size_t pos = 0;
  const size_t n = data.size();

  while (pos < n) {
    const auto [tag, tn] = decode_varint(data.subspan(pos));
    if (tn == 0 || (tag >> 4) > MAX_FIELD_NUMBER) {
      return Error{"omnibuf: invalid or truncated tag in Field #0 SchemaMeta"};
    }
    pos += tn;
    const uint32_t fnum = static_cast<uint32_t>(tag >> 4);
    const uint8_t wt = static_cast<uint8_t>(tag & 0x0Fu);

    switch (fnum) {
      case 1: {
        if (wt != WireString) {
          return Error{"omnibuf: SchemaMeta field #1 (type_name) must be WireString"};
        }
        auto s_res = read_str_slice(data, pos);
        if (!s_res) return Error{s_res.error()};
        pos = s_res.value().next_pos;
        break;
      }
      case 2: {
        if (wt != WireMap) {
          return Error{"omnibuf: SchemaMeta field #2 (fields) must be WireMap"};
        }
        if (depth + 1 > state.opts.max_depth) {
          return Error{"omnibuf: SchemaMeta fields map exceeds max_depth"};
        }
        auto body_res = read_len_slice(data, pos, 3);
        if (!body_res) return Error{body_res.error()};
        pos = body_res.value().next_pos;
        const auto body = body_res.value().slice;
        const auto hdr = parse_map_header(body);
        if (!hdr.ok || hdr.key_wt != WireUint || hdr.val_wt != WireString) {
          return Error{"omnibuf: invalid SchemaMeta fields map header"};
        }
        if (!state.charge_elements(hdr.count)) {
          return Error{"omnibuf: SchemaMeta fields map exceeds max_total_elements"};
        }
        size_t p = hdr.pos;
        for (size_t i = 0; i < hdr.count; ++i) {
          const auto [k64, kn] = decode_varint(body.subspan(p));
          if (kn == 0 || k64 == 0 || k64 > MAX_FIELD_NUMBER) {
            return Error{"omnibuf: invalid SchemaMeta field number"};
          }
          p += kn;
          auto name_res = read_str_slice(body, p);
          if (!name_res) return Error{name_res.error()};
          p = name_res.value().next_pos;
        }
        if (p != body.size()) {
          return Error{"omnibuf: SchemaMeta fields map trailing bytes"};
        }
        break;
      }
      case 3: {
        if (wt != WireMap) {
          return Error{"omnibuf: SchemaMeta field #3 (children) must be WireMap"};
        }
        if (depth + 1 > state.opts.max_depth) {
          return Error{"omnibuf: SchemaMeta children map exceeds max_depth"};
        }
        auto body_res = read_len_slice(data, pos, 3);
        if (!body_res) return Error{body_res.error()};
        pos = body_res.value().next_pos;
        const auto body = body_res.value().slice;
        const auto hdr = parse_map_header(body);
        if (!hdr.ok || hdr.key_wt != WireUint || hdr.val_wt != WireSubMsg) {
          return Error{"omnibuf: invalid SchemaMeta children map header"};
        }
        if (!state.charge_elements(hdr.count)) {
          return Error{"omnibuf: SchemaMeta children map exceeds max_total_elements"};
        }
        size_t p = hdr.pos;
        for (size_t i = 0; i < hdr.count; ++i) {
          const auto [k64, kn] = decode_varint(body.subspan(p));
          if (kn == 0 || k64 == 0 || k64 > MAX_FIELD_NUMBER) {
            return Error{"omnibuf: invalid SchemaMeta child field number"};
          }
          p += kn;
          auto child_slice = read_len_slice(body, p, 0);
          if (!child_slice) return Error{child_slice.error()};
          auto child_ok = validate_field0_slice(child_slice.value().slice, depth + 2, state);
          if (!child_ok) return Error{child_ok.error()};
          p = child_slice.value().next_pos;
        }
        if (p != body.size()) {
          return Error{"omnibuf: SchemaMeta children map trailing bytes"};
        }
        break;
      }
      default: {
        if (fnum == 0) {
          return Error{"omnibuf: invalid field #0 inside SchemaMeta"};
        }
        auto skip_res = validate_value(data, pos, wt, depth, state);
        if (!skip_res) return Error{skip_res.error()};
        pos = skip_res.value();
        break;
      }
    }
  }
  return Result<void>::success();
}

[[nodiscard]] inline Result<size_t> validate_field0(std::span<const uint8_t> data,
                                                    size_t pos,
                                                    uint8_t wt,
                                                    int depth,
                                                    DecodeState& state) {
  if (wt != WireSubMsg) {
    return Error{"omnibuf: Field #0 must use WireSubMsg (12)"};
  }
  auto body_res = read_len_slice(data, pos, 0);
  if (!body_res) return Error{body_res.error()};
  auto ok = validate_field0_slice(body_res.value().slice, depth + 1, state);
  if (!ok) return Error{ok.error()};
  return body_res.value().next_pos;
}

[[nodiscard]] inline Result<size_t> validate_field0(std::span<const uint8_t> data,
                                                    size_t pos,
                                                    uint8_t wt,
                                                    int depth) {
  DecodeState state{};
  return validate_field0(data, pos, wt, depth, state);
}

[[nodiscard]] inline Result<void> validate_message_bytes(std::span<const uint8_t> data,
                                                         int depth,
                                                         DecodeState& state) {
  if (depth > state.opts.max_depth) {
    return Error{"omnibuf: message nesting depth exceeds max_depth"};
  }
  if (data.size() > state.opts.max_message_bytes) {
    return Error{"omnibuf: message exceeds max_message_bytes"};
  }
  size_t pos = 0;
  const size_t n = data.size();
  while (pos < n) {
    const auto [tag, tn] = decode_varint(data.subspan(pos));
    if (tn == 0 || (tag >> 4) > MAX_FIELD_NUMBER) {
      return Error{"omnibuf: invalid or truncated field tag"};
    }
    pos += tn;
    const uint32_t fnum = static_cast<uint32_t>(tag >> 4);
    const uint8_t wt = static_cast<uint8_t>(tag & 0x0Fu);
    if (fnum == 0) {
      auto f0_res = validate_field0(data, pos, wt, depth, state);
      if (!f0_res) return Error{f0_res.error()};
      pos = f0_res.value();
      continue;
    }
    auto val_res = validate_value(data, pos, wt, depth, state);
    if (!val_res) return Error{val_res.error()};
    pos = val_res.value();
  }
  return Result<void>::success();
}

[[nodiscard]] inline Result<void> validate_message_bytes(std::span<const uint8_t> data, int depth) {
  DecodeState state{};
  return validate_message_bytes(data, depth, state);
}

// Decodes and merges all Field #0 SchemaMeta blocks in packet.
[[nodiscard]] inline std::optional<SchemaMeta> read_field0_schema_meta(
    std::span<const uint8_t> packet,
    DecodeOptions opts = {}) {
  DecodeState state{opts, 0};
  if (packet.size() > state.opts.max_message_bytes) {
    return std::nullopt;
  }
  std::shared_ptr<SchemaMeta> merged;
  size_t pos = 0;
  const size_t n = packet.size();
  while (pos < n) {
    const auto [tag, tn] = decode_varint(packet.subspan(pos));
    if (tn == 0 || (tag >> 4) > MAX_FIELD_NUMBER) {
      return std::nullopt;
    }
    pos += tn;
    const uint32_t fnum = static_cast<uint32_t>(tag >> 4);
    const uint8_t wt = static_cast<uint8_t>(tag & 0x0Fu);
    if (fnum == 0) {
      if (wt != WireSubMsg) return std::nullopt;
      auto body_res = read_len_slice(packet, pos, 0);
      if (!body_res) return std::nullopt;
      auto meta_res = decode_schema_meta(body_res.value().slice, 1, state);
      if (!meta_res) return std::nullopt;
      merge_schema_meta_in_place(merged, std::move(meta_res.value()));
      pos = body_res.value().next_pos;
      continue;
    }
    auto skip_res = validate_value(packet, pos, wt, 0, state);
    if (!skip_res) return std::nullopt;
    pos = skip_res.value();
  }
  if (!merged) return std::nullopt;
  return *merged;
}

}  // namespace omnibuf
