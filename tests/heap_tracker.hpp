#pragma once

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <new>
#include <vector>

#if !defined(__SANITIZE_ADDRESS__) && \
    !(defined(__has_feature) && __has_feature(address_sanitizer))
#define OMNIBUF_TRACK_HEAP 1
#else
#define OMNIBUF_TRACK_HEAP 0
#endif

namespace omnibuf_test {

inline volatile uintptr_t g_volatile_sink = 0;

inline void scrub_freed_heap_slots(size_t target_size, uint8_t fill_byte) {
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

}  // namespace omnibuf_test

#if OMNIBUF_TRACK_HEAP
namespace omnibuf_test {

inline std::atomic<std::ptrdiff_t> g_live_heap_bytes{0};
inline std::atomic<bool> g_fail_allocs{false};

struct AllocPrefix {
  void* base;
  std::size_t size;
  std::size_t magic;
};

constexpr std::size_t kAllocMagic = 0x4F4D4E4942554631ULL;

inline void* tracked_alloc_aligned(std::size_t size, std::size_t alignment) noexcept {
  if (g_fail_allocs.load(std::memory_order_relaxed)) {
    return nullptr;
  }
  const std::size_t align = std::max<std::size_t>(alignment, alignof(std::max_align_t));
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

inline void* tracked_alloc(std::size_t size) noexcept {
  return tracked_alloc_aligned(size, alignof(std::max_align_t));
}

inline void tracked_free(void* ptr) noexcept {
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

}  // namespace omnibuf_test

void* operator new(std::size_t size) {
  if (void* p = omnibuf_test::tracked_alloc(size)) {
    return p;
  }
  throw std::bad_alloc();
}

void* operator new(std::size_t size, const std::nothrow_t&) noexcept {
  return omnibuf_test::tracked_alloc(size);
}

void* operator new[](std::size_t size) {
  if (void* p = omnibuf_test::tracked_alloc(size)) {
    return p;
  }
  throw std::bad_alloc();
}

void* operator new[](std::size_t size, const std::nothrow_t&) noexcept {
  return omnibuf_test::tracked_alloc(size);
}

void* operator new(std::size_t size, std::align_val_t al) {
  if (void* p = omnibuf_test::tracked_alloc_aligned(size, static_cast<std::size_t>(al))) {
    return p;
  }
  throw std::bad_alloc();
}

void* operator new(std::size_t size, std::align_val_t al, const std::nothrow_t&) noexcept {
  return omnibuf_test::tracked_alloc_aligned(size, static_cast<std::size_t>(al));
}

void* operator new[](std::size_t size, std::align_val_t al) {
  if (void* p = omnibuf_test::tracked_alloc_aligned(size, static_cast<std::size_t>(al))) {
    return p;
  }
  throw std::bad_alloc();
}

void* operator new[](std::size_t size, std::align_val_t al, const std::nothrow_t&) noexcept {
  return omnibuf_test::tracked_alloc_aligned(size, static_cast<std::size_t>(al));
}

void operator delete(void* ptr) noexcept { omnibuf_test::tracked_free(ptr); }
void operator delete(void* ptr, std::size_t) noexcept { omnibuf_test::tracked_free(ptr); }
void operator delete(void* ptr, const std::nothrow_t&) noexcept { omnibuf_test::tracked_free(ptr); }
void operator delete[](void* ptr) noexcept { omnibuf_test::tracked_free(ptr); }
void operator delete[](void* ptr, std::size_t) noexcept { omnibuf_test::tracked_free(ptr); }
void operator delete[](void* ptr, const std::nothrow_t&) noexcept {
  omnibuf_test::tracked_free(ptr);
}
void operator delete(void* ptr, std::align_val_t) noexcept { omnibuf_test::tracked_free(ptr); }
void operator delete(void* ptr, std::size_t, std::align_val_t) noexcept {
  omnibuf_test::tracked_free(ptr);
}
void operator delete(void* ptr, std::align_val_t, const std::nothrow_t&) noexcept {
  omnibuf_test::tracked_free(ptr);
}
void operator delete[](void* ptr, std::align_val_t) noexcept { omnibuf_test::tracked_free(ptr); }
void operator delete[](void* ptr, std::size_t, std::align_val_t) noexcept {
  omnibuf_test::tracked_free(ptr);
}
void operator delete[](void* ptr, std::align_val_t, const std::nothrow_t&) noexcept {
  omnibuf_test::tracked_free(ptr);
}
#endif
