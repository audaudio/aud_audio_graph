// @license
// Copyright (c) Audanika. All Rights Reserved.
//
// Use of this source code is governed by terms that can be
// found in the LICENSE file in the root of this package.

// The clock of the render thread: the start of a block, the time it took
// and the origin of a synthesized stream time. With AUD_GRAPH_TEST_CLOCK a
// native test replaces it by a clock of its own, so the measures of the
// render are exact and do not depend on how the host schedules the test.
// Without the define it is aud_clock_now_ns, and a release build carries
// none of this.

#ifndef AUD_GRAPH_TEST_CLOCK_HPP
#define AUD_GRAPH_TEST_CLOCK_HPP

#include <cstdint>

#include "aud_clock.h"

#if AUD_GRAPH_TEST_CLOCK
#include <atomic>
#endif

namespace aud {

#if AUD_GRAPH_TEST_CLOCK

// The clock that replaces aud_clock_now_ns; nullptr for the real one.
using TestClock = int64_t (*)();
extern std::atomic<TestClock> g_testClock;

// Replaces the clock of the render thread; nullptr restores the real one.
inline void setTestClock(TestClock clock) {
  g_testClock.store(clock, std::memory_order_release);
}

inline int64_t renderNowNs() {
  const TestClock clock = g_testClock.load(std::memory_order_acquire);
  return clock != nullptr ? clock() : aud_clock_now_ns();
}

#else

inline int64_t renderNowNs() { return aud_clock_now_ns(); }

#endif

}  // namespace aud

#endif  // AUD_GRAPH_TEST_CLOCK_HPP
