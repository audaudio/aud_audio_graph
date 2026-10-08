// @license
// Copyright (c) Audanika. All Rights Reserved.
//
// Use of this source code is governed by terms that can be
// found in the LICENSE file in the root of this package.

// The control-thread side of the graph: creation and the lifecycle
// (lifecycle-001), the host api for packages, node instances, the
// transactions that compile and publish programs (graph-003), the producer
// ends of the queues (interop-002) and the notifications. Nothing here runs
// on the realtime thread except the host callbacks that are tagged so.

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <thread>

#include "aud_clock.h"
#include "aud_graph_internal.hpp"
#include "aud_transport.h"

#ifdef __ANDROID__
#include <android/log.h>
#endif

using namespace aud;

namespace aud {
namespace {

// ............................................................................
// The built-in node types: the feedback node and the tap have no vtable,
// the engine renders them itself.

const AudBusDescriptor kFeedbackInputs[] = {
    {sizeof(AudBusDescriptor), "in", "Input", AUD_BUS_MAIN, 1, kMaxChannels,
     2},
};

const AudBusDescriptor kFeedbackOutputs[] = {
    {sizeof(AudBusDescriptor), "out", "Output", AUD_BUS_MAIN, 1, kMaxChannels,
     2},
};

const AudNodeDescriptor kFeedbackDescriptor = {
    sizeof(AudNodeDescriptor),
    AUD_ABI_VERSION_MAJOR,
    AUD_ABI_VERSION_MINOR,
    1,
    AUD_GRAPH_FEEDBACK_TYPE_ID,
    "Feedback",
    "Audanika",
    AUD_NODE_CAP_VARIABLE_BLOCK | AUD_NODE_CAP_TAIL,
    0,
    1,
    1,
    kFeedbackInputs,
    kFeedbackOutputs,
    0,
    0,
    nullptr,
    nullptr,
    0,
    0,
    nullptr,
    nullptr,
    nullptr,
};

const AudBusDescriptor kTapInputs[] = {
    {sizeof(AudBusDescriptor), "in", "Input", AUD_BUS_MAIN, 1, kMaxChannels,
     2},
};

const AudBusDescriptor kTapOutputs[] = {
    {sizeof(AudBusDescriptor), "out", "Output", AUD_BUS_MAIN | AUD_BUS_OPTIONAL,
     1, kMaxChannels, 2},
};

const AudNodeDescriptor kTapDescriptor = {
    sizeof(AudNodeDescriptor),
    AUD_ABI_VERSION_MAJOR,
    AUD_ABI_VERSION_MINOR,
    1,
    AUD_GRAPH_TAP_TYPE_ID,
    "Tap",
    "Audanika",
    AUD_NODE_CAP_VARIABLE_BLOCK,
    0,
    1,
    1,
    kTapInputs,
    kTapOutputs,
    0,
    0,
    nullptr,
    nullptr,
    0,
    0,
    nullptr,
    nullptr,
    nullptr,
};

// ............................................................................
// The host api

int32_t hostRegisterNodeType(void* host, const AudNodeDescriptor* d) {
  auto* graph = static_cast<AudGraph*>(host);
  if (graph == nullptr || d == nullptr ||
      d->struct_size < sizeof(AudNodeDescriptor) || d->type_id == nullptr ||
      d->vtable == nullptr) {
    return AUD_ERROR_INVALID_ARGUMENT;
  }
  const AudNodeVTable* vtable = d->vtable;
  if (vtable->struct_size < sizeof(AudNodeVTable) || vtable->create == nullptr ||
      vtable->destroy == nullptr || vtable->prepare == nullptr ||
      vtable->process == nullptr) {
    return AUD_ERROR_INVALID_ARGUMENT;
  }
  if (d->abi_major != AUD_ABI_VERSION_MAJOR) return AUD_ERROR_ABI_MAJOR;
  if (!aud_abi_is_compatible(d->abi_major, d->abi_minor, AUD_ABI_VERSION_MAJOR,
                             AUD_ABI_VERSION_MINOR)) {
    return AUD_ERROR_ABI_MINOR;
  }
  for (const AudNodeDescriptor* type : graph->types) {
    if (std::strcmp(type->type_id, d->type_id) == 0) {
      return AUD_ERROR_DUPLICATE_TYPE;
    }
  }
  graph->types.push_back(d);
  return AUD_OK;
}

int32_t hostRegisterTransportProvider(void* host, const char* id,
                                      const AudTransportProviderVTable* v) {
  auto* graph = static_cast<AudGraph*>(host);
  if (graph == nullptr || id == nullptr || v == nullptr ||
      v->struct_size < sizeof(AudTransportProviderVTable) ||
      v->create == nullptr || v->destroy == nullptr || v->capture == nullptr ||
      v->request == nullptr || v->capabilities == nullptr) {
    return AUD_ERROR_INVALID_ARGUMENT;
  }
  for (const TransportProvider& provider : graph->providers) {
    if (provider.id == id) return AUD_ERROR_DUPLICATE_TYPE;
  }
  TransportProvider provider;
  provider.id = id;
  provider.vtable = v;
  graph->providers.push_back(provider);
  return AUD_OK;
}

void* hostAlloc(void*, size_t bytes) { return std::malloc(bytes); }

void hostFree(void*, void* memory) { std::free(memory); }

void hostLog(void*, int32_t level, const char* message) {
#ifdef __ANDROID__
  __android_log_print(
      level >= AUD_LOG_ERROR ? ANDROID_LOG_ERROR : ANDROID_LOG_INFO, "aud_audio",
      "%s", message);
#else
  std::fprintf(stderr, "[aud_audio %d] %s\n", level, message);
#endif
}

// [realtime] A node emits an event on one of its outputs.
void hostEmitEvent(void* host, void* instance, const AudEvent* event) {
  auto* graph = static_cast<AudGraph*>(host);
  if (graph == nullptr || event == nullptr) return;
  emitEvent(graph, instance, event);
}

// ............................................................................
// Helpers

uint32_t orDefault(uint32_t value, uint32_t fallback) {
  return value == 0 ? fallback : value;
}

uint32_t framesOf(double seconds, double sampleRate) {
  return static_cast<uint32_t>(std::lround(seconds * sampleRate));
}

const AudNodeDescriptor* typeById(AudGraph* graph, const char* typeId) {
  for (const AudNodeDescriptor* type : graph->types) {
    if (std::strcmp(type->type_id, typeId) == 0) return type;
  }
  return nullptr;
}

void releaseProgram(AudGraph* graph, Program* program) {
  if (program == nullptr) return;
  for (Edge* edge : program->edges) edge->programs -= 1;
  for (ProgramNode& node : program->nodes) node.instance->programs -= 1;
  (void)graph;
  delete program;
}

void freeRetiredPrograms(AudGraph* graph) {
  for (auto& slot : graph->retired) {
    releaseProgram(graph, slot.exchange(nullptr, std::memory_order_acq_rel));
  }
}

void destroyInstance(AudGraph* graph, NodeInstance* instance) {
  if (instance->instance != nullptr) {
    instance->descriptor->vtable->destroy(instance->instance);
    instance->instance = nullptr;
  }
  (void)graph;
}

// Frees what no program references any more: finished instances and the
// edges that faded out.
void collect(AudGraph* graph) {
  for (auto it = graph->instances.begin(); it != graph->instances.end();) {
    NodeInstance* instance = it->get();
    if (instance->retired && instance->programs == 0 &&
        instance->done.load(std::memory_order_acquire)) {
      destroyInstance(graph, instance);
      it = graph->instances.erase(it);
    } else {
      ++it;
    }
  }
  for (auto it = graph->edges.begin(); it != graph->edges.end();) {
    Edge* edge = it->get();
    if (edge->retiring && edge->programs == 0 &&
        edge->inactive.load(std::memory_order_acquire)) {
      it = graph->edges.erase(it);
    } else {
      ++it;
    }
  }
  for (auto it = graph->zombieEdges.begin(); it != graph->zombieEdges.end();) {
    if ((*it)->programs == 0) {
      it = graph->zombieEdges.erase(it);
    } else {
      ++it;
    }
  }
}

// Publishes a compiled program; the realtime thread adopts it at the next
// block start.
void publish(AudGraph* graph, Program* program) {
  program->revision = graph->nextRevision++;
  graph->hasProgram = true;
  graph->outputLatency = program->outputLatency;
  for (ProgramNode& node : program->nodes) {
    if (!node.instance->retired) node.instance->committed = true;
  }
  freeRetiredPrograms(graph);
  // A pending program the realtime thread never saw is dropped.
  releaseProgram(graph,
                 graph->pending.exchange(program, std::memory_order_acq_rel));
}

// Compiles the committed topology again, e.g. after instances finished.
int32_t recompile(AudGraph* graph) {
  int32_t error = AUD_OK;
  Program* program = compileProgram(graph, graph->topology, &error);
  if (program == nullptr) return error;
  publish(graph, program);
  return AUD_OK;
}

// Waits until the realtime thread has left aud_graph_render.
void waitForRender(AudGraph* graph) {
  while (graph->inFlight.load(std::memory_order_seq_cst)) {
    std::this_thread::yield();
  }
}

bool isLive(const NodeInstance* instance) {
  return instance != nullptr && !instance->retired;
}

// ............................................................................
// Instances

int32_t prepareInstance(AudGraph* graph, NodeInstance* instance) {
  const uint32_t maxFrames = graph->maxFrames;
  if (instance->builtin == Builtin::feedback) {
    const uint32_t frames = std::max(instance->delayFrames, maxFrames);
    instance->delayFrames = frames;
    instance->delayLine.assign(
        static_cast<size_t>(frames) * instance->inputChannels[0], 0.0f);
    instance->delayPos = 0;
    instance->tail = frames;
    instance->latency = 0;
    return AUD_OK;
  }
  if (instance->builtin == Builtin::tap) {
    TapState& tap = instance->tap;
    tap.channels = instance->inputChannels[0];
    tap.ringFrames = std::max(maxFrames * 2, uint32_t{8192});
    tap.ring.assign(static_cast<size_t>(tap.ringFrames) * tap.channels, 0.0f);
    tap.writePos = 0;
    for (uint32_t c = 0; c < kMaxChannels; ++c) {
      tap.peak[c].store(0, std::memory_order_relaxed);
      tap.rms[c].store(0, std::memory_order_relaxed);
    }
    instance->latency = 0;
    instance->tail = 0;
    return AUD_OK;
  }
  AudPrepareInfo info{};
  info.struct_size = sizeof(AudPrepareInfo);
  info.max_frames = maxFrames;
  info.sample_rate = graph->sampleRate;
  info.flags = 0;
  info.num_input_buses = static_cast<uint32_t>(instance->inputChannels.size());
  info.input_channels = instance->inputChannels.data();
  info.num_output_buses = static_cast<uint32_t>(instance->outputChannels.size());
  info.output_channels = instance->outputChannels.data();
  const AudNodeDescriptor* d = instance->descriptor;
  const int32_t result = d->vtable->prepare(instance->instance, &info);
  if (result != AUD_OK) return result;
  const bool variable = (d->capabilities & AUD_NODE_CAP_VARIABLE_BLOCK) != 0;
  uint32_t latency = 0;
  if ((d->capabilities & AUD_NODE_CAP_LATENCY) && d->vtable->get_latency) {
    latency = d->vtable->get_latency(instance->instance);
  }
  instance->tail = 0;
  if ((d->capabilities & AUD_NODE_CAP_TAIL) && d->vtable->get_tail) {
    instance->tail = d->vtable->get_tail(instance->instance);
  }
  if (!variable) {
    auto wrapper = std::make_unique<FixedBlockWrapper>();
    wrapper->blockSize = maxFrames;
    size_t inputs = 0;
    for (uint32_t channels : instance->inputChannels) inputs += channels;
    size_t outputs = 0;
    for (uint32_t channels : instance->outputChannels) outputs += channels;
    wrapper->inputs.assign(inputs, std::vector<float>(maxFrames, 0.0f));
    wrapper->outputs.assign(outputs, std::vector<float>(maxFrames, 0.0f));
    wrapper->inputPointers.resize(inputs);
    wrapper->outputPointers.resize(outputs);
    for (size_t i = 0; i < inputs; ++i) {
      wrapper->inputPointers[i] = wrapper->inputs[i].data();
    }
    for (size_t i = 0; i < outputs; ++i) {
      wrapper->outputPointers[i] = wrapper->outputs[i].data();
    }
    size_t offset = 0;
    for (uint32_t channels : instance->inputChannels) {
      AudAudioBus bus{sizeof(AudAudioBus), channels,
                      wrapper->inputPointers.data() + offset};
      wrapper->inputBuses.push_back(bus);
      offset += channels;
    }
    offset = 0;
    for (uint32_t channels : instance->outputChannels) {
      AudAudioBus bus{sizeof(AudAudioBus), channels,
                      wrapper->outputPointers.data() + offset};
      wrapper->outputBuses.push_back(bus);
      offset += channels;
    }
    wrapper->events.resize(graph->blockEvents.size());
    instance->wrapper = std::move(wrapper);
    latency += maxFrames;
  } else {
    instance->wrapper.reset();
  }
  instance->latency = latency;
  return AUD_OK;
}

// The channels of the buses of a new instance: the configuration or the
// descriptor's defaults, checked against the descriptor's ranges.
int32_t busChannels(const AudBusDescriptor* buses, uint32_t count,
                    uint32_t configured, const uint32_t* channels,
                    std::vector<uint32_t>* out) {
  if (configured != 0 && configured != count) return AUD_ERROR_FORMAT;
  for (uint32_t b = 0; b < count; ++b) {
    const uint32_t value =
        configured == 0 ? buses[b].default_channels : channels[b];
    if (value < buses[b].min_channels || value > buses[b].max_channels ||
        value > kMaxChannels) {
      return AUD_ERROR_FORMAT;
    }
    out->push_back(value);
  }
  return AUD_OK;
}

// ............................................................................
// Ends of connections inside a transaction

bool knownEnd(AudGraph* graph, int32_t handle) {
  if (handle == AUD_GRAPH_NODE) return true;
  const NodeInstance* instance = instanceOf(graph, handle);
  if (!isLive(instance)) return false;
  return std::find(graph->workingRemoved.begin(), graph->workingRemoved.end(),
                   handle) == graph->workingRemoved.end();
}

uint32_t numBuses(AudGraph* graph, int32_t handle, bool output) {
  if (handle == AUD_GRAPH_NODE) {
    return static_cast<uint32_t>(output ? graph->inputChannels.size()
                                        : graph->outputChannels.size());
  }
  const NodeInstance* instance = instanceOf(graph, handle);
  return static_cast<uint32_t>(output ? instance->outputChannels.size()
                                      : instance->inputChannels.size());
}

uint32_t numEventPorts(AudGraph* graph, int32_t handle, bool output) {
  if (handle == AUD_GRAPH_NODE) return 1;
  const AudNodeDescriptor* d = instanceOf(graph, handle)->descriptor;
  return output ? d->num_event_outputs : d->num_event_inputs;
}

int32_t checkAudioEnds(AudGraph* graph, int32_t from, uint32_t fromBus,
                       int32_t to, uint32_t toBus) {
  if (!knownEnd(graph, from) || !knownEnd(graph, to)) {
    return AUD_ERROR_NOT_FOUND;
  }
  if (fromBus >= numBuses(graph, from, true) ||
      toBus >= numBuses(graph, to, false)) {
    return AUD_ERROR_INVALID_ARGUMENT;
  }
  return AUD_OK;
}

int32_t checkEventEnds(AudGraph* graph, int32_t from, uint32_t fromPort,
                       int32_t to, uint32_t toPort) {
  if (!knownEnd(graph, from) || !knownEnd(graph, to)) {
    return AUD_ERROR_NOT_FOUND;
  }
  if (fromPort >= numEventPorts(graph, from, true) ||
      toPort >= numEventPorts(graph, to, false)) {
    return AUD_ERROR_INVALID_ARGUMENT;
  }
  return AUD_OK;
}

// Frees an instance that never rendered.
void dropInstance(AudGraph* graph, int32_t handle) {
  for (auto it = graph->instances.begin(); it != graph->instances.end(); ++it) {
    if ((*it)->handle != handle) continue;
    destroyInstance(graph, it->get());
    graph->instances.erase(it);
    return;
  }
}

// Whether a timestamp lies beyond the lookahead of interop-002.
int32_t checkLookahead(AudGraph* graph, const AudTimestamp* at) {
  const double lookaheadSeconds = static_cast<double>(graph->lookaheadNs) / 1e9;
  switch (at->domain) {
    case AUD_TIME_IMMEDIATE:
      return AUD_OK;
    case AUD_TIME_SAMPLE: {
      const double ahead = static_cast<double>(
          at->value - graph->samplePosition.load(std::memory_order_relaxed));
      return ahead / graph->sampleRate > lookaheadSeconds ? AUD_ERROR_LOOKAHEAD
                                                          : AUD_OK;
    }
    case AUD_TIME_HOST:
      return at->value - aud_clock_now_ns() > graph->lookaheadNs
                 ? AUD_ERROR_LOOKAHEAD
                 : AUD_OK;
    case AUD_TIME_BEAT: {
      if (graph->publishedPlaying.load(std::memory_order_relaxed) == 0) {
        return AUD_OK;
      }
      const double beats = aud_beats_from_ticks(
          at->value - graph->publishedBeat.load(std::memory_order_relaxed));
      const double tempo = graph->publishedTempo.load(std::memory_order_relaxed);
      return tempo > 0 && beats * 60.0 / tempo > lookaheadSeconds
                 ? AUD_ERROR_LOOKAHEAD
                 : AUD_OK;
    }
    default:
      return AUD_ERROR_INVALID_ARGUMENT;
  }
}

// The notification thread: waits for the realtime thread and wakes the
// listener; it never touches the queue itself.
void notifierLoop(AudGraph* graph) {
  while (true) {
    graph->wake.wait();
    if (graph->stopNotifier.load(std::memory_order_acquire)) return;
    const AudGraphListener listener =
        graph->listener.load(std::memory_order_acquire);
    if (listener != nullptr) {
      listener(graph->listenerUser.load(std::memory_order_acquire));
    }
  }
}

}  // namespace

NodeInstance* instanceOf(AudGraph* graph, int32_t handle) {
  if (graph == nullptr || handle <= 0) return nullptr;
  for (auto& instance : graph->instances) {
    if (instance->handle == handle) return instance.get();
  }
  return nullptr;
}

}  // namespace aud

