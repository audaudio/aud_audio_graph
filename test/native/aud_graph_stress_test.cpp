// @license
// Copyright (c) Audanika. All Rights Reserved.
//
// Use of this source code is governed by terms that can be
// found in the LICENSE file in the root of this package.

// The stress tests of ticket 20 (S2b): the realtime contract of
// interop-002 and the route-change sequence of lifecycle-001 under load,
// with a click detector on the output and a stream that renders on its own
// thread while the control thread calls everything it may call. They run
// under every sanitizer of scripts/test-native.js and under the watchdog,
// like every native test.

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <set>
#include <thread>
#include <vector>

#include "aud_audio_graph.h"
#include "aud_graph_fixture.hpp"
#include "aud_test.hpp"
#include "aud_transport.h"
#include "aud_ump.h"

using namespace aud_test_fixture;

namespace {

// A small deterministic generator, so that a failure repeats.
struct Random {
  uint64_t state;
  explicit Random(uint64_t seed) : state(seed * 0x9E3779B97F4A7C15ull + 1) {}
  uint32_t next() {
    state ^= state << 13;
    state ^= state >> 7;
    state ^= state << 17;
    return static_cast<uint32_t>(state >> 32);
  }
  uint32_t below(uint32_t n) { return next() % n; }
};

// The click detector: the largest second difference of a signal. A step of
// size A shows as about A; a fade of a signal of amplitude A over 240
// frames as A / 240; a steady sine as A (2 pi f / rate)^2.
constexpr double kClickFree = 0.02;   // the swaps stay below it
constexpr double kHardClick = 0.05;   // a swap without a fade exceeds it
double clickScore(const std::vector<float>& samples) {
  double worst = 0;
  for (size_t i = 2; i < samples.size(); ++i) {
    const double d = static_cast<double>(samples[i]) - 2.0 * samples[i - 1] +
                     samples[i - 2];
    worst = std::max(worst, std::fabs(d));
  }
  return worst;
}

// A control event that carries `id` as its control number.
AudEvent control(uint32_t id) { return aud_event_control_float(id, 0.0f, 0, 0); }

// The number of diagnostics of `code` in the notifications, summed.
uint32_t diagnostics(const std::vector<AudGraphNotification>& notifications,
                     int32_t code) {
  uint32_t count = 0;
  for (const AudGraphNotification& n : notifications) {
    if (n.type == AUD_NOTIFY_DIAGNOSTIC && n.code == code) count += n.count;
  }
  return count;
}

int64_t ticksOf(double frames, double rate, double tempo) {
  return aud_ticks_from_beats(frames * tempo / (60.0 * rate));
}

}  // namespace

// ############################################################################
// Queue overflow (interop-002)

AUD_TEST(stress_queues_refuse_when_full_and_carry_over_in_order) {
  Fixture f(0, 1, 256, [](AudGraphConfig& c) {
    c.event_queue_capacity = 64;
    c.param_queue_capacity = 32;
    c.max_events_per_block = 16;
  });
  const int32_t recorder = f.node("aud.test.recorder");
  f.commit([&] { f.connect(recorder, AUD_GRAPH_NODE); });
  aud_graph_start(f.graph);
  f.render(256);
  // Events: 64 fit, the rest is refused at the enqueue.
  uint32_t accepted = 0;
  for (uint32_t i = 0; i < 100; ++i) {
    const AudEvent e = control(i);
    const int32_t result = aud_graph_send_event(f.graph, recorder, &e, nullptr, 0);
    if (result == AUD_OK) {
      accepted += 1;
    } else {
      AUD_CHECK(result == AUD_ERROR_QUEUE_FULL);
    }
  }
  AUD_CHECK(accepted == 64);
  AUD_CHECK(f.stats().rejected == 36);
  // 16 per block, carried over in order; new events queue behind them.
  for (uint32_t block = 0; block < 4; ++block) {
    f.render(256);
    AUD_CHECK(g_records.size() == 16 * (block + 1));
    const AudEvent e = control(1000 + block);
    AUD_CHECK(aud_graph_send_event(f.graph, recorder, &e, nullptr, 0) == AUD_OK);
  }
  f.render(256);
  AUD_CHECK(g_records.size() == 68);
  for (uint32_t k = 0; k < 64; ++k) AUD_CHECK(g_records[k].word0 == k);
  for (uint32_t k = 0; k < 4; ++k) AUD_CHECK(g_records[64 + k].word0 == 1000 + k);
  // Parameters: 32 fit; the budget applies them over two blocks in order,
  // so the latest value of a parameter wins.
  accepted = 0;
  for (uint32_t i = 0; i < 40; ++i) {
    if (aud_graph_set_param(f.graph, recorder, i % 2, static_cast<float>(i), 0) ==
        AUD_OK) {
      accepted += 1;
    }
  }
  AUD_CHECK(accepted == 32);
  AUD_CHECK(f.stats().rejected == 36 + 8);
  f.render(256);
  AUD_CHECK(g_params.size() == 16);
  f.render(256);
  AUD_CHECK(g_params.size() == 32);
  for (uint32_t k = 0; k < 32; ++k) {
    AUD_CHECK(g_params[k].first == k % 2 && g_params[k].second == static_cast<float>(k));
  }
  // Nothing was dropped on the way.
  const AudGraphStats s = f.stats();
  AUD_CHECK(s.events_dropped == 0 && s.events_delivered == 68 && s.params_applied == 32);
  AUD_CHECK(diagnostics(f.takeAll(), AUD_ERROR_QUEUE_FULL) == 0);
}

// ############################################################################
// Late events (interop-002)

