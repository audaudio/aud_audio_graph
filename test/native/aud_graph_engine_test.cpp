// @license
// Copyright (c) Audanika. All Rights Reserved.
//
// Use of this source code is governed by terms that can be
// found in the LICENSE file in the root of this package.

// The native tests of what ticket 20 added to the engine: the host's event
// output and freewheel flag, the node park for state calls, the tail of
// the program, the policy of a full note tracker and the debug watchdog.

#include <atomic>
#include <cstdio>
#include <cstring>
#include <thread>
#include <vector>

#include "aud_audio_graph.h"
#include "aud_graph_fixture.hpp"
#include "aud_test.hpp"
#include "aud_ump.h"

using namespace aud_test_fixture;

// ############################################################################

AUD_TEST(hands_graph_events_to_the_host_in_order) {
  Fixture f;
  const int32_t emitter = f.node("aud.test.emitter");
  f.commit([&] {
    AUD_CHECK(aud_graph_connect_events(f.graph, AUD_GRAPH_NODE, 0, emitter, 0) == AUD_OK);
    AUD_CHECK(aud_graph_connect_events(f.graph, emitter, 0, AUD_GRAPH_NODE, 0) == AUD_OK);
  });
  aud_graph_start(f.graph);
  AudEvent in[2] = {noteOn(60), noteOn(62)};
  in[0].sample_offset = 10;
  in[1].sample_offset = 5;
  AudEvent out[4];
  AudHostRenderRequest host;
  AUD_CHECK(f.renderHost(256, out, 4, &host, in, 2) == AUD_OK);
  AUD_CHECK(host.num_output_events == 2 && host.dropped_output_events == 0);
  AUD_CHECK(out[0].sample_offset == 5 && aud_ump_note(out[0].words[0]) == 62);
  AUD_CHECK(out[1].sample_offset == 10 && aud_ump_note(out[1].words[0]) == 60);
  AUD_CHECK(out[0].port == 0 && out[0].struct_size == sizeof(AudEvent));
  // Nothing reached the control thread as a notification.
  AUD_CHECK(f.countNotifications(AUD_NOTIFY_EVENT) == 0);
  // A buffer too small drops the rest and says so.
  AUD_CHECK(f.renderHost(256, out, 1, &host, in, 2) == AUD_OK);
  AUD_CHECK(host.num_output_events == 1 && host.dropped_output_events == 1);
  AUD_CHECK(f.countNotifications(AUD_NOTIFY_DIAGNOSTIC, AUD_ERROR_QUEUE_FULL) == 1);
  AUD_CHECK(f.stats().events_dropped == 1);
  // Without an event output the events reach the control thread again.
  AUD_CHECK(f.renderHost(256, nullptr, 0, &host, in, 2) == AUD_OK);
  AUD_CHECK(host.num_output_events == 0);
  AUD_CHECK(f.countNotifications(AUD_NOTIFY_EVENT) == 2);
  // Events at one offset keep their order.
  AudEvent same[3] = {noteOn(1), noteOn(2), noteOn(3)};
  AUD_CHECK(f.renderHost(256, out, 4, &host, same, 3) == AUD_OK);
  AUD_CHECK(host.num_output_events == 3);
  AUD_CHECK(aud_ump_note(out[0].words[0]) == 1 && aud_ump_note(out[1].words[0]) == 2 &&
            aud_ump_note(out[2].words[0]) == 3);
  // Invalid requests.
  AUD_CHECK(aud_graph_render_host(nullptr, &host) == AUD_ERROR_INVALID_ARGUMENT);
  AUD_CHECK(aud_graph_render_host(f.graph, nullptr) == AUD_ERROR_INVALID_ARGUMENT);
  AudHostRenderRequest bad = host;
  bad.request = nullptr;
  AUD_CHECK(aud_graph_render_host(f.graph, &bad) == AUD_ERROR_INVALID_ARGUMENT);
  bad = host;
  bad.output_events = nullptr;
  AUD_CHECK(aud_graph_render_host(f.graph, &bad) == AUD_ERROR_INVALID_ARGUMENT);
  bad = host;
  bad.struct_size = 4;
  AUD_CHECK(aud_graph_render_host(f.graph, &bad) == AUD_ERROR_INVALID_ARGUMENT);
  // Not running: silence and no events.
  aud_graph_stop(f.graph);
  AUD_CHECK(f.renderHost(256, out, 4, &host, in, 2) == AUD_OK);
  AUD_CHECK(host.num_output_events == 0 && f.out[0][0] == 0);
}

