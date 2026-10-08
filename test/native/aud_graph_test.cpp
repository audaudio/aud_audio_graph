// @license
// Copyright (c) Audanika. All Rights Reserved.
//
// Use of this source code is governed by terms that can be
// found in the LICENSE file in the root of this package.

// The native tests of the graph engine; `node scripts/test-native.js`
// builds and runs them with the sanitizers. They drive the C API the way
// aud_audio_io and the plugin shells do and register test node types
// through the host api the way DSP packages do.

#include <cmath>
#include <cstring>
#include <functional>
#include <vector>

#include "aud_audio_graph.h"
#include "aud_test.hpp"
#include "aud_ump.h"

namespace {

constexpr double kRate = 48000.0;
constexpr uint32_t kFade = 240;  // 5 ms at 48 kHz

// ............................................................................
// Test node types, registered like the nodes of a DSP package

struct Record {
  int64_t position;
  uint32_t offset;
  uint32_t port;
  uint32_t type;
  uint32_t word0;
};

std::vector<Record> g_records;
std::vector<uint32_t> g_resets;
std::vector<uint32_t> g_fixedFrames;

// aud.test.recorder: records the events it receives and its resets.
struct Recorder {
  int64_t position = 0;
};

void* recorderCreate(const AudNodeDescriptor*, const AudHostApi*) {
  return new Recorder();
}
void recorderDestroy(void* instance) { delete static_cast<Recorder*>(instance); }
int32_t recorderPrepare(void*, const AudPrepareInfo*) { return AUD_OK; }
void recorderReset(void*, uint32_t reason) { g_resets.push_back(reason); }
void recorderSetParam(void*, uint32_t, float) {}
void recorderProcess(void*, const AudProcessContext* context) {
  for (uint32_t i = 0; i < context->num_events; ++i) {
    const AudEvent& e = context->events[i];
    g_records.push_back({context->sample_position, e.sample_offset, e.port,
                         e.type, e.words[0]});
  }
  for (uint32_t b = 0; b < context->num_output_buses; ++b) {
    for (uint32_t c = 0; c < context->outputs[b].num_channels; ++c) {
      std::memset(context->outputs[b].channels[c], 0,
                  sizeof(float) * context->frames);
    }
  }
}

const AudBusDescriptor kMonoOut[] = {
    {sizeof(AudBusDescriptor), "out", "Output", AUD_BUS_MAIN | AUD_BUS_OPTIONAL,
     1, 2, 1},
};
const AudBusDescriptor kMonoIn[] = {
    {sizeof(AudBusDescriptor), "in", "Input", AUD_BUS_MAIN, 1, 2, 1},
};
const AudEventPortDescriptor kEventIn[] = {
    {sizeof(AudEventPortDescriptor), "events", "Events",
     AUD_EVENT_PORT_MIDI | AUD_EVENT_PORT_CONTROL, 0},
};
const AudEventPortDescriptor kEventOut[] = {
    {sizeof(AudEventPortDescriptor), "out", "Out", AUD_EVENT_PORT_MIDI, 0},
};

const AudNodeVTable kRecorderVTable = {
    sizeof(AudNodeVTable), recorderCreate, recorderDestroy, recorderPrepare,
    recorderReset,         recorderSetParam, recorderProcess, nullptr,
    nullptr,               nullptr,          nullptr,         nullptr,
};

const AudNodeDescriptor kRecorderDescriptor = {
    sizeof(AudNodeDescriptor),
    AUD_ABI_VERSION_MAJOR,
    AUD_ABI_VERSION_MINOR,
    1,
    "aud.test.recorder",
    "Recorder",
    "Test",
    AUD_NODE_CAP_VARIABLE_BLOCK | AUD_NODE_CAP_EVENTS |
        AUD_NODE_CAP_RESET_ON_STOP | AUD_NODE_CAP_RESET_ON_SEEK,
    0,
    0,
    1,
    nullptr,
    kMonoOut,
    1,
    0,
    kEventIn,
    nullptr,
    0,
    0,
    nullptr,
    nullptr,
    &kRecorderVTable,
};

// aud.test.delay: delays its mono input by 100 frames and reports it.
constexpr uint32_t kTestLatency = 100;

struct Delay {
  std::vector<float> line = std::vector<float>(kTestLatency, 0.0f);
  uint32_t pos = 0;
};

void* delayCreate(const AudNodeDescriptor*, const AudHostApi*) {
  return new Delay();
}
void delayDestroy(void* instance) { delete static_cast<Delay*>(instance); }
int32_t delayPrepare(void*, const AudPrepareInfo*) { return AUD_OK; }
void delayReset(void*, uint32_t) {}
void delaySetParam(void*, uint32_t, float) {}
void delayProcess(void* instance, const AudProcessContext* context) {
  auto* delay = static_cast<Delay*>(instance);
  const float* in = context->inputs[0].channels[0];
  float* out = context->outputs[0].channels[0];
  for (uint32_t i = 0; i < context->frames; ++i) {
    out[i] = delay->line[delay->pos];
    delay->line[delay->pos] = in[i];
    delay->pos = (delay->pos + 1) % kTestLatency;
  }
}
uint32_t delayLatency(void*) { return kTestLatency; }

const AudNodeVTable kDelayVTable = {
    sizeof(AudNodeVTable), delayCreate, delayDestroy, delayPrepare, delayReset,
    delaySetParam,         delayProcess, nullptr,     delayLatency, nullptr,
    nullptr,               nullptr,
};

const AudNodeDescriptor kDelayDescriptor = {
    sizeof(AudNodeDescriptor),
    AUD_ABI_VERSION_MAJOR,
    AUD_ABI_VERSION_MINOR,
    1,
    "aud.test.delay",
    "Delay",
    "Test",
    AUD_NODE_CAP_VARIABLE_BLOCK | AUD_NODE_CAP_LATENCY,
    0,
    1,
    1,
    kMonoIn,
    kMonoOut,
    0,
    0,
    nullptr,
    nullptr,
    0,
    0,
    nullptr,
    nullptr,
    &kDelayVTable,
};

// aud.test.fixed: needs constant blocks; copies its input and records the
// frame counts it sees.
void* fixedCreate(const AudNodeDescriptor*, const AudHostApi*) {
  return new int(0);
}
void fixedDestroy(void* instance) { delete static_cast<int*>(instance); }
void fixedProcess(void*, const AudProcessContext* context) {
  g_fixedFrames.push_back(context->frames);
  std::memcpy(context->outputs[0].channels[0], context->inputs[0].channels[0],
              sizeof(float) * context->frames);
}

const AudNodeVTable kFixedVTable = {
    sizeof(AudNodeVTable), fixedCreate, fixedDestroy, delayPrepare, delayReset,
    delaySetParam,         fixedProcess, nullptr,     nullptr,      nullptr,
    nullptr,               nullptr,
};

const AudNodeDescriptor kFixedDescriptor = {
    sizeof(AudNodeDescriptor),
    AUD_ABI_VERSION_MAJOR,
    AUD_ABI_VERSION_MINOR,
    1,
    "aud.test.fixed",
    "Fixed",
    "Test",
    0,
    0,
    1,
    1,
    kMonoIn,
    kMonoOut,
    0,
    0,
    nullptr,
    nullptr,
    0,
    0,
    nullptr,
    nullptr,
    &kFixedVTable,
};

// aud.test.emitter: emits every event it receives on its event output.
struct Emitter {
  const AudHostApi* host;
};

void* emitterCreate(const AudNodeDescriptor*, const AudHostApi* host) {
  return new Emitter{host};
}
void emitterDestroy(void* instance) { delete static_cast<Emitter*>(instance); }
void emitterProcess(void* instance, const AudProcessContext* context) {
  auto* emitter = static_cast<Emitter*>(instance);
  for (uint32_t i = 0; i < context->num_events; ++i) {
    AudEvent event = context->events[i];
    event.port = 0;
    emitter->host->emit_event(emitter->host->host, instance, &event);
  }
}

const AudNodeVTable kEmitterVTable = {
    sizeof(AudNodeVTable), emitterCreate, emitterDestroy, delayPrepare,
    delayReset,            delaySetParam, emitterProcess, nullptr,
    nullptr,               nullptr,       nullptr,        nullptr,
};

const AudNodeDescriptor kEmitterDescriptor = {
    sizeof(AudNodeDescriptor),
    AUD_ABI_VERSION_MAJOR,
    AUD_ABI_VERSION_MINOR,
    1,
    "aud.test.emitter",
    "Emitter",
    "Test",
    AUD_NODE_CAP_VARIABLE_BLOCK | AUD_NODE_CAP_EVENTS |
        AUD_NODE_CAP_EVENT_OUTPUT,
    0,
    0,
    0,
    nullptr,
    nullptr,
    1,
    1,
    kEventIn,
    kEventOut,
    0,
    0,
    nullptr,
    nullptr,
    &kEmitterVTable,
};

// ............................................................................
// A graph with one input bus and one output bus

struct Fixture {
  AudGraph* graph = nullptr;
  uint32_t inChannels;
  uint32_t outChannels;
  uint32_t maxFrames;
  std::vector<std::vector<float>> in;
  std::vector<std::vector<float>> out;
  std::vector<float*> inPointers;
  std::vector<float*> outPointers;
  AudAudioBus inBus{};
  AudAudioBus outBus{};