AUD_TEST(stress_late_events_play_at_the_block_start_or_drop_by_policy) {
  for (const bool drop : {false, true}) {
    Fixture f(0, 1, 256, [drop](AudGraphConfig& c) {
      if (drop) c.flags = AUD_GRAPH_DROP_LATE_EVENTS;
    });
    const int32_t recorder = f.node("aud.test.recorder");
    f.commit([&] { f.connect(recorder, AUD_GRAPH_NODE); });
    aud_graph_start(f.graph);
    const uint32_t sizes[] = {64, 37, 256, 1, 128};
    std::vector<int64_t> due;     // by control number
    std::vector<bool> isLate;     // by control number
    std::vector<int64_t> blockOf;  // the block start a late event plays at
    Random random(drop ? 2 : 1);
    uint32_t late = 0;
    std::vector<AudGraphNotification> notifications;
    for (uint32_t block = 0; block < 300; ++block) {
      const uint32_t frames = sizes[block % 5];
      const int64_t position = aud_graph_sample_position(f.graph);
      const uint32_t count = random.below(6);
      for (uint32_t k = 0; k < count; ++k) {
        const bool past = random.below(2) == 0;
        const int64_t at = past ? position - 1 - random.below(500)
                                : position + random.below(frames);
        const AudTimestamp t = atSample(at);
        const AudEvent e = control(static_cast<uint32_t>(due.size()));
        AUD_CHECK(aud_graph_send_event(f.graph, recorder, &e, &t, 0) == AUD_OK);
        due.push_back(at);
        isLate.push_back(past);
        blockOf.push_back(position);
        if (past) late += 1;
      }
      f.render(frames);
      const std::vector<AudGraphNotification> batch = f.takeAll();
      notifications.insert(notifications.end(), batch.begin(), batch.end());
    }
    const AudGraphStats s = f.stats();
    AUD_CHECK(diagnostics(notifications, AUD_ERROR_LATE) == late);
    if (drop) {
      AUD_CHECK(s.events_dropped == late && s.events_late == 0);
      AUD_CHECK(g_records.size() == due.size() - late);
    } else {
      AUD_CHECK(s.events_late == late && s.events_dropped == 0);
      AUD_CHECK(g_records.size() == due.size());
    }
    // Every delivered event lands at its time, a late one at the start of
    // the block that took it.
    for (const Record& r : g_records) {
      const uint32_t id = r.word0;
      if (isLate[id]) {
        AUD_CHECK(!drop && r.offset == 0 && r.position == blockOf[id]);
      } else {
        AUD_CHECK(r.position + r.offset == due[id]);
      }
    }
  }
}

// ############################################################################
// Graph swaps under load (graph-003)

namespace {

constexpr uint32_t kSwapSlots = 6;

// Renders `blocks` blocks with a transaction in front of every one:
// oscillators are connected to a mixer, disconnected, removed and replaced
// while notes retune and gate them. Returns the output; counts the removed
// nodes and the nodes the engine reported done.
std::vector<float> swapUnderLoad(Fixture& f, uint32_t blocks, uint64_t seed,
                                 uint32_t* removed, uint32_t* done) {
  const uint32_t one = 1;
  const uint32_t ones[AUD_MIXER_NUM_INPUTS] = {1, 1, 1, 1, 1, 1, 1, 1};
  AudNodeConfig mixerConfig{sizeof(AudNodeConfig), AUD_MIXER_NUM_INPUTS, ones, 1,
                            &one, 0, 0};
  const int32_t mixer = f.node(AUD_GRAPH_MIXER_TYPE_ID, &mixerConfig);
  struct Slot {
    int32_t node = 0;
    bool connected = false;
  };
  Slot slots[kSwapSlots];
  const auto oscillator = [&](uint32_t k) {
    const int32_t osc = f.node(AUD_GRAPH_OSCILLATOR_TYPE_ID);
    aud_graph_set_param(f.graph, osc, AUD_OSCILLATOR_PARAM_FREQUENCY,
                        110.0f * static_cast<float>(k + 1), 0);
    aud_graph_set_param(f.graph, osc, AUD_OSCILLATOR_PARAM_AMPLITUDE, 0.15f, 0);
    return osc;
  };
  f.commit([&] {
    for (uint32_t k = 0; k < kSwapSlots; ++k) slots[k].node = oscillator(k);
    f.connect(mixer, AUD_GRAPH_NODE);
  });
  aud_graph_start(f.graph);
  Random random(seed);
  const uint32_t sizes[] = {256, 64, 128, 31, 200};
  std::vector<float> output;
  for (uint32_t block = 0; block < blocks; ++block) {
    const uint32_t k = random.below(kSwapSlots);
    Slot& slot = slots[k];
    const uint32_t action = random.below(3);
    const int32_t revision = f.commit([&] {
      if (!slot.connected) {
        AUD_CHECK(f.connect(slot.node, mixer, 0, k) == AUD_OK);
        slot.connected = true;
      } else if (action == 0) {
        // The old node fades out, its replacement fades in.
        AUD_CHECK(aud_graph_remove_node(f.graph, slot.node) == AUD_OK);
        *removed += 1;
        slot.node = oscillator(k);
        AUD_CHECK(f.connect(slot.node, mixer, 0, k) == AUD_OK);
      } else {
        AUD_CHECK(aud_graph_disconnect(f.graph, slot.node, 0, mixer, k) == AUD_OK);
        slot.connected = false;
      }
    });
    AUD_CHECK(revision > 0);
    if (random.below(3) == 0) {
      // A note on retunes and gates an oscillator, a note off closes it.
      const uint32_t note = 45 + random.below(12);
      const AudEvent e = random.below(2) == 0 ? noteOn(note) : noteOff(note);
      const Slot& target = slots[random.below(kSwapSlots)];
      AUD_CHECK(aud_graph_send_event(f.graph, target.node, &e, nullptr, 0) == AUD_OK);
    }
    const uint32_t frames = sizes[block % 5];
    f.render(frames);
    output.insert(output.end(), f.out[0].begin(), f.out[0].begin() + frames);
    for (const AudGraphNotification& n : f.takeAll()) {
      if (n.type == AUD_NOTIFY_NODE_DONE) *done += 1;
    }
  }
  // The last removed nodes finish their fades.
  for (uint32_t block = 0; block < 4; ++block) {
    f.render(256);
    output.insert(output.end(), f.out[0].begin(), f.out[0].begin() + 256);
    for (const AudGraphNotification& n : f.takeAll()) {
      if (n.type == AUD_NOTIFY_NODE_DONE) *done += 1;
    }
  }
  return output;
}

}  // namespace

