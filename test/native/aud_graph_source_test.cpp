// @license
// Copyright (c) Audanika. All Rights Reserved.
//
// Use of this source code is governed by terms that can be
// found in the LICENSE file in the root of this package.

// The event sources of ticket 26 (interop-003, midi-003): creation for a
// committed target, the drain with its peek and pop, the timing policies
// to the sample, late and reordered packets, the budget, the release with
// its flush rule, the overflow of a note off, the retirement of the
// target, the lifetime of a source, the hand-over between producers, the
// routes of retiring emitters and the timing of the engine.

#include <atomic>
#include <chrono>
#include <cstring>
#include <set>
#include <thread>
#include <vector>

#include "aud_audio_graph.h"
#include "aud_graph_fixture.hpp"
#include "aud_test.hpp"
#include "aud_ump.h"

using namespace aud_test_fixture;

namespace {

bool isNoteOff(const Record& r) { return aud_ump_is_note_off(r.word0, 0) == 1; }
bool isControl(const Record& r, uint32_t controller, uint32_t value) {
  return aud_ump_is_channel_voice(r.word0) &&
         aud_ump_status(r.word0) == AUD_UMP_STATUS_CONTROL_CHANGE &&
         aud_ump_midi1_data1(r.word0) == controller &&
         aud_ump_midi1_data2(r.word0) == value;
}

// A graph with a recorder connected to the output, running.
struct SourceFixture {
  Fixture f;
  int32_t recorder;

  explicit SourceFixture(std::function<void(AudGraphConfig&)> tweak = nullptr)
      : f(0, 1, 256, std::move(tweak)), recorder(f.node("aud.test.recorder")) {
    f.commit([&] { f.connect(recorder, AUD_GRAPH_NODE); });
    aud_graph_start(f.graph);
  }
};

// The renders the period histogram counts.
uint64_t periodTotal(const AudGraphTiming& t) {
  uint64_t total = 0;
  for (uint32_t i = 0; i <= AUD_GRAPH_PERIOD_BINS; ++i) {
    total += t.period_histogram[i];
  }
  return total;
}

// The upper edge of the bin at which `fraction` of the renders is reached,
// as the Dart side computes it; 0 without renders.
int64_t periodPercentileNs(const AudGraphTiming& t, double fraction) {
  const uint64_t total = periodTotal(t);
  if (total == 0) return 0;
  const double wanted = fraction * static_cast<double>(total);
  uint64_t seen = 0;
  for (uint32_t i = 0; i <= AUD_GRAPH_PERIOD_BINS; ++i) {
    seen += t.period_histogram[i];
    if (seen > 0 && static_cast<double>(seen) >= wanted) {
      return static_cast<int64_t>(i + 1) * AUD_GRAPH_PERIOD_BIN_NS;
    }
  }
  return static_cast<int64_t>(AUD_GRAPH_PERIOD_BINS + 1) * AUD_GRAPH_PERIOD_BIN_NS;
}

// Renders `count` blocks of 256 frames, one every `periodNs` from `from`.
void renderPaced(TimedStream& stream, std::chrono::steady_clock::time_point from,
                 int64_t periodNs, uint32_t count) {
  for (uint32_t i = 1; i <= count; ++i) {
    std::this_thread::sleep_until(from + std::chrono::nanoseconds(periodNs) * i);
    stream.render(256);
  }
}

}  // namespace

// ############################################################################
// Creation and lifetime

AUD_TEST(source_needs_a_committed_target) {
  Fixture f;
  const int32_t recorder = f.node("aud.test.recorder");
  int32_t result = 0;
  AUD_CHECK(makeSource(f, AUD_GRAPH_NODE, 0, 0, 0, 0, &result) == nullptr);
  AUD_CHECK(result == AUD_ERROR_INVALID_ARGUMENT);
  AUD_CHECK(makeSource(f, 42, 0, 0, 0, 0, &result) == nullptr);
  AUD_CHECK(result == AUD_ERROR_NOT_FOUND);
  AUD_CHECK(makeSource(f, recorder, 0, 0, 0, 0, &result) == nullptr);
  AUD_CHECK(result == AUD_ERROR_STATE);
  f.commit([&] { f.connect(recorder, AUD_GRAPH_NODE); });
  AUD_CHECK(makeSource(f, recorder, 7, 0, 0, 0, &result) == nullptr);
  AUD_CHECK(result == AUD_ERROR_INVALID_ARGUMENT);
  AUD_CHECK(makeSource(f, recorder, 0, 0, 0, 3, &result) == nullptr);
  AUD_CHECK(result == AUD_ERROR_INVALID_ARGUMENT);
  AUD_CHECK(aud_graph_create_event_source(f.graph, nullptr, &result) == nullptr);
  AUD_CHECK(result == AUD_ERROR_INVALID_ARGUMENT);
  AUD_CHECK(aud_graph_create_event_source(nullptr, nullptr, nullptr) == nullptr);
  AudEventSource* source = makeSource(f, recorder, AUD_SOURCE_TIMING_FIXED, 0, 100,
                                      0, &result);
  AUD_CHECK(source != nullptr && result == AUD_OK);
  AUD_CHECK(aud_event_source_capacity(source) == 128);
  AUD_CHECK(aud_event_source_size(source) == 0);
  AudEventSource* second = makeSource(f, recorder);
  AUD_CHECK(aud_event_source_capacity(second) == 4096);
  // A malformed packet is refused and counted.
  const uint32_t two[2] = {aud_ump_midi1_word(0, 0x90, 60, 100), 0};
  AUD_CHECK(aud_event_source_push(source, two, 2, 0) == AUD_ERROR_INVALID_ARGUMENT);
  AUD_CHECK(aud_event_source_push(source, nullptr, 1, 0) == AUD_ERROR_INVALID_ARGUMENT);
  AUD_CHECK(aud_event_source_push(source, two, 0, 0) == AUD_ERROR_INVALID_ARGUMENT);
  AUD_CHECK(aud_event_source_push(nullptr, two, 1, 0) == AUD_ERROR_INVALID_ARGUMENT);
  AUD_CHECK(sourceStats(source).rejected == 3);
  AUD_CHECK(aud_event_source_size(nullptr) == 0);
  AUD_CHECK(aud_event_source_capacity(nullptr) == 0);
  AUD_CHECK(aud_event_source_set_latency(nullptr, 1) == AUD_ERROR_INVALID_ARGUMENT);
  AUD_CHECK(aud_event_source_stats(nullptr, nullptr) == AUD_ERROR_INVALID_ARGUMENT);
  AUD_CHECK(aud_graph_destroy_event_source(f.graph, nullptr) == AUD_ERROR_INVALID_ARGUMENT);
  aud_event_source_release(nullptr);
  // The graph does not run: the sources go at once.
  AUD_CHECK(aud_graph_destroy_event_source(f.graph, source) == AUD_OK);
  AUD_CHECK(aud_graph_destroy_event_source(f.graph, second) == AUD_OK);
}