  explicit Fixture(uint32_t inputs = 0, uint32_t outputs = 1,
                   uint32_t frames = 256,
                   std::function<void(AudGraphConfig&)> tweak = nullptr)
      : inChannels(inputs), outChannels(outputs), maxFrames(frames) {
    AudGraphConfig config{};
    config.struct_size = sizeof(AudGraphConfig);
    config.sample_rate = kRate;
    config.max_frames = frames;
    config.num_input_buses = inputs > 0 ? 1 : 0;
    config.input_channels = &inChannels;
    config.num_output_buses = outputs > 0 ? 1 : 0;
    config.output_channels = &outChannels;
    if (tweak) tweak(config);
    graph = aud_graph_create(&config);
    // One frame more than the largest block: a refused block is cleared.
    in.assign(inputs, std::vector<float>(frames + 1, 0.0f));
    out.assign(outputs, std::vector<float>(frames + 1, 0.0f));
    for (auto& channel : in) inPointers.push_back(channel.data());
    for (auto& channel : out) outPointers.push_back(channel.data());
    inBus = {sizeof(AudAudioBus), inputs, inPointers.data()};
    outBus = {sizeof(AudAudioBus), outputs, outPointers.data()};
    const AudHostApi* host = aud_graph_host_api(graph);
    host->register_node_type(host->host, &kRecorderDescriptor);
    host->register_node_type(host->host, &kDelayDescriptor);
    host->register_node_type(host->host, &kFixedDescriptor);
    host->register_node_type(host->host, &kEmitterDescriptor);
    g_records.clear();
    g_resets.clear();
    g_fixedFrames.clear();
  }

  ~Fixture() { aud_graph_destroy(graph); }

  int32_t node(const char* type, const AudNodeConfig* config = nullptr) {
    return aud_graph_create_node(graph, type, config);
  }

  int32_t commit(const std::function<void()>& edits) {
    const int32_t begun = aud_graph_begin(graph);
    if (begun != AUD_OK) return begun;
    edits();
    return aud_graph_commit(graph);
  }

  int32_t connect(int32_t from, int32_t to, uint32_t fromBus = 0,
                  uint32_t toBus = 0, uint32_t flags = 0) {
    return aud_graph_connect(graph, from, fromBus, to, toBus, flags);
  }

  int32_t render(uint32_t frames, const AudStreamTime* time = nullptr,
                 const AudEvent* events = nullptr, uint32_t numEvents = 0) {
    AudRenderRequest request{};
    request.struct_size = sizeof(AudRenderRequest);
    request.frames = frames;
    request.num_input_buses = inChannels > 0 ? 1 : 0;
    request.num_output_buses = outChannels > 0 ? 1 : 0;
    request.inputs = &inBus;
    request.outputs = &outBus;
    request.time = time;
    request.num_events = numEvents;
    request.events = events;
    return aud_graph_render(graph, &request);
  }

  // Renders blocks and collects the first output channel.
  std::vector<float> renderFrames(uint32_t frames, uint32_t block = 256) {
    std::vector<float> result;
    while (result.size() < frames) {
      const uint32_t n = std::min<uint32_t>(block, frames - result.size());
      render(n);
      result.insert(result.end(), out[0].begin(), out[0].begin() + n);
    }
    return result;
  }

  std::vector<AudGraphNotification> take() {
    std::vector<AudGraphNotification> result(64);
    const int32_t count = aud_graph_take_notifications(graph, result.data(), 64);
    result.resize(count < 0 ? 0 : count);
    return result;
  }

  uint32_t countNotifications(uint32_t type, int32_t code = INT32_MIN) {
    uint32_t count = 0;
    for (const AudGraphNotification& n : take()) {
      if (n.type == type && (code == INT32_MIN || n.code == code)) count += 1;
    }
    return count;
  }

  AudGraphStats stats() {
    AudGraphStats s{};
    s.struct_size = sizeof(AudGraphStats);
    aud_graph_get_stats(graph, &s);
    return s;
  }

  AudGraphTransportState transport() {
    AudGraphTransportState t{};
    t.struct_size = sizeof(AudGraphTransportState);
    aud_graph_transport_state(graph, &t);
    return t;
  }

  int32_t request(uint32_t type, double value = 0, int64_t beat = 0,
                  int64_t beatEnd = 0, const AudTimestamp* at = nullptr) {
    AudTransportRequest r{};
    r.struct_size = sizeof(AudTransportRequest);
    r.type = type;
    r.at.struct_size = sizeof(AudTimestamp);
    if (at) r.at = *at;
    r.value = value;
    r.beat = beat;
    r.beat_end = beatEnd;
    r.numerator = 3;
    r.denominator = 4;
    return aud_graph_transport(graph, &r);
  }
};

uint32_t risingCrossings(const std::vector<float>& samples) {
  uint32_t count = 0;
  for (size_t i = 1; i < samples.size(); ++i) {
    if (samples[i - 1] < 0 && samples[i] >= 0) count += 1;
  }
  return count;
}

AudEvent noteOn(uint32_t note, uint32_t velocity = 100, uint32_t port = 0) {
  const uint32_t word = aud_ump_midi1_word(0, 0x90, note, velocity);
  return aud_event_ump(&word, 1, 0, port);
}

AudEvent noteOff(uint32_t note, uint32_t port = 0) {
  const uint32_t word = aud_ump_midi1_word(0, 0x80, note, 64);
  return aud_event_ump(&word, 1, 0, port);
}

AudTimestamp atSample(int64_t position) {
  AudTimestamp t{};
  t.struct_size = sizeof(AudTimestamp);
  t.domain = AUD_TIME_SAMPLE;
  t.value = position;
  return t;
}

AudTimestamp atBeat(double beats) {
  AudTimestamp t{};
  t.struct_size = sizeof(AudTimestamp);
  t.domain = AUD_TIME_BEAT;
  t.value = static_cast<int64_t>(beats * static_cast<double>(AUD_BEAT_FACTOR));
  return t;
}

// A square wave at 0.5 Hz is a constant for the first 48000 frames: the
// steady signal the fade tests look at.
int32_t steadyOscillator(Fixture& f, float amplitude = 0.5f) {
  const int32_t osc = f.node(AUD_GRAPH_OSCILLATOR_TYPE_ID);
  aud_graph_set_param(f.graph, osc, AUD_OSCILLATOR_PARAM_WAVEFORM, 2, 0);
  aud_graph_set_param(f.graph, osc, AUD_OSCILLATOR_PARAM_FREQUENCY, 0.5f, 0);
  aud_graph_set_param(f.graph, osc, AUD_OSCILLATOR_PARAM_AMPLITUDE, amplitude, 0);
  return osc;
}

}  // namespace

// ############################################################################

