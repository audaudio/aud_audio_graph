// @license
// Copyright (c) Audanika. All Rights Reserved.
//
// Use of this source code is governed by terms that can be
// found in the LICENSE file in the root of this package.

// The native tests of the reference node aud.graph.channel_split (ticket
// 37): it hands each channel voice message from the graph's event input to
// the output of its MIDI channel and every other event to output 0, in a
// graph built through the C API and in a document of the headless host;
// a block full of events fits through it, and the nodes behind it close
// their own notes. A recorder behind each output tells by its instance
// where an event went.

#include <algorithm>
#include <cstring>
#include <vector>

#include "aud_audio_graph.h"
#include "aud_graph_fixture.hpp"
#include "aud_test.hpp"
#include "aud_ump.h"

using namespace aud_test_fixture;

namespace {

// The UMP channels of the four receivers: MIDI channels 1, 2, 3 and 6.
constexpr uint32_t kChannels[4] = {0, 1, 2, 5};

// The document of a plugin that plays one instrument per MIDI channel:
// channel 1 the bass, 2 the chords, 3 the melody, 4 to 16 an external
// instrument.
const char* const kSplitDocument = R"({
  "schema": 1,
  "name": "Channel split",
  "outputChannels": [1],
  "nodes": [
    { "id": "split", "type": "aud.graph.channel_split" },
    { "id": "bass", "type": "aud.test.recorder" },
    { "id": "chord", "type": "aud.test.recorder" },
    { "id": "melody", "type": "aud.test.recorder" },
    { "id": "external", "type": "aud.test.recorder" }
  ],
  "eventConnections": [
    { "from": "graph", "to": "split" },
    { "from": "split", "fromPort": 0, "to": "bass" },
    { "from": "split", "fromPort": 1, "to": "chord" },
    { "from": "split", "fromPort": 2, "to": "melody" },
    { "from": "split", "fromPort": 3, "to": "external" },
    { "from": "split", "fromPort": 4, "to": "external" },
    { "from": "split", "fromPort": 5, "to": "external" },
    { "from": "split", "fromPort": 6, "to": "external" },
    { "from": "split", "fromPort": 7, "to": "external" },
    { "from": "split", "fromPort": 8, "to": "external" },
    { "from": "split", "fromPort": 9, "to": "external" },
    { "from": "split", "fromPort": 10, "to": "external" },
    { "from": "split", "fromPort": 11, "to": "external" },
    { "from": "split", "fromPort": 12, "to": "external" },
    { "from": "split", "fromPort": 13, "to": "external" },
    { "from": "split", "fromPort": 14, "to": "external" },
    { "from": "split", "fromPort": 15, "to": "external" }
  ]
})";

// An event moved to an offset.
AudEvent at(AudEvent event, uint32_t offset) {
  event.sample_offset = offset;
  return event;
}

// A MIDI 2.0 note on or off (UMP message type 4) in group 2.
AudEvent midi2Note(bool on, uint32_t note, uint32_t channel, uint32_t offset) {
  uint32_t words[2];
  aud_ump_midi2_note(2, channel, on ? 1 : 0, note, on ? 0xC000 : 0x8000, 0, 0,
                     words);
  return aud_event_ump(words, 2, offset, 0);
}

// The records of one recorder, in the order it received them.
std::vector<Record> recordsOf(const void* recorder) {
  std::vector<Record> result;
  for (const Record& r : g_records) {
    if (r.node == recorder) result.push_back(r);
  }
  return result;
}

// Whether `recorder` received exactly `expected`, in its order, each on its
// input 0 with the type, offset, flags and words it entered the graph with.
bool received(const void* recorder, const std::vector<AudEvent>& expected) {
  const std::vector<Record> got = recordsOf(recorder);
  if (got.size() != expected.size()) return false;
  for (size_t i = 0; i < got.size(); ++i) {
    const Record& r = got[i];
    const AudEvent& e = expected[i];
    if (r.type != e.type || r.offset != e.sample_offset || r.port != 0 ||
        r.word0 != e.words[0] || r.word1 != e.words[1] || r.flags != e.flags) {
      return false;
    }
  }
  return true;
}