AUD_TEST(stress_swaps_the_graph_every_block_without_clicks) {
  uint32_t removed = 0;
  uint32_t done = 0;
  Fixture f;
  const std::vector<float> output = swapUnderLoad(f, 1500, 11, &removed, &done);
  const double score = clickScore(output);
  AUD_CHECK(score < kClickFree);
  AUD_CHECK(removed > 100 && done == removed);
  float peak = 0;
  for (const float v : output) peak = std::max(peak, std::fabs(v));
  AUD_CHECK(peak > 0.1f);  // the oscillators were heard
  // Every transaction was adopted - a finished node recompiles, so the
  // revision may be later; the retired instances are gone.
  AUD_CHECK(aud_graph_revision(f.graph) >= 1501);
  AUD_CHECK(aud_graph_nodes(f.graph, nullptr, 0) == static_cast<int32_t>(kSwapSlots + 1));
  // The detector itself: without fades the same script clicks.
  uint32_t removedHard = 0;
  uint32_t doneHard = 0;
  Fixture hard(0, 1, 256, [](AudGraphConfig& c) { c.fade_frames = 1; });
  const double hardScore = clickScore(swapUnderLoad(hard, 300, 11, &removedHard, &doneHard));
  AUD_CHECK(hardScore > kHardClick);
}

// ############################################################################
// Route changes (lifecycle-001)

AUD_TEST(stress_route_changes_keep_the_transport_and_close_the_notes) {
  // Buffers for the largest block of the test; the graph starts at 256.
  Fixture f(0, 1, 1024);
  AUD_CHECK(aud_graph_prepare(f.graph, kRate, 256) == AUD_OK);
  const int32_t osc = f.node(AUD_GRAPH_OSCILLATOR_TYPE_ID);
  const int32_t recorder = f.node("aud.test.recorder");
  f.commit([&] {
    f.connect(osc, AUD_GRAPH_NODE);
    f.connect(recorder, AUD_GRAPH_NODE);
  });
  aud_graph_start(f.graph);
  AUD_CHECK(f.request(AUD_TRANSPORT_REQUEST_START) == AUD_OK);
  f.render(256);
  for (uint32_t note = 60; note < 64; ++note) {
    const AudEvent on = noteOn(note);
    AUD_CHECK(aud_graph_send_event(f.graph, osc, &on, nullptr, 0) == AUD_OK);
    AUD_CHECK(aud_graph_send_event(f.graph, recorder, &on, nullptr, 0) == AUD_OK);
  }
  for (int i = 0; i < 20; ++i) f.render(256);
  AUD_CHECK(std::fabs(f.out[0][100]) > 0 || std::fabs(f.out[0][101]) > 0);
  const AudGraphTransportState before = f.transport();
  AUD_CHECK(before.playing == 1);
  // An event two beats ahead must survive the route change.
  AudTimestamp twoBeats{};
  twoBeats.struct_size = sizeof(AudTimestamp);
  twoBeats.domain = AUD_TIME_BEAT;
  twoBeats.value = before.beat + 2 * AUD_BEAT_FACTOR;
  const AudEvent marker = control(777);
  AUD_CHECK(aud_graph_send_event(f.graph, recorder, &marker, &twoBeats, 0) == AUD_OK);
  const int64_t positionBefore = aud_graph_sample_position(f.graph);
  // The device switches to 44.1 kHz and blocks of 512: suspend, prepare,
  // resume - while four notes play.
  g_records.clear();
  AUD_CHECK(aud_graph_suspend(f.graph) == AUD_OK);
  AUD_CHECK(aud_graph_prepare(f.graph, 44100, 512) == AUD_OK);
  AUD_CHECK(aud_graph_resume(f.graph) == AUD_OK);
  AUD_CHECK(f.render(512) == AUD_OK);
  AUD_CHECK(aud_graph_sample_position(f.graph) == positionBefore + 512);
  // The notes were closed at the start of the first block after it, and
  // the oscillator's reset closed its gate.
  AUD_CHECK(g_records.size() == 4);
  for (const Record& r : g_records) {
    AUD_CHECK(r.offset == 0 && r.position == positionBefore);
    AUD_CHECK(aud_ump_is_note_off(r.word0, 0));
  }
  AUD_CHECK(f.out[0][100] == 0 && f.out[0][511] == 0);
  // The transport went on where it was, at the new rate.
  const AudGraphTransportState after = f.transport();
  AUD_CHECK(after.playing == 1);
  const int64_t expected = before.beat + ticksOf(512, 44100, 120);
  AUD_CHECK(std::llabs(after.beat - expected) <= 2);
  // The marker plays two beats after it was set: 44100 frames later.
  for (int i = 0; i < 90; ++i) f.render(512);
  const Record* found = nullptr;
  for (const Record& r : g_records) {
    if (r.type == AUD_EVENT_CONTROL && r.word0 == 777) found = &r;
  }
  AUD_CHECK(found != nullptr);
  if (found != nullptr) {
    // Two beats at 120 bpm are one second: 44100 frames at the new rate.
    AUD_CHECK(std::llabs(found->position + found->offset - (positionBefore + 44100)) <= 2);
  }
  // A stop keeps the position; after a prepare and a start the transport
  // plays on from it, and the notes played since are closed.
  const AudEvent on = noteOn(70);
  AUD_CHECK(aud_graph_send_event(f.graph, recorder, &on, nullptr, 0) == AUD_OK);
  f.render(512);
  const AudGraphTransportState playing = f.transport();
  AUD_CHECK(aud_graph_stop(f.graph) == AUD_OK);
  const AudGraphTransportState stopped = f.transport();
  AUD_CHECK(stopped.playing == 0 && std::llabs(stopped.beat - playing.beat) <= 2);
  AUD_CHECK(aud_graph_prepare(f.graph, kRate, 256) == AUD_OK);
  AUD_CHECK(aud_graph_start(f.graph) == AUD_OK);
  g_records.clear();
  f.render(256);
  AUD_CHECK(f.transport().beat == stopped.beat && f.transport().playing == 0);
  AUD_CHECK(g_records.size() == 1 && aud_ump_is_note_off(g_records[0].word0, 0) &&
            aud_ump_note(g_records[0].word0) == 70);
  AUD_CHECK(f.request(AUD_TRANSPORT_REQUEST_START) == AUD_OK);
  f.render(256);
  AUD_CHECK(std::llabs(f.transport().beat - (stopped.beat + ticksOf(256, kRate, 120))) <= 2);
  // Blocks beyond the new largest one are refused.
  AUD_CHECK(f.render(512) == AUD_ERROR_INVALID_ARGUMENT);
  // No note stayed open: removing the recorder closes nothing more.
  g_records.clear();
  f.commit([&] { aud_graph_remove_node(f.graph, recorder); });
  f.render(256);
  AUD_CHECK(g_records.empty());
}