AUD_TEST(create_refuses_invalid_configurations) {
  AudGraphConfig config{};
  AUD_CHECK(aud_graph_create(nullptr) == nullptr);
  AUD_CHECK(aud_graph_create(&config) == nullptr);
  config.struct_size = sizeof(AudGraphConfig);
  config.sample_rate = kRate;
  AUD_CHECK(aud_graph_create(&config) == nullptr);  // no max_frames
  config.max_frames = 64;
  const uint32_t zero = 0;
  config.num_output_buses = 1;
  config.output_channels = &zero;
  AUD_CHECK(aud_graph_create(&config) == nullptr);  // zero channels
  config.num_output_buses = 0;
  AudGraph* graph = aud_graph_create(&config);
  AUD_CHECK(graph != nullptr);
  AUD_CHECK(aud_graph_state(graph) == AUD_GRAPH_CREATED);
  AUD_CHECK(aud_graph_sample_rate(graph) == kRate);
  AUD_CHECK(aud_graph_max_frames(graph) == 64);
  aud_graph_destroy(graph);
  aud_graph_destroy(nullptr);
}

AUD_TEST(registers_the_reference_nodes_and_test_nodes) {
  Fixture f;
  AUD_CHECK(aud_graph_num_node_types(f.graph) == 9);
  AUD_CHECK(std::strcmp(aud_graph_node_type(f.graph, 0)->type_id,
                        AUD_GRAPH_FEEDBACK_TYPE_ID) == 0);
  AUD_CHECK(aud_graph_node_type(f.graph, 99) == nullptr);
  const AudNodeDescriptor* osc =
      aud_graph_node_type_by_id(f.graph, AUD_GRAPH_OSCILLATOR_TYPE_ID);
  AUD_CHECK(osc != nullptr && osc->num_params == 3);
  AUD_CHECK(aud_graph_node_type_by_id(f.graph, "aud.nothing") == nullptr);
  const AudHostApi* host = aud_graph_host_api(f.graph);
  AUD_CHECK(host->abi_major == AUD_ABI_VERSION_MAJOR);
  AUD_CHECK(host->abi_minor == AUD_ABI_VERSION_MINOR);
  AUD_CHECK(host->register_node_type(host->host, &kDelayDescriptor) ==
            AUD_ERROR_DUPLICATE_TYPE);
  AudNodeDescriptor old = kDelayDescriptor;
  old.type_id = "aud.test.old";
  old.abi_minor = AUD_ABI_VERSION_MINOR + 1;
  AUD_CHECK(host->register_node_type(host->host, &old) == AUD_ERROR_ABI_MINOR);
  old.abi_major = 7;
  AUD_CHECK(host->register_node_type(host->host, &old) == AUD_ERROR_ABI_MAJOR);
  AUD_CHECK(host->register_node_type(host->host, nullptr) ==
            AUD_ERROR_INVALID_ARGUMENT);
}

AUD_TEST(creates_instances_with_checked_formats) {
  Fixture f;
  const int32_t osc = f.node(AUD_GRAPH_OSCILLATOR_TYPE_ID);
  AUD_CHECK(osc == 1);
  AUD_CHECK(aud_graph_node_channels(f.graph, osc, 1, 0) == 1);
  AUD_CHECK(aud_graph_node_channels(f.graph, osc, 0, 0) ==
            AUD_ERROR_INVALID_ARGUMENT);
  const int32_t filter = f.node(AUD_GRAPH_FILTER_TYPE_ID);
  AUD_CHECK(filter == 2);
  AUD_CHECK(aud_graph_node_channels(f.graph, filter, 0, 0) == 2);
  AUD_CHECK(f.node("aud.nothing") == AUD_ERROR_UNKNOWN_TYPE);
  const uint32_t tooMany = 99;
  AudNodeConfig config{sizeof(AudNodeConfig), 1, &tooMany, 0, nullptr, 0, 0};
  AUD_CHECK(f.node(AUD_GRAPH_FILTER_TYPE_ID, &config) == AUD_ERROR_FORMAT);
  const uint32_t four = 4;
  const uint32_t two = 2;
  AudNodeConfig mismatch{sizeof(AudNodeConfig), 1, &four, 1, &two, 0, 0};
  AUD_CHECK(f.node(AUD_GRAPH_FEEDBACK_TYPE_ID, &mismatch) == AUD_ERROR_FORMAT);
  AUD_CHECK(aud_graph_node_descriptor(f.graph, osc)->num_params == 3);
  AUD_CHECK(aud_graph_node_descriptor(f.graph, 42) == nullptr);
  AUD_CHECK(aud_graph_node_channels(f.graph, AUD_GRAPH_NODE, 0, 0) == 1);
  int32_t handles[4];
  AUD_CHECK(aud_graph_nodes(f.graph, handles, 4) == 2);
  AUD_CHECK(handles[0] == 1 && handles[1] == 2);
}

AUD_TEST(renders_a_sine_after_the_first_commit) {
  Fixture f;
  const int32_t osc = f.node(AUD_GRAPH_OSCILLATOR_TYPE_ID);
  AUD_CHECK(f.commit([&] { AUD_CHECK(f.connect(osc, AUD_GRAPH_NODE) == AUD_OK); }) == 1);
  // Not running: silence, no error.
  AUD_CHECK(f.render(256) == AUD_OK);
  AUD_CHECK(f.out[0][10] == 0);
  AUD_CHECK(aud_graph_start(f.graph) == AUD_OK);
  const std::vector<float> second = f.renderFrames(48000);
  AUD_CHECK(risingCrossings(second) >= 439 && risingCrossings(second) <= 441);
  float peak = 0;
  for (float v : second) peak = std::max(peak, std::fabs(v));
  AUD_CHECK_NEAR(peak, 0.5, 0.01);
  AUD_CHECK(aud_graph_revision(f.graph) == 1);
  const auto notifications = f.take();
  bool revision = false;
  bool state = false;
  for (const AudGraphNotification& n : notifications) {
    if (n.type == AUD_NOTIFY_REVISION && n.revision == 1) revision = true;
    if (n.type == AUD_NOTIFY_STATE && n.code == AUD_GRAPH_RUNNING) state = true;
  }
  AUD_CHECK(revision && state);
  // The block before the start was not rendered, only cleared.
  const AudGraphStats s = f.stats();
  AUD_CHECK(s.blocks_rendered == 48000 / 256 + 1);
  AUD_CHECK(s.frames_rendered == 48000);
  AUD_CHECK_NEAR(s.output_peak, 0.5, 0.01);
  // Too long a block is refused with silence.
  f.out[0][0] = 1;
  AUD_CHECK(f.render(257) == AUD_ERROR_INVALID_ARGUMENT);
  AUD_CHECK(f.out[0][0] == 0);
}

AUD_TEST(validates_transactions_and_rejects_cycles) {
  Fixture f;
  const int32_t a = f.node(AUD_GRAPH_FILTER_TYPE_ID);
  const int32_t b = f.node(AUD_GRAPH_FILTER_TYPE_ID);
  AUD_CHECK(f.connect(a, b) == AUD_ERROR_STATE);
  AUD_CHECK(aud_graph_commit(f.graph) == AUD_ERROR_STATE);
  AUD_CHECK(aud_graph_begin(f.graph) == AUD_OK);
  AUD_CHECK(aud_graph_begin(f.graph) == AUD_ERROR_STATE);
  AUD_CHECK(f.connect(a, 77) == AUD_ERROR_NOT_FOUND);
  AUD_CHECK(f.connect(a, b, 3, 0) == AUD_ERROR_INVALID_ARGUMENT);
  AUD_CHECK(f.connect(a, b) == AUD_OK);
  AUD_CHECK(f.connect(a, b) == AUD_OK);  // twice is once
  AUD_CHECK(f.connect(b, a) == AUD_OK);
  AUD_CHECK(aud_graph_commit(f.graph) == AUD_ERROR_CYCLE);
  AUD_CHECK(aud_graph_disconnect(f.graph, b, 0, a, 0) == AUD_OK);
  AUD_CHECK(aud_graph_disconnect(f.graph, b, 0, a, 0) == AUD_ERROR_NOT_FOUND);
  AUD_CHECK(f.connect(b, AUD_GRAPH_NODE) == AUD_OK);
  AUD_CHECK(aud_graph_commit(f.graph) == 1);
  AUD_CHECK(aud_graph_output_latency(f.graph) == 0);
  // A node removed before it ever rendered is freed at once.
  const int32_t c = f.node(AUD_GRAPH_OSCILLATOR_TYPE_ID);
  AUD_CHECK(f.commit([&] { AUD_CHECK(aud_graph_remove_node(f.graph, c) == AUD_OK); }) == 2);
  AUD_CHECK(aud_graph_nodes(f.graph, nullptr, 0) == 2);
  AUD_CHECK(aud_graph_node_descriptor(f.graph, c) == nullptr);
  AUD_CHECK(aud_graph_begin(f.graph) == AUD_OK);
  AUD_CHECK(aud_graph_remove_node(f.graph, AUD_GRAPH_NODE) ==
            AUD_ERROR_INVALID_ARGUMENT);
  AUD_CHECK(aud_graph_rollback(f.graph) == AUD_OK);
  AUD_CHECK(aud_graph_rollback(f.graph) == AUD_ERROR_STATE);
  // A feedback node may feed itself.
  const int32_t fb = f.node(AUD_GRAPH_FEEDBACK_TYPE_ID);
  AUD_CHECK(f.commit([&] { AUD_CHECK(f.connect(fb, fb) == AUD_OK); }) == 3);
}