// ############################################################################
// Lifecycle

AUD_EXPORT AudGraph* aud_graph_create(const AudGraphConfig* config) {
  if (config == nullptr || config->struct_size < sizeof(AudGraphConfig) ||
      config->sample_rate <= 0 || config->max_frames == 0 ||
      (config->num_input_buses > 0 && config->input_channels == nullptr) ||
      (config->num_output_buses > 0 && config->output_channels == nullptr)) {
    return nullptr;
  }
  for (uint32_t b = 0; b < config->num_input_buses; ++b) {
    if (config->input_channels[b] == 0 ||
        config->input_channels[b] > kMaxChannels) {
      return nullptr;
    }
  }
  for (uint32_t b = 0; b < config->num_output_buses; ++b) {
    if (config->output_channels[b] == 0 ||
        config->output_channels[b] > kMaxChannels) {
      return nullptr;
    }
  }
  auto* graph = new (std::nothrow) AudGraph();
  if (graph == nullptr) return nullptr;
  graph->config = *config;
  graph->inputChannels.assign(config->input_channels,
                              config->input_channels + config->num_input_buses);
  graph->outputChannels.assign(
      config->output_channels,
      config->output_channels + config->num_output_buses);
  graph->config.input_channels = graph->inputChannels.data();
  graph->config.output_channels = graph->outputChannels.data();
  graph->sampleRate = config->sample_rate;
  graph->maxFrames = config->max_frames;
  AudGraphConfig& c = graph->config;
  c.max_nodes = orDefault(c.max_nodes, kDefaultMaxNodes);
  c.max_connections = orDefault(c.max_connections, kDefaultMaxConnections);
  c.param_queue_capacity =
      orDefault(c.param_queue_capacity, kDefaultParamQueueCapacity);
  c.event_queue_capacity =
      orDefault(c.event_queue_capacity, kDefaultEventQueueCapacity);
  c.scheduler_capacity =
      orDefault(c.scheduler_capacity, kDefaultSchedulerCapacity);
  c.notification_capacity =
      orDefault(c.notification_capacity, kDefaultNotificationCapacity);
  c.max_events_per_block =
      orDefault(c.max_events_per_block, kDefaultMaxEventsPerBlock);
  graph->lookaheadNs =
      c.lookahead_ns == 0 ? kDefaultLookaheadNs : c.lookahead_ns;
  graph->fadeFrames = c.fade_frames != 0
                          ? c.fade_frames
                          : framesOf(kDefaultFadeSeconds, graph->sampleRate);
  graph->maxTailFrames =
      c.max_tail_frames != 0
          ? c.max_tail_frames
          : framesOf(kDefaultMaxTailSeconds, graph->sampleRate);

  graph->hostApi = {sizeof(AudHostApi),
                    AUD_ABI_VERSION_MAJOR,
                    AUD_ABI_VERSION_MINOR,
                    AUD_ABI_VERSION_MINOR,
                    graph,
                    hostRegisterNodeType,
                    hostRegisterTransportProvider,
                    hostAlloc,
                    hostFree,
                    hostLog,
                    hostEmitEvent};
  graph->types.push_back(&kFeedbackDescriptor);
  graph->types.push_back(&kTapDescriptor);
  if (registerReferenceNodes(&graph->hostApi) != AUD_OK) {
    delete graph;
    return nullptr;
  }

  graph->commands = std::make_unique<AudSpscQueue<Command>>(c.event_queue_capacity);
  graph->params =
      std::make_unique<AudSpscQueue<ParamCommand>>(c.param_queue_capacity);
  graph->notifications = std::make_unique<AudSpscQueue<AudGraphNotification>>(
      c.notification_capacity);
  graph->scheduler.init(c.scheduler_capacity);
  graph->popped.resize(c.max_events_per_block);
  graph->blockEvents.resize(c.max_events_per_block + NoteTracker::kCapacity);
  graph->emitted.resize(c.max_events_per_block);
  graph->nodeEvents.resize(graph->blockEvents.size() + graph->emitted.size());
  graph->nodeRanges.resize(c.max_nodes);
  prepareRealtime(graph);
  resetTransport(graph);
  graph->state.store(AUD_GRAPH_CREATED, std::memory_order_release);
  graph->notifier = std::thread(notifierLoop, graph);
  return graph;
}