// ############################################################################
// The scheduler full (interop-002)

AUD_TEST(stress_a_full_scheduler_refuses_at_the_enqueue_and_never_drops) {
  Fixture f(0, 1, 256, [](AudGraphConfig& c) {
    c.scheduler_capacity = 32;
    c.max_events_per_block = 8;
  });
  const int32_t recorder = f.node("aud.test.recorder");
  f.commit([&] { f.connect(recorder, AUD_GRAPH_NODE); });
  aud_graph_start(f.graph);
  f.render(256);
  // A burst within one block: 32 places, the rest is refused at once.
  const int64_t base = aud_graph_sample_position(f.graph) + 4096;
  uint32_t accepted = 0;
  for (uint32_t i = 0; i < 40; ++i) {
    // Ten frames apart, in falling time: the scheduler sorts them.
    const AudTimestamp at = atSample(base + 1000 - 10 * static_cast<int64_t>(i));
    const AudEvent e = control(i);
    const int32_t result = aud_graph_send_event(f.graph, recorder, &e, &at, i + 1);
    if (result == AUD_OK) {
      accepted += 1;
    } else {
      AUD_CHECK(result == AUD_ERROR_CAPACITY);
    }
  }
  AUD_CHECK(accepted == 32 && f.stats().rejected == 8);
  // Events without a time need no place. The queue hands the realtime
  // thread 8 commands per block: the 33 take five blocks.
  const AudEvent now = control(999);
  AUD_CHECK(aud_graph_send_event(f.graph, recorder, &now, nullptr, 0) == AUD_OK);
  for (int i = 0; i < 5; ++i) f.render(256);
  AUD_CHECK(f.stats().scheduled == 32 && f.stats().events_dropped == 0);
  AUD_CHECK(g_records.size() == 1 && g_records[0].word0 == 999);
  // A cancellation gives its places back at the next block.
  for (uint32_t id = 1; id <= 4; ++id) {
    AUD_CHECK(aud_graph_cancel(f.graph, recorder, id) == AUD_OK);
  }
  const AudTimestamp later = atSample(base + 2000);
  const AudEvent extra = control(500);
  AUD_CHECK(aud_graph_send_event(f.graph, recorder, &extra, &later, 0) == AUD_ERROR_CAPACITY);
  f.render(256);
  AUD_CHECK(f.stats().scheduled == 28);
  for (uint32_t k = 0; k < 4; ++k) {
    const AudEvent e = control(500 + k);
    AUD_CHECK(aud_graph_send_event(f.graph, recorder, &e, &later, 0) == AUD_OK);
  }
  // Everything plays, in time order: the 28 events lie 270 frames apart at
  // most, more than the budget of 8 per block takes, so some follow a block
  // late - at the block start, with a diagnostic, but none is lost.
  std::vector<AudGraphNotification> notifications;
  while (aud_graph_sample_position(f.graph) < base + 2600) {
    f.render(256);
    const std::vector<AudGraphNotification> batch = f.takeAll();
    notifications.insert(notifications.end(), batch.begin(), batch.end());
  }
  const AudGraphStats s = f.stats();
  AUD_CHECK(s.events_dropped == 0 && s.scheduled == 0);
  AUD_CHECK(g_records.size() == 1 + 28 + 4);
  AUD_CHECK(s.events_late > 0 && diagnostics(notifications, AUD_ERROR_LATE) == s.events_late);
  int64_t previous = 0;
  for (size_t k = 1; k < g_records.size(); ++k) {
    const Record& r = g_records[k];
    const int64_t at = r.position + r.offset;
    AUD_CHECK(at >= previous);
    previous = at;
    if (k <= 28) AUD_CHECK(r.word0 >= 4 && r.word0 < 32);  // 0..3 were cancelled
  }
  // All places are free again.
  for (uint32_t i = 0; i < 32; ++i) {
    const AudTimestamp at = atSample(aud_graph_sample_position(f.graph) + 5000 + i);
    const AudEvent e = control(i);
    AUD_CHECK(aud_graph_send_event(f.graph, recorder, &e, &at, 0) == AUD_OK);
  }
  // A removed node takes its places along, those still queued as well
  // (8 commands per block).
  f.commit([&] { aud_graph_remove_node(f.graph, recorder); });
  for (int i = 0; i < 5; ++i) f.render(256);
  AUD_CHECK(f.stats().scheduled == 0);
  AUD_CHECK(diagnostics(f.takeAll(), AUD_ERROR_RETIRED) == 32);
}

// ############################################################################
// A stream that renders on its own thread (graph-003, interop-002)