AUD_TEST(fades_connections_and_retires_nodes) {
  Fixture f;
  const int32_t osc = steadyOscillator(f);
  AUD_CHECK(f.commit([&] { f.connect(osc, AUD_GRAPH_NODE); }) == 1);
  aud_graph_start(f.graph);
  f.render(256);
  AUD_CHECK_NEAR(f.out[0][0], 0.5, 1e-6);  // the first program starts at full gain
  AUD_CHECK_NEAR(f.out[0][255], 0.5, 1e-6);
  // Disconnecting fades out over 240 frames.
  AUD_CHECK(f.commit([&] { aud_graph_disconnect(f.graph, osc, 0, AUD_GRAPH_NODE, 0); }) == 2);
  f.render(256);
  AUD_CHECK_NEAR(f.out[0][0], 0.5, 1e-6);
  AUD_CHECK_NEAR(f.out[0][120], 0.25, 1e-3);
  AUD_CHECK_NEAR(f.out[0][239], 0.5 / kFade, 1e-3);
  AUD_CHECK(f.out[0][240] == 0 && f.out[0][255] == 0);
  f.render(256);
  AUD_CHECK(f.out[0][0] == 0);
  // Connecting again fades in from silence.
  AUD_CHECK(f.commit([&] { f.connect(osc, AUD_GRAPH_NODE); }) == 3);
  f.render(256);
  AUD_CHECK_NEAR(f.out[0][0], 0, 1e-6);
  AUD_CHECK_NEAR(f.out[0][120], 0.25, 1e-3);
  AUD_CHECK_NEAR(f.out[0][255], 0.5, 1e-6);
  // A second oscillator joins through a fade.
  const int32_t second = steadyOscillator(f, 0.25f);
  AUD_CHECK(f.commit([&] { f.connect(second, AUD_GRAPH_NODE); }) == 4);
  f.render(256);
  AUD_CHECK_NEAR(f.out[0][0], 0.5, 1e-6);
  AUD_CHECK_NEAR(f.out[0][255], 0.75, 1e-6);
  // Removing a node: the fade, then AUD_NOTIFY_NODE_DONE and the free.
  AUD_CHECK(f.commit([&] { aud_graph_remove_node(f.graph, second); }) == 5);
  AUD_CHECK(aud_graph_send_event(f.graph, second, nullptr, nullptr, 0) ==
            AUD_ERROR_INVALID_ARGUMENT);
  const AudEvent on = noteOn(60);
  AUD_CHECK(aud_graph_send_event(f.graph, second, &on, nullptr, 0) ==
            AUD_ERROR_RETIRED);
  AUD_CHECK(aud_graph_set_param(f.graph, second, 0, 1, 0) == AUD_ERROR_RETIRED);
  f.render(256);
  AUD_CHECK_NEAR(f.out[0][0], 0.75, 1e-6);
  AUD_CHECK_NEAR(f.out[0][255], 0.5, 1e-6);
  AUD_CHECK(aud_graph_nodes(f.graph, nullptr, 0) == 1);
  bool done = false;
  for (const AudGraphNotification& n : f.take()) {
    if (n.type == AUD_NOTIFY_NODE_DONE && n.node == second) done = true;
  }
  AUD_CHECK(done);
  AUD_CHECK(aud_graph_node_descriptor(f.graph, second) == nullptr ||
            aud_graph_revision(f.graph) == 5);
  // The collection compiled a program without the node.
  f.render(256);
  AUD_CHECK(aud_graph_revision(f.graph) == 6);
  f.take();
  AUD_CHECK(aud_graph_node_descriptor(f.graph, second) == nullptr);
  AUD_CHECK_NEAR(f.out[0][100], 0.5, 1e-6);
}

AUD_TEST(applies_parameters_at_once_or_through_the_queue) {
  Fixture f(0, 1, 256, [](AudGraphConfig& c) { c.param_queue_capacity = 4; });
  const int32_t osc = steadyOscillator(f, 0.0f);
  AUD_CHECK(aud_graph_set_param(f.graph, osc, 9, 1, 0) == AUD_ERROR_INVALID_ARGUMENT);
  AUD_CHECK(aud_graph_set_param(f.graph, 42, 0, 1, 0) == AUD_ERROR_NOT_FOUND);
  f.commit([&] { f.connect(osc, AUD_GRAPH_NODE); });
  aud_graph_start(f.graph);
  f.render(256);
  AUD_CHECK(f.out[0][100] == 0);
  AUD_CHECK(aud_graph_set_param(f.graph, osc, AUD_OSCILLATOR_PARAM_AMPLITUDE, 0.5f, 0) == AUD_OK);
  f.render(256);
  AUD_CHECK_NEAR(f.out[0][0], 0.5, 1e-6);
  // A ramp over 128 frames travels as a parameter event.
  AUD_CHECK(aud_graph_set_param(f.graph, osc, AUD_OSCILLATOR_PARAM_AMPLITUDE, 0.0f, 128) == AUD_OK);
  f.render(256);
  AUD_CHECK_NEAR(f.out[0][0], 0.5, 1e-6);
  AUD_CHECK_NEAR(f.out[0][64], 0.25, 1e-2);
  AUD_CHECK(f.out[0][200] == 0);
  // The queue holds four changes; the fifth is refused.
  for (int i = 0; i < 4; ++i) {
    AUD_CHECK(aud_graph_set_param(f.graph, osc, AUD_OSCILLATOR_PARAM_AMPLITUDE, 0.1f * i, 0) == AUD_OK);
  }
  AUD_CHECK(aud_graph_set_param(f.graph, osc, AUD_OSCILLATOR_PARAM_AMPLITUDE, 1, 0) == AUD_ERROR_QUEUE_FULL);
  f.render(256);
  AUD_CHECK_NEAR(f.out[0][0], 0.3, 1e-6);  // the latest value wins
  AUD_CHECK(f.stats().rejected == 1);
  AUD_CHECK(f.stats().params_applied == 6);
}