AUD_TEST(source_respects_the_capacity_of_the_graph) {
  SourceFixture s;
  std::vector<AudEventSource*> sources;
  for (uint32_t i = 0; i < AUD_GRAPH_MAX_EVENT_SOURCES; ++i) {
    AudEventSource* source = makeSource(s.f, s.recorder, 0, 0, 16);
    AUD_CHECK(source != nullptr);
    sources.push_back(source);
  }
  int32_t result = 0;
  AUD_CHECK(makeSource(s.f, s.recorder, 0, 0, 16, 0, &result) == nullptr);
  AUD_CHECK(result == AUD_ERROR_CAPACITY);
  for (AudEventSource* source : sources) {
    AUD_CHECK(aud_graph_destroy_event_source(s.f.graph, source) == AUD_OK);
  }
  // Running: the realtime thread lets go at the next block.
  s.f.render(256);
  AUD_CHECK(s.f.countNotifications(AUD_NOTIFY_SOURCE_DONE) ==
            AUD_GRAPH_MAX_EVENT_SOURCES);
  AUD_CHECK(makeSource(s.f, s.recorder, 0, 0, 16, 0, &result) != nullptr);
}

AUD_TEST(source_destroy_waits_for_the_audio_thread) {
  SourceFixture s;
  AudEventSource* source = makeSource(s.f, s.recorder, AUD_SOURCE_TIMING_IMMEDIATE);
  pushWord(source, aud_ump_midi1_word(0, 0x90, 60, 100));
  pushWord(source, ccWord(64, 127));
  s.f.render(256);
  AUD_CHECK(g_records.size() == 2);
  AUD_CHECK(aud_event_source_attach(source, 5) == AUD_OK);
  AUD_CHECK(aud_graph_destroy_event_source(s.f.graph, source) == AUD_GRAPH_ERROR_BUSY);
  AUD_CHECK(aud_event_source_detach(source, 5) == AUD_OK);
  AUD_CHECK(aud_graph_destroy_event_source(s.f.graph, source) == AUD_OK);
  AUD_CHECK(aud_graph_destroy_event_source(s.f.graph, source) == AUD_ERROR_STATE);
  // Until the next block the source is still there for the counters.
  AUD_CHECK(sourceStats(source).delivered == 2);
  s.f.render(256);
  // The notes of the target closed, the pedal lifted; the source is
  // announced and freed as the notification is taken.
  AUD_CHECK(g_records.size() == 4);
  AUD_CHECK(isNoteOff(g_records[2]) && aud_ump_note(g_records[2].word0) == 60);
  AUD_CHECK(isControl(g_records[3], 64, 0));
  bool announced = false;
  for (const AudGraphNotification& n : s.f.takeAll()) {
    if (n.type != AUD_NOTIFY_SOURCE_DONE) continue;
    announced = true;
    AUD_CHECK(n.node == s.recorder);
    AUD_CHECK(n.value == static_cast<int64_t>(reinterpret_cast<intptr_t>(source)));
  }
  AUD_CHECK(announced);
  // A source retired while the graph does not run goes at the next take.
  AudEventSource* idle = makeSource(s.f, s.recorder);
  aud_graph_stop(s.f.graph);
  AUD_CHECK(aud_graph_destroy_event_source(s.f.graph, idle) == AUD_OK);
  AudEventSource* stopped = makeSource(s.f, s.recorder);
  aud_graph_start(s.f.graph);
  AUD_CHECK(aud_graph_destroy_event_source(s.f.graph, stopped) == AUD_OK);
  aud_graph_stop(s.f.graph);
  s.f.takeAll();
  // The destroy of the graph frees what is left.
  makeSource(s.f, s.recorder);
}

AUD_TEST(source_slot_is_free_before_the_source_is_let_go) {
  // The audio thread clears the slot of a retired source before it lets
  // go of it: the control thread, which frees the source then, creates
  // the next one into the same slot at once - the allocator may give it
  // the same address - and that one must be drained, not cleared by a
  // late clearing of the slot.
  SourceFixture s;
  std::atomic<bool> stop{false};
  std::thread stream([&] {
    while (!stop.load(std::memory_order_acquire)) {
      s.f.render(256);
      std::this_thread::yield();
    }
  });
  uint32_t delivered = 0;
  uint32_t announced = 0;
  for (uint32_t round = 0; round < 200; ++round) {
    AudEventSource* source = makeSource(s.f, s.recorder, AUD_SOURCE_TIMING_IMMEDIATE);
    AUD_CHECK(source != nullptr);
    if (source == nullptr) break;
    AUD_CHECK(pushWord(source, aud_ump_midi1_word(0, 0x90, 60, 100)) == AUD_OK);
    AUD_CHECK(pushWord(source, aud_ump_midi1_word(0, 0x80, 60, 64)) == AUD_OK);
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (sourceStats(source).delivered < 2 &&
           std::chrono::steady_clock::now() < until) {
      std::this_thread::yield();
    }
    delivered += sourceStats(source).delivered == 2 ? 1 : 0;
    AUD_CHECK(aud_graph_destroy_event_source(s.f.graph, source) == AUD_OK);
    // The take of the notification frees the source once it is let go.
    bool gone = false;
    while (!gone && std::chrono::steady_clock::now() < until) {
      for (const AudGraphNotification& n : s.f.takeAll()) {
        if (n.type == AUD_NOTIFY_SOURCE_DONE) gone = true;
      }
      if (!gone) std::this_thread::yield();
    }
    announced += gone ? 1 : 0;
  }
  stop.store(true, std::memory_order_release);
  stream.join();
  AUD_CHECK(delivered == 200);
  AUD_CHECK(announced == 200);
  AUD_CHECK(g_records.size() == 400);
  AUD_CHECK(s.f.stats().events_dropped == 0);
  AUD_CHECK(s.f.stats().realtime_violations == 0);
}

// ############################################################################
// The drain