AUD_TEST(stress_soaks_a_running_stream_with_every_control_call) {
  Fixture f(0, 1, 256, [](AudGraphConfig& c) {
    c.event_queue_capacity = 256;
    c.param_queue_capacity = 256;
  });
  const uint32_t one = 1;
  const uint32_t ones[AUD_MIXER_NUM_INPUTS] = {1, 1, 1, 1, 1, 1, 1, 1};
  AudNodeConfig mixerConfig{sizeof(AudNodeConfig), AUD_MIXER_NUM_INPUTS, ones, 1,
                            &one, 0, 0};
  AudNodeConfig mono{sizeof(AudNodeConfig), 1, &one, 1, &one, 0, 0};
  const int32_t mixer = f.node(AUD_GRAPH_MIXER_TYPE_ID, &mixerConfig);
  const int32_t filter = f.node(AUD_GRAPH_FILTER_TYPE_ID, &mono);
  const int32_t tap = f.node(AUD_GRAPH_TAP_TYPE_ID, &mono);
  const int32_t preset = f.node("aud.test.preset");
  const int32_t emitter = f.node("aud.test.emitter");
  int32_t oscillators[4];
  for (int32_t& osc : oscillators) osc = steadyOscillator(f, 0.1f);
  int32_t revision = f.commit([&] {
    for (uint32_t k = 0; k < 4; ++k) f.connect(oscillators[k], mixer, 0, k);
    f.connect(preset, mixer, 0, 4);
    f.connect(mixer, filter);
    f.connect(filter, tap);
    f.connect(tap, AUD_GRAPH_NODE);
    aud_graph_connect_events(f.graph, AUD_GRAPH_NODE, 0, emitter, 0);
    aud_graph_connect_events(f.graph, emitter, 0, oscillators[0], 0);
    aud_graph_connect_events(f.graph, emitter, 0, AUD_GRAPH_NODE, 0);
  });
  aud_graph_start(f.graph);
  std::atomic<bool> stop{false};
  std::atomic<uint32_t> blocks{0};
  std::thread stream([&] {
    const uint32_t sizes[] = {256, 64, 128, 17, 200};
    uint32_t i = 0;
    while (!stop.load(std::memory_order_acquire)) {
      f.render(sizes[i++ % 5]);
      blocks.fetch_add(1, std::memory_order_release);
    }
  });
  Random random(23);
  bool connected[4] = {true, true, true, true};
  uint32_t replaced = 0;
  uint32_t done = 0;
  float samples[512];
  unsigned char blob[16];
  for (uint32_t step = 0; step < 4000; ++step) {
    const uint32_t k = random.below(4);
    switch (random.below(9)) {
      case 0: {  // a transaction
        const int32_t r = f.commit([&] {
          if (random.below(3) == 0) {
            aud_graph_remove_node(f.graph, oscillators[k]);
            oscillators[k] = steadyOscillator(f, 0.1f);
            f.connect(oscillators[k], mixer, 0, k);
            connected[k] = true;
            replaced += 1;
          } else if (connected[k]) {
            aud_graph_disconnect(f.graph, oscillators[k], 0, mixer, k);
            connected[k] = false;
          } else {
            f.connect(oscillators[k], mixer, 0, k);
            connected[k] = true;
          }
        });
        AUD_CHECK(r > revision);
        revision = r;
        break;
      }
      case 1:
        aud_graph_set_param(f.graph, filter, AUD_FILTER_PARAM_CUTOFF,
                            200.0f + static_cast<float>(random.below(5000)),
                            random.below(2) * 64);
        break;
      case 2: {
        const AudEvent e = random.below(2) == 0 ? noteOn(40 + random.below(30))
                                                : noteOff(40 + random.below(30));
        aud_graph_send_event(f.graph, oscillators[k], &e, nullptr, 0);
        break;
      }
      case 3: {
        const AudTimestamp at =
            atSample(aud_graph_sample_position(f.graph) + random.below(20000));
        const AudEvent e = noteOn(50 + random.below(10));
        aud_graph_send_event(f.graph, AUD_GRAPH_NODE, &e, &at, 1 + random.below(8));
        break;
      }
      case 4:
        aud_graph_cancel(f.graph, AUD_GRAPH_NODE, 1 + random.below(8));
        break;
      case 5: {
        static const uint32_t requests[] = {
            AUD_TRANSPORT_REQUEST_START, AUD_TRANSPORT_REQUEST_STOP,
            AUD_TRANSPORT_REQUEST_SEEK, AUD_TRANSPORT_REQUEST_SET_TEMPO};
        f.request(requests[random.below(4)], 60 + random.below(120),
                  static_cast<int64_t>(random.below(16)) * AUD_BEAT_FACTOR);
        break;
      }
      case 6: {
        size_t size = 0;
        AUD_CHECK(aud_graph_node_save_state(f.graph, preset, blob, sizeof(blob), &size) == AUD_OK);
        AUD_CHECK(aud_graph_node_load_state(f.graph, preset, blob, size, kPresetStateVersion) == AUD_OK);
        break;
      }
      case 7: {
        float peak = 0;
        float rms = 0;
        AUD_CHECK(aud_graph_tap_meter(f.graph, tap, 0, &peak, &rms) == AUD_OK);
        const int32_t read = aud_graph_tap_read(f.graph, tap, 0, samples, 512);
        AUD_CHECK(read == AUD_OK || read == AUD_ERROR_FAILED);
        AudGraphStats s{};
        s.struct_size = sizeof(AudGraphStats);
        aud_graph_get_stats(f.graph, &s);
        AudGraphTransportState t{};
        t.struct_size = sizeof(AudGraphTransportState);
        aud_graph_transport_state(f.graph, &t);
        break;
      }
      default:
        for (const AudGraphNotification& n : f.take()) {
          if (n.type == AUD_NOTIFY_NODE_DONE) done += 1;
        }
    }
  }
  // Let the stream catch up, then stop it.
  const uint32_t target = blocks.load(std::memory_order_acquire) + 20;
  while (blocks.load(std::memory_order_acquire) < target) std::this_thread::yield();
  stop.store(true, std::memory_order_release);
  stream.join();
  for (int i = 0; i < 8; ++i) {
    f.render(256);
    for (const AudGraphNotification& n : f.takeAll()) {
      if (n.type == AUD_NOTIFY_NODE_DONE) done += 1;
    }
  }
  // The last transaction is running, every replaced node is gone, and the
  // engine counted what it did.
  AUD_CHECK(aud_graph_revision(f.graph) >= static_cast<uint32_t>(revision));
  AUD_CHECK(done == replaced);
  AUD_CHECK(aud_graph_nodes(f.graph, nullptr, 0) == 9);
  const AudGraphStats s = f.stats();
  AUD_CHECK(s.blocks_rendered > 100 && s.realtime_violations == 0);
  AUD_CHECK(g_lastPreset->overlaps.load() == 0);
}