AUD_EXPORT void aud_graph_destroy(AudGraph* graph) {
  if (graph == nullptr) return;
  graph->state.store(AUD_GRAPH_DISPOSED, std::memory_order_seq_cst);
  waitForRender(graph);
  graph->stopNotifier.store(true, std::memory_order_release);
  graph->wake.post();
  if (graph->notifier.joinable()) graph->notifier.join();
  releaseProgram(graph, graph->pending.exchange(nullptr));
  freeRetiredPrograms(graph);
  releaseProgram(graph, graph->current);
  graph->current = nullptr;
  for (auto& instance : graph->instances) destroyInstance(graph, instance.get());
  graph->instances.clear();
  for (TransportProvider& provider : graph->providers) {
    if (provider.provider != nullptr) {
      provider.vtable->destroy(provider.provider);
    }
  }
  delete graph;
}

AUD_EXPORT int32_t aud_graph_prepare(AudGraph* graph, double sample_rate,
                                     uint32_t max_frames) {
  if (graph == nullptr || sample_rate < 0) return AUD_ERROR_INVALID_ARGUMENT;
  const uint32_t state = graph->state.load(std::memory_order_acquire);
  if (state == AUD_GRAPH_RUNNING || state == AUD_GRAPH_DISPOSED) {
    return AUD_ERROR_STATE;
  }
  if (sample_rate > 0) graph->sampleRate = sample_rate;
  if (max_frames > 0) graph->maxFrames = max_frames;
  if (graph->config.fade_frames == 0) {
    graph->fadeFrames = framesOf(kDefaultFadeSeconds, graph->sampleRate);
  }
  if (graph->config.max_tail_frames == 0) {
    graph->maxTailFrames = framesOf(kDefaultMaxTailSeconds, graph->sampleRate);
  }
  prepareRealtime(graph);
  for (auto& instance : graph->instances) {
    const int32_t result = prepareInstance(graph, instance.get());
    if (result != AUD_OK) return result;
    if (instance->instance != nullptr && instance->descriptor->vtable->reset) {
      instance->descriptor->vtable->reset(instance->instance, AUD_RESET_PREPARE);
    }
    instance->notes.count = 0;
  }
  resetTime(graph);
  resetTransport(graph);
  if (graph->hasProgram) {
    const int32_t result = recompile(graph);
    if (result != AUD_OK) return result;
  }
  if (state == AUD_GRAPH_CREATED) {
    graph->state.store(AUD_GRAPH_PREPARED, std::memory_order_release);
  }
  return AUD_OK;
}