AUD_TEST(source_pushes_drain_with_peek_and_pop_to_the_sample) {
  SourceFixture s;
  TimedStream stream(s.f);
  AudEventSource* source = makeSource(s.f, s.recorder);
  // Frame 1000 lies in the fourth block of 256 at offset 232; frame 2000
  // in the eighth at offset 208. Both wait in the queue until their block.
  AUD_CHECK(pushWord(source, aud_ump_midi1_word(0, 0x90, 60, 100),
                     stream.atFrame(999.5)) == AUD_OK);
  AUD_CHECK(pushWord(source, aud_ump_midi1_word(0, 0x80, 60, 64),
                     stream.atFrame(1999.5)) == AUD_OK);
  AUD_CHECK(aud_event_source_size(source) == 2);
  for (int block = 0; block < 3; ++block) stream.render(256);
  AUD_CHECK(g_records.empty());
  AUD_CHECK(aud_event_source_size(source) == 2);
  stream.render(256);
  AUD_CHECK(g_records.size() == 1);
  AUD_CHECK(g_records[0].position == 768 && g_records[0].offset == 232);
  AUD_CHECK(g_records[0].flags == 0);
  AUD_CHECK(aud_event_source_size(source) == 1);
  for (int block = 4; block < 8; ++block) stream.render(256);
  AUD_CHECK(g_records.size() == 2);
  AUD_CHECK(g_records[1].position == 1792 && g_records[1].offset == 208);
  const AudEventSourceStats stats = sourceStats(source);
  AUD_CHECK(stats.pushed == 2 && stats.delivered == 2 && stats.late == 0);
  AUD_CHECK(stats.last_word0 == aud_ump_midi1_word(0, 0x80, 60, 64));
  AUD_CHECK(stats.last_timestamp_ns == stream.atFrame(1999.5));
  AUD_CHECK(stats.max_delay_ns > 0);  // the timestamps lie in the past
  AUD_CHECK(stats.delay_histogram[AUD_SOURCE_DELAY_BINS] == 2);
  // The input to output time of both: half a frame, the first bin.
  AUD_CHECK(stats.latency_histogram[0] == 2);
  AUD_CHECK(s.f.stats().events_delivered == 2);
}

AUD_TEST(source_fixed_applies_the_latency_and_the_lead) {
  Fixture f;
  const int32_t recorder = f.node("aud.test.recorder");
  const int32_t delay = f.node("aud.test.delay");
  f.commit([&] {
    f.connect(recorder, delay);
    f.connect(delay, AUD_GRAPH_NODE);
  });
  aud_graph_start(f.graph);
  AUD_CHECK(aud_graph_node_lead(f.graph, recorder) == kTestLatency);
  TimedStream stream(f);
  // 10 ms = 480 frames: a packet at frame 100 sounds at frame 580 and is
  // pre-delivered by the lead of 100 frames: frame 480, the block start
  // of the second block.
  AudEventSource* fixed = makeSource(f, recorder, AUD_SOURCE_TIMING_FIXED, 10000000);
  AudEventSource* immediate =
      makeSource(f, recorder, AUD_SOURCE_TIMING_IMMEDIATE, 10000000);
  pushWord(fixed, aud_ump_midi1_word(0, 0x90, 60, 100), stream.atFrame(99.5));
  pushWord(immediate, aud_ump_midi1_word(0, 0x90, 62, 100), stream.atFrame(99.5));
  stream.render(256);
  AUD_CHECK(g_records.size() == 1);
  AUD_CHECK(aud_ump_note(g_records[0].word0) == 62 && g_records[0].offset == 0);
  AUD_CHECK(g_records[0].flags == AUD_EVENT_FLAG_LIVE);
  stream.render(256);
  AUD_CHECK(g_records.size() == 2);
  AUD_CHECK(aud_ump_note(g_records[1].word0) == 60);
  AUD_CHECK(g_records[1].position == 256 && g_records[1].offset == 224);
  AUD_CHECK(g_records[1].flags == 0);
  // A smaller C from now on: frame 1000 + 240 - 100 = 1140, offset 116
  // of the fifth block.
  AUD_CHECK(aud_event_source_set_latency(fixed, 5000000) == AUD_OK);
  pushWord(fixed, aud_ump_midi1_word(0, 0x90, 64, 100), stream.atFrame(999.5));
  for (int block = 2; block < 6; ++block) stream.render(256);
  AUD_CHECK(g_records.size() == 3);
  AUD_CHECK(g_records[2].position == 1024 && g_records[2].offset == 116);
  // The input to output time counts the lead: 240 frames = 5 ms, bin 20.
  AUD_CHECK(sourceStats(fixed).latency_histogram[20] == 1);
}

AUD_TEST(source_late_packets_play_at_the_block_start_and_count) {
  SourceFixture s;
  TimedStream stream(s.f);
  AudEventSource* source = makeSource(s.f, s.recorder);
  stream.render(256);
  // 500 frames before the next block: 10.4 ms late.
  pushWord(source, aud_ump_midi1_word(0, 0x90, 60, 100), stream.atFrame(-244));
  stream.render(256);
  AUD_CHECK(g_records.size() == 1);
  AUD_CHECK(g_records[0].position == 256 && g_records[0].offset == 0);
  const AudEventSourceStats stats = sourceStats(source);
  AUD_CHECK(stats.late == 1);
  AUD_CHECK_NEAR(static_cast<double>(stats.max_late_ns), 500 * 1e9 / kRate, 1000);
  AUD_CHECK(stats.latency_histogram[AUD_SOURCE_LATENCY_BINS] == 0);
  // The graph's policy of dropping late events does not touch a source.
  Fixture dropping(0, 1, 256, [](AudGraphConfig& c) {
    c.flags = AUD_GRAPH_DROP_LATE_EVENTS;
  });
  const int32_t recorder = dropping.node("aud.test.recorder");
  dropping.commit([&] { dropping.connect(recorder, AUD_GRAPH_NODE); });
  aud_graph_start(dropping.graph);
  TimedStream other(dropping);
  AudEventSource* late = makeSource(dropping, recorder);
  other.render(256);
  pushWord(late, aud_ump_midi1_word(0, 0x90, 60, 100), other.atFrame(-10));
  other.render(256);
  AUD_CHECK(g_records.size() == 1 && g_records[0].offset == 0);
  AUD_CHECK(sourceStats(late).late == 1 && sourceStats(late).dropped == 0);
}

AUD_TEST(source_raises_reordered_timestamps) {
  SourceFixture s;
  TimedStream stream(s.f);
  AudEventSource* source = makeSource(s.f, s.recorder);
  pushWord(source, aud_ump_midi1_word(0, 0x90, 60, 100), stream.atFrame(999.5));
  pushWord(source, aud_ump_midi1_word(0, 0x90, 62, 100), stream.atFrame(499.5));
  pushWord(source, aud_ump_midi1_word(0, 0x90, 64, 100), stream.atFrame(1999.5));
  for (int block = 0; block < 8; ++block) stream.render(256);
  AUD_CHECK(g_records.size() == 3);
  AUD_CHECK(g_records[0].offset == 232 && aud_ump_note(g_records[0].word0) == 60);
  AUD_CHECK(g_records[1].offset == 232 && aud_ump_note(g_records[1].word0) == 62);
  AUD_CHECK(g_records[1].position == 768);
  AUD_CHECK(g_records[2].position == 1792 && g_records[2].offset == 208);
  AUD_CHECK(sourceStats(source).reordered == 1);
}

