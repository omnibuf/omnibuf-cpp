#pragma once

#if (defined(__aarch64__) || defined(_M_ARM64)) && defined(__ARM_NEON)
#include <arm_neon.h>
#elif defined(__x86_64__) || defined(_M_X64) || defined(__SSE2__)
#include <immintrin.h>
#endif

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

#include "omnibuf/varint.hpp"

namespace omnibuf {

namespace detail {

[[nodiscard]] OMNIBUF_NOINLINE inline bool is_valid_utf8_long(const uint8_t* p,
                                                              size_t n) noexcept {
  if (n <= 64) {
#if (defined(__aarch64__) || defined(_M_ARM64)) && defined(__ARM_NEON)
    if (n > 32) {
      const uint8x16_t v0 = vld1q_u8(p);
      const uint8x16_t v1 = vld1q_u8(p + 16);
      const uint8x16_t v2 = vld1q_u8(p + n - 32);
      const uint8x16_t v3 = vld1q_u8(p + n - 16);
      if (vmaxvq_u8(vorrq_u8(vorrq_u8(v0, v1), vorrq_u8(v2, v3))) < 0x80u) [[likely]] {
        return true;
      }
    }
#elif defined(__AVX2__)
    if (n > 32) {
      const __m256i v0 = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(p));
      const __m256i v1 = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(p + n - 32));
      if (_mm256_movemask_epi8(_mm256_or_si256(v0, v1)) == 0) [[likely]] {
        return true;
      }
    }
#elif defined(__x86_64__) || defined(_M_X64) || defined(__SSE2__)
    if (n > 32) {
      const __m128i v0 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(p));
      const __m128i v1 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(p + 16));
      const __m128i v2 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(p + n - 32));
      const __m128i v3 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(p + n - 16));
      const __m128i ored = _mm_or_si128(_mm_or_si128(v0, v1), _mm_or_si128(v2, v3));
      if (_mm_movemask_epi8(ored) == 0) [[likely]] {
        return true;
      }
    }
#endif
  }

  size_t i = 0;
  while (i < n) {
#if (defined(__aarch64__) || defined(_M_ARM64)) && defined(__ARM_NEON)
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
#elif defined(__AVX2__)
    while (i + 64 <= n) {
      const __m256i v0 = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(p + i));
      const __m256i v1 = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(p + i + 32));
      if (_mm256_movemask_epi8(_mm256_or_si256(v0, v1)) == 0) {
        i += 64;
        continue;
      }
      break;
    }
    while (i + 32 <= n) {
      const __m256i v = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(p + i));
      if (_mm256_movemask_epi8(v) == 0) {
        i += 32;
        continue;
      }
      break;
    }
    while (i + 16 <= n) {
      const __m128i v = _mm_loadu_si128(reinterpret_cast<const __m128i*>(p + i));
      if (_mm_movemask_epi8(v) == 0) {
        i += 16;
        continue;
      }
      break;
    }
#elif defined(__x86_64__) || defined(_M_X64) || defined(__SSE2__)
    while (i + 64 <= n) {
      const __m128i v0 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(p + i));
      const __m128i v1 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(p + i + 16));
      const __m128i v2 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(p + i + 32));
      const __m128i v3 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(p + i + 48));
      const __m128i ored = _mm_or_si128(_mm_or_si128(v0, v1), _mm_or_si128(v2, v3));
      if (_mm_movemask_epi8(ored) == 0) {
        i += 64;
        continue;
      }
      break;
    }
    while (i + 16 <= n) {
      const __m128i v = _mm_loadu_si128(reinterpret_cast<const __m128i*>(p + i));
      if (_mm_movemask_epi8(v) == 0) {
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

}  // namespace detail

// Strict RFC 3629 UTF-8 validator with inline overlapping-word pure-ASCII fast paths
// up to 32 bytes and out-of-line SIMD/multi-byte validation for longer or non-ASCII inputs.
[[nodiscard]] inline bool is_valid_utf8(std::span<const uint8_t> b) noexcept {
  const uint8_t* p = b.data();
  const size_t n = b.size();
  if (n <= 16) [[likely]] {
    if (n >= 8) {
      if (((read_u64_le(p) | read_u64_le(p + n - 8)) & 0x8080808080808080ULL) == 0) [[likely]] {
        return true;
      }
    } else if (n >= 4) {
      if (((read_u32_le(p) | read_u32_le(p + n - 4)) & 0x80808080U) == 0) [[likely]] {
        return true;
      }
    } else {
      if (n == 0) return true;
      if (((p[0] | p[n >> 1] | p[n - 1]) & 0x80u) == 0) [[likely]] {
        return true;
      }
    }
    return detail::is_valid_utf8_long(p, n);
  }
  if (n <= 32) [[likely]] {
    const uint64_t w0 = read_u64_le(p);
    const uint64_t w1 = read_u64_le(p + 8);
    const uint64_t w2 = read_u64_le(p + n - 16);
    const uint64_t w3 = read_u64_le(p + n - 8);
    if (((w0 | w1 | w2 | w3) & 0x8080808080808080ULL) == 0) [[likely]] {
      return true;
    }
  }
  return detail::is_valid_utf8_long(p, n);
}

[[nodiscard]] inline bool is_valid_utf8(std::string_view s) noexcept {
  return is_valid_utf8(std::span<const uint8_t>(
      reinterpret_cast<const uint8_t*>(s.data()), s.size()));
}

}  // namespace omnibuf