AUD_EXPORT int32_t aud_graph_start(AudGraph* graph) {
  if (graph == nullptr) return AUD_ERROR_INVALID_ARGUMENT;
  const uint32_t state = graph->state.load(std::memory_order_acquire);
  if (state != AUD_GRAPH_CREATED && state != AUD_GRAPH_PREPARED &&
      state != AUD_GRAPH_STOPPED) {
    return AUD_ERROR_STATE;
  }
  graph->stateObserved = false;
  graph->state.store(AUD_GRAPH_RUNNING, std::memory_order_seq_cst);
  return AUD_OK;
}

AUD_EXPORT int32_t aud_graph_suspend(AudGraph* graph) {
  if (graph == nullptr) return AUD_ERROR_INVALID_ARGUMENT;
  if (graph->state.load(std::memory_order_acquire) != AUD_GRAPH_RUNNING) {
    return AUD_ERROR_STATE;
  }
  graph->state.store(AUD_GRAPH_SUSPENDED, std::memory_order_seq_cst);
  waitForRender(graph);
  return AUD_OK;
}

AUD_EXPORT int32_t aud_graph_resume(AudGraph* graph) {
  if (graph == nullptr) return AUD_ERROR_INVALID_ARGUMENT;
  if (graph->state.load(std::memory_order_acquire) != AUD_GRAPH_SUSPENDED) {
    return AUD_ERROR_STATE;
  }
  graph->stateObserved = false;
  graph->state.store(AUD_GRAPH_RUNNING, std::memory_order_seq_cst);
  return AUD_OK;
}

AUD_EXPORT int32_t aud_graph_stop(AudGraph* graph) {
  if (graph == nullptr) return AUD_ERROR_INVALID_ARGUMENT;
  const uint32_t state = graph->state.load(std::memory_order_acquire);
  if (state != AUD_GRAPH_RUNNING && state != AUD_GRAPH_SUSPENDED) {
    return AUD_ERROR_STATE;
  }
  graph->state.store(AUD_GRAPH_STOPPED, std::memory_order_seq_cst);
  waitForRender(graph);
  // No render is in flight: the instances can be reset here.
  for (auto& instance : graph->instances) {
    if (instance->instance != nullptr && instance->descriptor->vtable->reset) {
      instance->descriptor->vtable->reset(instance->instance, AUD_RESET_STOP);
    }
    instance->notes.count = 0;
  }
  graph->transport.playing = false;
  graph->publishedPlaying.store(0, std::memory_order_release);
  resetTime(graph);
  return AUD_OK;
}

