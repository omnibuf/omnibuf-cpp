#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

#include "omnibuf/varint.hpp"

namespace omnibuf {

// Strict RFC 3629 UTF-8 validator with fast 8-byte ASCII scanning.
// Rejects overlong sequences, UTF-16 surrogates (U+D800..U+DFFF), code points > U+10FFFF,
// stray continuation bytes, and truncated multi-byte sequences.
[[nodiscard]] inline bool is_valid_utf8(std::span<const uint8_t> b) noexcept {
  const uint8_t* p = b.data();
  const size_t n = b.size();
  size_t i = 0;

  while (i + 8 <= n) {
    const uint64_t w = read_u64_le(p + i);
    if ((w & 0x8080808080808080ULL) == 0) {
      i += 8;
      continue;
    }
    break;
  }

  while (i < n) {
    const uint8_t b0 = p[i];
    if (b0 < 0x80u) {
      ++i;
      continue;
    }
    if (b0 < 0xC2u) {
      // 0x80..0xBF continuation byte or 0xC0..0xC1 overlong 2-byte starter
      return false;
    }
    if (b0 <= 0xDFu) {
      if (i + 2 > n) return false;
      const uint8_t b1 = p[i + 1];
      if ((b1 & 0xC0u) != 0x80u) return false;
      i += 2;
      continue;
    }
    if (b0 <= 0xEFu) {
      if (i + 3 > n) return false;
      const uint8_t b1 = p[i + 1];
      const uint8_t b2 = p[i + 2];
      if ((b2 & 0xC0u) != 0x80u) return false;
      if (b0 == 0xE0u) {
        if (b1 < 0xA0u || b1 > 0xBFu) return false;
      } else if (b0 == 0xEDu) {
        if (b1 < 0x80u || b1 > 0x9Fu) return false;
      } else {
        if ((b1 & 0xC0u) != 0x80u) return false;
      }
      i += 3;
      continue;
    }
    if (b0 <= 0xF4u) {
      if (i + 4 > n) return false;
      const uint8_t b1 = p[i + 1];
      const uint8_t b2 = p[i + 2];
      const uint8_t b3 = p[i + 3];
      if ((b2 & 0xC0u) != 0x80u || (b3 & 0xC0u) != 0x80u) return false;
      if (b0 == 0xF0u) {
        if (b1 < 0x90u || b1 > 0xBFu) return false;
      } else if (b0 == 0xF4u) {
        if (b1 < 0x80u || b1 > 0x8Fu) return false;
      } else {
        if ((b1 & 0xC0u) != 0x80u) return false;
      }
      i += 4;
      continue;
    }
    return false;
  }
  return true;
}

[[nodiscard]] inline bool is_valid_utf8(std::string_view s) noexcept {
  return is_valid_utf8(std::span<const uint8_t>(
      reinterpret_cast<const uint8_t*>(s.data()), s.size()));
}

}  // namespace omnibuf
