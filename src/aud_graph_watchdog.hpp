// @license
// Copyright (c) Audanika. All Rights Reserved.
//
// Use of this source code is governed by terms that can be
// found in the LICENSE file in the root of this package.

// The debug watchdog of the realtime thread (ticket 20): a thread-local
// guard is raised while a block renders; the replaced global operator new
// and delete and the host api's alloc, free and log count a hit when they
// run under the guard, then do their work anyway. Everything compiles to
// nothing without AUD_GRAPH_WATCHDOG, so a release build is free of it.
// The first access of a thread-local on a new thread may allocate its
// storage (dyld on Darwin, the dynamic TLS of a dlopen'ed library on
// Linux, emutls on older Android): once per thread, in debug builds only,
// before the guard is up. In an app the replaced operators serve the
// library of aud_audio_graph only; a DSP package's library keeps its own
// allocator, which the watchdog cannot see (see aud_audio_graph.h).

#ifndef AUD_GRAPH_WATCHDOG_HPP
#define AUD_GRAPH_WATCHDOG_HPP

#include <cstdint>

#include "aud_audio_graph.h"

namespace aud {

#if AUD_GRAPH_WATCHDOG

// Raised while the calling thread renders a block.
extern thread_local bool g_watchdogGuard;
// The hits of the block the calling thread renders.
extern thread_local uint32_t g_watchdogHits;
// The AUD_GRAPH_VIOLATION_* kinds of those hits.
extern thread_local uint32_t g_watchdogKinds;

// Counts a hit under the guard.
void watchdogHit(uint32_t kind);

// Raises the guard for the scope of a block.
struct WatchdogScope {
  WatchdogScope() {
    g_watchdogHits = 0;
    g_watchdogKinds = 0;
    g_watchdogGuard = true;
  }
  ~WatchdogScope() { g_watchdogGuard = false; }
  WatchdogScope(const WatchdogScope&) = delete;
  WatchdogScope& operator=(const WatchdogScope&) = delete;
};

// Counts a hit of `kind` when the guard is raised.
inline void watchdogCheck(uint32_t kind) {
  if (g_watchdogGuard) watchdogHit(kind);
}

// The hits of the block being rendered and their kinds.
inline uint32_t watchdogBlockHits() { return g_watchdogHits; }
inline uint32_t watchdogBlockKinds() { return g_watchdogKinds; }

#else

struct WatchdogScope {
  WatchdogScope() {}
  ~WatchdogScope() {}
  WatchdogScope(const WatchdogScope&) = delete;
  WatchdogScope& operator=(const WatchdogScope&) = delete;
};

inline void watchdogCheck(uint32_t) {}
inline uint32_t watchdogBlockHits() { return 0; }
inline uint32_t watchdogBlockKinds() { return 0; }

#endif

}  // namespace aud

#endif  // AUD_GRAPH_WATCHDOG_HPP