// Whether `recorder` received one note off at the block start for each of
// the `count` notes from `first` on UMP channel `channel`, and nothing
// else.
bool closedOnce(const void* recorder, uint32_t channel, uint32_t first,
                uint32_t count) {
  const std::vector<Record> got = recordsOf(recorder);
  if (got.size() != count) return false;
  std::vector<bool> seen(128, false);
  for (const Record& r : got) {
    const uint32_t note = aud_ump_note(r.word0);
    if (r.offset != 0 || !aud_ump_is_note_off(r.word0, r.word1) ||
        aud_ump_channel(r.word0) != channel || note < first ||
        note >= first + count || seen[note]) {
      return false;
    }
    seen[note] = true;
  }
  return true;
}

// A split behind the graph's event input with a recorder behind the
// outputs of kChannels; MIDI channel 16 leaves through the graph's event
// output, e.g. to an external instrument.
struct SplitFixture {
  Fixture f;
  int32_t split = 0;
  int32_t receivers[4] = {};
  const void* recorders[4] = {};

  SplitFixture() {
    split = f.node(AUD_GRAPH_CHANNEL_SPLIT_TYPE_ID);
    for (uint32_t k = 0; k < 4; ++k) {
      receivers[k] = f.node("aud.test.recorder");
      recorders[k] = g_recorders.back();
    }
    f.commit([&] {
      AUD_CHECK(aud_graph_connect_events(f.graph, AUD_GRAPH_NODE, 0, split,
                                         0) == AUD_OK);
      for (uint32_t k = 0; k < 4; ++k) {
        AUD_CHECK(aud_graph_connect_events(f.graph, split, kChannels[k],
                                           receivers[k], 0) == AUD_OK);
      }
      AUD_CHECK(aud_graph_connect_events(f.graph, split, 15, AUD_GRAPH_NODE,
                                         0) == AUD_OK);
    });
    aud_graph_start(f.graph);
  }

  // Holds `external` notes from `first` on MIDI channel 16, then the
  // `count` notes from `first` on the channel of every recorder: note ons
  // from the graph's event input, one per frame, as many blocks as they
  // take. The split's own tracker sees the notes in this order.
  void hold(uint32_t first, uint32_t count, uint32_t external) {
    std::vector<AudEvent> ons;
    for (uint32_t note = first; note < first + external; ++note) {
      ons.push_back(noteOn(note, 100, 0, 15));
    }
    for (uint32_t note = first; note < first + count; ++note) {
      for (uint32_t k = 0; k < 4; ++k) {
        ons.push_back(noteOn(note, 100, 0, kChannels[k]));
      }
    }
    for (size_t from = 0; from < ons.size(); from += 256) {
      const uint32_t size =
          static_cast<uint32_t>(std::min<size_t>(256, ons.size() - from));
      for (uint32_t i = 0; i < size; ++i) ons[from + i].sample_offset = i;
      AUD_CHECK(f.render(256, nullptr, ons.data() + from, size) == AUD_OK);
    }
  }

  // The note offs on MIDI channel 16 that left the graph's event output
  // since the notifications were taken last.
  uint32_t externalNoteOffs() {
    uint32_t count = 0;
    for (const AudGraphNotification& n : f.takeAll()) {
      if (n.type == AUD_NOTIFY_EVENT && n.node == split &&
          aud_ump_is_note_off(n.event.words[0], n.event.words[1]) &&
          aud_ump_channel(n.event.words[0]) == 15) {
        count += 1;
      }
    }
    return count;
  }
};

}  // namespace

// ############################################################################

AUD_TEST(channel_split_describes_one_input_and_sixteen_outputs) {
  Fixture f;
  const AudNodeDescriptor* d =
      aud_graph_node_type_by_id(f.graph, AUD_GRAPH_CHANNEL_SPLIT_TYPE_ID);
  AUD_CHECK(d != nullptr);
  if (d == nullptr) return;
  AUD_CHECK(d->num_input_buses == 0 && d->num_output_buses == 0);
  AUD_CHECK(d->num_params == 0 && d->num_string_keys == 0);
  AUD_CHECK((d->capabilities & AUD_NODE_CAP_EVENTS) != 0 &&
            (d->capabilities & AUD_NODE_CAP_EVENT_OUTPUT) != 0 &&
            (d->capabilities & AUD_NODE_CAP_PASSES_EVENTS) != 0);
  AUD_CHECK(d->num_event_inputs == 1 &&
            std::strcmp(d->event_inputs[0].id, "in") == 0);
  AUD_CHECK(d->num_event_outputs == AUD_CHANNEL_SPLIT_NUM_OUTPUTS);
  AUD_CHECK(std::strcmp(d->event_outputs[0].id, "ch1") == 0);
  AUD_CHECK(std::strcmp(d->event_outputs[5].id, "ch6") == 0);
  AUD_CHECK(std::strcmp(d->event_outputs[15].id, "ch16") == 0);
  // A seventeenth output does not exist.
  const int32_t split = f.node(AUD_GRAPH_CHANNEL_SPLIT_TYPE_ID);
  const int32_t recorder = f.node("aud.test.recorder");
  AUD_CHECK(aud_graph_begin(f.graph) == AUD_OK);
  AUD_CHECK(aud_graph_connect_events(f.graph, split,
                                     AUD_CHANNEL_SPLIT_NUM_OUTPUTS, recorder,
                                     0) == AUD_ERROR_INVALID_ARGUMENT);
  AUD_CHECK(aud_graph_rollback(f.graph) == AUD_OK);
}