AUD_EXPORT int32_t aud_graph_state(AudGraph* graph) {
  return graph == nullptr ? AUD_ERROR_INVALID_ARGUMENT
                          : static_cast<int32_t>(graph->state.load());
}

AUD_EXPORT double aud_graph_sample_rate(AudGraph* graph) {
  return graph == nullptr ? 0 : graph->sampleRate;
}

AUD_EXPORT uint32_t aud_graph_max_frames(AudGraph* graph) {
  return graph == nullptr ? 0 : graph->maxFrames;
}

// ############################################################################
// Node types and instances

AUD_EXPORT const AudHostApi* aud_graph_host_api(AudGraph* graph) {
  return graph == nullptr ? nullptr : &graph->hostApi;
}

AUD_EXPORT int32_t aud_graph_num_node_types(AudGraph* graph) {
  return graph == nullptr ? 0 : static_cast<int32_t>(graph->types.size());
}

AUD_EXPORT const AudNodeDescriptor* aud_graph_node_type(AudGraph* graph,
                                                        int32_t index) {
  if (graph == nullptr || index < 0 ||
      static_cast<size_t>(index) >= graph->types.size()) {
    return nullptr;
  }
  return graph->types[static_cast<size_t>(index)];
}

AUD_EXPORT const AudNodeDescriptor* aud_graph_node_type_by_id(
    AudGraph* graph, const char* type_id) {
  if (graph == nullptr || type_id == nullptr) return nullptr;
  return typeById(graph, type_id);
}

AUD_EXPORT int32_t aud_graph_create_node(AudGraph* graph, const char* type_id,
                                         const AudNodeConfig* config) {
  if (graph == nullptr || type_id == nullptr ||
      (config != nullptr && config->struct_size < sizeof(AudNodeConfig))) {
    return AUD_ERROR_INVALID_ARGUMENT;
  }
  if (graph->state.load(std::memory_order_acquire) == AUD_GRAPH_DISPOSED) {
    return AUD_ERROR_STATE;
  }
  const AudNodeDescriptor* type = typeById(graph, type_id);
  if (type == nullptr) return AUD_ERROR_UNKNOWN_TYPE;
  if (graph->instances.size() >= graph->config.max_nodes) {
    return AUD_ERROR_CAPACITY;
  }
  auto instance = std::make_unique<NodeInstance>();
  instance->descriptor = type;
  const AudNodeConfig empty{sizeof(AudNodeConfig), 0, nullptr, 0, nullptr, 0, 0};
  const AudNodeConfig& c = config == nullptr ? empty : *config;
  int32_t result =
      busChannels(type->input_buses, type->num_input_buses, c.num_input_buses,
                  c.input_channels, &instance->inputChannels);
  if (result != AUD_OK) return result;
  result = busChannels(type->output_buses, type->num_output_buses,
                       c.num_output_buses, c.output_channels,
                       &instance->outputChannels);
  if (result != AUD_OK) return result;
  if (type == &kFeedbackDescriptor) {
    instance->builtin = Builtin::feedback;
    if (instance->inputChannels[0] != instance->outputChannels[0]) {
      return AUD_ERROR_FORMAT;
    }
    instance->delayFrames = c.delay_frames;
  } else if (type == &kTapDescriptor) {
    instance->builtin = Builtin::tap;
  } else {
    instance->instance = type->vtable->create(type, &graph->hostApi);
    if (instance->instance == nullptr) return AUD_ERROR_FAILED;
  }
  result = prepareInstance(graph, instance.get());
  if (result != AUD_OK) {
    destroyInstance(graph, instance.get());
    return result;
  }
  instance->handle = graph->nextHandle++;
  const int32_t handle = instance->handle;
  graph->instances.push_back(std::move(instance));
  return handle;
}

AUD_EXPORT const AudNodeDescriptor* aud_graph_node_descriptor(AudGraph* graph,
                                                              int32_t node) {
  const NodeInstance* instance = instanceOf(graph, node);
  return instance == nullptr ? nullptr : instance->descriptor;
}

AUD_EXPORT int32_t aud_graph_node_channels(AudGraph* graph, int32_t node,
                                           uint32_t direction, uint32_t bus) {
  if (node == AUD_GRAPH_NODE && graph != nullptr) {
    const auto& list = direction == 0 ? graph->outputChannels
                                      : graph->inputChannels;
    return bus < list.size() ? static_cast<int32_t>(list[bus])
                             : AUD_ERROR_INVALID_ARGUMENT;
  }
  const NodeInstance* instance = instanceOf(graph, node);
  if (instance == nullptr) return AUD_ERROR_NOT_FOUND;
  const auto& list =
      direction == 0 ? instance->inputChannels : instance->outputChannels;
  return bus < list.size() ? static_cast<int32_t>(list[bus])
                           : AUD_ERROR_INVALID_ARGUMENT;
}

AUD_EXPORT int32_t aud_graph_nodes(AudGraph* graph, int32_t* handles,
                                   uint32_t capacity) {
  if (graph == nullptr || (handles == nullptr && capacity > 0)) {
    return AUD_ERROR_INVALID_ARGUMENT;
  }
  int32_t count = 0;
  for (auto& instance : graph->instances) {
    if (instance->retired) continue;
    if (static_cast<uint32_t>(count) < capacity) handles[count] = instance->handle;
    count += 1;
  }
  return count;
}

AUD_EXPORT int32_t aud_graph_node_latency(AudGraph* graph, int32_t node) {
  const NodeInstance* instance = instanceOf(graph, node);
  return instance == nullptr ? AUD_ERROR_NOT_FOUND
                             : static_cast<int32_t>(instance->latency);
}

AUD_EXPORT int32_t aud_graph_node_lead(AudGraph* graph, int32_t node) {
  const NodeInstance* instance = instanceOf(graph, node);
  return instance == nullptr ? AUD_ERROR_NOT_FOUND
                             : static_cast<int32_t>(instance->lead);
}

AUD_EXPORT int32_t aud_graph_output_latency(AudGraph* graph) {
  return graph == nullptr ? AUD_ERROR_INVALID_ARGUMENT
                          : static_cast<int32_t>(graph->outputLatency);
}

// ############################################################################
// Transactions

AUD_EXPORT int32_t aud_graph_begin(AudGraph* graph) {
  if (graph == nullptr) return AUD_ERROR_INVALID_ARGUMENT;
  if (graph->transactionOpen) return AUD_ERROR_STATE;
  freeRetiredPrograms(graph);
  collect(graph);
  graph->working = graph->topology;
  graph->workingRemoved.clear();
  graph->transactionOpen = true;
  return AUD_OK;
}

