// @license
// Copyright (c) Audanika. All Rights Reserved.
//
// Use of this source code is governed by terms that can be
// found in the LICENSE file in the root of this package.

// The native tests of the reference node aud.graph.channel_split (ticket
// 37): it hands each channel voice message from the graph's event input to
// the output of its MIDI channel and every other event to output 0, in a
// graph built through the C API and in a document of the headless host.
// A recorder behind each output tells by its instance where an event went.

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
            (d->capabilities & AUD_NODE_CAP_EVENT_OUTPUT) != 0);
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
  Fixture f;
  const int32_t split = f.node(AUD_GRAPH_CHANNEL_SPLIT_TYPE_ID);
  int32_t receivers[4];
  const void* recorders[4];
  for (uint32_t k = 0; k < 4; ++k) {
    receivers[k] = f.node("aud.test.recorder");
    recorders[k] = g_recorders.back();
  }
  f.commit([&] {
    AUD_CHECK(aud_graph_connect_events(f.graph, AUD_GRAPH_NODE, 0, split, 0) ==
              AUD_OK);
    for (uint32_t k = 0; k < 4; ++k) {
      AUD_CHECK(aud_graph_connect_events(f.graph, split, kChannels[k],
                                         receivers[k], 0) == AUD_OK);
    }
  });
  aud_graph_start(f.graph);
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
  AUD_CHECK(f.render(256, nullptr, in, 8) == AUD_OK);
  AUD_CHECK(g_records.size() == 8);
  for (uint32_t k = 0; k < 4; ++k) {
    AUD_CHECK(received(recorders[k], {in[k], in[4 + k]}));
  }
  // The receivers track the notes the split hands them like those of any
  // event route: a transport stop closes them, each on its own channel.
  AUD_CHECK(f.request(AUD_TRANSPORT_REQUEST_START) == AUD_OK);
  f.render(256);
  AudEvent held[4];
  for (uint32_t k = 0; k < 3; ++k) held[k] = noteOn(70 + k, 100, 0, kChannels[k]);
  held[3] = midi2Note(true, 73, kChannels[3], 0);
  AUD_CHECK(f.render(256, nullptr, held, 4) == AUD_OK);
  g_records.clear();
  AUD_CHECK(f.request(AUD_TRANSPORT_REQUEST_STOP) == AUD_OK);
  f.render(256);
  AUD_CHECK(g_records.size() == 4);
  for (uint32_t k = 0; k < 4; ++k) {
    const std::vector<Record> got = recordsOf(recorders[k]);
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
