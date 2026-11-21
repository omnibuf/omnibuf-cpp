#pragma once

#if defined(__aarch64__) || defined(_M_ARM64)
#include <arm_neon.h>
#endif

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

#include "omnibuf/varint.hpp"

namespace omnibuf {

// Strict RFC 3629 UTF-8 validator with NEON 64B/16B and 64-bit word ASCII fast paths.
// Resumes vectorized ASCII scanning immediately after every valid multi-byte code point.
// Rejects overlong sequences, UTF-16 surrogates (U+D800..U+DFFF), code points > U+10FFFF,
// stray continuation bytes, and truncated multi-byte sequences.
[[nodiscard]] inline bool is_valid_utf8(std::span<const uint8_t> b) noexcept {
  const uint8_t* p = b.data();
  const size_t n = b.size();
  size_t i = 0;

  // Fast path for short strings (< 16 bytes): avoid checking 64B/16B SIMD loops.
  if (n < 16) {
    if (n >= 8) {
      if ((read_u64_le(p) & 0x8080808080808080ULL) == 0) {
        i = 8;
      }
    }
    while (i < n && p[i] < 0x80u) {
      ++i;
    }
    if (i >= n) return true;
  }

  while (i < n) {
#if defined(__aarch64__) || defined(_M_ARM64)
    while (i + 64 <= n) {
      const uint8x16_t v0 = vld1q_u8(p + i);
      const uint8x16_t v1 = vld1q_u8(p + i + 16);
      const uint8x16_t v2 = vld1q_u8(p + i + 32);
      const uint8x16_t v3 = vld1q_u8(p + i + 48);
      const uint8x16_t ored = vorrq_u8(vorrq_u8(v0, v1), vorrq_u8(v2, v3));
      if (vmaxvq_u8(ored) < 0x80u) {
        i += 64;
        continue;
      }
      break;
    }
    while (i + 16 <= n) {
      const uint8x16_t v = vld1q_u8(p + i);
      if (vmaxvq_u8(v) < 0x80u) {
        i += 16;
        continue;
      }
      break;
    }
#else
    while (i + 32 <= n) {
      const uint64_t w0 = read_u64_le(p + i);
      const uint64_t w1 = read_u64_le(p + i + 8);
      const uint64_t w2 = read_u64_le(p + i + 16);
      const uint64_t w3 = read_u64_le(p + i + 24);
      if (((w0 | w1 | w2 | w3) & 0x8080808080808080ULL) == 0) {
        i += 32;
        continue;
      }
      break;
    }
#endif
    while (i + 8 <= n) {
      const uint64_t w = read_u64_le(p + i);
      if ((w & 0x8080808080808080ULL) == 0) {
        i += 8;
        continue;
      }
      break;
    }
    while (i < n && p[i] < 0x80u) {
      ++i;
    }
    if (i >= n) break;

    const uint8_t b0 = p[i];
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