AUD_TEST(delivers_events_now_later_late_or_not_at_all) {
  Fixture f;
  const int32_t recorder = f.node("aud.test.recorder");
  const AudEvent on = noteOn(60);
  AUD_CHECK(aud_graph_send_event(f.graph, recorder, &on, nullptr, 0) == AUD_ERROR_STATE);
  f.commit([&] { f.connect(recorder, AUD_GRAPH_NODE); });
  aud_graph_start(f.graph);
  AudEvent badPort = on;
  badPort.port = 3;
  AUD_CHECK(aud_graph_send_event(f.graph, recorder, &badPort, nullptr, 0) == AUD_ERROR_INVALID_ARGUMENT);
  AUD_CHECK(aud_graph_send_event(f.graph, 42, &on, nullptr, 0) == AUD_ERROR_NOT_FOUND);
  // Immediately: the next block at offset 0.
  AUD_CHECK(aud_graph_send_event(f.graph, recorder, &on, nullptr, 0) == AUD_OK);
  // At a sample position inside the fourth block, with an id.
  const AudTimestamp later = atSample(1000);
  AUD_CHECK(aud_graph_send_event(f.graph, recorder, &on, &later, 7) == AUD_OK);
  // Beyond the lookahead.
  const AudTimestamp far = atSample(48000 * 11);
  AUD_CHECK(aud_graph_send_event(f.graph, recorder, &on, &far, 0) == AUD_ERROR_LOOKAHEAD);
  // Cancelled before it fires.
  const AudTimestamp cancelled = atSample(2000);
  AUD_CHECK(aud_graph_send_event(f.graph, recorder, &on, &cancelled, 9) == AUD_OK);
  AUD_CHECK(aud_graph_cancel(f.graph, recorder, 9) == AUD_OK);
  for (int i = 0; i < 10; ++i) f.render(256);
  AUD_CHECK(g_records.size() == 2);
  AUD_CHECK(g_records[0].position == 0 && g_records[0].offset == 0);
  AUD_CHECK(g_records[1].position == 768 && g_records[1].offset == 232);
  AUD_CHECK(f.stats().scheduled == 0);
  AUD_CHECK(f.stats().events_delivered == 2);
  // A late event plays at the block start with a diagnostic.
  g_records.clear();
  const AudTimestamp past = atSample(10);
  AUD_CHECK(aud_graph_send_event(f.graph, recorder, &on, &past, 0) == AUD_OK);
  f.render(256);
  AUD_CHECK(g_records.size() == 1 && g_records[0].offset == 0);
  AUD_CHECK(f.stats().events_late == 1);
  AUD_CHECK(f.countNotifications(AUD_NOTIFY_DIAGNOSTIC, AUD_ERROR_LATE) == 1);
  // Cancelling everything of a node, and of the graph.
  AUD_CHECK(aud_graph_send_event(f.graph, recorder, &on, &far, 0) == AUD_ERROR_LOOKAHEAD);
  const AudTimestamp soon = atSample(aud_graph_sample_position(f.graph) + 3000);
  aud_graph_send_event(f.graph, recorder, &on, &soon, 1);
  aud_graph_send_event(f.graph, recorder, &on, &soon, 2);
  AUD_CHECK(aud_graph_cancel(f.graph, recorder, 0) == AUD_OK);
  aud_graph_send_event(f.graph, recorder, &on, &soon, 3);
  AUD_CHECK(aud_graph_cancel(f.graph, AUD_GRAPH_NODE, 0) == AUD_OK);
  AUD_CHECK(aud_graph_cancel(f.graph, 42, 0) == AUD_ERROR_NOT_FOUND);
  g_records.clear();
  for (int i = 0; i < 20; ++i) f.render(256);
  AUD_CHECK(g_records.empty());
}

AUD_TEST(drops_late_events_when_configured) {
  Fixture f(0, 1, 256, [](AudGraphConfig& c) { c.flags = AUD_GRAPH_DROP_LATE_EVENTS; });
  const int32_t recorder = f.node("aud.test.recorder");
  f.commit([&] { f.connect(recorder, AUD_GRAPH_NODE); });
  aud_graph_start(f.graph);
  f.render(256);
  const AudEvent on = noteOn(60);
  const AudTimestamp past = atSample(10);
  aud_graph_send_event(f.graph, recorder, &on, &past, 0);
  f.render(256);
  AUD_CHECK(g_records.empty());
  AUD_CHECK(f.stats().events_dropped == 1);
}

AUD_TEST(closes_running_notes_of_a_removed_node) {
  Fixture f;
  const int32_t recorder = f.node("aud.test.recorder");
  f.commit([&] { f.connect(recorder, AUD_GRAPH_NODE); });
  aud_graph_start(f.graph);
  const AudEvent on = noteOn(64);
  aud_graph_send_event(f.graph, recorder, &on, nullptr, 0);
  f.render(256);
  AUD_CHECK(g_records.size() == 1);
  f.commit([&] { aud_graph_remove_node(f.graph, recorder); });
  f.render(256);
  AUD_CHECK(g_records.size() == 2);
  AUD_CHECK(aud_ump_is_note_off(g_records[1].word0, 0) == 1);
  AUD_CHECK(aud_ump_note(g_records[1].word0) == 64);
}

AUD_TEST(plays_notes_on_the_oscillator) {
  Fixture f;
  const int32_t osc = f.node(AUD_GRAPH_OSCILLATOR_TYPE_ID);
  f.commit([&] { f.connect(osc, AUD_GRAPH_NODE); });
  aud_graph_start(f.graph);
  const AudEvent on = noteOn(81);  // 880 Hz
  aud_graph_send_event(f.graph, osc, &on, nullptr, 0);
  f.renderFrames(256);
  const std::vector<float> second = f.renderFrames(48000);
  AUD_CHECK(risingCrossings(second) >= 879 && risingCrossings(second) <= 881);
  const AudEvent off = noteOff(81);
  aud_graph_send_event(f.graph, osc, &off, nullptr, 0);
  f.renderFrames(256);
  const std::vector<float> silent = f.renderFrames(2048);
  AUD_CHECK(silent[2000] == 0);
}

AUD_TEST(feeds_back_through_a_delay_in_samples) {
  Fixture f(1, 1);
  const uint32_t one = 1;
  AudNodeConfig config{sizeof(AudNodeConfig), 1, &one, 1, &one, 512, 0};
  const int32_t fb = f.node(AUD_GRAPH_FEEDBACK_TYPE_ID, &config);
  AUD_CHECK(aud_graph_node_latency(f.graph, fb) == 0);
  AudNodeConfig mixerConfig{sizeof(AudNodeConfig), 0, nullptr, 0, nullptr, 0, 0};
  const uint32_t ones[AUD_MIXER_NUM_INPUTS] = {1, 1, 1, 1, 1, 1, 1, 1};
  mixerConfig.num_input_buses = AUD_MIXER_NUM_INPUTS;
  mixerConfig.input_channels = ones;
  mixerConfig.num_output_buses = 1;
  mixerConfig.output_channels = &one;
  const int32_t mixer = f.node(AUD_GRAPH_MIXER_TYPE_ID, &mixerConfig);
  aud_graph_set_param(f.graph, mixer, 0, 0.5f, 0);
  AUD_CHECK(f.commit([&] {
    AUD_CHECK(f.connect(AUD_GRAPH_NODE, mixer, 0, 1) == AUD_OK);
    AUD_CHECK(f.connect(fb, mixer, 0, 0) == AUD_OK);
    AUD_CHECK(f.connect(mixer, fb) == AUD_OK);
    AUD_CHECK(f.connect(fb, AUD_GRAPH_NODE) == AUD_OK);
  }) == 1);
  aud_graph_start(f.graph);
  std::vector<float> output;
  for (int block = 0; block < 8; ++block) {
    std::fill(f.in[0].begin(), f.in[0].end(), 0.0f);
    if (block == 0) f.in[0][0] = 1;
    f.render(256);
    output.insert(output.end(), f.out[0].begin(), f.out[0].begin() + 256);
  }
  AUD_CHECK(output[0] == 0);
  AUD_CHECK_NEAR(output[512], 1.0, 1e-6);
  AUD_CHECK_NEAR(output[1024], 0.5, 1e-6);
  AUD_CHECK_NEAR(output[1536], 0.25, 1e-6);
  AUD_CHECK(output[513] == 0 && output[1023] == 0);
}

AUD_TEST(aligns_the_latency_of_parallel_paths) {
  Fixture f;
  const int32_t osc = steadyOscillator(f, 0.5f);
  const int32_t delay = f.node("aud.test.delay");
  AUD_CHECK(aud_graph_node_latency(f.graph, delay) == kTestLatency);
  f.commit([&] {
    f.connect(osc, AUD_GRAPH_NODE);
    f.connect(osc, delay);
    f.connect(delay, AUD_GRAPH_NODE);
  });
  AUD_CHECK(aud_graph_output_latency(f.graph) == kTestLatency);
  AUD_CHECK(aud_graph_node_lead(f.graph, osc) == kTestLatency);
  AUD_CHECK(aud_graph_node_lead(f.graph, delay) == kTestLatency);
  aud_graph_start(f.graph);
  f.render(256);
  // Both paths arrive 100 frames late and add up.
  AUD_CHECK(f.out[0][99] == 0);
  AUD_CHECK_NEAR(f.out[0][100], 1.0, 1e-6);
  AUD_CHECK_NEAR(f.out[0][255], 1.0, 1e-6);
  // The direct path may opt out of the alignment.
  f.commit([&] { f.connect(osc, AUD_GRAPH_NODE, 0, 0, AUD_CONNECTION_LOW_LATENCY); });
  f.render(256);
  f.render(256);
  AUD_CHECK_NEAR(f.out[0][50], 1.0, 1e-6);
  AUD_CHECK(aud_graph_node_lead(f.graph, osc) == kTestLatency);
}