AUD_TEST(renders_freewheeling_blocks_for_the_host) {
  Fixture f;
  const int32_t preset = f.node("aud.test.preset");
  Preset* p = g_lastPreset;
  f.commit([&] { f.connect(preset, AUD_GRAPH_NODE); });
  aud_graph_start(f.graph);
  AudHostRenderRequest host;
  AUD_CHECK(f.renderHost(256, nullptr, 0, &host, nullptr, 0, AUD_PROCESS_OFFLINE) == AUD_OK);
  AUD_CHECK(p->flags == AUD_PROCESS_OFFLINE);
  AUD_CHECK(f.renderHost(256, nullptr, 0, &host) == AUD_OK);
  AUD_CHECK(p->flags == 0);
  f.render(256);
  AUD_CHECK(p->flags == 0);
}

AUD_TEST(parks_a_node_for_its_state_calls) {
  Fixture f;
  const int32_t preset = f.node("aud.test.preset");
  Preset* p = g_lastPreset;
  const int32_t osc = f.node(AUD_GRAPH_OSCILLATOR_TYPE_ID);
  unsigned char blob[16];
  size_t size = 0;
  // Refusals.
  AUD_CHECK(aud_graph_node_save_state(f.graph, osc, blob, 16, &size) == AUD_ERROR_UNSUPPORTED);
  AUD_CHECK(aud_graph_node_load_state(f.graph, osc, blob, 4, 3) == AUD_ERROR_UNSUPPORTED);
  AUD_CHECK(aud_graph_node_save_state(f.graph, 99, blob, 16, &size) == AUD_ERROR_NOT_FOUND);
  AUD_CHECK(aud_graph_node_load_state(f.graph, 99, blob, 4, 3) == AUD_ERROR_NOT_FOUND);
  AUD_CHECK(aud_graph_node_save_state(f.graph, preset, nullptr, 16, &size) == AUD_ERROR_INVALID_ARGUMENT);
  AUD_CHECK(aud_graph_node_save_state(f.graph, preset, blob, 16, nullptr) == AUD_ERROR_INVALID_ARGUMENT);
  AUD_CHECK(aud_graph_node_save_state(nullptr, preset, blob, 16, &size) == AUD_ERROR_INVALID_ARGUMENT);
  AUD_CHECK(aud_graph_node_load_state(f.graph, preset, nullptr, 4, 3) == AUD_ERROR_INVALID_ARGUMENT);
  AUD_CHECK(aud_graph_node_load_state(nullptr, preset, blob, 4, 3) == AUD_ERROR_INVALID_ARGUMENT);
  // The size first.
  AUD_CHECK(aud_graph_node_save_state(f.graph, preset, nullptr, 0, &size) == AUD_ERROR_BUFFER_TOO_SMALL);
  AUD_CHECK(size == 4);
  // Not running: straight through.
  float offset = 0.25f;
  std::memcpy(blob, &offset, sizeof(float));
  AUD_CHECK(aud_graph_node_load_state(f.graph, preset, blob, 4, 99) == AUD_ERROR_STATE_VERSION);
  AUD_CHECK(aud_graph_node_load_state(f.graph, preset, blob, 4, kPresetStateVersion) == AUD_OK);
  AUD_CHECK(p->offset == 0.25f && p->loads == 1);
  std::memset(blob, 0, sizeof(blob));
  AUD_CHECK(aud_graph_node_save_state(f.graph, preset, blob, 16, &size) == AUD_OK && size == 4);
  std::memcpy(&offset, blob, sizeof(float));
  AUD_CHECK(offset == 0.25f);
  // Running: the node is parked for the call and renders again afterwards.
  f.commit([&] { f.connect(preset, AUD_GRAPH_NODE); });
  aud_graph_start(f.graph);
  f.render(256);
  AUD_CHECK_NEAR(f.out[0][0], 0.75, 1e-6);  // a 0.5 + offset 0.25
  offset = 0.125f;
  std::memcpy(blob, &offset, sizeof(float));
  AUD_CHECK(aud_graph_node_load_state(f.graph, preset, blob, 4, kPresetStateVersion) == AUD_OK);
  f.render(256);
  AUD_CHECK_NEAR(f.out[0][0], 0.625, 1e-6);
  AUD_CHECK(aud_graph_node_save_state(f.graph, preset, blob, 16, &size) == AUD_OK);
  AUD_CHECK(p->overlaps.load() == 0);
  // A stream keeps rendering while the control thread saves and loads:
  // no state call overlaps a process call, the parked blocks skip the
  // node, and every event sent meanwhile is either delivered or reported
  // as dropped for the parked node.
  p->stateCallNs = 200000;
  std::atomic<bool> stop{false};
  std::atomic<uint32_t> blocks{0};
  const uint32_t callsBefore = p->calls;
  std::thread stream([&] {
    while (!stop.load()) {
      f.render(64);
      blocks.fetch_add(1);
    }
  });
  // Parameters, transport seeks that reset the node, and note pairs go
  // out while the state calls run: none of them may meet a state call,
  // none may be lost - a note off dropped for a parked node would hang.
  uint32_t sent = 0;
  const auto send = [&](const AudEvent& event) {
    if (aud_graph_send_event(f.graph, preset, &event, nullptr, 0) == AUD_OK) {
      sent += 1;
    }
  };
  std::vector<AudGraphNotification> notifications;
  for (int i = 0; i < 40; ++i) {
    AUD_CHECK(aud_graph_node_save_state(f.graph, preset, blob, 16, &size) == AUD_OK);
    AUD_CHECK(aud_graph_node_load_state(f.graph, preset, blob, 4, kPresetStateVersion) == AUD_OK);
    for (int k = 0; k < 10; ++k) {
      send(noteOn(60 + k));
      AUD_CHECK(aud_graph_set_param(f.graph, preset, 1, static_cast<float>(k), 0) == AUD_OK);
      send(noteOff(60 + k));
    }
    AUD_CHECK(f.request(AUD_TRANSPORT_REQUEST_SEEK, 0, i * AUD_BEAT_FACTOR) == AUD_OK);
    const std::vector<AudGraphNotification> batch = f.takeAll();
    notifications.insert(notifications.end(), batch.begin(), batch.end());
  }
  stop.store(true);
  stream.join();
  AUD_CHECK(blocks.load() > p->calls - callsBefore);
  // The commands sent after the last block of the stream are still queued.
  for (int i = 0; i < 4; ++i) f.render(64);
  AUD_CHECK(p->overlaps.load() == 0);
  AUD_CHECK(p->events >= sent && p->held() == 0);
  AUD_CHECK(p->resets > 0);
  const std::vector<AudGraphNotification> rest = f.takeAll();
  notifications.insert(notifications.end(), rest.begin(), rest.end());
  uint32_t postponed = 0;
  for (const AudGraphNotification& n : notifications) {
    if (n.type == AUD_NOTIFY_DIAGNOSTIC && n.code == AUD_ERROR_STATE) {
      AUD_CHECK(n.node == preset);
      postponed += n.count;
    }
  }
  AUD_CHECK(f.stats().events_dropped == 0);
  AUD_CHECK(f.stats().notifications_dropped == 0);
  AUD_CHECK(postponed > 0);
  p->stateCallNs = 0;
  // A removed node refuses.
  f.commit([&] { aud_graph_remove_node(f.graph, preset); });
  AUD_CHECK(aud_graph_node_save_state(f.graph, preset, blob, 16, &size) == AUD_ERROR_RETIRED);
  AUD_CHECK(aud_graph_node_load_state(f.graph, preset, blob, 4, kPresetStateVersion) == AUD_ERROR_RETIRED);
}