AUD_EXPORT int32_t aud_graph_connect(AudGraph* graph, int32_t from,
                                     uint32_t from_bus, int32_t to,
                                     uint32_t to_bus, uint32_t flags) {
  if (graph == nullptr) return AUD_ERROR_INVALID_ARGUMENT;
  if (!graph->transactionOpen) return AUD_ERROR_STATE;
  const int32_t checked = checkAudioEnds(graph, from, from_bus, to, to_bus);
  if (checked != AUD_OK) return checked;
  AudioEdgeId id{from, from_bus, to, to_bus, flags};
  for (AudioEdgeId& existing : graph->working.audio) {
    if (existing.sameEnds(id)) {
      existing.flags = flags;
      return AUD_OK;
    }
  }
  if (graph->working.audio.size() + graph->working.events.size() >=
      graph->config.max_connections) {
    return AUD_ERROR_CAPACITY;
  }
  graph->working.audio.push_back(id);
  return AUD_OK;
}

AUD_EXPORT int32_t aud_graph_disconnect(AudGraph* graph, int32_t from,
                                        uint32_t from_bus, int32_t to,
                                        uint32_t to_bus) {
  if (graph == nullptr) return AUD_ERROR_INVALID_ARGUMENT;
  if (!graph->transactionOpen) return AUD_ERROR_STATE;
  AudioEdgeId id{from, from_bus, to, to_bus, 0};
  auto& audio = graph->working.audio;
  for (auto it = audio.begin(); it != audio.end(); ++it) {
    if (!it->sameEnds(id)) continue;
    audio.erase(it);
    return AUD_OK;
  }
  return AUD_ERROR_NOT_FOUND;
}

AUD_EXPORT int32_t aud_graph_connect_events(AudGraph* graph, int32_t from,
                                            uint32_t from_port, int32_t to,
                                            uint32_t to_port) {
  if (graph == nullptr) return AUD_ERROR_INVALID_ARGUMENT;
  if (!graph->transactionOpen) return AUD_ERROR_STATE;
  const int32_t checked = checkEventEnds(graph, from, from_port, to, to_port);
  if (checked != AUD_OK) return checked;
  EventEdgeId id{from, from_port, to, to_port};
  for (const EventEdgeId& existing : graph->working.events) {
    if (existing == id) return AUD_OK;
  }
  if (graph->working.audio.size() + graph->working.events.size() >=
      graph->config.max_connections) {
    return AUD_ERROR_CAPACITY;
  }
  graph->working.events.push_back(id);
  return AUD_OK;
}

AUD_EXPORT int32_t aud_graph_disconnect_events(AudGraph* graph, int32_t from,
                                               uint32_t from_port, int32_t to,
                                               uint32_t to_port) {
  if (graph == nullptr) return AUD_ERROR_INVALID_ARGUMENT;
  if (!graph->transactionOpen) return AUD_ERROR_STATE;
  EventEdgeId id{from, from_port, to, to_port};
  auto& events = graph->working.events;
  for (auto it = events.begin(); it != events.end(); ++it) {
    if (!(*it == id)) continue;
    events.erase(it);
    return AUD_OK;
  }
  return AUD_ERROR_NOT_FOUND;
}

AUD_EXPORT int32_t aud_graph_remove_node(AudGraph* graph, int32_t node) {
  if (graph == nullptr) return AUD_ERROR_INVALID_ARGUMENT;
  if (!graph->transactionOpen) return AUD_ERROR_STATE;
  if (node == AUD_GRAPH_NODE) return AUD_ERROR_INVALID_ARGUMENT;
  const NodeInstance* instance = instanceOf(graph, node);
  if (!isLive(instance)) return AUD_ERROR_NOT_FOUND;
  auto& removed = graph->workingRemoved;
  if (std::find(removed.begin(), removed.end(), node) == removed.end()) {
    removed.push_back(node);
  }
  auto& audio = graph->working.audio;
  audio.erase(std::remove_if(audio.begin(), audio.end(),
                             [node](const AudioEdgeId& id) {
                               return id.from == node || id.to == node;
                             }),
              audio.end());
  auto& events = graph->working.events;
  events.erase(std::remove_if(events.begin(), events.end(),
                              [node](const EventEdgeId& id) {
                                return id.from == node || id.to == node;
                              }),
               events.end());
  return AUD_OK;
}

AUD_EXPORT int32_t aud_graph_commit(AudGraph* graph) {
  if (graph == nullptr) return AUD_ERROR_INVALID_ARGUMENT;
  if (!graph->transactionOpen) return AUD_ERROR_STATE;
  freeRetiredPrograms(graph);
  // Removed nodes retire; those that never rendered go at once.
  std::vector<NodeInstance*> retiring;
  std::vector<int32_t> dropped;
  for (int32_t handle : graph->workingRemoved) {
    NodeInstance* instance = instanceOf(graph, handle);
    if (instance == nullptr || instance->retired) continue;
    if (!instance->committed && instance->programs == 0) {
      dropped.push_back(handle);
      continue;
    }
    instance->retired = true;
    instance->retireSequence = graph->nextSequence - 1;
    retiring.push_back(instance);
  }
  for (int32_t handle : dropped) dropInstance(graph, handle);
  int32_t error = AUD_OK;
  Program* program = compileProgram(graph, graph->working, &error);
  if (program == nullptr) {
    for (NodeInstance* instance : retiring) instance->retired = false;
    return error;
  }
  graph->topology = graph->working;
  graph->workingRemoved.clear();
  graph->transactionOpen = false;
  publish(graph, program);
  return static_cast<int32_t>(program->revision);
}

AUD_EXPORT int32_t aud_graph_rollback(AudGraph* graph) {
  if (graph == nullptr) return AUD_ERROR_INVALID_ARGUMENT;
  if (!graph->transactionOpen) return AUD_ERROR_STATE;
  graph->transactionOpen = false;
  graph->workingRemoved.clear();
  return AUD_OK;
}

AUD_EXPORT uint32_t aud_graph_revision(AudGraph* graph) {
  return graph == nullptr ? 0 : graph->adoptedRevision.load();
}

// ############################################################################
// Commands

AUD_EXPORT int32_t aud_graph_set_param(AudGraph* graph, int32_t node,
                                       uint32_t param, float value,
                                       uint32_t ramp_frames) {
  NodeInstance* instance = instanceOf(graph, node);
  if (instance == nullptr) return AUD_ERROR_NOT_FOUND;
  if (instance->retired) return AUD_ERROR_RETIRED;
  const AudNodeDescriptor* d = instance->descriptor;
  if (param >= d->num_params || instance->instance == nullptr ||
      d->vtable->set_param == nullptr) {
    return AUD_ERROR_INVALID_ARGUMENT;
  }
  if (!instance->committed) {
    // Nothing renders the node yet: the change applies at once.
    d->vtable->set_param(instance->instance, param, value);
    return AUD_OK;
  }
  ParamCommand command;
  command.instance = instance;
  command.param = param;
  command.rampFrames = ramp_frames;
  command.value = value;
  command.sequence = graph->nextSequence;
  if (!graph->params->push(command)) {
    graph->rejected.fetch_add(1, std::memory_order_relaxed);
    return AUD_ERROR_QUEUE_FULL;
  }
  graph->nextSequence += 1;
  return AUD_OK;
}