AUD_TEST(reblocks_nodes_that_need_constant_frames) {
  Fixture f(1, 1, 256);
  const int32_t fixed = f.node("aud.test.fixed");
  AUD_CHECK(aud_graph_node_latency(f.graph, fixed) == 256);
  f.commit([&] {
    f.connect(AUD_GRAPH_NODE, fixed);
    f.connect(fixed, AUD_GRAPH_NODE);
  });
  aud_graph_start(f.graph);
  std::vector<float> output;
  for (int block = 0; block < 6; ++block) {
    for (uint32_t i = 0; i < 100; ++i) f.in[0][i] = static_cast<float>(block * 100 + i);
    f.render(100);
    output.insert(output.end(), f.out[0].begin(), f.out[0].begin() + 100);
  }
  for (uint32_t frames : g_fixedFrames) AUD_CHECK(frames == 256);
  AUD_CHECK(g_fixedFrames.size() == 2);
  AUD_CHECK(output[255] == 0);
  AUD_CHECK(output[256] == 0);
  AUD_CHECK(output[300] == 44);  // input 44 arrives 256 frames later
}

AUD_TEST(runs_the_internal_transport) {
  Fixture f;
  const int32_t recorder = f.node("aud.test.recorder");
  f.commit([&] { f.connect(recorder, AUD_GRAPH_NODE); });
  aud_graph_start(f.graph);
  AUD_CHECK(f.transport().playing == 0);
  AUD_CHECK(f.request(AUD_TRANSPORT_REQUEST_START) == AUD_OK);
  AUD_CHECK(f.request(AUD_TRANSPORT_REQUEST_SET_TIME_SIGNATURE) == AUD_OK);
  AUD_CHECK(f.request(99) == AUD_ERROR_INVALID_ARGUMENT);
  const AudEvent on = noteOn(60);
  const AudTimestamp beatOne = atBeat(1.0);
  AUD_CHECK(aud_graph_send_event(f.graph, recorder, &on, &beatOne, 0) == AUD_OK);
  f.renderFrames(48000);
  const AudGraphTransportState t = f.transport();
  AUD_CHECK(t.playing == 1);
  AUD_CHECK_NEAR(static_cast<double>(t.beat) / AUD_BEAT_FACTOR, 2.0, 1e-6);
  AUD_CHECK(t.numerator == 3 && t.denominator == 4);
  AUD_CHECK(g_records.size() == 1);
  AUD_CHECK(g_records[0].position + g_records[0].offset == 24000);
  AUD_CHECK(f.countNotifications(AUD_NOTIFY_TRANSPORT) == 2);
  // A tempo change and a stop; the recorder resets on the stop.
  AUD_CHECK(f.request(AUD_TRANSPORT_REQUEST_SET_TEMPO, 60) == AUD_OK);
  f.renderFrames(48000);
  AUD_CHECK_NEAR(static_cast<double>(f.transport().beat) / AUD_BEAT_FACTOR, 3.0, 1e-6);
  AUD_CHECK(f.request(AUD_TRANSPORT_REQUEST_STOP) == AUD_OK);
  f.renderFrames(512);
  AUD_CHECK(f.transport().playing == 0);
  AUD_CHECK(g_resets.size() == 1 && g_resets[0] == AUD_RESET_STOP);
  // A seek while stopped moves the position and resets on seek.
  AUD_CHECK(f.request(AUD_TRANSPORT_REQUEST_SEEK, 0, 5 * AUD_BEAT_FACTOR) == AUD_OK);
  f.renderFrames(256);
  AUD_CHECK_NEAR(static_cast<double>(f.transport().beat) / AUD_BEAT_FACTOR, 5.0, 1e-9);
  AUD_CHECK(g_resets.size() == 2 && g_resets[1] == AUD_RESET_SEEK);
  // The quantum is Link's and refused.
  AUD_CHECK(f.request(AUD_TRANSPORT_REQUEST_SET_QUANTUM, 4) == AUD_OK);
  f.renderFrames(256);
  AUD_CHECK(f.countNotifications(AUD_NOTIFY_DIAGNOSTIC, AUD_ERROR_UNSUPPORTED) == 1);
  // A start at a sample time splits the block.
  const AudTimestamp startAt = atSample(aud_graph_sample_position(f.graph) + 100);
  AUD_CHECK(f.request(AUD_TRANSPORT_REQUEST_START, 0, 0, 0, &startAt) == AUD_OK);
  f.request(AUD_TRANSPORT_REQUEST_SET_TEMPO, 120);
  f.renderFrames(256);
  AUD_CHECK_NEAR(static_cast<double>(f.transport().beat) / AUD_BEAT_FACTOR,
                 5.0 + 156.0 / 24000.0, 1e-6);
}

AUD_TEST(loops_the_internal_transport) {
  Fixture f;
  const int32_t recorder = f.node("aud.test.recorder");
  f.commit([&] { f.connect(recorder, AUD_GRAPH_NODE); });
  aud_graph_start(f.graph);
  AUD_CHECK(f.request(AUD_TRANSPORT_REQUEST_SET_LOOP, 0, 0, AUD_BEAT_FACTOR) == AUD_OK);
  AUD_CHECK(f.request(AUD_TRANSPORT_REQUEST_START) == AUD_OK);
  f.renderFrames(30000);
  const AudGraphTransportState t = f.transport();
  AUD_CHECK(t.looping == 1);
  AUD_CHECK_NEAR(static_cast<double>(t.beat) / AUD_BEAT_FACTOR, 0.25, 1e-4);
  AUD_CHECK(t.loop_end == AUD_BEAT_FACTOR);
  AUD_CHECK(g_resets.empty());  // a loop wrap is no seek
}

AUD_TEST(taps_the_signal_for_the_control_thread) {
  Fixture f;
  const int32_t osc = f.node(AUD_GRAPH_OSCILLATOR_TYPE_ID);
  const uint32_t one = 1;
  AudNodeConfig config{sizeof(AudNodeConfig), 1, &one, 1, &one, 0, 0};
  const int32_t tap = f.node(AUD_GRAPH_TAP_TYPE_ID, &config);
  f.commit([&] {
    f.connect(osc, tap);
    f.connect(tap, AUD_GRAPH_NODE);
  });
  aud_graph_start(f.graph);
  f.render(256);
  f.render(256);
  float recent[64];
  AUD_CHECK(aud_graph_tap_read(f.graph, tap, 0, recent, 64) == AUD_OK);
  for (int i = 0; i < 64; ++i) AUD_CHECK(recent[i] == f.out[0][192 + i]);
  float peak = 0;
  float rms = 0;
  AUD_CHECK(aud_graph_tap_meter(f.graph, tap, 0, &peak, &rms) == AUD_OK);
  AUD_CHECK_NEAR(peak, 0.5, 0.01);
  AUD_CHECK_NEAR(rms, 0.5 / std::sqrt(2.0), 0.02);
  AUD_CHECK(aud_graph_tap_read(f.graph, osc, 0, recent, 64) == AUD_ERROR_INVALID_ARGUMENT);
  AUD_CHECK(aud_graph_tap_read(f.graph, tap, 5, recent, 64) == AUD_ERROR_INVALID_ARGUMENT);
  AUD_CHECK(aud_graph_tap_meter(f.graph, 42, 0, &peak, &rms) == AUD_ERROR_NOT_FOUND);
}