// ############################################################################
// Event sources (ticket 26)

namespace {

// The notes the recorder saw on and not off again, in the order of the
// records: a stuck note is a note on without a later note off.
std::set<uint32_t> soundingNotes() {
  std::set<uint32_t> sounding;
  for (const Record& r : g_records) {
    if (r.type != AUD_EVENT_UMP) continue;
    const uint32_t key = (aud_ump_channel(r.word0) << 8) | aud_ump_note(r.word0);
    if (aud_ump_is_note_on(r.word0, 0)) sounding.insert(key);
    if (aud_ump_is_note_off(r.word0, 0)) sounding.erase(key);
  }
  return sounding;
}

uint32_t countRecords(bool (*predicate)(uint32_t word0)) {
  uint32_t count = 0;
  for (const Record& r : g_records) {
    if (r.type == AUD_EVENT_UMP && predicate(r.word0)) count += 1;
  }
  return count;
}

bool isSysEx(uint32_t word0) {
  return aud_ump_message_type(word0) == AUD_UMP_TYPE_DATA64;
}
bool isNote(uint32_t word0) {
  return aud_ump_is_note_on(word0, 0) || aud_ump_is_note_off(word0, 0);
}

// A stream on its own thread that renders a block as soon as its time has
// come on the synthesized timeline, so that the host times of the blocks
// keep step with the clock the producers stamp with.
struct PacedStream {
  Fixture& f;
  uint32_t frames;
  std::atomic<bool> stop{false};
  std::atomic<uint32_t> blocks{0};
  std::thread thread;

  PacedStream(Fixture& fixture, uint32_t blockFrames)
      : f(fixture), frames(blockFrames), thread([this] { run(); }) {}

  void run() {
    const auto origin = std::chrono::steady_clock::now();
    const auto block = std::chrono::nanoseconds(
        static_cast<int64_t>(std::llround(frames * 1e9 / kRate)));
    uint32_t n = 0;
    while (!stop.load(std::memory_order_acquire)) {
      f.render(frames);
      blocks.fetch_add(1, std::memory_order_release);
      n += 1;
      std::this_thread::sleep_until(origin + block * n);
    }
  }

  // Renders `count` more blocks and stops.
  void finish(uint32_t count) {
    const uint32_t target = blocks.load(std::memory_order_acquire) + count;
    while (blocks.load(std::memory_order_acquire) < target) {
      std::this_thread::yield();
    }
    stop.store(true, std::memory_order_release);
    thread.join();
  }
};

}  // namespace

AUD_TEST(stress_source_takes_10000_events_per_second_from_a_producer_thread) {
  Fixture f;
  const int32_t recorder = f.node("aud.test.recorder");
  f.commit([&] { f.connect(recorder, AUD_GRAPH_NODE); });
  aud_graph_start(f.graph);
  AudEventSource* source =
      makeSource(f, recorder, AUD_SOURCE_TIMING_FIXED, 20000000);
  PacedStream stream(f, 256);
  // Ten packets every millisecond for ten seconds: note ons and note
  // offs in pairs, stamped now.
  std::atomic<uint32_t> pushed{0};
  std::atomic<uint32_t> refused{0};
  std::thread producer([&] {
    AUD_CHECK(aud_event_source_attach(source, 1) == AUD_OK);
    const auto origin = std::chrono::steady_clock::now();
    uint32_t note = 0;
    for (uint32_t ms = 1; ms <= 10000; ++ms) {
      for (uint32_t k = 0; k < 5; ++k) {
        const uint32_t pitch = 1 + note % 120;
        note += 1;
        const uint32_t on = aud_ump_midi1_word(0, 0x90, pitch, 100);
        const uint32_t off = aud_ump_midi1_word(0, 0x80, pitch, 64);
        if (aud_event_source_push(source, &on, 1, 0) == AUD_OK) {
          pushed.fetch_add(1, std::memory_order_relaxed);
          while (aud_event_source_push(source, &off, 1, 0) != AUD_OK) {
            std::this_thread::yield();
          }
          pushed.fetch_add(1, std::memory_order_relaxed);
        } else {
          refused.fetch_add(1, std::memory_order_relaxed);
        }
      }
      std::this_thread::sleep_until(origin + std::chrono::milliseconds(ms));
    }
    AUD_CHECK(aud_event_source_detach(source, 1) == AUD_OK);
  });
  producer.join();
  stream.finish(8);
  const AudEventSourceStats s = sourceStats(source);
  AUD_CHECK(s.pushed == pushed.load());
  AUD_CHECK(s.delivered == s.pushed);
  AUD_CHECK(s.pushed >= 99000);  // a stalled machine refuses a few
  AUD_CHECK(s.dropped == 0 && s.reordered == 0);
  AUD_CHECK(g_records.size() == s.delivered);
  AUD_CHECK(soundingNotes().empty());
  AUD_CHECK(f.stats().realtime_violations == 0);
  AUD_CHECK(f.stats().events_dropped == 0);
  std::printf("  10 s at 10,000 events/s: %llu delivered, %llu late, max %.2f ms late, "
              "max delay %.3f ms, %u refused\n",
              static_cast<unsigned long long>(s.delivered),
              static_cast<unsigned long long>(s.late),
              static_cast<double>(s.max_late_ns) / 1e6,
              static_cast<double>(s.max_delay_ns) / 1e6, refused.load());
}

