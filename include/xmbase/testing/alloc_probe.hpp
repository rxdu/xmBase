/*
 * testing/alloc_probe.hpp — RAII allocation counter.
 *
 * TEST/BENCH TIER ONLY. This header replaces the program's global operator
 * new/delete — it must never be included from runtime (shipped) code, and it
 * must be included from exactly ONE translation unit per test/bench binary
 * (the replacement definitions below are deliberately non-inline: one
 * definition per program).
 *
 * The family S1 methodology (xmTelemetry perf tier, xmMessaging M9, quickviz
 * ingestion I2), unified here at ADR 0007 W1: replace the global allocation
 * functions and count allocations made ON THE PROBING THREAD between
 * AllocProbe construction and query.
 *
 * Every form is replaced, including the nothrow ones: leaving those to the
 * runtime pairs the sanitizer's operator-new allocation (libstdc++'s
 * get_temporary_buffer uses nothrow new) with THIS free-based operator
 * delete — a false alloc-dealloc mismatch under ASan. Replacing every form
 * keeps all pairs malloc/free-consistent for the interceptors.
 *
 * Copyright (c) 2026 Ruixiang Du (rdu)
 */

#pragma once

#include <cstdint>
#include <cstdlib>
#include <new>

namespace xmotion {
namespace testing {

inline thread_local bool g_alloc_counting = false;
inline thread_local std::uint64_t g_alloc_count = 0;

class AllocProbe {
 public:
  AllocProbe() {
    g_alloc_count = 0;
    g_alloc_counting = true;
  }
  ~AllocProbe() { g_alloc_counting = false; }
  AllocProbe(const AllocProbe&) = delete;
  AllocProbe& operator=(const AllocProbe&) = delete;

  std::uint64_t allocations() const { return g_alloc_count; }
};

inline void* CountingAlloc(std::size_t size) {
  if (g_alloc_counting) {
    ++g_alloc_count;
  }
  if (void* p = std::malloc(size != 0 ? size : 1)) {
    return p;
  }
  throw std::bad_alloc();
}

inline void* CountingAllocNoThrow(std::size_t size) noexcept {
  if (g_alloc_counting) {
    ++g_alloc_count;
  }
  return std::malloc(size != 0 ? size : 1);
}

}  // namespace testing
}  // namespace xmotion

// Replaceable global allocation functions (see the header comment for why
// every form — throwing and nothrow — is replaced).
void* operator new(std::size_t size) {
  return xmotion::testing::CountingAlloc(size);
}
void* operator new[](std::size_t size) {
  return xmotion::testing::CountingAlloc(size);
}
void* operator new(std::size_t size, const std::nothrow_t&) noexcept {
  return xmotion::testing::CountingAllocNoThrow(size);
}
void* operator new[](std::size_t size, const std::nothrow_t&) noexcept {
  return xmotion::testing::CountingAllocNoThrow(size);
}
void operator delete(void* ptr) noexcept { std::free(ptr); }
void operator delete[](void* ptr) noexcept { std::free(ptr); }
void operator delete(void* ptr, std::size_t) noexcept { std::free(ptr); }
void operator delete[](void* ptr, std::size_t) noexcept { std::free(ptr); }
void operator delete(void* ptr, const std::nothrow_t&) noexcept {
  std::free(ptr);
}
void operator delete[](void* ptr, const std::nothrow_t&) noexcept {
  std::free(ptr);
}