AUD_TEST(renders_offline_independent_of_the_block_size) {
  std::vector<float> fixed(4800);
  std::vector<float> varying(4800);
  for (int variant = 0; variant < 2; ++variant) {
    Fixture f;
    const int32_t osc = f.node(AUD_GRAPH_OSCILLATOR_TYPE_ID);
    const int32_t filter = f.node(AUD_GRAPH_FILTER_TYPE_ID);
    f.commit([&] {
      f.connect(osc, filter);
      f.connect(filter, AUD_GRAPH_NODE);
    });
    const AudEvent on = noteOn(60);
    const AudTimestamp at = atSample(1000);
    AudOfflineRequest request{};
    request.struct_size = sizeof(AudOfflineRequest);
    request.frames = 4800;
    float* channel = variant == 0 ? fixed.data() : varying.data();
    AudAudioBus output{sizeof(AudAudioBus), 1, &channel};
    request.outputs = &output;
    const uint32_t sizes[] = {100, 37, 256, 1};
    if (variant == 1) {
      request.num_block_sizes = 4;
      request.block_sizes = sizes;
    }
    AUD_CHECK(aud_graph_render_offline(f.graph, &request) == AUD_ERROR_STATE);
    aud_graph_start(f.graph);
    AUD_CHECK(aud_graph_send_event(f.graph, osc, &on, &at, 0) == AUD_OK);
    AUD_CHECK(aud_graph_render_offline(f.graph, &request) == AUD_OK);
    AUD_CHECK(f.stats().overloads == 0);
  }
  for (size_t i = 0; i < 4800; ++i) {
    AUD_CHECK_NEAR(fixed[i], varying[i], 1e-5);
  }
  AUD_CHECK(fixed[2000] != 0);
}

AUD_TEST(walks_the_lifecycle) {
  Fixture f;
  const int32_t osc = steadyOscillator(f);
  f.commit([&] { f.connect(osc, AUD_GRAPH_NODE); });
  AUD_CHECK(aud_graph_suspend(f.graph) == AUD_ERROR_STATE);
  AUD_CHECK(aud_graph_resume(f.graph) == AUD_ERROR_STATE);
  AUD_CHECK(aud_graph_stop(f.graph) == AUD_ERROR_STATE);
  AUD_CHECK(aud_graph_prepare(f.graph, 44100, 128) == AUD_OK);
  AUD_CHECK(aud_graph_state(f.graph) == AUD_GRAPH_PREPARED);
  AUD_CHECK(aud_graph_max_frames(f.graph) == 128);
  AUD_CHECK(aud_graph_sample_rate(f.graph) == 44100);
  AUD_CHECK(aud_graph_start(f.graph) == AUD_OK);
  AUD_CHECK(aud_graph_start(f.graph) == AUD_ERROR_STATE);
  AUD_CHECK(aud_graph_prepare(f.graph, 0, 0) == AUD_ERROR_STATE);
  f.render(128);
  AUD_CHECK_NEAR(f.out[0][0], 0.5, 1e-6);
  AUD_CHECK(aud_graph_suspend(f.graph) == AUD_OK);
  AUD_CHECK(aud_graph_state(f.graph) == AUD_GRAPH_SUSPENDED);
  f.render(128);
  AUD_CHECK(f.out[0][0] == 0);
  AUD_CHECK(aud_graph_prepare(f.graph, 48000, 256) == AUD_OK);
  AUD_CHECK(aud_graph_state(f.graph) == AUD_GRAPH_SUSPENDED);
  AUD_CHECK(aud_graph_resume(f.graph) == AUD_OK);
  f.render(256);
  AUD_CHECK_NEAR(f.out[0][0], 0.5, 1e-6);
  AUD_CHECK(aud_graph_stop(f.graph) == AUD_OK);
  AUD_CHECK(aud_graph_state(f.graph) == AUD_GRAPH_STOPPED);
  AUD_CHECK(aud_graph_start(f.graph) == AUD_OK);
  f.render(256);
  AUD_CHECK_NEAR(f.out[0][0], 0.5, 1e-6);
  AUD_CHECK(f.countNotifications(AUD_NOTIFY_STATE, AUD_GRAPH_RUNNING) == 3);
}

AUD_TEST(routes_emitted_events_to_nodes_and_to_the_control_thread) {
  Fixture f;
  const int32_t emitter = f.node("aud.test.emitter");
  const int32_t recorder = f.node("aud.test.recorder");
  f.commit([&] {
    AUD_CHECK(aud_graph_connect_events(f.graph, emitter, 0, recorder, 0) == AUD_OK);
    AUD_CHECK(aud_graph_connect_events(f.graph, emitter, 0, recorder, 0) == AUD_OK);
    AUD_CHECK(aud_graph_connect_events(f.graph, emitter, 0, AUD_GRAPH_NODE, 0) == AUD_OK);
    AUD_CHECK(aud_graph_connect_events(f.graph, AUD_GRAPH_NODE, 0, emitter, 0) == AUD_OK);
    AUD_CHECK(aud_graph_connect_events(f.graph, emitter, 5, recorder, 0) == AUD_ERROR_INVALID_ARGUMENT);
    AUD_CHECK(aud_graph_connect_events(f.graph, 42, 0, recorder, 0) == AUD_ERROR_NOT_FOUND);
    f.connect(recorder, AUD_GRAPH_NODE);
  });
  aud_graph_start(f.graph);
  // An event into the graph's event output reaches the emitter, which
  // echoes it to the recorder and to the control thread.
  AudEvent on = noteOn(62);
  AUD_CHECK(aud_graph_send_event(f.graph, AUD_GRAPH_NODE, &on, nullptr, 0) == AUD_OK);
  f.render(256);
  AUD_CHECK(g_records.size() == 1 && aud_ump_note(g_records[0].word0) == 62);
  bool echoed = false;
  for (const AudGraphNotification& n : f.take()) {
    if (n.type == AUD_NOTIFY_EVENT && n.node == emitter &&
        aud_ump_note(n.event.words[0]) == 62) {
      echoed = true;
    }
  }
  AUD_CHECK(echoed);
  // Events a host hands to the render call take the same way.
  on.sample_offset = 10;
  f.render(256, nullptr, &on, 1);
  AUD_CHECK(g_records.size() == 2 && g_records[1].offset == 10);
  f.commit([&] {
    AUD_CHECK(aud_graph_disconnect_events(f.graph, emitter, 0, recorder, 0) == AUD_OK);
    AUD_CHECK(aud_graph_disconnect_events(f.graph, emitter, 0, recorder, 0) == AUD_ERROR_NOT_FOUND);
  });
  f.render(256);
  aud_graph_send_event(f.graph, AUD_GRAPH_NODE, &on, nullptr, 0);
  f.render(256);
  AUD_CHECK(g_records.size() == 2);
}

AUD_TEST(filters_the_time_of_the_stream) {
  Fixture f;
  const int32_t osc = f.node(AUD_GRAPH_OSCILLATOR_TYPE_ID);
  f.commit([&] { f.connect(osc, AUD_GRAPH_NODE); });
  aud_graph_start(f.graph);
  AudStreamTime time{};
  time.struct_size = sizeof(AudStreamTime);
  time.frames = 256;
  time.sample_rate = kRate;
  time.host_time_source = AUD_TIME_SOURCE_HARDWARE;
  for (int block = 0; block < 20; ++block) {
    time.sample_position = block * 256;
    time.host_time_ns = 1000000000LL + block * 256 * 20833 + (block % 2) * 5000;
    AUD_CHECK(f.render(256, &time) == AUD_OK);
  }
  AUD_CHECK(f.stats().time_filter_resets == 0);
  time.sample_position = 100000;  // a discontinuity
  f.render(256, &time);
  AUD_CHECK(f.stats().time_filter_resets == 1);
  AUD_CHECK(f.countNotifications(AUD_NOTIFY_TIME_RESET) == 1);
  AUD_CHECK(aud_graph_sample_position(f.graph) == 100256);
  // A host time scheduled 1 ms ahead of the filtered time of the next block.
  const int32_t recorder = f.node("aud.test.recorder");
  f.commit([&] { f.connect(recorder, AUD_GRAPH_NODE); });
  time.sample_position = 100256;
  time.host_time_ns += 256 * 20833;
  f.render(256, &time);
  AudTimestamp hostAt{};
  hostAt.struct_size = sizeof(AudTimestamp);
  hostAt.domain = AUD_TIME_HOST;
  hostAt.value = time.host_time_ns + 256 * 20833 + 1000000;
  const AudEvent on = noteOn(60);
  AUD_CHECK(aud_graph_send_event(f.graph, recorder, &on, &hostAt, 0) == AUD_OK);
  for (int block = 0; block < 3; ++block) {
    time.sample_position += 256;
    time.host_time_ns += 256 * 20833;
    f.render(256, &time);
  }
  AUD_CHECK(g_records.size() == 1);
  AUD_CHECK(g_records[0].offset >= 46 && g_records[0].offset <= 50);
}