AUD_TEST(channel_split_routes_each_channel_to_its_output) {
  SplitFixture s;
  // Note ons and offs on MIDI channels 1, 2, 3 and 6 in the events of one
  // render request; the note of channel 6 is MIDI 2.0 in group 2, the first
  // note on is live.
  AudEvent in[8];
  for (uint32_t k = 0; k < 3; ++k) {
    in[k] = at(noteOn(60 + k, 100, 0, kChannels[k]), 10 + 16 * k);
    in[4 + k] = at(noteOff(60 + k, 0, kChannels[k]), 100 + 16 * k);
  }
  in[3] = midi2Note(true, 63, kChannels[3], 58);
  in[7] = midi2Note(false, 63, kChannels[3], 148);
  in[0].flags = AUD_EVENT_FLAG_LIVE;
  AUD_CHECK(s.f.render(256, nullptr, in, 8) == AUD_OK);
  AUD_CHECK(g_records.size() == 8);
  for (uint32_t k = 0; k < 4; ++k) {
    AUD_CHECK(received(s.recorders[k], {in[k], in[4 + k]}));
  }
  // The receivers track the notes the split hands them like those of any
  // event route: a transport stop closes them, each on its own channel.
  AUD_CHECK(s.f.request(AUD_TRANSPORT_REQUEST_START) == AUD_OK);
  s.f.render(256);
  AudEvent held[4];
  for (uint32_t k = 0; k < 3; ++k) held[k] = noteOn(70 + k, 100, 0, kChannels[k]);
  held[3] = midi2Note(true, 73, kChannels[3], 0);
  AUD_CHECK(s.f.render(256, nullptr, held, 4) == AUD_OK);
  g_records.clear();
  AUD_CHECK(s.f.request(AUD_TRANSPORT_REQUEST_STOP) == AUD_OK);
  s.f.render(256);
  AUD_CHECK(g_records.size() == 4);
  for (uint32_t k = 0; k < 4; ++k) {
    const std::vector<Record> got = recordsOf(s.recorders[k]);
    AUD_CHECK(got.size() == 1);
    if (got.size() != 1) continue;
    const Record& r = got[0];
    AUD_CHECK(r.offset == 0 && aud_ump_is_note_off(r.word0, r.word1));
    AUD_CHECK(aud_ump_note(r.word0) == 70 + k &&
              aud_ump_channel(r.word0) == kChannels[k]);
    AUD_CHECK(aud_ump_message_type(r.word0) ==
              aud_ump_message_type(held[k].words[0]));
  }
}