AUD_TEST(reports_the_tail_of_the_program) {
  Fixture f;
  AUD_CHECK(aud_graph_output_tail(f.graph) == 0);
  AUD_CHECK(aud_graph_output_tail(nullptr) == 0);
  const int32_t osc = steadyOscillator(f);
  const int32_t delay = f.node("aud.test.delay");
  f.commit([&] {
    f.connect(osc, delay);
    f.connect(delay, AUD_GRAPH_NODE);
  });
  AUD_CHECK(aud_graph_output_latency(f.graph) == 100);
  AUD_CHECK(aud_graph_output_tail(f.graph) == 100);
  // The tail of the first delay still travels through the second one.
  const int32_t delay2 = f.node("aud.test.delay");
  f.commit([&] {
    aud_graph_disconnect(f.graph, delay, 0, AUD_GRAPH_NODE, 0);
    f.connect(delay, delay2);
    f.connect(delay2, AUD_GRAPH_NODE);
  });
  AUD_CHECK(aud_graph_output_latency(f.graph) == 200);
  AUD_CHECK(aud_graph_output_tail(f.graph) == 200);
  // A feedback node with a delay line of 512 frames; its output is delayed
  // by 200 frames to line up with the delay chain, so its tail reaches the
  // output that much later.
  const uint32_t one = 1;
  AudNodeConfig config{sizeof(AudNodeConfig), 1, &one, 1, &one, 512, 0};
  const int32_t feedback = f.node(AUD_GRAPH_FEEDBACK_TYPE_ID, &config);
  f.commit([&] {
    f.connect(delay2, feedback);
    f.connect(feedback, AUD_GRAPH_NODE);
  });
  AUD_CHECK(aud_graph_output_tail(f.graph) == 712);
  // A node that never ends.
  const int32_t infinite = f.node("aud.test.infinite");
  f.commit([&] { f.connect(infinite, AUD_GRAPH_NODE); });
  AUD_CHECK(aud_graph_output_tail(f.graph) == AUD_TAIL_INFINITE);
  // A node whose connection fades out does not count.
  f.commit([&] { aud_graph_disconnect(f.graph, infinite, 0, AUD_GRAPH_NODE, 0); });
  AUD_CHECK(aud_graph_output_tail(f.graph) == 712);
  // A removed node counts until its tail has rendered.
  aud_graph_start(f.graph);
  f.render(256);
  f.commit([&] { aud_graph_remove_node(f.graph, feedback); });
  AUD_CHECK(aud_graph_output_tail(f.graph) == 712);
  for (int i = 0; i < 4; ++i) f.render(256);
  AUD_CHECK(f.countNotifications(AUD_NOTIFY_NODE_DONE) == 1);
  AUD_CHECK(aud_graph_output_tail(f.graph) == 200);
}