AUD_TEST(source_carries_the_rest_over_beyond_the_budget) {
  SourceFixture s([](AudGraphConfig& c) { c.max_events_per_block = 4; });
  AudEventSource* source = makeSource(s.f, s.recorder, AUD_SOURCE_TIMING_IMMEDIATE);
  for (uint32_t i = 0; i < 10; ++i) {
    pushWord(source, aud_ump_midi1_word(0, 0x90, 60 + i, 100));
  }
  s.f.render(256);
  AUD_CHECK(g_records.size() == 4);
  s.f.render(256);
  AUD_CHECK(g_records.size() == 8);
  s.f.render(256);
  AUD_CHECK(g_records.size() == 10);
  for (uint32_t i = 0; i < 10; ++i) {
    AUD_CHECK(aud_ump_note(g_records[i].word0) == 60 + i);
  }
  AUD_CHECK(aud_event_source_size(source) == 0);
  // Two sources share the budget in the order of their slots.
  AudEventSource* second = makeSource(s.f, s.recorder, AUD_SOURCE_TIMING_IMMEDIATE);
  for (uint32_t i = 0; i < 3; ++i) {
    pushWord(source, aud_ump_midi1_word(0, 0x90, 70 + i, 100));
    pushWord(second, aud_ump_midi1_word(0, 0x90, 80 + i, 100));
  }
  g_records.clear();
  s.f.render(256);
  AUD_CHECK(g_records.size() == 4);
  AUD_CHECK(aud_ump_note(g_records[3].word0) == 80);
  s.f.render(256);
  AUD_CHECK(g_records.size() == 6);
  AUD_CHECK(s.f.stats().events_dropped == 0);
}

AUD_TEST(source_keeps_a_packet_that_does_not_fit_the_block) {
  // A table of 3 * 4 + 256 + 512 entries; the queue of the control thread
  // fills a part of it, so the drain stops before a packet is lost.
  SourceFixture s([](AudGraphConfig& c) { c.max_events_per_block = 4; });
  AudEventSource* source = makeSource(s.f, s.recorder, AUD_SOURCE_TIMING_IMMEDIATE);
  const AudGraphStats before = s.f.stats();
  for (uint32_t i = 0; i < 800; ++i) {
    pushWord(source, aud_ump_midi1_word(0, 0x90, 60, 100));
  }
  aud_event_source_release(source);
  // The flush drops the note ons within the budget of a block.
  s.f.render(256);
  s.f.render(256);
  AUD_CHECK(sourceStats(source).dropped == 8);
  AUD_CHECK(sourceStats(source).releases == 0);
  for (uint32_t i = 0; i < 198; ++i) s.f.render(256);
  AUD_CHECK(s.f.stats().events_dropped == before.events_dropped);
  AUD_CHECK(sourceStats(source).dropped == 800);
  AUD_CHECK(sourceStats(source).releases == 1);
  AUD_CHECK(aud_event_source_size(source) == 0);
}

AUD_TEST(source_drops_within_the_budget_for_a_target_that_takes_no_events) {
  SourceFixture s([](AudGraphConfig& c) { c.max_events_per_block = 4; });
  AudEventSource* source = makeSource(s.f, s.recorder, AUD_SOURCE_TIMING_IMMEDIATE);
  s.f.commit([&] { aud_graph_remove_node(s.f.graph, s.recorder); });
  for (uint32_t i = 0; i < 10; ++i) {
    pushWord(source, aud_ump_midi1_word(0, 0x90, 60 + i, 100));
  }
  // The retired target takes no events: the drain drops at most the
  // budget per block, the rest waits in the queue.
  s.f.render(256);
  AUD_CHECK(sourceStats(source).dropped == 4);
  AUD_CHECK(aud_event_source_size(source) == 6);
  s.f.render(256);
  AUD_CHECK(sourceStats(source).dropped == 8);
  s.f.render(256);
  AUD_CHECK(sourceStats(source).dropped == 10);
  AUD_CHECK(aud_event_source_size(source) == 0);
  AUD_CHECK(sourceStats(source).releases == 1);
  // The flush of a release drops within the budget too and carries the
  // release over.
  for (uint32_t i = 0; i < 10; ++i) {
    pushWord(source, aud_ump_midi1_word(0, 0x90, 60 + i, 100));
  }
  aud_event_source_release(source);
  s.f.render(256);
  AUD_CHECK(sourceStats(source).dropped == 14);
  AUD_CHECK(sourceStats(source).releases == 1);
  s.f.render(256);
  s.f.render(256);
  AUD_CHECK(sourceStats(source).dropped == 20);
  AUD_CHECK(sourceStats(source).releases == 2);
  AUD_CHECK(aud_event_source_size(source) == 0);
  AUD_CHECK(s.f.stats().events_dropped == 0);
}

// ############################################################################
// Release

AUD_TEST(source_release_flushes_the_queue_and_closes_the_notes) {
  SourceFixture s;
  AudEventSource* source = makeSource(s.f, s.recorder, AUD_SOURCE_TIMING_IMMEDIATE);
  pushWord(source, aud_ump_midi1_word(0, 0x90, 60, 100));
  pushWord(source, aud_ump_midi1_word(0, 0x90, 64, 100));
  pushWord(source, ccWord(64, 127));
  pushWord(source, ccWord(66, 127, 2));
  s.f.render(256);
  AUD_CHECK(g_records.size() == 4);
  g_records.clear();
  // Pending: a note on (dropped), a note off (at 0), a controller (at 0);
  // then the notes still open close and the pedals lift.
  pushWord(source, aud_ump_midi1_word(0, 0x90, 62, 100));
  pushWord(source, aud_ump_midi1_word(0, 0x80, 60, 64));
  pushWord(source, ccWord(1, 50));
  aud_event_source_release(source);
  aud_event_source_release(source);  // idempotent
  s.f.render(256);
  AUD_CHECK(g_records.size() == 5);
  for (const Record& r : g_records) AUD_CHECK(r.offset == 0);
  // The note offs first - the flushed one and the one of the tracker -,
  // then the controllers, then the pedals.
  AUD_CHECK(isNoteOff(g_records[0]) && isNoteOff(g_records[1]));
  AUD_CHECK(aud_ump_note(g_records[0].word0) + aud_ump_note(g_records[1].word0) == 124);
  AUD_CHECK(isControl(g_records[2], 1, 50));
  AUD_CHECK(isControl(g_records[3], 64, 0) && aud_ump_channel(g_records[3].word0) == 0);
  AUD_CHECK(isControl(g_records[4], 66, 0) && aud_ump_channel(g_records[4].word0) == 2);
  const AudEventSourceStats stats = sourceStats(source);
  AUD_CHECK(stats.dropped == 1 && stats.releases == 1);
  AUD_CHECK(stats.delivered == 6);
  // Nothing is left: a second release only closes what is open - nothing.
  g_records.clear();
  aud_event_source_release(source);
  s.f.render(256);
  AUD_CHECK(g_records.empty());
  AUD_CHECK(sourceStats(source).releases == 2);
}