AUD_EXPORT int32_t aud_graph_send_event(AudGraph* graph, int32_t node,
                                        const AudEvent* event,
                                        const AudTimestamp* at, uint32_t id) {
  if (graph == nullptr || event == nullptr ||
      event->struct_size < sizeof(AudEvent)) {
    return AUD_ERROR_INVALID_ARGUMENT;
  }
  NodeInstance* instance = nullptr;
  if (node != AUD_GRAPH_NODE) {
    instance = instanceOf(graph, node);
    if (instance == nullptr) return AUD_ERROR_NOT_FOUND;
    if (instance->retired) return AUD_ERROR_RETIRED;
    if (!instance->committed) return AUD_ERROR_STATE;
    if (event->port >= instance->descriptor->num_event_inputs) {
      return AUD_ERROR_INVALID_ARGUMENT;
    }
  } else if (event->port != 0) {
    return AUD_ERROR_INVALID_ARGUMENT;
  }
  Command command;
  command.kind = CommandKind::event;
  command.instance = instance;
  command.id = id;
  command.event = *event;
  command.event.struct_size = sizeof(AudEvent);
  command.event.sample_offset = 0;
  if (at == nullptr) {
    command.at.struct_size = sizeof(AudTimestamp);
    command.at.domain = AUD_TIME_IMMEDIATE;
  } else {
    command.at = *at;
    const int32_t checked = checkLookahead(graph, at);
    if (checked != AUD_OK) return checked;
    if (at->domain != AUD_TIME_IMMEDIATE &&
        graph->scheduledCount.load(std::memory_order_relaxed) >=
            graph->config.scheduler_capacity) {
      graph->rejected.fetch_add(1, std::memory_order_relaxed);
      return AUD_ERROR_CAPACITY;
    }
  }
  command.sequence = graph->nextSequence;
  if (!graph->commands->push(command)) {
    graph->rejected.fetch_add(1, std::memory_order_relaxed);
    return AUD_ERROR_QUEUE_FULL;
  }
  graph->nextSequence += 1;
  return AUD_OK;
}

AUD_EXPORT int32_t aud_graph_cancel(AudGraph* graph, int32_t node,
                                    uint32_t id) {
  if (graph == nullptr) return AUD_ERROR_INVALID_ARGUMENT;
  NodeInstance* instance = nullptr;
  if (node != AUD_GRAPH_NODE) {
    instance = instanceOf(graph, node);
    if (instance == nullptr) return AUD_ERROR_NOT_FOUND;
  }
  Command command;
  command.kind = CommandKind::cancel;
  command.instance = instance;
  command.id = id;
  command.sequence = graph->nextSequence;
  if (!graph->commands->push(command)) {
    graph->rejected.fetch_add(1, std::memory_order_relaxed);
    return AUD_ERROR_QUEUE_FULL;
  }
  graph->nextSequence += 1;
  return AUD_OK;
}

AUD_EXPORT int32_t aud_graph_set_string(AudGraph* graph, int32_t node,
                                        uint32_t key, const char* value) {
  NodeInstance* instance = instanceOf(graph, node);
  if (instance == nullptr) return AUD_ERROR_NOT_FOUND;
  if (value == nullptr) return AUD_ERROR_INVALID_ARGUMENT;
  if (instance->retired) return AUD_ERROR_RETIRED;
  const AudNodeDescriptor* d = instance->descriptor;
  if ((d->capabilities & AUD_NODE_CAP_STRINGS) == 0 ||
      instance->instance == nullptr || d->vtable->set_string == nullptr) {
    return AUD_ERROR_UNSUPPORTED;
  }
  return d->vtable->set_string(instance->instance, key, value);
}

AUD_EXPORT int32_t aud_graph_transport(AudGraph* graph,
                                       const AudTransportRequest* request) {
  if (graph == nullptr || request == nullptr ||
      request->struct_size < sizeof(AudTransportRequest) ||
      request->type < AUD_TRANSPORT_REQUEST_START ||
      request->type > AUD_TRANSPORT_REQUEST_SET_QUANTUM) {
    return AUD_ERROR_INVALID_ARGUMENT;
  }
  const int32_t checked = checkLookahead(graph, &request->at);
  if (checked != AUD_OK) return checked;
  Command command;
  command.kind = CommandKind::transport;
  command.request = *request;
  command.sequence = graph->nextSequence;
  if (!graph->commands->push(command)) {
    graph->rejected.fetch_add(1, std::memory_order_relaxed);
    return AUD_ERROR_QUEUE_FULL;
  }
  graph->nextSequence += 1;
  return AUD_OK;
}

AUD_EXPORT int32_t aud_graph_transport_state(AudGraph* graph,
                                             AudGraphTransportState* state) {
  if (graph == nullptr || state == nullptr ||
      state->struct_size < sizeof(AudGraphTransportState)) {
    return AUD_ERROR_INVALID_ARGUMENT;
  }
  state->playing = graph->publishedPlaying.load(std::memory_order_acquire);
  state->beat = graph->publishedBeat.load(std::memory_order_acquire);
  state->tempo = graph->publishedTempo.load(std::memory_order_acquire);
  state->numerator = graph->publishedNumerator.load(std::memory_order_acquire);
  state->denominator =
      graph->publishedDenominator.load(std::memory_order_acquire);
  state->looping = graph->publishedLooping.load(std::memory_order_acquire);
  state->loop_start = graph->publishedLoopStart.load(std::memory_order_acquire);
  state->loop_end = graph->publishedLoopEnd.load(std::memory_order_acquire);
  state->reserved = 0;
  return AUD_OK;
}

AUD_EXPORT int32_t aud_graph_set_transport_provider(AudGraph* graph,
                                                    const char* id) {
  if (graph == nullptr) return AUD_ERROR_INVALID_ARGUMENT;
  if (graph->state.load(std::memory_order_acquire) == AUD_GRAPH_RUNNING) {
    return AUD_ERROR_STATE;
  }
  if (id == nullptr || std::strcmp(id, AUD_GRAPH_INTERNAL_TRANSPORT_ID) == 0) {
    graph->selectedProvider = -1;
    return AUD_OK;
  }
  for (size_t i = 0; i < graph->providers.size(); ++i) {
    TransportProvider& provider = graph->providers[i];
    if (provider.id != id) continue;
    if (provider.provider == nullptr) {
      provider.provider = provider.vtable->create(&graph->hostApi);
      if (provider.provider == nullptr) return AUD_ERROR_FAILED;
    }
    provider.capabilities = provider.vtable->capabilities(provider.provider);
    graph->selectedProvider = static_cast<int32_t>(i);
    return AUD_OK;
  }
  return AUD_ERROR_NOT_FOUND;
}

AUD_EXPORT int64_t aud_graph_sample_position(AudGraph* graph) {
  return graph == nullptr ? 0 : graph->samplePosition.load();
}

// ############################################################################
// Rendering