AUD_TEST(drops_a_note_on_the_tracker_cannot_hold_and_closes_the_rest) {
  Fixture f;
  const int32_t recorder = f.node("aud.test.recorder");
  f.commit([&] { f.connect(recorder, AUD_GRAPH_NODE); });
  aud_graph_start(f.graph);
  // 256 distinct notes fill the tracker; the 257th is dropped.
  for (uint32_t i = 0; i < 257; ++i) {
    const AudEvent on = noteOn(i % 128, 100, 0, i / 128);
    AUD_CHECK(aud_graph_send_event(f.graph, recorder, &on, nullptr, 0) == AUD_OK);
  }
  f.render(256);
  AUD_CHECK(g_records.size() == 256);
  AUD_CHECK(f.stats().events_dropped == 1);
  AUD_CHECK(f.stats().events_delivered == 256);
  bool reported = false;
  for (const AudGraphNotification& n : f.takeAll()) {
    if (n.type == AUD_NOTIFY_DIAGNOSTIC && n.code == AUD_ERROR_CAPACITY &&
        n.node == recorder && n.count == 1) {
      reported = true;
    }
  }
  AUD_CHECK(reported);
  // A note off frees a slot for the next note on.
  const AudEvent off = noteOff(5);
  const AudEvent on = noteOn(0, 100, 0, 2);
  AUD_CHECK(aud_graph_send_event(f.graph, recorder, &off, nullptr, 0) == AUD_OK);
  AUD_CHECK(aud_graph_send_event(f.graph, recorder, &on, nullptr, 0) == AUD_OK);
  f.render(256);
  AUD_CHECK(g_records.size() == 258);
  // Removing the node closes every tracked note: 256 note offs.
  g_records.clear();
  f.commit([&] { aud_graph_remove_node(f.graph, recorder); });
  f.render(256);
  uint32_t offs = 0;
  for (const Record& r : g_records) {
    if (aud_ump_is_note_off(r.word0, 0)) offs += 1;
  }
  AUD_CHECK(offs == 256);
}