AUD_TEST(source_overflow_of_a_note_off_raises_release) {
  SourceFixture s;
  AudEventSource* source = makeSource(s.f, s.recorder, AUD_SOURCE_TIMING_IMMEDIATE, 0, 4);
  pushWord(source, aud_ump_midi1_word(0, 0x90, 60, 100));
  s.f.render(256);
  for (uint32_t i = 0; i < 4; ++i) {
    AUD_CHECK(pushWord(source, aud_ump_midi1_word(0, 0x90, 61 + i, 100)) == AUD_OK);
  }
  // A controller that does not fit is lost without a release.
  AUD_CHECK(pushWord(source, ccWord(1, 1)) == AUD_ERROR_QUEUE_FULL);
  AUD_CHECK(sourceStats(source).rejected == 1);
  // A note off that does not fit raises it: the pending note ons are
  // dropped and the sounding note closes.
  AUD_CHECK(pushWord(source, aud_ump_midi1_word(0, 0x80, 60, 64)) == AUD_ERROR_QUEUE_FULL);
  g_records.clear();
  s.f.render(256);
  AUD_CHECK(g_records.size() == 1);
  AUD_CHECK(isNoteOff(g_records[0]) && aud_ump_note(g_records[0].word0) == 60);
  const AudEventSourceStats stats = sourceStats(source);
  AUD_CHECK(stats.rejected == 2 && stats.dropped == 4 && stats.releases == 1);
  // A MIDI 1.0 note on with velocity 0 is a note off as well.
  for (uint32_t i = 0; i < 4; ++i) pushWord(source, ccWord(7, i));
  AUD_CHECK(pushWord(source, aud_ump_midi1_word(0, 0x90, 60, 0)) == AUD_ERROR_QUEUE_FULL);
  s.f.render(256);
  AUD_CHECK(sourceStats(source).releases == 2);
}

AUD_TEST(source_release_waits_for_room_in_the_block) {
  // A queue of 2048 note offs against a table of 3 * 1024 + 768: the
  // flush spans two blocks, no note off is lost, the notes close after it.
  SourceFixture s;
  AudEventSource* source = makeSource(s.f, s.recorder, AUD_SOURCE_TIMING_IMMEDIATE, 0, 8192);
  pushWord(source, aud_ump_midi1_word(0, 0x90, 60, 100));
  s.f.render(256);
  for (uint32_t i = 0; i < 5000; ++i) {
    AUD_CHECK(pushWord(source, aud_ump_midi1_word(0, 0x80, 61, 64)) == AUD_OK);
  }
  aud_event_source_release(source);
  g_records.clear();
  s.f.render(256);
  AUD_CHECK(g_records.size() < 5000);
  AUD_CHECK(sourceStats(source).releases == 0);
  s.f.render(256);
  AUD_CHECK(g_records.size() == 5001);
  uint32_t closed = 0;
  for (const Record& r : g_records) {
    if (r.position == 512 && isNoteOff(r) && aud_ump_note(r.word0) == 60) closed += 1;
  }
  AUD_CHECK(closed == 1);
  AUD_CHECK(sourceStats(source).releases == 1);
  AUD_CHECK(s.f.stats().events_dropped == 0);
}

AUD_TEST(source_releases_of_several_sources_carry_over_the_full_table) {
  // Three sources on three targets, each with 250 held notes and the
  // sustain and soft pedal on every group and channel, released in the
  // same block, with queues of controllers that fill the table of
  // 3 * 1024 + 768 at the moment the third source's notes close: every
  // note off and every pedal off arrives, in this block or the next.
  // Three fills, so that the room runs out before the notes, among the
  // notes and among the pedals of the third source.
  Fixture f;
  int32_t recorders[3];
  AudEventSource* sources[3];
  for (int32_t& recorder : recorders) recorder = f.node("aud.test.recorder");
  f.commit([&] {
    for (const int32_t recorder : recorders) f.connect(recorder, AUD_GRAPH_NODE);
  });
  aud_graph_start(f.graph);
  for (uint32_t i = 0; i < 3; ++i) {
    sources[i] = makeSource(f, recorders[i], AUD_SOURCE_TIMING_IMMEDIATE);
  }
  constexpr uint32_t kTable = 3 * 1024 + 256 + 512;
  constexpr uint32_t kNotes = 250;
  constexpr uint32_t kPedals = 512;
  constexpr uint32_t kFill = 900;
  const uint32_t shorts[] = {0, 100, kNotes + 100};
  for (const uint32_t shortBy : shorts) {
    for (AudEventSource* source : sources) {
      for (uint32_t i = 0; i < kNotes; ++i) {
        AUD_CHECK(pushWord(source, aud_ump_midi1_word(0, 0x90 | (i / 125), i % 125,
                                                      100)) == AUD_OK);
      }
      for (uint32_t group = 0; group < 16; ++group) {
        for (uint32_t channel = 0; channel < 16; ++channel) {
          AUD_CHECK(pushWord(source, ccWord(64, 127, channel, group)) == AUD_OK);
          AUD_CHECK(pushWord(source, ccWord(66, 127, channel, group)) == AUD_OK);
        }
      }
    }
    g_records.clear();
    for (int i = 0; i < 3; ++i) f.render(256);
    AUD_CHECK(g_records.size() == 3 * (kNotes + kPedals));
    // The fills: the first two sources release in full, the third one's
    // flush ends where the table ends, less `shortBy` events.
    const uint32_t fills[] = {kFill, kFill,
                              kTable - 2 * (kFill + kNotes + kPedals) - shortBy};
    for (uint32_t i = 0; i < 3; ++i) {
      for (uint32_t n = 0; n < fills[i]; ++n) {
        AUD_CHECK(pushWord(sources[i], ccWord(7, 1)) == AUD_OK);
      }
      aud_event_source_release(sources[i]);
    }
    g_records.clear();
    f.render(256);
    AUD_CHECK(g_records.size() == kTable);
    AUD_CHECK(sourceStats(sources[0]).releases == 1);
    AUD_CHECK(sourceStats(sources[1]).releases == 1);
    AUD_CHECK(sourceStats(sources[2]).releases == 0);
    f.render(256);
    AUD_CHECK(sourceStats(sources[2]).releases == 1);
    uint32_t noteOffs = 0;
    uint32_t pedalOffs = 0;
    uint32_t controllers = 0;
    for (const Record& r : g_records) {
      if (isNoteOff(r)) noteOffs += 1;
      if (isControl(r, 64, 0) || isControl(r, 66, 0)) pedalOffs += 1;
      if (isControl(r, 7, 1)) controllers += 1;
    }
    AUD_CHECK(noteOffs == 3 * kNotes);
    AUD_CHECK(pedalOffs == 3 * kPedals);
    AUD_CHECK(controllers == fills[0] + fills[1] + fills[2]);
    AUD_CHECK(g_records.size() == noteOffs + pedalOffs + controllers);
    AUD_CHECK(f.stats().events_dropped == 0);
    // Nothing is left for a further release.
    g_records.clear();
    f.render(256);
    AUD_CHECK(g_records.empty());
    for (AudEventSource* source : sources) aud_event_source_release(source);
    f.render(256);
    AUD_CHECK(g_records.empty());
    for (AudEventSource* source : sources) {
      AUD_CHECK(sourceStats(source).releases == 2);
    }
    for (AudEventSource* source : sources) {
      AUD_CHECK(aud_graph_destroy_event_source(f.graph, source) == AUD_OK);
    }
    f.render(256);
    f.takeAll();
    for (uint32_t i = 0; i < 3; ++i) {
      sources[i] = makeSource(f, recorders[i], AUD_SOURCE_TIMING_IMMEDIATE);
    }
  }
  AUD_CHECK(f.stats().realtime_violations == 0);
}

