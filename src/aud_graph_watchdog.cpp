// @license
// Copyright (c) Audanika. All Rights Reserved.
//
// Use of this source code is governed by terms that can be
// found in the LICENSE file in the root of this package.

// The counters of the debug watchdog and, with AUD_GRAPH_WATCHDOG, the
// replacements of the global allocation functions: every form of operator
// new and delete counts a call under the guard and then allocates or frees
// through malloc and free as the library's own would. The process-wide
// count is what the native tests check after every test.

#include "aud_graph_watchdog.hpp"

#include <atomic>
#include <cstdlib>
#include <new>

#if _WIN32
#include <malloc.h>
#endif

namespace aud {
namespace {

std::atomic<uint64_t> g_watchdogTotal{0};

}  // namespace

#if AUD_GRAPH_WATCHDOG

thread_local bool g_watchdogGuard = false;
thread_local uint32_t g_watchdogHits = 0;
thread_local uint32_t g_watchdogKinds = 0;

void watchdogHit(uint32_t kind) {
  g_watchdogHits += 1;
  g_watchdogKinds |= kind;
  g_watchdogTotal.fetch_add(1, std::memory_order_relaxed);
}

#endif

}  // namespace aud

// ............................................................................
// The C API

AUD_EXPORT int32_t aud_graph_watchdog_enabled(void) {
#if AUD_GRAPH_WATCHDOG
  return 1;
#else
  return 0;
#endif
}

AUD_EXPORT uint64_t aud_graph_watchdog_violations(void) {
  return aud::g_watchdogTotal.load(std::memory_order_relaxed);
}

AUD_EXPORT void aud_graph_watchdog_reset(void) {
  aud::g_watchdogTotal.store(0, std::memory_order_relaxed);
}

// ............................................................................
// The replaced allocation functions

#if AUD_GRAPH_WATCHDOG

namespace {

void* plainAllocate(std::size_t size) {
  aud::watchdogCheck(AUD_GRAPH_VIOLATION_NEW);
  return std::malloc(size == 0 ? 1 : size);
}

void* alignedAllocate(std::size_t size, std::size_t alignment) {
  aud::watchdogCheck(AUD_GRAPH_VIOLATION_NEW);
  if (alignment < sizeof(void*)) alignment = sizeof(void*);
  if (size == 0) size = alignment;
#if _WIN32
  return _aligned_malloc(size, alignment);
#else
  void* memory = nullptr;
  if (posix_memalign(&memory, alignment, size) != 0) return nullptr;
  return memory;
#endif
}

void plainFree(void* memory) {
  aud::watchdogCheck(AUD_GRAPH_VIOLATION_DELETE);
  std::free(memory);
}

void alignedFree(void* memory) {
  aud::watchdogCheck(AUD_GRAPH_VIOLATION_DELETE);
#if _WIN32
  _aligned_free(memory);
#else
  std::free(memory);
#endif
}

void* plainAllocateOrThrow(std::size_t size) {
  void* memory = plainAllocate(size);
  if (memory == nullptr) throw std::bad_alloc();
  return memory;
}

void* alignedAllocateOrThrow(std::size_t size, std::size_t alignment) {
  void* memory = alignedAllocate(size, alignment);
  if (memory == nullptr) throw std::bad_alloc();
  return memory;
}

}  // namespace

void* operator new(std::size_t size) { return plainAllocateOrThrow(size); }
void* operator new[](std::size_t size) { return plainAllocateOrThrow(size); }
void* operator new(std::size_t size, const std::nothrow_t&) noexcept {
  return plainAllocate(size);
}
void* operator new[](std::size_t size, const std::nothrow_t&) noexcept {
  return plainAllocate(size);
}
void* operator new(std::size_t size, std::align_val_t alignment) {
  return alignedAllocateOrThrow(size, static_cast<std::size_t>(alignment));
}
void* operator new[](std::size_t size, std::align_val_t alignment) {
  return alignedAllocateOrThrow(size, static_cast<std::size_t>(alignment));
}
void* operator new(std::size_t size, std::align_val_t alignment,
                   const std::nothrow_t&) noexcept {
  return alignedAllocate(size, static_cast<std::size_t>(alignment));
}
void* operator new[](std::size_t size, std::align_val_t alignment,
                     const std::nothrow_t&) noexcept {
  return alignedAllocate(size, static_cast<std::size_t>(alignment));
}

void operator delete(void* memory) noexcept { plainFree(memory); }
void operator delete[](void* memory) noexcept { plainFree(memory); }
void operator delete(void* memory, std::size_t) noexcept { plainFree(memory); }
void operator delete[](void* memory, std::size_t) noexcept {
  plainFree(memory);
}
void operator delete(void* memory, const std::nothrow_t&) noexcept {
  plainFree(memory);
}
void operator delete[](void* memory, const std::nothrow_t&) noexcept {
  plainFree(memory);
}
void operator delete(void* memory, std::align_val_t) noexcept {
  alignedFree(memory);
}
void operator delete[](void* memory, std::align_val_t) noexcept {
  alignedFree(memory);
}
void operator delete(void* memory, std::size_t, std::align_val_t) noexcept {
  alignedFree(memory);
}
void operator delete[](void* memory, std::size_t, std::align_val_t) noexcept {
  alignedFree(memory);
}
void operator delete(void* memory, std::align_val_t,
                     const std::nothrow_t&) noexcept {
  alignedFree(memory);
}
void operator delete[](void* memory, std::align_val_t,
                       const std::nothrow_t&) noexcept {
  alignedFree(memory);
}

#endif  // AUD_GRAPH_WATCHDOG
