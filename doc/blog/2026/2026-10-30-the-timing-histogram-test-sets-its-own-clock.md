<!--
@license
Copyright (c) Audanika. All Rights Reserved.

Use of this source code is governed by terms that can be
found in the LICENSE file in the root of this package.
-->

# The timing histogram test sets its own clock

## Motivation

- The native test `graph_timing_histogram_ignores_a_single_gap_at_start_up`
  checks that one start-up gap of 177 ms does not move p99.9 of the period
  histogram: the median and p99.9 stay the callback of 5.33 ms.
- It measured real waits: `sleep_for` for the gap, then 1000 renders paced
  by `sleep_until`, and it accepted p99.9 between 5.4 ms and 40 ms.
- With 1001 gaps, p99.9 is the second longest one. Under the thread
  sanitizer on a loaded host one late wake-up above 40 ms was enough to
  fail the test. With 16 busy loops on 8 cores it failed 1 run in 15.
- A test that fails without a bug hides the runs where it fails because of
  one. It should check the histogram, not how the host schedules a thread.

## Strategy

- The engine reads the start of a block from `aud_clock_now_ns`. A test
  cannot set that time, so the engine gets a render clock that a native
  test can replace.
- The replacement is compiled in only with `AUD_GRAPH_TEST_CLOCK`, like
  the debug watchdog with `AUD_GRAPH_WATCHDOG`: a release build reads
  `aud_clock_now_ns` directly and carries no indirection.
- The public C API stays as it is; the tests reach the clock through an
  internal header, as they link the engine into one program.
- The test advances the clock by hand and checks exact bins instead of
  wide bounds.

## The render clock

- New header `src/aud_graph_test_clock.hpp` with `aud::renderNowNs()`.
- With `AUD_GRAPH_TEST_CLOCK` it calls the clock that
  `aud::setTestClock()` stored, or `aud_clock_now_ns` while none is set.
  The pointer is an atomic, so tests that render on a thread of their own
  stay clean under the thread sanitizer.
- `aud_graph_render.cpp` reads the render thread's time through it at
  three places: the start of a block, which the period histogram
  measures, the time a block took, and the origin of a synthesized stream
  time. All three come from the same clock, so the time a block took never
  mixes a test clock with the real one.
- `scripts/test-native.js` builds every variant with
  `-DAUD_GRAPH_TEST_CLOCK=1`, next to the watchdog.

## The test

- A `ManualClock` in the test sets the clock for its scope and restores
  the real one at its end.
- The test renders one block, advances the clock by 177 ms, renders again
  and then advances by 5333333 ns before each of 1000 renders. No thread
  sleeps.
- It checks exact values: the longest gap is 177 ms, bin 53 (5.3 ms) holds
  1000 gaps, the overflow bin holds the gap, and both the median and p99.9
  are 5.4 ms, the upper edge of bin 53.
- The test now runs in milliseconds instead of about five and a half
  seconds.

## Result

- `node scripts/test-native.js` passed 20 times in a row: 137 of 137
  tests in the address and undefined behaviour sanitizer build and in the
  thread sanitizer build.
- The test passed 15 of 15 runs under the load that made it fail before.
- A build without the define compiles the render path unchanged.

## Open Points

- The RealtimeSanitizer build was not run: no compiler on the test
  machine has it. `renderNowNs` is an atomic load and a call, both
  allowed on the audio thread.
- Other timing tests still sleep and keep wide bounds, for example
  `graph_timing_histogram_takes_the_gap_of_split_callbacks` and
  `graph_timing_histogram_drops_the_gap_that_reset_the_filter`. They can
  move to the test clock when they turn flaky.