AUD_EXPORT int32_t aud_graph_render(void* user, const AudRenderRequest* request) {
  auto* graph = static_cast<AudGraph*>(user);
  if (graph == nullptr || request == nullptr ||
      request->struct_size < sizeof(AudRenderRequest)) {
    return AUD_ERROR_INVALID_ARGUMENT;
  }
  // The handshake with the control thread: the flag goes up before the
  // state is read, the control thread sets the state and then waits for
  // the flag; with sequentially consistent order one of them sees the other.
  graph->inFlight.store(true, std::memory_order_seq_cst);
  const uint32_t state = graph->state.load(std::memory_order_seq_cst);
  int32_t result = AUD_OK;
  if (state == AUD_GRAPH_RUNNING) {
    result = renderBlock(graph, request);
  } else {
    for (uint32_t b = 0; b < request->num_output_buses; ++b) {
      const AudAudioBus& bus = request->outputs[b];
      for (uint32_t c = 0; c < bus.num_channels; ++c) {
        std::memset(bus.channels[c], 0, sizeof(float) * request->frames);
      }
    }
  }
  graph->inFlight.store(false, std::memory_order_seq_cst);
  return result;
}

// ############################################################################
// Notifications and taps

AUD_EXPORT int32_t aud_graph_set_listener(AudGraph* graph,
                                          AudGraphListener listener,
                                          void* user) {
  if (graph == nullptr) return AUD_ERROR_INVALID_ARGUMENT;
  graph->listenerUser.store(user, std::memory_order_release);
  graph->listener.store(listener, std::memory_order_release);
  return AUD_OK;
}

AUD_EXPORT int32_t aud_graph_take_notifications(AudGraph* graph,
                                                AudGraphNotification* out,
                                                uint32_t capacity) {
  if (graph == nullptr || (out == nullptr && capacity > 0)) {
    return AUD_ERROR_INVALID_ARGUMENT;
  }
  freeRetiredPrograms(graph);
  graph->wakePending.store(false, std::memory_order_seq_cst);
  uint32_t count = 0;
  bool finished = false;
  while (count < capacity && graph->notifications->pop(out[count])) {
    if (out[count].type == AUD_NOTIFY_NODE_DONE) finished = true;
    count += 1;
  }
  collect(graph);
  if (finished && !graph->transactionOpen && graph->hasProgram) {
    recompile(graph);
  }
  return static_cast<int32_t>(count);
}

AUD_EXPORT int32_t aud_graph_tap_read(AudGraph* graph, int32_t node,
                                      uint32_t channel, float* out,
                                      uint32_t frames) {
  NodeInstance* instance = instanceOf(graph, node);
  if (instance == nullptr) return AUD_ERROR_NOT_FOUND;
  if (instance->builtin != Builtin::tap || out == nullptr) {
    return AUD_ERROR_INVALID_ARGUMENT;
  }
  TapState& tap = instance->tap;
  if (channel >= tap.channels || frames > tap.ringFrames) {
    return AUD_ERROR_INVALID_ARGUMENT;
  }
  // A seqlock: the generation is odd while the realtime thread writes.
  for (int attempt = 0; attempt < 16; ++attempt) {
    const uint32_t before = tap.generation.load(std::memory_order_acquire);
    if (before & 1) continue;
    const uint32_t writePos = tap.writePos;
    const float* ring = tap.ring.data() + static_cast<size_t>(channel) * tap.ringFrames;
    for (uint32_t i = 0; i < frames; ++i) {
      const uint32_t position =
          (writePos + tap.ringFrames - frames + i) % tap.ringFrames;
      out[i] = ring[position];
    }
    std::atomic_thread_fence(std::memory_order_acquire);
    if (tap.generation.load(std::memory_order_acquire) == before) return AUD_OK;
  }
  return AUD_ERROR_FAILED;
}

AUD_EXPORT int32_t aud_graph_tap_meter(AudGraph* graph, int32_t node,
                                       uint32_t channel, float* peak,
                                       float* rms) {
  NodeInstance* instance = instanceOf(graph, node);
  if (instance == nullptr) return AUD_ERROR_NOT_FOUND;
  if (instance->builtin != Builtin::tap || peak == nullptr || rms == nullptr) {
    return AUD_ERROR_INVALID_ARGUMENT;
  }
  const TapState& tap = instance->tap;
  if (channel >= tap.channels) return AUD_ERROR_INVALID_ARGUMENT;
  *peak = tap.peak[channel].load(std::memory_order_acquire);
  *rms = tap.rms[channel].load(std::memory_order_acquire);
  return AUD_OK;
}

AUD_EXPORT int32_t aud_graph_get_stats(AudGraph* graph, AudGraphStats* stats) {
  if (graph == nullptr || stats == nullptr ||
      stats->struct_size < sizeof(AudGraphStats)) {
    return AUD_ERROR_INVALID_ARGUMENT;
  }
  stats->state = graph->state.load(std::memory_order_acquire);
  stats->revision = graph->adoptedRevision.load(std::memory_order_acquire);
  stats->scheduled = graph->scheduledCount.load(std::memory_order_acquire);
  stats->blocks_rendered = graph->blocksRendered.load(std::memory_order_relaxed);
  stats->frames_rendered = graph->framesRendered.load(std::memory_order_relaxed);
  stats->render_time_max_ns = graph->renderMax.load(std::memory_order_relaxed);
  stats->render_time_sum_ns = graph->renderSum.load(std::memory_order_relaxed);
  stats->events_delivered =
      graph->eventsDelivered.load(std::memory_order_relaxed);
  stats->events_late = graph->eventsLate.load(std::memory_order_relaxed);
  stats->events_dropped = graph->eventsDropped.load(std::memory_order_relaxed);
  stats->params_applied = graph->paramsApplied.load(std::memory_order_relaxed);
  stats->rejected = graph->rejected.load(std::memory_order_relaxed);
  stats->notifications_dropped =
      graph->notificationsDropped.load(std::memory_order_relaxed);
  stats->overloads = graph->overloads.load(std::memory_order_relaxed);
  stats->time_filter_resets =
      graph->filterResets.load(std::memory_order_relaxed);
  stats->reserved = 0;
  stats->output_peak = graph->outputPeak.load(std::memory_order_relaxed);
  return AUD_OK;
}

AUD_EXPORT void aud_graph_reset_stats(AudGraph* graph) {
  if (graph == nullptr) return;
  graph->blocksRendered.store(0, std::memory_order_relaxed);
  graph->framesRendered.store(0, std::memory_order_relaxed);
  graph->renderMax.store(0, std::memory_order_relaxed);
  graph->renderSum.store(0, std::memory_order_relaxed);
  graph->eventsDelivered.store(0, std::memory_order_relaxed);
  graph->eventsLate.store(0, std::memory_order_relaxed);
  graph->eventsDropped.store(0, std::memory_order_relaxed);
  graph->paramsApplied.store(0, std::memory_order_relaxed);
  graph->rejected.store(0, std::memory_order_relaxed);
  graph->notificationsDropped.store(0, std::memory_order_relaxed);
  graph->overloads.store(0, std::memory_order_relaxed);
  graph->filterResets.store(0, std::memory_order_relaxed);
  graph->outputPeak.store(0, std::memory_order_relaxed);
}