AUD_TEST(channel_split_hands_other_events_to_output_0_only) {
  Fixture f;
  const int32_t split = f.node(AUD_GRAPH_CHANNEL_SPLIT_TYPE_ID);
  // Recorders behind outputs 0, 1 and 15: MIDI channels 1, 2 and 16.
  const uint32_t outputs[3] = {0, 1, 15};
  int32_t receivers[3];
  const void* recorders[3];
  for (uint32_t k = 0; k < 3; ++k) {
    receivers[k] = f.node("aud.test.recorder");
    recorders[k] = g_recorders.back();
  }
  f.commit([&] {
    AUD_CHECK(aud_graph_connect_events(f.graph, AUD_GRAPH_NODE, 0, split, 0) ==
              AUD_OK);
    for (uint32_t k = 0; k < 3; ++k) {
      AUD_CHECK(aud_graph_connect_events(f.graph, split, outputs[k],
                                         receivers[k], 0) == AUD_OK);
    }
  });
  aud_graph_start(f.graph);
  // A timing clock, a JR timestamp and a SysEx7 packet: read as channel
  // voice messages, the timing clock and the SysEx would name channels 8
  // and 2. Then a numeric control, a program change on MIDI channel 1 and
  // control changes on channels 2 and 16.
  const uint32_t clock = 0x10F80000;
  const uint32_t timestamp = 0x00201234;
  const uint32_t sysex[2] = {0x31027E7F, 0};
  const uint32_t program = aud_ump_midi1_word(0, 0xC0, 5, 0);
  const uint32_t cc2 = ccWord(1, 64, 1);
  const uint32_t cc16 = ccWord(7, 100, 15, 3);
  const AudEvent in[8] = {
      aud_event_ump(&clock, 1, 5, 0),
      aud_event_ump(&timestamp, 1, 6, 0),
      aud_event_ump(sysex, 2, 7, 0),
      aud_event_control_float(7, 0.5f, 40, 0),
      // A parameter event addresses the split itself, which has none: it
      // leaves on no output.
      aud_event_param(0, 0.5f, 0, 50),
      aud_event_ump(&cc2, 1, 70, 0),
      aud_event_ump(&cc16, 1, 80, 0),
      aud_event_ump(&program, 1, 90, 0),
  };
  AUD_CHECK(f.render(256, nullptr, in, 8) == AUD_OK);
  AUD_CHECK(g_records.size() == 7);
  AUD_CHECK(received(recorders[0], {in[0], in[1], in[2], in[3], in[7]}));
  AUD_CHECK(received(recorders[1], {in[5]}));
  AUD_CHECK(received(recorders[2], {in[6]}));
}

AUD_TEST(channel_split_hands_a_full_block_to_two_receivers) {
  Fixture f;
  const int32_t split = f.node(AUD_GRAPH_CHANNEL_SPLIT_TYPE_ID);
  const int32_t first = f.node("aud.test.recorder");
  const int32_t second = f.node("aud.test.recorder");
  f.commit([&] {
    AUD_CHECK(aud_graph_connect_events(f.graph, AUD_GRAPH_NODE, 0, split, 0) ==
              AUD_OK);
    AUD_CHECK(aud_graph_connect_events(f.graph, split, 0, first, 0) == AUD_OK);
    AUD_CHECK(aud_graph_connect_events(f.graph, split, 0, second, 0) == AUD_OK);
  });
  aud_graph_start(f.graph);
  // A chase of the song position: a block's budget of controllers on MIDI
  // channel 1 at one time leaves the scheduler in the second block, and
  // the split hands each of them to both receivers - twice the budget
  // that the nodes emit in one block.
  const uint32_t count = 1024;  // the default of max_events_per_block
  const AudTimestamp time = atSample(300);
  std::vector<AudEvent> expected;
  for (uint32_t i = 0; i < count; ++i) {
    const uint32_t word = ccWord(i % 120, i % 128);
    const AudEvent event = aud_event_ump(&word, 1, 0, 0);
    AUD_CHECK(aud_graph_send_event(f.graph, AUD_GRAPH_NODE, &event, &time,
                                   0) == AUD_OK);
    expected.push_back(at(event, 300 - 256));
  }
  f.render(256);
  AUD_CHECK(g_records.empty());
  f.render(256);
  AUD_CHECK(received(g_recorders[0], expected));
  AUD_CHECK(received(g_recorders[1], expected));
  AUD_CHECK(f.stats().events_dropped == 0);
  AUD_CHECK(f.countNotifications(AUD_NOTIFY_DIAGNOSTIC, AUD_ERROR_QUEUE_FULL) ==
            0);
}