AUD_TEST(stress_source_plays_64_note_chords_every_block) {
  Fixture f;
  const int32_t recorder = f.node("aud.test.recorder");
  f.commit([&] { f.connect(recorder, AUD_GRAPH_NODE); });
  aud_graph_start(f.graph);
  AudEventSource* source = makeSource(f, recorder, AUD_SOURCE_TIMING_IMMEDIATE);
  for (uint32_t block = 0; block < 200; ++block) {
    const bool on = block % 2 == 0;
    for (uint32_t n = 0; n < 64; ++n) {
      const uint32_t word = on ? aud_ump_midi1_word(0, 0x90, 30 + n, 100)
                               : aud_ump_midi1_word(0, 0x80, 30 + n, 64);
      AUD_CHECK(pushWord(source, word) == AUD_OK);
    }
    f.render(256);
    AUD_CHECK(g_records.size() == 64 * (block + 1));
    AUD_CHECK(soundingNotes().size() == (on ? 64 : 0));
  }
  AUD_CHECK(sourceStats(source).delivered == 12800);
  AUD_CHECK(f.stats().events_dropped == 0);
  AUD_CHECK(f.stats().realtime_violations == 0);
}

AUD_TEST(stress_source_survives_sysex_floods_between_the_notes) {
  Fixture f;
  const int32_t recorder = f.node("aud.test.recorder");
  f.commit([&] { f.connect(recorder, AUD_GRAPH_NODE); });
  aud_graph_start(f.graph);
  AudEventSource* source = makeSource(f, recorder, AUD_SOURCE_TIMING_IMMEDIATE);
  Random random(7);
  uint32_t sysex = 0;
  uint32_t notes = 0;
  for (uint32_t burst = 0; burst < 40; ++burst) {
    // A 64 KiB dump is 10923 packets of 6 bytes: a burst of 1000 at a
    // time, with a note on and off around each burst.
    const uint32_t pitch = 40 + random.below(40);
    const uint32_t on = aud_ump_midi1_word(0, 0x90, pitch, 100);
    AUD_CHECK(pushWord(source, on) == AUD_OK);
    notes += 1;
    for (uint32_t i = 0; i < 1000; ++i) {
      uint32_t words[2] = {0x30260000u | (random.next() & 0x7f7f),
                           random.next() & 0x7f7f7f7fu};
      if (aud_event_source_push(source, words, 2, 0) == AUD_OK) sysex += 1;
    }
    const uint32_t off = aud_ump_midi1_word(0, 0x80, pitch, 64);
    AUD_CHECK(pushWord(source, off) == AUD_OK);
    notes += 1;
    f.render(256);
    f.render(256);
  }
  for (int i = 0; i < 50; ++i) f.render(256);
  AUD_CHECK(sysex == 40000);
  AUD_CHECK(countRecords(isSysEx) == sysex);
  AUD_CHECK(countRecords(isNote) == notes);
  AUD_CHECK(soundingNotes().empty());
  AUD_CHECK(sourceStats(source).delivered == sysex + notes);
  AUD_CHECK(f.stats().events_dropped == 0);
  AUD_CHECK(f.stats().realtime_violations == 0);
}