// ############################################################################
// Retirement of the target

AUD_TEST(source_drops_packets_for_a_retired_target_and_keeps_it_alive) {
  SourceFixture s;
  AudEventSource* source = makeSource(s.f, s.recorder, AUD_SOURCE_TIMING_IMMEDIATE);
  pushWord(source, aud_ump_midi1_word(0, 0x90, 60, 100));
  pushWord(source, ccWord(64, 127));
  s.f.render(256);
  s.f.commit([&] { aud_graph_remove_node(s.f.graph, s.recorder); });
  pushWord(source, aud_ump_midi1_word(0, 0x90, 62, 100));
  g_records.clear();
  s.f.render(256);
  // The adoption closed the note; the source lifted its pedal with it.
  AUD_CHECK(g_records.size() == 2);
  AUD_CHECK(isNoteOff(g_records[0]) && aud_ump_note(g_records[0].word0) == 60);
  AUD_CHECK(isControl(g_records[1], 64, 0));
  AUD_CHECK(sourceStats(source).dropped == 1);
  AUD_CHECK(sourceStats(source).releases == 1);
  for (int i = 0; i < 3; ++i) s.f.render(256);
  AUD_CHECK(s.f.countNotifications(AUD_NOTIFY_NODE_DONE) == 1);
  // The instance stays while the source targets it.
  AUD_CHECK(aud_graph_node_descriptor(s.f.graph, s.recorder) != nullptr);
  pushWord(source, aud_ump_midi1_word(0, 0x90, 62, 100));
  s.f.render(256);
  AUD_CHECK(sourceStats(source).dropped == 2);
  AUD_CHECK(aud_graph_destroy_event_source(s.f.graph, source) == AUD_OK);
  s.f.render(256);
  AUD_CHECK(s.f.countNotifications(AUD_NOTIFY_SOURCE_DONE) == 1);
  AUD_CHECK(aud_graph_node_descriptor(s.f.graph, s.recorder) == nullptr);
}

AUD_TEST(retiring_emitters_keep_their_routes_to_living_nodes) {
  Fixture f;
  const int32_t emitter = f.node("aud.test.emitter");
  const int32_t recorder = f.node("aud.test.recorder");
  f.commit([&] {
    f.connect(recorder, AUD_GRAPH_NODE);
    aud_graph_connect_events(f.graph, emitter, 0, recorder, 0);
    aud_graph_connect_events(f.graph, emitter, 0, AUD_GRAPH_NODE, 0);
  });
  aud_graph_start(f.graph);
  const AudEvent on = noteOn(60);
  aud_graph_send_event(f.graph, emitter, &on, nullptr, 0);
  f.render(256);
  AUD_CHECK(g_records.size() == 1);
  f.takeAll();
  // The emitter retires: the note off it forwards while it closes its
  // notes arrives at the recorder and at the graph's event input.
  f.commit([&] { aud_graph_remove_node(f.graph, emitter); });
  f.render(256);
  AUD_CHECK(g_records.size() == 2);
  AUD_CHECK(isNoteOff(g_records[1]) && aud_ump_note(g_records[1].word0) == 60);
  for (int i = 0; i < 3; ++i) f.render(256);
  uint32_t forwarded = 0;
  uint32_t done = 0;
  for (const AudGraphNotification& n : f.takeAll()) {
    if (n.type == AUD_NOTIFY_EVENT && n.node == emitter) forwarded += 1;
    if (n.type == AUD_NOTIFY_NODE_DONE) done += 1;
  }
  AUD_CHECK(forwarded == 1 && done == 1);
  // The program without the emitter is adopted, then the instance goes.
  f.render(256);
  f.takeAll();
  AUD_CHECK(aud_graph_node_descriptor(f.graph, emitter) == nullptr);
  // A route to a node removed in the same commit is not kept.
  const int32_t second = f.node("aud.test.emitter");
  const int32_t third = f.node("aud.test.recorder");
  f.commit([&] {
    f.connect(third, AUD_GRAPH_NODE);
    aud_graph_connect_events(f.graph, second, 0, third, 0);
  });
  aud_graph_send_event(f.graph, second, &on, nullptr, 0);
  f.render(256);
  g_records.clear();
  f.commit([&] {
    aud_graph_remove_node(f.graph, second);
    aud_graph_remove_node(f.graph, third);
  });
  f.render(256);
  AUD_CHECK(g_records.size() == 1);  // the recorder's own closing only
}

AUD_TEST(a_route_the_transaction_disconnected_does_not_survive_the_emitter) {
  Fixture f;
  const int32_t emitter = f.node("aud.test.emitter");
  const int32_t recorder = f.node("aud.test.recorder");
  f.commit([&] {
    f.connect(recorder, AUD_GRAPH_NODE);
    aud_graph_connect_events(f.graph, emitter, 0, recorder, 0);
    aud_graph_connect_events(f.graph, emitter, 0, AUD_GRAPH_NODE, 0);
  });
  aud_graph_start(f.graph);
  const AudEvent on = noteOn(60);
  aud_graph_send_event(f.graph, emitter, &on, nullptr, 0);
  f.render(256);
  AUD_CHECK(g_records.size() == 1);
  f.takeAll();
  // The transaction disconnects the emitter from the recorder and removes
  // it: only the route its removal dropped - to the graph's event input -
  // stays while it closes its notes; the recorder gets nothing.
  f.commit([&] {
    AUD_CHECK(aud_graph_disconnect_events(f.graph, emitter, 0, recorder, 0) == AUD_OK);
    aud_graph_remove_node(f.graph, emitter);
  });
  for (int i = 0; i < 4; ++i) f.render(256);
  AUD_CHECK(g_records.size() == 1);
  uint32_t forwarded = 0;
  uint32_t done = 0;
  for (const AudGraphNotification& n : f.takeAll()) {
    if (n.type == AUD_NOTIFY_EVENT && n.node == emitter) forwarded += 1;
    if (n.type == AUD_NOTIFY_NODE_DONE) done += 1;
  }
  AUD_CHECK(forwarded == 1 && done == 1);
  // A rolled back removal drops no route: the emitter plays on.
  const int32_t second = f.node("aud.test.emitter");
  f.commit([&] { aud_graph_connect_events(f.graph, second, 0, recorder, 0); });
  aud_graph_begin(f.graph);
  aud_graph_remove_node(f.graph, second);
  aud_graph_rollback(f.graph);
  aud_graph_send_event(f.graph, second, &on, nullptr, 0);
  f.render(256);
  AUD_CHECK(g_records.size() == 2);
}