AUD_TEST(watchdog_counts_allocations_on_the_realtime_thread) {
  AUD_CHECK(aud_graph_watchdog_enabled() == 1);
  Fixture f;
  const int32_t allocator = f.node("aud.test.allocator");
  f.commit([&] { f.connect(allocator, AUD_GRAPH_NODE); });
  aud_graph_start(f.graph);
  aud_graph_watchdog_reset();
  f.render(256);
  f.render(256);
  const AudGraphStats s = f.stats();
  // new, delete, alloc, free and log: at least five hits per block.
  AUD_CHECK(s.realtime_violations >= 10);
  AUD_CHECK(aud_graph_watchdog_violations() == s.realtime_violations);
  uint32_t kinds = 0;
  uint32_t reports = 0;
  uint64_t counted = 0;
  for (const AudGraphNotification& n : f.takeAll()) {
    if (n.type != AUD_NOTIFY_DIAGNOSTIC ||
        n.code != AUD_GRAPH_ERROR_REALTIME_VIOLATION) {
      continue;
    }
    reports += 1;
    counted += n.count;
    kinds |= static_cast<uint32_t>(n.value);
    AUD_CHECK(n.node == 0 && n.count >= 5);
  }
  AUD_CHECK(reports == 2 && counted == s.realtime_violations);
  AUD_CHECK(kinds == (AUD_GRAPH_VIOLATION_NEW | AUD_GRAPH_VIOLATION_DELETE |
                      AUD_GRAPH_VIOLATION_ALLOC | AUD_GRAPH_VIOLATION_FREE |
                      AUD_GRAPH_VIOLATION_LOG));
  aud_graph_reset_stats(f.graph);
  AUD_CHECK(f.stats().realtime_violations == 0);
  aud_graph_watchdog_reset();
  AUD_CHECK(aud_graph_watchdog_violations() == 0);
  // The reference chain stays clean.
  Fixture g;
  const int32_t osc = steadyOscillator(g);
  const int32_t filter = g.node(AUD_GRAPH_FILTER_TYPE_ID);
  g.commit([&] {
    g.connect(osc, filter);
    g.connect(filter, AUD_GRAPH_NODE);
  });
  aud_graph_start(g.graph);
  for (int i = 0; i < 10; ++i) g.render(256);
  AUD_CHECK(g.stats().realtime_violations == 0);
  AUD_CHECK(aud_graph_watchdog_violations() == 0);
}

AUD_TEST(keeps_a_short_note_in_order_and_retriggers_a_sounding_one) {
  Fixture f;
  const int32_t recorder = f.node("aud.test.recorder");
  f.commit([&] { f.connect(recorder, AUD_GRAPH_NODE); });
  aud_graph_start(f.graph);
  f.render(256);
  // A note on and its own note off in one block, on a pitch that is
  // silent: they keep their order - swapped, the note would hang.
  const AudEvent on = noteOn(60);
  const AudEvent off = noteOff(60);
  AUD_CHECK(aud_graph_send_event(f.graph, recorder, &on, nullptr, 0) == AUD_OK);
  AUD_CHECK(aud_graph_send_event(f.graph, recorder, &off, nullptr, 0) == AUD_OK);
  f.render(256);
  AUD_CHECK(g_records.size() == 2);
  AUD_CHECK(aud_ump_is_note_on(g_records[0].word0, 0) &&
            aud_ump_is_note_off(g_records[1].word0, 0));
  // A sounding pitch retriggers: the note off of the earlier note goes
  // first, although the next note on was queued before it.
  AUD_CHECK(aud_graph_send_event(f.graph, recorder, &on, nullptr, 0) == AUD_OK);
  f.render(256);
  g_records.clear();
  AUD_CHECK(aud_graph_send_event(f.graph, recorder, &on, nullptr, 0) == AUD_OK);
  AUD_CHECK(aud_graph_send_event(f.graph, recorder, &off, nullptr, 0) == AUD_OK);
  f.render(256);
  AUD_CHECK(g_records.size() == 2);
  AUD_CHECK(aud_ump_is_note_off(g_records[0].word0, 0) &&
            aud_ump_is_note_on(g_records[1].word0, 0));
}

AUD_TEST(refuses_descriptors_without_parameter_or_string_ids) {
  Fixture f;
  const AudHostApi* host = aud_graph_host_api(f.graph);
  AudParamDescriptor params[2] = {kPresetParams[0], kPresetParams[1]};
  params[1].id = nullptr;
  AudNodeDescriptor d = kPresetDescriptor;
  d.type_id = "aud.test.noid";
  d.params = params;
  AUD_CHECK(host->register_node_type(host->host, &d) == AUD_ERROR_INVALID_ARGUMENT);
  AudStringKeyDescriptor keys[2] = {kPresetStrings[0], kPresetStrings[1]};
  keys[0].id = nullptr;
  d.params = kPresetParams;
  d.string_keys = keys;
  AUD_CHECK(host->register_node_type(host->host, &d) == AUD_ERROR_INVALID_ARGUMENT);
  d.string_keys = nullptr;
  AUD_CHECK(host->register_node_type(host->host, &d) == AUD_ERROR_INVALID_ARGUMENT);
  d.string_keys = kPresetStrings;
  d.output_buses = nullptr;
  AUD_CHECK(host->register_node_type(host->host, &d) == AUD_ERROR_INVALID_ARGUMENT);
  d.output_buses = kMonoOut;
  AUD_CHECK(host->register_node_type(host->host, &d) == AUD_OK);
}