AUD_TEST(channel_split_leaves_the_closing_of_notes_to_the_nodes_behind_it) {
  SplitFixture s;
  // Two notes leave the graph on MIDI channel 16, then 70 notes on the
  // channel of each receiver, 280 in all: more than the split's tracker
  // holds - the 256 notes it closes when it goes -, fewer than each
  // receiver does.
  s.hold(30, 70, 2);
  for (uint32_t k = 0; k < 4; ++k) {
    AUD_CHECK(recordsOf(s.recorders[k]).size() == 70);
  }
  AUD_CHECK(s.f.stats().events_dropped == 0);
  s.f.takeAll();
  // A stop and a start: every receiver closes its own notes, one note off
  // each, in the first block after the restart; the notes that left the
  // graph close as well.
  g_records.clear();
  AUD_CHECK(aud_graph_stop(s.f.graph) == AUD_OK);
  AUD_CHECK(aud_graph_start(s.f.graph) == AUD_OK);
  s.f.render(256);
  for (uint32_t k = 0; k < 4; ++k) {
    AUD_CHECK(closedOnce(s.recorders[k], kChannels[k], 30, 70));
  }
  AUD_CHECK(s.externalNoteOffs() == 2);
  // A prepare while suspended closes them the same way.
  s.hold(40, 10, 1);
  s.f.takeAll();
  g_records.clear();
  AUD_CHECK(aud_graph_suspend(s.f.graph) == AUD_OK);
  AUD_CHECK(aud_graph_prepare(s.f.graph, kRate, 256) == AUD_OK);
  AUD_CHECK(aud_graph_resume(s.f.graph) == AUD_OK);
  s.f.render(256);
  for (uint32_t k = 0; k < 4; ++k) {
    AUD_CHECK(closedOnce(s.recorders[k], kChannels[k], 40, 10));
  }
  AUD_CHECK(s.externalNoteOffs() == 1);
  // Nothing more follows.
  g_records.clear();
  s.f.render(256);
  AUD_CHECK(g_records.empty() && s.externalNoteOffs() == 0);
}

AUD_TEST(channel_split_closes_the_notes_behind_it_when_it_is_removed) {
  SplitFixture s;
  s.hold(60, 3, 2);
  s.f.takeAll();
  g_records.clear();
  // The split retires: it hands on the note offs of the notes it handed
  // on, once each, while it fades out.
  s.f.commit([&] { aud_graph_remove_node(s.f.graph, s.split); });
  for (int i = 0; i < 4; ++i) s.f.render(256);
  for (uint32_t k = 0; k < 4; ++k) {
    AUD_CHECK(closedOnce(s.recorders[k], kChannels[k], 60, 3));
  }
  AUD_CHECK(s.externalNoteOffs() == 2);
}

AUD_TEST(channel_split_routes_a_document_of_the_headless_host) {
  Fixture f;
  AudHost* host = aud_host_create(f.graph, nullptr);
  AUD_CHECK(host != nullptr);
  if (host == nullptr) return;
  const int32_t loaded =
      aud_host_load(host, kSplitDocument, std::strlen(kSplitDocument));
  AUD_CHECK_MSG(loaded == AUD_OK, aud_host_last_error(host));
  // The host creates the nodes in document order, so the recorders came
  // as bass, chord, melody and external; their handles say so.
  const char* const ids[4] = {"bass", "chord", "melody", "external"};
  AUD_CHECK(g_recorders.size() == 4);
  for (uint32_t k = 0; k + 1 < 4; ++k) {
    AUD_CHECK(aud_host_node(host, ids[k]) > 0 &&
              aud_host_node(host, ids[k]) < aud_host_node(host, ids[k + 1]));
  }
  if (loaded != AUD_OK || g_recorders.size() != 4) {
    aud_host_destroy(host);
    return;
  }
  aud_graph_start(f.graph);
  // Notes on MIDI channels 1, 2, 3, 4, 10 and 16, on in the first block and
  // off in the second.
  const uint32_t channels[6] = {0, 1, 2, 3, 9, 15};
  const uint32_t notes[6] = {36, 60, 72, 50, 38, 90};
  for (uint32_t block = 0; block < 2; ++block) {
    AudEvent in[6];
    for (uint32_t k = 0; k < 6; ++k) {
      const AudEvent note = block == 0 ? noteOn(notes[k], 100, 0, channels[k])
                                       : noteOff(notes[k], 0, channels[k]);
      in[k] = at(note, 3 + 37 * k + 11 * block);
    }
    g_records.clear();
    AudEvent out[4];
    f.hostRequest = f.renderRequest(256, nullptr, in, 6);
    AudHostRenderRequest request{
        sizeof(AudHostRenderRequest), 0, &f.hostRequest, out, 4, 0, 0, 0};
    AUD_CHECK(aud_host_render(host, &request) == AUD_OK);
    // Nothing leaves the graph; each instrument gets its channels.
    AUD_CHECK(request.num_output_events == 0);
    AUD_CHECK(g_records.size() == 6);
    AUD_CHECK(received(g_recorders[0], {in[0]}));
    AUD_CHECK(received(g_recorders[1], {in[1]}));
    AUD_CHECK(received(g_recorders[2], {in[2]}));
    AUD_CHECK(received(g_recorders[3], {in[3], in[4], in[5]}));
  }
  aud_host_destroy(host);
}