// ############################################################################
// Producers

AUD_TEST(source_hands_over_between_producers) {
  SourceFixture s;
  AudEventSource* source = makeSource(s.f, s.recorder, AUD_SOURCE_TIMING_IMMEDIATE);
  AUD_CHECK(aud_event_source_attach(source, 0) == AUD_ERROR_INVALID_ARGUMENT);
  AUD_CHECK(aud_event_source_attach(nullptr, 1) == AUD_ERROR_INVALID_ARGUMENT);
  AUD_CHECK(aud_event_source_detach(nullptr, 1) == AUD_ERROR_INVALID_ARGUMENT);
  AUD_CHECK(aud_event_source_detach(source, 1) == AUD_ERROR_INVALID_ARGUMENT);
  AUD_CHECK(aud_event_source_attach(source, 1) == AUD_OK);
  AUD_CHECK(sourceStats(source).attached == 1);
  AUD_CHECK(aud_event_source_attach(source, 2) == AUD_GRAPH_ERROR_BUSY);
  AUD_CHECK(aud_event_source_detach(source, 2) == AUD_ERROR_INVALID_ARGUMENT);
  AUD_CHECK(aud_event_source_detach(source, 1) == AUD_OK);
  AUD_CHECK(sourceStats(source).attached == 0);
  // Two producers one after the other on threads of their own, while the
  // main thread renders: the detach orders the pushes of the first before
  // those of the second.
  std::atomic<bool> stop{false};
  std::atomic<uint32_t> blocks{0};
  std::thread stream([&] {
    while (!stop.load(std::memory_order_acquire)) {
      s.f.render(256);
      blocks.fetch_add(1, std::memory_order_release);
      std::this_thread::sleep_for(std::chrono::microseconds(200));
    }
  });
  auto produce = [&](uint64_t token, uint32_t first) {
    AUD_CHECK(aud_event_source_attach(source, token) == AUD_OK);
    for (uint32_t i = 0; i < 500; ++i) {
      while (pushWord(source, aud_ump_midi1_word(0, 0x90, 1 + (first + i) % 120, 100)) !=
             AUD_OK) {
        std::this_thread::yield();
      }
      while (pushWord(source, aud_ump_midi1_word(0, 0x80, 1 + (first + i) % 120, 64)) !=
             AUD_OK) {
        std::this_thread::yield();
      }
    }
    AUD_CHECK(aud_event_source_detach(source, token) == AUD_OK);
  };
  std::thread first([&] { produce(11, 0); });
  first.join();
  std::thread second([&] { produce(12, 60); });
  second.join();
  const uint32_t target = blocks.load(std::memory_order_acquire) + 4;
  while (blocks.load(std::memory_order_acquire) < target) std::this_thread::yield();
  stop.store(true, std::memory_order_release);
  stream.join();
  AUD_CHECK(g_records.size() == 2000);
  AUD_CHECK(sourceStats(source).pushed == 2000);
  AUD_CHECK(sourceStats(source).delivered == 2000);
  AUD_CHECK(sourceStats(source).reordered == 0);
  AUD_CHECK(s.f.stats().realtime_violations == 0);
}

// ############################################################################
// The timing of the engine

AUD_TEST(graph_timing_reports_the_last_block) {
  Fixture f;
  int32_t result = 0;
  AudGraphTiming t = timingOf(f.graph, &result);
  AUD_CHECK(result == AUD_ERROR_STATE && t.flags == 0);
  AUD_CHECK(aud_graph_timing(nullptr, &t) == AUD_ERROR_INVALID_ARGUMENT);
  AUD_CHECK(aud_graph_timing(f.graph, nullptr) == AUD_ERROR_INVALID_ARGUMENT);
  const int32_t osc = f.node(AUD_GRAPH_OSCILLATOR_TYPE_ID);
  f.commit([&] { f.connect(osc, AUD_GRAPH_NODE); });
  aud_graph_start(f.graph);
  TimedStream stream(f, 1000000000LL, 480);
  stream.render(256);
  std::this_thread::sleep_for(std::chrono::milliseconds(20));
  stream.render(128);
  t = timingOf(f.graph, &result);
  AUD_CHECK(result == AUD_OK);
  AUD_CHECK(t.flags == AUD_TIMING_VALID);
  AUD_CHECK(t.sample_rate == kRate);
  AUD_CHECK(t.sample_position == 256 && t.frames == 128);
  AUD_CHECK(t.host_time_ns == stream.atFrame(256));
  AUD_CHECK(t.output_latency_frames == 480);
  AUD_CHECK(t.nominal_period_ns == 5333333);
  AUD_CHECK(t.max_period_ns >= 20000000);
  AUD_CHECK(t.time_filter_resets == 0);
  // The histogram holds the one gap; the first render counts nothing.
  AUD_CHECK(periodTotal(t) == 1);
  AUD_CHECK(periodPercentileNs(t, 0.999) >= 20000000);
  // A reset of the filter starts the measure again: the render that reset
  // it counts nothing, like the first render of a stream; the next one is
  // the first gap counted.
  stream.position += 100000;
  stream.render(256);
  t = timingOf(f.graph);
  AUD_CHECK(t.time_filter_resets == 1);
  AUD_CHECK(t.max_period_ns == 0);
  AUD_CHECK(periodTotal(t) == 0);
  AUD_CHECK(periodPercentileNs(t, 0.999) == 0);
  stream.render(256);
  t = timingOf(f.graph);
  AUD_CHECK(t.time_filter_resets == 1);
  AUD_CHECK(periodTotal(t) == 1);
  AUD_CHECK(periodPercentileNs(t, 0.999) < 20000000);
  // Offline: the longest block.
  aud_graph_stop(f.graph);
  aud_graph_start(f.graph);
  std::vector<float> out(2000, 0.0f);
  float* channel = out.data();
  AudAudioBus bus{sizeof(AudAudioBus), 1, &channel};
  const uint32_t sizes[] = {64, 960, 256};
  AudOfflineRequest request{};
  request.struct_size = sizeof(AudOfflineRequest);
  request.frames = 2000;
  request.num_block_sizes = 3;
  request.block_sizes = sizes;
  request.outputs = &bus;
  AUD_CHECK(aud_graph_render_offline(f.graph, &request) == AUD_ERROR_INVALID_ARGUMENT);
  aud_graph_stop(f.graph);
  aud_graph_prepare(f.graph, 0, 1024);
  aud_graph_start(f.graph);
  AUD_CHECK(aud_graph_render_offline(f.graph, &request) == AUD_OK);
  t = timingOf(f.graph);
  AUD_CHECK(t.flags == (AUD_TIMING_VALID | AUD_TIMING_OFFLINE));
  AUD_CHECK(t.max_period_ns == 20000000);
  AUD_CHECK(t.nominal_period_ns == 21333333);
  // The histogram holds the block durations: 64, 960, 256, 64 and the
  // rest of 656 frames - deterministic, so a second run matches.
  AUD_CHECK(periodTotal(t) == 5);
  AUD_CHECK(t.period_histogram[13] == 2);   // 1.33 ms
  AUD_CHECK(t.period_histogram[53] == 1);   // 5.33 ms
  AUD_CHECK(t.period_histogram[136] == 1);  // 13.67 ms
  AUD_CHECK(t.period_histogram[200] == 1);  // 20 ms
  AUD_CHECK(periodPercentileNs(t, 0.5) == 5400000);
  AUD_CHECK(periodPercentileNs(t, 0.999) == 20100000);
  aud_graph_stop(f.graph);
  aud_graph_start(f.graph);
  AUD_CHECK(aud_graph_render_offline(f.graph, &request) == AUD_OK);
  const AudGraphTiming again = timingOf(f.graph);
  AUD_CHECK(std::memcmp(again.period_histogram, t.period_histogram,
                        sizeof(t.period_histogram)) == 0);
}