AUD_TEST(filters_and_mixes) {
  Fixture f;
  const int32_t osc = f.node(AUD_GRAPH_OSCILLATOR_TYPE_ID);
  aud_graph_set_param(f.graph, osc, AUD_OSCILLATOR_PARAM_FREQUENCY, 10000, 0);
  const uint32_t one = 1;
  AudNodeConfig mono{sizeof(AudNodeConfig), 1, &one, 1, &one, 0, 0};
  const int32_t filter = f.node(AUD_GRAPH_FILTER_TYPE_ID, &mono);
  aud_graph_set_param(f.graph, filter, AUD_FILTER_PARAM_CUTOFF, 100, 0);
  f.commit([&] {
    f.connect(osc, filter);
    f.connect(filter, AUD_GRAPH_NODE);
  });
  aud_graph_start(f.graph);
  f.renderFrames(4800);
  const std::vector<float> filtered = f.renderFrames(4800);
  float peak = 0;
  for (float v : filtered) peak = std::max(peak, std::fabs(v));
  AUD_CHECK(peak < 0.002);
  // High pass lets it through.
  aud_graph_set_param(f.graph, filter, AUD_FILTER_PARAM_MODE, 1, 0);
  f.renderFrames(4800);
  const std::vector<float> high = f.renderFrames(4800);
  peak = 0;
  for (float v : high) peak = std::max(peak, std::fabs(v));
  AUD_CHECK(peak > 0.45);
  // The mixer scales its inputs.
  Fixture m;
  const int32_t a = steadyOscillator(m, 0.5f);
  const int32_t b = steadyOscillator(m, 0.5f);
  const uint32_t ones[AUD_MIXER_NUM_INPUTS] = {1, 1, 1, 1, 1, 1, 1, 1};
  AudNodeConfig mixerConfig{sizeof(AudNodeConfig), AUD_MIXER_NUM_INPUTS, ones, 1, &one, 0, 0};
  const int32_t mixer = m.node(AUD_GRAPH_MIXER_TYPE_ID, &mixerConfig);
  AUD_CHECK(aud_graph_set_param(m.graph, mixer, 1, 2.0f, 0) == AUD_OK);
  AUD_CHECK(aud_graph_set_param(m.graph, mixer, AUD_MIXER_PARAM_MASTER, 0.5f, 0) == AUD_OK);
  m.commit([&] {
    m.connect(a, mixer, 0, 0);
    m.connect(b, mixer, 0, 1);
    m.connect(mixer, AUD_GRAPH_NODE);
  });
  aud_graph_start(m.graph);
  m.render(256);
  AUD_CHECK_NEAR(m.out[0][100], 0.5 * (0.5 + 1.0), 1e-6);
}

AUD_TEST(scales_each_mixer_input) {
  for (uint32_t bus = 0; bus < 3; ++bus) {
    Fixture m;
    const int32_t a = steadyOscillator(m, 0.5f);
    const uint32_t one = 1;
    const uint32_t ones[AUD_MIXER_NUM_INPUTS] = {1, 1, 1, 1, 1, 1, 1, 1};
    AudNodeConfig mixerConfig{sizeof(AudNodeConfig), AUD_MIXER_NUM_INPUTS, ones, 1, &one, 0, 0};
    const int32_t mixer = m.node(AUD_GRAPH_MIXER_TYPE_ID, &mixerConfig);
    AUD_CHECK(aud_graph_set_param(m.graph, mixer, bus, 2.0f, 0) == AUD_OK);
    m.commit([&] {
      m.connect(a, mixer, 0, bus);
      m.connect(mixer, AUD_GRAPH_NODE);
    });
    aud_graph_start(m.graph);
    m.render(256);
    AUD_CHECK_NEAR(m.out[0][100], 1.0, 1e-6);
  }
}

AUD_TEST(maps_channels_between_buses) {
  Fixture f(0, 2);
  const int32_t osc = steadyOscillator(f, 0.5f);  // mono
  const uint32_t two = 2;
  AudNodeConfig stereoOsc{sizeof(AudNodeConfig), 0, nullptr, 1, &two, 0, 0};
  const int32_t wide = f.node(AUD_GRAPH_OSCILLATOR_TYPE_ID, &stereoOsc);
  aud_graph_set_param(f.graph, wide, AUD_OSCILLATOR_PARAM_WAVEFORM, 2, 0);
  aud_graph_set_param(f.graph, wide, AUD_OSCILLATOR_PARAM_FREQUENCY, 20, 0);
  aud_graph_set_param(f.graph, wide, AUD_OSCILLATOR_PARAM_AMPLITUDE, 0.25f, 0);
  const uint32_t one = 1;
  AudNodeConfig monoTap{sizeof(AudNodeConfig), 1, &one, 1, &one, 0, 0};
  const int32_t tap = f.node(AUD_GRAPH_TAP_TYPE_ID, &monoTap);
  f.commit([&] {
    f.connect(osc, AUD_GRAPH_NODE);   // mono into stereo: broadcast
    f.connect(wide, tap);             // stereo into mono: average
    f.connect(tap, AUD_GRAPH_NODE);   // mono into stereo
  });
  aud_graph_start(f.graph);
  f.render(256);
  AUD_CHECK_NEAR(f.out[0][100], 0.75, 1e-6);
  AUD_CHECK_NEAR(f.out[1][100], 0.75, 1e-6);
}

AUD_TEST(respects_capacities) {
  Fixture f(0, 1, 256, [](AudGraphConfig& c) {
    c.max_nodes = 2;
    c.max_connections = 1;
  });
  const int32_t a = f.node(AUD_GRAPH_OSCILLATOR_TYPE_ID);
  const int32_t b = f.node(AUD_GRAPH_FILTER_TYPE_ID);
  AUD_CHECK(f.node(AUD_GRAPH_FILTER_TYPE_ID) == AUD_ERROR_CAPACITY);
  AUD_CHECK(aud_graph_begin(f.graph) == AUD_OK);
  AUD_CHECK(f.connect(a, b) == AUD_OK);
  AUD_CHECK(f.connect(b, AUD_GRAPH_NODE) == AUD_ERROR_CAPACITY);
  AUD_CHECK(aud_graph_connect_events(f.graph, AUD_GRAPH_NODE, 0, a, 0) == AUD_ERROR_CAPACITY);
  AUD_CHECK(aud_graph_commit(f.graph) == 1);
}

AUD_TEST(keeps_the_latest_pending_program) {
  Fixture f;
  const int32_t osc = steadyOscillator(f);
  AUD_CHECK(f.commit([&] { f.connect(osc, AUD_GRAPH_NODE); }) == 1);
  AUD_CHECK(f.commit([&] { aud_graph_disconnect(f.graph, osc, 0, AUD_GRAPH_NODE, 0); }) == 2);
  aud_graph_start(f.graph);
  f.render(256);
  AUD_CHECK(f.out[0][100] == 0);
  AUD_CHECK(aud_graph_revision(f.graph) == 2);
  AUD_CHECK(f.countNotifications(AUD_NOTIFY_REVISION) == 1);
}

AUD_TEST(wakes_the_listener_once_per_batch) {
  Fixture f;
  static int wakes = 0;
  wakes = 0;
  AUD_CHECK(aud_graph_set_listener(f.graph, [](void* user) { *static_cast<int*>(user) += 1; }, &wakes) == AUD_OK);
  const int32_t osc = f.node(AUD_GRAPH_OSCILLATOR_TYPE_ID);
  f.commit([&] { f.connect(osc, AUD_GRAPH_NODE); });
  aud_graph_start(f.graph);
  for (int i = 0; i < 5; ++i) f.render(256);
  for (int i = 0; i < 100 && wakes == 0; ++i) {
    struct timespec ts {0, 1000000};
    nanosleep(&ts, nullptr);
  }
  AUD_CHECK(wakes == 1);
  AUD_CHECK(f.take().size() >= 2);
  AUD_CHECK(aud_graph_set_listener(f.graph, nullptr, nullptr) == AUD_OK);
}

int main() { return aud_test::run(); }