AUD_TEST(stress_source_release_during_a_burst_leaves_no_note_on) {
  Fixture f;
  const int32_t recorder = f.node("aud.test.recorder");
  f.commit([&] { f.connect(recorder, AUD_GRAPH_NODE); });
  aud_graph_start(f.graph);
  AudEventSource* source = makeSource(f, recorder, AUD_SOURCE_TIMING_IMMEDIATE);
  std::atomic<bool> stop{false};
  std::atomic<uint32_t> blocks{0};
  // The stream renders every millisecond, the producer pushes pairs of
  // note on and note off as fast as the queue takes them, a third thread
  // raises release every three milliseconds.
  std::thread stream([&] {
    while (!stop.load(std::memory_order_acquire)) {
      f.render(256);
      blocks.fetch_add(1, std::memory_order_release);
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
  });
  std::thread producer([&] {
    Random random(3);
    AUD_CHECK(aud_event_source_attach(source, 9) == AUD_OK);
    // At most 30,000 triples, so that the recorder's reserved vector holds
    // what arrives without growing on the audio thread.
    const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(300);
    for (uint32_t i = 0; i < 30000 && std::chrono::steady_clock::now() < until; ++i) {
      const uint32_t pitch = 1 + random.below(120);
      pushWord(source, aud_ump_midi1_word(0, 0x90, pitch, 100));
      pushWord(source, ccWord(64, 127, random.below(16)));
      pushWord(source, aud_ump_midi1_word(0, 0x80, pitch, 64));
    }
    AUD_CHECK(aud_event_source_detach(source, 9) == AUD_OK);
  });
  std::thread releaser([&] {
    const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(300);
    while (std::chrono::steady_clock::now() < until) {
      aud_event_source_release(source);
      std::this_thread::sleep_for(std::chrono::milliseconds(3));
    }
  });
  producer.join();
  releaser.join();
  aud_event_source_release(source);
  const uint32_t target = blocks.load(std::memory_order_acquire) + 4;
  while (blocks.load(std::memory_order_acquire) < target) std::this_thread::yield();
  stop.store(true, std::memory_order_release);
  stream.join();
  const AudEventSourceStats s = sourceStats(source);
  AUD_CHECK(s.releases >= 2);
  AUD_CHECK(s.pushed > 1000);
  AUD_CHECK(soundingNotes().empty());
  AUD_CHECK(aud_event_source_size(source) == 0);
  AUD_CHECK(f.stats().events_dropped == 0);
  AUD_CHECK(f.stats().realtime_violations == 0);
  std::printf("  release during a burst: %llu pushed, %llu dropped, %llu releases\n",
              static_cast<unsigned long long>(s.pushed),
              static_cast<unsigned long long>(s.dropped),
              static_cast<unsigned long long>(s.releases));
}

AUD_TEST(stress_removing_a_pass_through_node_closes_the_chord_and_the_pedal) {
  Fixture f;
  const int32_t emitter = f.node("aud.test.emitter");
  const int32_t recorder = f.node("aud.test.recorder");
  f.commit([&] {
    f.connect(recorder, AUD_GRAPH_NODE);
    aud_graph_connect_events(f.graph, emitter, 0, recorder, 0);
  });
  aud_graph_start(f.graph);
  AudEventSource* source = makeSource(f, emitter, AUD_SOURCE_TIMING_IMMEDIATE);
  for (uint32_t n = 0; n < 8; ++n) {
    pushWord(source, aud_ump_midi1_word(0, 0x90, 60 + n, 100));
  }
  pushWord(source, ccWord(64, 127));
  pushWord(source, ccWord(66, 127, 3));
  f.render(256);
  AUD_CHECK(g_records.size() == 10);
  AUD_CHECK(soundingNotes().size() == 8);
  // The pass-through node goes while the chord and the pedals are held:
  // its routes stay until it is done, so the recorder sees every note
  // closed and both pedals off.
  f.commit([&] { aud_graph_remove_node(f.graph, emitter); });
  g_records.clear();
  f.render(256);
  AUD_CHECK(g_records.size() == 10);
  AUD_CHECK(soundingNotes().empty());
  uint32_t pedalsOff = 0;
  for (const Record& r : g_records) {
    if (aud_ump_status(r.word0) == AUD_UMP_STATUS_CONTROL_CHANGE &&
        aud_ump_midi1_data2(r.word0) == 0) {
      pedalsOff += 1;
    }
  }
  AUD_CHECK(pedalsOff == 2);
  for (int i = 0; i < 3; ++i) f.render(256);
  AUD_CHECK(f.countNotifications(AUD_NOTIFY_NODE_DONE) == 1);
  AUD_CHECK(aud_graph_destroy_event_source(f.graph, source) == AUD_OK);
  f.render(256);
  f.takeAll();
  f.render(256);
  f.takeAll();
  AUD_CHECK(aud_graph_node_descriptor(f.graph, emitter) == nullptr);
  AUD_CHECK(f.stats().realtime_violations == 0);
}

AUD_TEST(stress_time_filter_reset_during_held_notes_keeps_the_timing) {
  Fixture f;
  const int32_t recorder = f.node("aud.test.recorder");
  f.commit([&] { f.connect(recorder, AUD_GRAPH_NODE); });
  aud_graph_start(f.graph);
  TimedStream stream(f);
  // C = 10 ms = 480 frames.
  AudEventSource* source = makeSource(f, recorder, AUD_SOURCE_TIMING_FIXED, 10000000);
  for (uint32_t n = 0; n < 4; ++n) {
    pushWord(source, aud_ump_midi1_word(0, 0x90, 60 + n, 100), stream.atFrame(99.5));
  }
  for (int block = 0; block < 4; ++block) stream.render(256);
  AUD_CHECK(g_records.size() == 4);
  AUD_CHECK(g_records[0].position == 512 && g_records[0].offset == 68);
  // A route change: the stream continues at another position, the
  // filter resets. The note offs come stamped on the new timeline and
  // land at their frame; nothing hangs.
  stream.position = 100000;
  stream.render(256);
  AUD_CHECK(f.stats().time_filter_resets == 1);
  for (uint32_t n = 0; n < 4; ++n) {
    pushWord(source, aud_ump_midi1_word(0, 0x80, 60 + n, 64), stream.atFrame(100355.5));
  }
  for (int block = 0; block < 4; ++block) stream.render(256);
  AUD_CHECK(g_records.size() == 8);
  AUD_CHECK(g_records[4].position == 100768 && g_records[4].offset == 68);
  AUD_CHECK(soundingNotes().empty());
  const AudEventSourceStats s = sourceStats(source);
  AUD_CHECK(s.late == 0 && s.delivered == 8);
  AUD_CHECK(timingOf(f.graph).time_filter_resets == 1);
  AUD_CHECK(f.stats().realtime_violations == 0);
}

AUD_TEST(stress_variable_blocks_offline_keep_fixed_timing_to_the_sample) {
  Fixture f(0, 1, 1024);
  const int32_t recorder = f.node("aud.test.recorder");
  f.commit([&] { f.connect(recorder, AUD_GRAPH_NODE); });
  aud_graph_start(f.graph);
  // C = 20 ms = 960 frames on the virtual timeline from 5 s.
  const int64_t origin = 5000000000LL;
  AudEventSource* source = makeSource(f, recorder, AUD_SOURCE_TIMING_FIXED, 20000000);
  for (uint32_t k = 0; k < 50; ++k) {
    const double frame = k * 333.0 - 0.5;
    const int64_t at = origin + static_cast<int64_t>(std::llround(frame * 1e9 / kRate));
    pushWord(source, aud_ump_midi1_word(0, k % 2 == 0 ? 0x90 : 0x80, 60, 100), at);
  }
  const uint32_t sizes[] = {64, 128, 960, 256, 17, 512, 300, 1024, 100};
  const uint32_t frames = 50 * 333 + 960 + 2048;
  std::vector<float> out(frames, 0.0f);
  float* channel = out.data();
  AudAudioBus bus{sizeof(AudAudioBus), 1, &channel};
  AudOfflineRequest request{};
  request.struct_size = sizeof(AudOfflineRequest);
  request.frames = frames;
  request.num_block_sizes = 9;
  request.block_sizes = sizes;
  request.outputs = &bus;
  request.start_host_time_ns = origin;
  AUD_CHECK(aud_graph_render_offline(f.graph, &request) == AUD_OK);
  AUD_CHECK(g_records.size() == 50);
  for (uint32_t k = 0; k < g_records.size(); ++k) {
    AUD_CHECK(g_records[k].position + g_records[k].offset == k * 333 + 960);
  }
  AUD_CHECK(soundingNotes().empty());
  AUD_CHECK(sourceStats(source).late == 0);
  const AudGraphTiming t = timingOf(f.graph);
  AUD_CHECK(t.flags == (AUD_TIMING_VALID | AUD_TIMING_OFFLINE));
  AUD_CHECK(t.max_period_ns == 21333333);
  AUD_CHECK(f.stats().realtime_violations == 0);
}
