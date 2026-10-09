// @license
// Copyright (c) Audanika. All Rights Reserved.
//
// Use of this source code is governed by terms that can be
// found in the LICENSE file in the root of this package.

// What the native tests of the graph share: test node types registered
// like the nodes of a DSP package, a fixture with one input and one output
// bus, and the helpers that build events and timestamps. Header only;
// every definition is inline so that the test files see one copy.

#ifndef AUD_GRAPH_FIXTURE_HPP
#define AUD_GRAPH_FIXTURE_HPP

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <string>
#include <utility>
#include <vector>

#include "aud_audio_graph.h"
#include "aud_test.hpp"
#include "aud_ump.h"

// A test that violates the realtime contract on purpose tells the
// RealtimeSanitizer so; the watchdog still counts it.
#if defined(__has_feature)
#if __has_feature(realtime_sanitizer)
#include <sanitizer/rtsan_interface.h>
#define AUD_TEST_RTSAN_OFF() __rtsan_disable()
#define AUD_TEST_RTSAN_ON() __rtsan_enable()
#endif
#endif
#ifndef AUD_TEST_RTSAN_OFF
#define AUD_TEST_RTSAN_OFF()
#define AUD_TEST_RTSAN_ON()
#endif

namespace aud_test_fixture {

inline constexpr double kRate = 48000.0;
inline constexpr uint32_t kFade = 240;  // 5 ms at 48 kHz
// The vectors the test nodes record into are reserved once: pushing into
// them on the realtime thread must not allocate under the watchdog.
inline constexpr size_t kRecordCapacity = 1 << 17;

// ............................................................................
// Test node types, registered like the nodes of a DSP package

struct Record {
  int64_t position;
  uint32_t offset;
  uint32_t port;
  uint32_t type;
  uint32_t word0;
};

inline std::vector<Record> g_records;
inline std::vector<uint32_t> g_resets;
inline std::vector<uint32_t> g_fixedFrames;
inline std::vector<std::pair<uint32_t, float>> g_params;

// aud.test.recorder: records the events it receives, its parameter calls
// and its resets.
struct Recorder {
  int64_t position = 0;
};

inline void* recorderCreate(const AudNodeDescriptor*, const AudHostApi*) {
  return new Recorder();
}
inline void recorderDestroy(void* instance) {
  delete static_cast<Recorder*>(instance);
}
inline int32_t recorderPrepare(void*, const AudPrepareInfo*) { return AUD_OK; }
inline void recorderReset(void*, uint32_t reason) { g_resets.push_back(reason); }
inline void recorderSetParam(void*, uint32_t index, float value) {
  g_params.emplace_back(index, value);
}
inline void recorderProcess(void*, const AudProcessContext* context) {
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

inline const AudBusDescriptor kMonoOut[] = {
    {sizeof(AudBusDescriptor), "out", "Output", AUD_BUS_MAIN | AUD_BUS_OPTIONAL,
     1, 2, 1},
};
inline const AudBusDescriptor kMonoIn[] = {
    {sizeof(AudBusDescriptor), "in", "Input", AUD_BUS_MAIN, 1, 2, 1},
};
inline const AudEventPortDescriptor kEventIn[] = {
    {sizeof(AudEventPortDescriptor), "events", "Events",
     AUD_EVENT_PORT_MIDI | AUD_EVENT_PORT_CONTROL, 0},
};
inline const AudEventPortDescriptor kEventOut[] = {
    {sizeof(AudEventPortDescriptor), "out", "Out", AUD_EVENT_PORT_MIDI, 0},
};
inline const AudParamDescriptor kRecorderParams[] = {
    {sizeof(AudParamDescriptor), "p0", "P0", "", 0.0f, 1000.0f, 0.0f,
     AUD_PARAM_AUTOMATABLE, 0, 0},
    {sizeof(AudParamDescriptor), "p1", "P1", "", 0.0f, 1000.0f, 0.0f,
     AUD_PARAM_AUTOMATABLE, 0, 0},
};

inline const AudNodeVTable kRecorderVTable = {
    sizeof(AudNodeVTable), recorderCreate, recorderDestroy, recorderPrepare,
    recorderReset,         recorderSetParam, recorderProcess, nullptr,
    nullptr,               nullptr,          nullptr,         nullptr,
};

inline const AudNodeDescriptor kRecorderDescriptor = {
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
    2,
    0,
    kRecorderParams,
    nullptr,
    &kRecorderVTable,
};

// aud.test.delay: delays its mono input by 100 frames and reports the
// latency and a tail of the same length.
inline constexpr uint32_t kTestLatency = 100;

struct Delay {
  std::vector<float> line = std::vector<float>(kTestLatency, 0.0f);
  uint32_t pos = 0;
};

inline void* delayCreate(const AudNodeDescriptor*, const AudHostApi*) {
  return new Delay();
}
inline void delayDestroy(void* instance) { delete static_cast<Delay*>(instance); }
inline int32_t delayPrepare(void*, const AudPrepareInfo*) { return AUD_OK; }
inline void delayReset(void*, uint32_t) {}
inline void delaySetParam(void*, uint32_t, float) {}
inline void delayProcess(void* instance, const AudProcessContext* context) {
  auto* delay = static_cast<Delay*>(instance);
  const float* in = context->inputs[0].channels[0];
  float* out = context->outputs[0].channels[0];
  for (uint32_t i = 0; i < context->frames; ++i) {
    out[i] = delay->line[delay->pos];
    delay->line[delay->pos] = in[i];
    delay->pos = (delay->pos + 1) % kTestLatency;
  }
}
inline uint32_t delayLatency(void*) { return kTestLatency; }
inline uint32_t delayTail(void*) { return kTestLatency; }

inline const AudNodeVTable kDelayVTable = {
    sizeof(AudNodeVTable), delayCreate, delayDestroy, delayPrepare, delayReset,
    delaySetParam,         delayProcess, nullptr,     delayLatency, delayTail,
    nullptr,               nullptr,
};

inline const AudNodeDescriptor kDelayDescriptor = {
    sizeof(AudNodeDescriptor),
    AUD_ABI_VERSION_MAJOR,
    AUD_ABI_VERSION_MINOR,
    1,
    "aud.test.delay",
    "Delay",
    "Test",
    AUD_NODE_CAP_VARIABLE_BLOCK | AUD_NODE_CAP_LATENCY | AUD_NODE_CAP_TAIL,
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
inline void* fixedCreate(const AudNodeDescriptor*, const AudHostApi*) {
  return new int(0);
}
inline void fixedDestroy(void* instance) { delete static_cast<int*>(instance); }
inline void fixedProcess(void*, const AudProcessContext* context) {
  g_fixedFrames.push_back(context->frames);
  std::memcpy(context->outputs[0].channels[0], context->inputs[0].channels[0],
              sizeof(float) * context->frames);
}

inline const AudNodeVTable kFixedVTable = {
    sizeof(AudNodeVTable), fixedCreate, fixedDestroy, delayPrepare, delayReset,
    delaySetParam,         fixedProcess, nullptr,     nullptr,      nullptr,
    nullptr,               nullptr,
};

inline const AudNodeDescriptor kFixedDescriptor = {
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

inline void* emitterCreate(const AudNodeDescriptor*, const AudHostApi* host) {
  return new Emitter{host};
}
inline void emitterDestroy(void* instance) {
  delete static_cast<Emitter*>(instance);
}
inline void emitterProcess(void* instance, const AudProcessContext* context) {
  auto* emitter = static_cast<Emitter*>(instance);
  for (uint32_t i = 0; i < context->num_events; ++i) {
    AudEvent event = context->events[i];
    event.port = 0;
    emitter->host->emit_event(emitter->host->host, instance, &event);
  }
}

inline const AudNodeVTable kEmitterVTable = {
    sizeof(AudNodeVTable), emitterCreate, emitterDestroy, delayPrepare,
    delayReset,            delaySetParam, emitterProcess, nullptr,
    nullptr,               nullptr,       nullptr,        nullptr,
};

inline const AudNodeDescriptor kEmitterDescriptor = {
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

// aud.test.preset (ticket 20): two parameters, two string settings and a
// state blob - what a preset and the headless host touch. The state is an
// internal offset, not a parameter, as the host's convention asks. Its
// output is the constant `a + offset`; it counts its process calls and
// notices a state call that overlaps one.
inline constexpr uint32_t kPresetStateVersion = 3;
inline constexpr uint32_t kPresetKeyFile = 7;
inline constexpr uint32_t kPresetKeyName = 8;

struct Preset {
  float a = 0.5f;
  float b = 10.0f;
  float offset = 0.0f;  // the state
  uint32_t loads = 0;
  uint32_t flags = 0;  // of the last process call
  std::string file;
  std::string name;
  uint32_t calls = 0;
  uint32_t events = 0;  // events seen in process
  bool sounding[128] = {};  // the notes on
  uint32_t held() const {
    uint32_t count = 0;
    for (const bool on : sounding) count += on ? 1 : 0;
    return count;
  }
  uint32_t resets = 0;
  std::atomic<int> inProcess{0};
  std::atomic<int> inStateCall{0};
  // A realtime call - process, set_param, reset - that ran while a state
  // call did, or the other way round.
  std::atomic<int> overlaps{0};
  // How long a state call takes, so that blocks render while it runs.
  int64_t stateCallNs = 0;
};

inline Preset* presetOf(void* instance) { return static_cast<Preset*>(instance); }

// The instance created last, for the tests that look inside.
inline Preset* g_lastPreset = nullptr;

inline void* presetCreate(const AudNodeDescriptor*, const AudHostApi*) {
  g_lastPreset = new Preset();
  return g_lastPreset;
}
inline void presetDestroy(void* instance) { delete presetOf(instance); }
inline void presetSetParam(void* instance, uint32_t index, float value) {
  Preset* p = presetOf(instance);
  if (p->inStateCall.load(std::memory_order_seq_cst)) p->overlaps += 1;
  if (index == 0) p->a = value;
  if (index == 1) p->b = value;
}
inline void presetReset(void* instance, uint32_t) {
  Preset* p = presetOf(instance);
  if (p->inStateCall.load(std::memory_order_seq_cst)) p->overlaps += 1;
  p->resets += 1;
}
inline void presetProcess(void* instance, const AudProcessContext* context) {
  Preset* p = presetOf(instance);
  p->inProcess.store(1, std::memory_order_seq_cst);
  for (uint32_t b = 0; b < context->num_output_buses; ++b) {
    for (uint32_t c = 0; c < context->outputs[b].num_channels; ++c) {
      float* out = context->outputs[b].channels[c];
      for (uint32_t i = 0; i < context->frames; ++i) out[i] = p->a + p->offset;
    }
  }
  p->flags = context->flags;
  p->calls += 1;
  p->events += context->num_events;
  for (uint32_t i = 0; i < context->num_events; ++i) {
    const AudEvent& e = context->events[i];
    if (e.type == AUD_EVENT_PARAM) {
      presetSetParam(instance, e.words[0], aud_event_param_value(&e));
    } else if (e.type == AUD_EVENT_UMP && aud_ump_is_note_on(e.words[0], e.words[1])) {
      p->sounding[aud_ump_note(e.words[0])] = true;
    } else if (e.type == AUD_EVENT_UMP && aud_ump_is_note_off(e.words[0], e.words[1])) {
      p->sounding[aud_ump_note(e.words[0])] = false;
    }
  }
  p->inProcess.store(0, std::memory_order_seq_cst);
}
inline int32_t presetSetString(void* instance, uint32_t key, const char* value) {
  Preset* p = presetOf(instance);
  if (key == kPresetKeyFile) {
    p->file = value;
    return AUD_OK;
  }
  if (key == kPresetKeyName) {
    p->name = value;
    return AUD_OK;
  }
  return AUD_ERROR_INVALID_ARGUMENT;
}
inline void presetHold(Preset* p) {
  if (p->inProcess.load(std::memory_order_seq_cst)) p->overlaps += 1;
  if (p->stateCallNs == 0) return;
  const auto until = std::chrono::steady_clock::now() +
                     std::chrono::nanoseconds(p->stateCallNs);
  while (std::chrono::steady_clock::now() < until) {
    if (p->inProcess.load(std::memory_order_seq_cst)) p->overlaps += 1;
  }
}
inline int32_t presetSaveState(void* instance, void* buffer, size_t capacity,
                               size_t* size) {
  Preset* p = presetOf(instance);
  p->inStateCall.store(1, std::memory_order_seq_cst);
  presetHold(p);
  p->inStateCall.store(0, std::memory_order_seq_cst);
  *size = sizeof(float);
  if (capacity < *size) return AUD_ERROR_BUFFER_TOO_SMALL;
  std::memcpy(buffer, &p->offset, sizeof(float));
  return AUD_OK;
}
inline int32_t presetLoadState(void* instance, const void* data, size_t size,
                               uint32_t version) {
  Preset* p = presetOf(instance);
  p->inStateCall.store(1, std::memory_order_seq_cst);
  presetHold(p);
  p->inStateCall.store(0, std::memory_order_seq_cst);
  if (version != kPresetStateVersion) return AUD_ERROR_STATE_VERSION;
  if (size != sizeof(float)) return AUD_ERROR_INVALID_ARGUMENT;
  std::memcpy(&p->offset, data, sizeof(float));
  p->loads += 1;
  return AUD_OK;
}

inline const AudParamDescriptor kPresetParams[] = {
    {sizeof(AudParamDescriptor), "a", "A", "", 0.0f, 1.0f, 0.5f,
     AUD_PARAM_AUTOMATABLE, 0, 0},
    {sizeof(AudParamDescriptor), "b", "B", "Hz", 0.0f, 100.0f, 10.0f,
     AUD_PARAM_AUTOMATABLE, 0, 0},
};
inline const AudStringKeyDescriptor kPresetStrings[] = {
    {sizeof(AudStringKeyDescriptor), kPresetKeyFile, "file", "File"},
    {sizeof(AudStringKeyDescriptor), kPresetKeyName, "name", "Name"},
};

inline const AudNodeVTable kPresetVTable = {
    sizeof(AudNodeVTable), presetCreate,    presetDestroy,   delayPrepare,
    presetReset,           presetSetParam,  presetProcess,   presetSetString,
    nullptr,               nullptr,         presetSaveState, presetLoadState,
};

inline const AudNodeDescriptor kPresetDescriptor = {
    sizeof(AudNodeDescriptor),
    AUD_ABI_VERSION_MAJOR,
    AUD_ABI_VERSION_MINOR,
    2,
    "aud.test.preset",
    "Preset",
    "Test",
    AUD_NODE_CAP_VARIABLE_BLOCK | AUD_NODE_CAP_EVENTS | AUD_NODE_CAP_STRINGS |
        AUD_NODE_CAP_STATE | AUD_NODE_CAP_RESET_ON_STOP |
        AUD_NODE_CAP_RESET_ON_SEEK,
    kPresetStateVersion,
    0,
    1,
    nullptr,
    kMonoOut,
    1,
    0,
    kEventIn,
    nullptr,
    2,
    2,
    kPresetParams,
    kPresetStrings,
    &kPresetVTable,
};

// aud.test.allocator (ticket 20): breaks the realtime contract on purpose
// - it allocates, frees and calls the host api on the realtime thread. It
// tells the RealtimeSanitizer so, unless AUD_TEST_RTSAN_PROBE is set:
// scripts/test-native.js then checks that the rtsan run catches it.
inline const bool g_rtsanProbe = std::getenv("AUD_TEST_RTSAN_PROBE") != nullptr;
struct Allocator {
  const AudHostApi* host;
};

// Keeps the compiler from eliding the allocation of the test node.
inline void* volatile g_allocatorSink = nullptr;

inline void* allocatorCreate(const AudNodeDescriptor*, const AudHostApi* host) {
  return new Allocator{host};
}
inline void allocatorDestroy(void* instance) {
  delete static_cast<Allocator*>(instance);
}
inline void allocatorProcess(void* instance, const AudProcessContext* context) {
  auto* allocator = static_cast<Allocator*>(instance);
  const AudHostApi* host = allocator->host;
  if (!g_rtsanProbe) AUD_TEST_RTSAN_OFF();
  char* bytes = new char[16];
  g_allocatorSink = bytes;
  delete[] static_cast<char*>(g_allocatorSink);
  void* memory = host->alloc(host->host, 8);
  g_allocatorSink = memory;
  host->free(host->host, g_allocatorSink);
  host->log(host->host, AUD_LOG_DEBUG, "allocating on the realtime thread");
  if (!g_rtsanProbe) AUD_TEST_RTSAN_ON();
  for (uint32_t b = 0; b < context->num_output_buses; ++b) {
    for (uint32_t c = 0; c < context->outputs[b].num_channels; ++c) {
      std::memset(context->outputs[b].channels[c], 0,
                  sizeof(float) * context->frames);
    }
  }
}

inline const AudNodeVTable kAllocatorVTable = {
    sizeof(AudNodeVTable), allocatorCreate, allocatorDestroy, delayPrepare,
    delayReset,            delaySetParam,   allocatorProcess, nullptr,
    nullptr,               nullptr,         nullptr,          nullptr,
};

inline const AudNodeDescriptor kAllocatorDescriptor = {
    sizeof(AudNodeDescriptor),
    AUD_ABI_VERSION_MAJOR,
    AUD_ABI_VERSION_MINOR,
    1,
    "aud.test.allocator",
    "Allocator",
    "Test",
    AUD_NODE_CAP_VARIABLE_BLOCK,
    0,
    0,
    1,
    nullptr,
    kMonoOut,
    0,
    0,
    nullptr,
    nullptr,
    0,
    0,
    nullptr,
    nullptr,
    &kAllocatorVTable,
};

// aud.test.infinite: a silent node with an infinite tail.
inline uint32_t infiniteTail(void*) { return AUD_TAIL_INFINITE; }
inline void silentProcess(void*, const AudProcessContext* context) {
  for (uint32_t b = 0; b < context->num_output_buses; ++b) {
    for (uint32_t c = 0; c < context->outputs[b].num_channels; ++c) {
      std::memset(context->outputs[b].channels[c], 0,
                  sizeof(float) * context->frames);
    }
  }
}

inline const AudNodeVTable kInfiniteVTable = {
    sizeof(AudNodeVTable), fixedCreate,   fixedDestroy,  delayPrepare,
    delayReset,            delaySetParam, silentProcess, nullptr,
    nullptr,               infiniteTail,  nullptr,       nullptr,
};

inline const AudNodeDescriptor kInfiniteDescriptor = {
    sizeof(AudNodeDescriptor),
    AUD_ABI_VERSION_MAJOR,
    AUD_ABI_VERSION_MINOR,
    1,
    "aud.test.infinite",
    "Infinite",
    "Test",
    AUD_NODE_CAP_VARIABLE_BLOCK | AUD_NODE_CAP_TAIL,
    0,
    0,
    1,
    nullptr,
    kMonoOut,
    0,
    0,
    nullptr,
    nullptr,
    0,
    0,
    nullptr,
    nullptr,
    &kInfiniteVTable,
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
  AudRenderRequest hostRequest{};  // what the last renderHost rendered

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
    host->register_node_type(host->host, &kPresetDescriptor);
    host->register_node_type(host->host, &kAllocatorDescriptor);
    host->register_node_type(host->host, &kInfiniteDescriptor);
    g_records.clear();
    g_records.reserve(kRecordCapacity);
    g_resets.clear();
    g_resets.reserve(kRecordCapacity);
    g_fixedFrames.clear();
    g_fixedFrames.reserve(kRecordCapacity);
    g_params.clear();
    g_params.reserve(kRecordCapacity);
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

  AudRenderRequest renderRequest(uint32_t frames,
                                 const AudStreamTime* time = nullptr,
                                 const AudEvent* events = nullptr,
                                 uint32_t numEvents = 0) {
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
    return request;
  }

  int32_t render(uint32_t frames, const AudStreamTime* time = nullptr,
                 const AudEvent* events = nullptr, uint32_t numEvents = 0) {
    const AudRenderRequest r = renderRequest(frames, time, events, numEvents);
    return aud_graph_render(graph, &r);
  }

  // Renders through the host's render call with an event output.
  int32_t renderHost(uint32_t frames, AudEvent* outEvents, uint32_t capacity,
                     AudHostRenderRequest* host,
                     const AudEvent* events = nullptr, uint32_t numEvents = 0,
                     uint32_t flags = 0) {
    hostRequest = renderRequest(frames, nullptr, events, numEvents);
    *host = AudHostRenderRequest{};
    host->struct_size = sizeof(AudHostRenderRequest);
    host->flags = flags;
    host->max_output_events = capacity;
    host->request = &hostRequest;
    host->output_events = outEvents;
    return aud_graph_render_host(graph, host);
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

  // Takes every waiting notification, not only one batch.
  std::vector<AudGraphNotification> takeAll() {
    std::vector<AudGraphNotification> all;
    while (true) {
      std::vector<AudGraphNotification> batch = take();
      all.insert(all.end(), batch.begin(), batch.end());
      if (batch.size() < 64) return all;
    }
  }

  uint32_t countNotifications(uint32_t type, int32_t code = INT32_MIN) {
    uint32_t count = 0;
    for (const AudGraphNotification& n : takeAll()) {
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

inline uint32_t risingCrossings(const std::vector<float>& samples) {
  uint32_t count = 0;
  for (size_t i = 1; i < samples.size(); ++i) {
    if (samples[i - 1] < 0 && samples[i] >= 0) count += 1;
  }
  return count;
}

inline AudEvent noteOn(uint32_t note, uint32_t velocity = 100,
                       uint32_t port = 0, uint32_t channel = 0) {
  const uint32_t word = aud_ump_midi1_word(0, 0x90 | channel, note, velocity);
  return aud_event_ump(&word, 1, 0, port);
}

inline AudEvent noteOff(uint32_t note, uint32_t port = 0, uint32_t channel = 0) {
  const uint32_t word = aud_ump_midi1_word(0, 0x80 | channel, note, 64);
  return aud_event_ump(&word, 1, 0, port);
}

inline AudTimestamp atSample(int64_t position) {
  AudTimestamp t{};
  t.struct_size = sizeof(AudTimestamp);
  t.domain = AUD_TIME_SAMPLE;
  t.value = position;
  return t;
}

inline AudTimestamp atBeat(double beats) {
  AudTimestamp t{};
  t.struct_size = sizeof(AudTimestamp);
  t.domain = AUD_TIME_BEAT;
  t.value = static_cast<int64_t>(beats * static_cast<double>(AUD_BEAT_FACTOR));
  return t;
}

// A square wave at 0.5 Hz is a constant for the first 48000 frames: the
// steady signal the fade tests look at.
inline int32_t steadyOscillator(Fixture& f, float amplitude = 0.5f) {
  const int32_t osc = f.node(AUD_GRAPH_OSCILLATOR_TYPE_ID);
  aud_graph_set_param(f.graph, osc, AUD_OSCILLATOR_PARAM_WAVEFORM, 2, 0);
  aud_graph_set_param(f.graph, osc, AUD_OSCILLATOR_PARAM_FREQUENCY, 0.5f, 0);
  aud_graph_set_param(f.graph, osc, AUD_OSCILLATOR_PARAM_AMPLITUDE, amplitude, 0);
  return osc;
}

}  // namespace aud_test_fixture

#endif  // AUD_GRAPH_FIXTURE_HPP