AUD_TEST(graph_timing_histogram_ignores_a_single_gap_at_start_up) {
  Fixture f;
  const int32_t osc = f.node(AUD_GRAPH_OSCILLATOR_TYPE_ID);
  f.commit([&] { f.connect(osc, AUD_GRAPH_NODE); });
  aud_graph_start(f.graph);
  TimedStream stream(f);
  // The stream of an iPhone: a start-up gap of 177 ms, then a callback
  // every 5.33 ms. The maximum is the gap; p99.9 of 1001 gaps is not.
  stream.render(256);
  std::this_thread::sleep_for(std::chrono::milliseconds(177));
  stream.render(256);
  renderPaced(stream, std::chrono::steady_clock::now(), 5333333, 1000);
  const AudGraphTiming t = timingOf(f.graph);
  AUD_CHECK(t.time_filter_resets == 0);
  AUD_CHECK(t.max_period_ns >= 177000000);
  AUD_CHECK(periodTotal(t) == 1001);
  AUD_CHECK(t.period_histogram[AUD_GRAPH_PERIOD_BINS] == 1);
  // This host paces with jitter, so the bounds are wide: the median is
  // the callback, p99.9 stays far below the start-up gap.
  const int64_t p50 = periodPercentileNs(t, 0.5);
  AUD_CHECK(p50 >= 4000000 && p50 < 10000000);
  const int64_t p999 = periodPercentileNs(t, 0.999);
  AUD_CHECK(p999 >= 5400000 && p999 < 40000000);
  AUD_CHECK(periodPercentileNs(t, 1.0) > AUD_GRAPH_PERIOD_BINS * AUD_GRAPH_PERIOD_BIN_NS);
  AUD_CHECK(f.stats().realtime_violations == 0);
}

AUD_TEST(graph_timing_histogram_drops_the_gap_that_reset_the_filter) {
  Fixture f;
  const int32_t osc = f.node(AUD_GRAPH_OSCILLATOR_TYPE_ID);
  f.commit([&] { f.connect(osc, AUD_GRAPH_NODE); });
  aud_graph_start(f.graph);
  TimedStream stream(f);
  // A route change on an iPhone: the stream pauses for 60 ms and comes
  // back with a jump of its sample position against its host time, so
  // the time filter resets. The gap that reset it is no sample of the
  // new measure - the histogram counts the renders after the reset only,
  // none of them in the overflow bin, so p99.9 is the callback again and
  // not the edge of the histogram.
  renderPaced(stream, std::chrono::steady_clock::now(), 5333333, 20);
  std::this_thread::sleep_for(std::chrono::milliseconds(60));
  stream.position += 100000;
  stream.render(256);
  AudGraphTiming t = timingOf(f.graph);
  AUD_CHECK(t.time_filter_resets == 1);
  AUD_CHECK(periodTotal(t) == 0);
  AUD_CHECK(t.max_period_ns == 0);
  renderPaced(stream, std::chrono::steady_clock::now(), 5333333, 20);
  t = timingOf(f.graph);
  AUD_CHECK(t.time_filter_resets == 1);
  AUD_CHECK(periodTotal(t) == 20);
  AUD_CHECK(t.period_histogram[AUD_GRAPH_PERIOD_BINS] == 0);
  AUD_CHECK(t.max_period_ns < 50000000);
  AUD_CHECK(periodPercentileNs(t, 0.999) < 50000000);
}

AUD_TEST(graph_timing_histogram_takes_the_gap_of_split_callbacks) {
  Fixture f;
  const int32_t osc = f.node(AUD_GRAPH_OSCILLATOR_TYPE_ID);
  f.commit([&] { f.connect(osc, AUD_GRAPH_NODE); });
  aud_graph_start(f.graph);
  TimedStream stream(f);
  // A stream that splits a callback of 10.7 ms into two renders back to
  // back: half of the gaps are near zero, p99.9 is the callback.
  // The gap within a pair is the time one render takes, which the thread
  // sanitizer on a loaded host stretches past 1 ms. The test sleeps the
  // callback after each pair, so the gaps between the pairs never fall
  // below it, and 5 ms - less than the 5.33 ms of a block - tell them apart.
  for (uint32_t i = 0; i < 20; ++i) {
    if (i > 0) std::this_thread::sleep_for(std::chrono::nanoseconds(10666667));
    stream.render(256);
    stream.render(256);
  }
  const AudGraphTiming t = timingOf(f.graph);
  AUD_CHECK(periodTotal(t) == 39);
  uint64_t belowFiveMs = 0;
  for (uint32_t i = 0; i < 50; ++i) belowFiveMs += t.period_histogram[i];
  AUD_CHECK(belowFiveMs == 20);
  AUD_CHECK(periodPercentileNs(t, 0.5) <= 5000000);
  // With 39 gaps p99.9 is the longest one; a loaded host overshoots its
  // sleep by several milliseconds, so only the lower bound is tight.
  const int64_t p999 = periodPercentileNs(t, 0.999);
  AUD_CHECK(p999 >= 10700000 && p999 < 40000000);
  AUD_CHECK(t.max_period_ns >= 10666667);
}
