<!--
@license
Copyright (c) Audanika. All Rights Reserved.

Use of this source code is governed by terms that can be
found in the LICENSE file in the root of this package.
-->

# Every timing test sets its own clock

## Motivation

- The render clock of the tests (`AUD_GRAPH_TEST_CLOCK`) made the start-up
  gap test of the period histogram exact. Three more tests of the
  histogram still measured real waits:
  - `graph_timing_reports_the_last_block` slept 20 ms between two renders
  - `graph_timing_histogram_drops_the_gap_that_reset_the_filter` paced 40
    renders with `sleep_until` and slept 60 ms before the reset
  - `graph_timing_histogram_takes_the_gap_of_split_callbacks` slept the
    callback of 10.7 ms between pairs of renders
- They accepted wide bounds, for example p99.9 below 50 ms or between
  10.7 ms and 40 ms, and still depended on how late the host wakes a
  sleeping thread. Under the thread sanitizer on a loaded host that is
  the same risk that made the start-up gap test fail.

## Strategy

- Every test that checks periods, the histogram or the time a render
  takes runs on the `ManualClock` of the test file.
- Tests of real concurrency keep real time: the hand-over between
  producer threads, the stress tests, the tasks and the load gauge that
  reads while a thread renders.
- Wide bounds become exact bins.

## The tests

- `graph_timing_reports_the_last_block` advances the clock by 20 ms: the
  longest gap is 20 ms, bin 200 holds it and p99.9 is 20.1 ms. After the
  reset of the time filter the next gap is 5.33 ms in bin 53.
- `graph_timing_histogram_drops_the_gap_that_reset_the_filter` advances
  5333333 ns before each render and 60 ms before the reset. Before the
  reset bin 53 holds 19 gaps; after it bin 53 holds 20, the overflow bin
  none, the longest gap is 5.33 ms and p99.9 is 5.4 ms.
- `graph_timing_histogram_takes_the_gap_of_split_callbacks` keeps the
  clock still within a pair and advances 10666667 ns between the pairs:
  bin 0 holds 20 gaps, bin 106 holds 19, the median is 0.1 ms and p99.9
  is 10.7 ms.
- The helper `renderPaced` and the Windows timer resolution it needed are
  gone; no test of the file sleeps for a period any more.

## A hanging note under load

- The runs under load turned up a real bug: once in 20 runs the stress test
  `stress_source_takes_10000_events_per_second_from_a_producer_thread`
  ended with a sounding note, although every event was delivered.
- Events at one offset are sorted by rank. A note off ranked first
  whenever its pitch sounded, so that a note on of the same pitch
  retriggers.
- When the render thread falls behind, late packets all land at offset 0
  of one block. For a pitch that sounds, a block can then hold
  `off(1) on(2) off(2)`. Both note offs ranked first, the block played
  `off off on`, and the second note hung.
- Now only the first note off of a sounding pitch in a block ranks first:
  only one note off can close the earlier note. Every further note off
  belongs to a note on of the same block and keeps its order behind it.
- Each node keeps the pitches the block closed in a second `NoteTracker`,
  cleared lazily by a block counter. If its table of 256 entries fills,
  further note offs keep the order they came in.
- `keeps_a_short_note_in_order_and_retriggers_a_sounding_one` sends
  `off on off` for a sounding pitch in one block and expects that order.
  It fails without the fix.

## Result

- `node scripts/test-native.js` passed 20 times in a row while 16 busy
  loops kept the host loaded: 137 of 137 tests in the address and
  undefined behaviour sanitizer build and in the thread sanitizer build.
- `dart test` passed.
- The three timing tests no longer wait; they slept about half a second
  before.

## Open Points

- The load gauge tests of `aud_graph_load_test.cpp` still read the real
  clock, but only check lower bounds or counts, which load cannot break.
