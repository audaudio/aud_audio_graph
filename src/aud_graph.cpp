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
#include "aud_graph_resources.hpp"
#include "aud_graph_watchdog.hpp"
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
  // The arrays a count names exist, and the ids the host and the documents
  // look parameters and string keys up by are set (ticket 20).
  if ((d->num_input_buses > 0 && d->input_buses == nullptr) ||
      (d->num_output_buses > 0 && d->output_buses == nullptr) ||
      (d->num_event_inputs > 0 && d->event_inputs == nullptr) ||
      (d->num_event_outputs > 0 && d->event_outputs == nullptr) ||
      (d->num_params > 0 && d->params == nullptr) ||
      (d->num_string_keys > 0 && d->string_keys == nullptr)) {
    return AUD_ERROR_INVALID_ARGUMENT;
  }
  for (uint32_t i = 0; i < d->num_params; ++i) {
    if (d->params[i].id == nullptr) return AUD_ERROR_INVALID_ARGUMENT;
  }
  for (uint32_t i = 0; i < d->num_string_keys; ++i) {
    if (d->string_keys[i].id == nullptr) return AUD_ERROR_INVALID_ARGUMENT;
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

void* hostAlloc(void*, size_t bytes) {
  watchdogCheck(AUD_GRAPH_VIOLATION_ALLOC);
  return std::malloc(bytes);
}

void hostFree(void*, void* memory) {
  watchdogCheck(AUD_GRAPH_VIOLATION_FREE);
  std::free(memory);
}

void hostLog(void*, int32_t level, const char* message) {
  watchdogCheck(AUD_GRAPH_VIOLATION_LOG);
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
// Loads (lifecycle-002, ticket 25). A load and its notifications change
// under loadsMutex, so that the listener hears the notifications of a load
// in their order - a progress never after the end. The helpers expect the
// lock held unless they say that they take it.

AudGraphNotification loadNotification(const AudGraph* graph, const Load& load) {
  AudGraphNotification n{};
  n.struct_size = sizeof(AudGraphNotification);
  n.type = AUD_NOTIFY_LOAD;
  n.node = load.node;
  n.code = load.result.load(std::memory_order_acquire);
  n.value = load.id;
  n.number = load.progress.load(std::memory_order_acquire);
  n.sample_position = graph->samplePosition.load(std::memory_order_acquire);
  n.event.struct_size = sizeof(AudEvent);
  return n;
}

// Queues a notification of a load for the control thread, taken with those
// of the realtime thread; the caller wakes the listener once the lock is
// released. A host that never takes them keeps no more than a begin and an
// end per load: a progress replaces the progress of the same load that
// still waits, and is dropped while the queue holds as many as the queue of
// the realtime thread.
void queueLoadNotification(AudGraph* graph, const Load& load, bool progress) {
  auto& queue = graph->controlNotifications;
  const AudGraphNotification n = loadNotification(graph, load);
  if (progress) {
    for (auto it = queue.rbegin(); it != queue.rend(); ++it) {
      if (it->progress && it->notification.value == load.id) {
        it->notification = n;
        return;
      }
    }
    if (queue.size() >= graph->config.notification_capacity) {
      graph->notificationsDropped.fetch_add(1, std::memory_order_relaxed);
      return;
    }
  }
  queue.push_back(ControlNotification{n, progress});
}

// The load `id` of a graph.
Load* findLoad(AudGraph* graph, int32_t id) {
  for (auto& load : graph->loads) {
    if (load->id == id) return load.get();
  }
  return nullptr;
}

// The load `id` while it runs; NULL for one that ended or that the graph
// keeps no longer.
Load* runningLoad(AudGraph* graph, int32_t id) {
  Load* load = findLoad(graph, id);
  if (load == nullptr ||
      load->result.load(std::memory_order_acquire) != AUD_PENDING) {
    return nullptr;
  }
  return load;
}

// Keeps the loads that run and the last kKeptLoads that ended; the oldest
// that ended beyond them go.
void keepEndedLoads(AudGraph* graph) {
  auto& loads = graph->loads;
  size_t ended = 0;
  for (auto& load : loads) {
    if (load->result.load(std::memory_order_relaxed) != AUD_PENDING) ended += 1;
  }
  for (auto it = loads.begin(); ended > kKeptLoads && it != loads.end();) {
    if ((*it)->result.load(std::memory_order_relaxed) != AUD_PENDING) {
      it = loads.erase(it);
      ended -= 1;
    } else {
      ++it;
    }
  }
}

// Ends a running load with `result` and `message` and queues its end.
void endLoad(AudGraph* graph, Load& load, int32_t result, const char* message) {
  // AUD_PENDING is the state of a running load, never its result.
  load.result.store(result == AUD_PENDING ? AUD_ERROR_FAILED : result,
                    std::memory_order_release);
  if (result == AUD_OK) load.progress.store(1.0f, std::memory_order_release);
  load.message = message == nullptr ? "" : message;
  load.endNs = aud_clock_now_ns();
  queueLoadNotification(graph, load, false);
}

// Asks the running loads of a removed node to stop. Takes loadsMutex.
void cancelLoadsOf(AudGraph* graph, int32_t node) {
  if (node <= 0) return;
  std::lock_guard<std::mutex> lock(graph->loadsMutex);
  for (auto& load : graph->loads) {
    if (load->node == node &&
        load->result.load(std::memory_order_acquire) == AUD_PENDING) {
      load->cancelled.store(true, std::memory_order_release);
    }
  }
}

// Ends the loads a destroyed node left running as cancelled: nothing is
// left to end them, and a wait for the loads would never return. An
// end_load of the package that comes later finds the load ended and is
// ignored. Takes loadsMutex.
void endLoadsOf(AudGraph* graph, int32_t node) {
  if (node <= 0) return;
  bool ended = false;
  {
    std::lock_guard<std::mutex> lock(graph->loadsMutex);
    for (auto& load : graph->loads) {
      if (load->node != node ||
          load->result.load(std::memory_order_acquire) != AUD_PENDING) {
        continue;
      }
      load->cancelled.store(true, std::memory_order_release);
      endLoad(graph, *load, AUD_ERROR_CANCELLED, "the node was removed");
      ended = true;
    }
    if (ended) keepEndedLoads(graph);
  }
  if (!ended) return;
  graph->loadsChanged.notify_all();
  wakeListener(graph);
}

// ............................................................................
// The host api of minor 4 (ticket 25): tasks, loads, caches, diagnostics
// and messages.

// The graph counts the tasks its nodes post, so that it can wait for them
// when it goes.
int32_t hostPostTask(void* host, const AudTask* task) {
  auto* graph = static_cast<AudGraph*>(host);
  if (graph == nullptr || task == nullptr) return AUD_ERROR_INVALID_ARGUMENT;
  return Resources::get().post(*task, &graph->tasks);
}

// The handle of the node whose package instance `instance` is; 0 for one
// the graph does not know. A load begun while aud_graph_create_node runs
// the package's create - which has not returned the instance yet - or its
// first prepare belongs to the node being created.
int32_t handleOf(AudGraph* graph, const void* instance) {
  for (auto& node : graph->instances) {
    if (node->instance != nullptr && node->instance == instance) {
      return node->handle;
    }
  }
  const NodeInstance* creating = graph->creating;
  if (creating != nullptr &&
      (creating->instance == nullptr || creating->instance == instance)) {
    return creating->handle;
  }
  return 0;
}

int32_t hostBeginLoad(void* host, void* instance, const char* name) {
  watchdogCheck(AUD_GRAPH_VIOLATION_CONTROL);
  auto* graph = static_cast<AudGraph*>(host);
  if (graph == nullptr) return AUD_ERROR_INVALID_ARGUMENT;
  auto load = std::make_unique<Load>();
  load->id = Resources::get().nextLoadId();
  load->node = handleOf(graph, instance);
  load->name = name == nullptr ? "" : name;
  load->startNs = aud_clock_now_ns();
  const int32_t id = load->id;
  {
    std::lock_guard<std::mutex> lock(graph->loadsMutex);
    queueLoadNotification(graph, *load, false);
    graph->loads.push_back(std::move(load));
  }
  wakeListener(graph);
  return id;
}

void hostLoadProgress(void* host, int32_t id, float progress) {
  watchdogCheck(AUD_GRAPH_VIOLATION_CONTROL);
  auto* graph = static_cast<AudGraph*>(host);
  if (graph == nullptr) return;
  {
    std::lock_guard<std::mutex> lock(graph->loadsMutex);
    Load* load = runningLoad(graph, id);
    if (load == nullptr) return;
    progress = std::min(1.0f, std::max(0.0f, progress));
    load->progress.store(progress, std::memory_order_release);
    const int64_t now = aud_clock_now_ns();
    if (progress - load->notifiedProgress < kLoadProgressStep &&
        now - load->notifiedNs < kLoadProgressIntervalNs) {
      return;
    }
    load->notifiedProgress = progress;
    load->notifiedNs = now;
    queueLoadNotification(graph, *load, true);
  }
  wakeListener(graph);
}

void hostEndLoad(void* host, int32_t id, int32_t result, const char* message) {
  watchdogCheck(AUD_GRAPH_VIOLATION_CONTROL);
  auto* graph = static_cast<AudGraph*>(host);
  if (graph == nullptr) return;
  {
    std::lock_guard<std::mutex> lock(graph->loadsMutex);
    Load* load = runningLoad(graph, id);
    if (load == nullptr) return;
    endLoad(graph, *load, result, message);
    keepEndedLoads(graph);
  }
  graph->loadsChanged.notify_all();
  wakeListener(graph);
}

int32_t hostLoadCancelled(void* host, int32_t id) {
  watchdogCheck(AUD_GRAPH_VIOLATION_CONTROL);
  auto* graph = static_cast<AudGraph*>(host);
  if (graph == nullptr) return 0;
  std::lock_guard<std::mutex> lock(graph->loadsMutex);
  Load* load = findLoad(graph, id);
  return load != nullptr && load->cancelled.load(std::memory_order_acquire)
             ? 1
             : 0;
}

int32_t hostRegisterCache(void*, const AudCacheVTable* vtable, void* cache) {
  watchdogCheck(AUD_GRAPH_VIOLATION_CONTROL);
  return Resources::get().registerCache(vtable, cache);
}

void hostUnregisterCache(void*, void* cache) {
  watchdogCheck(AUD_GRAPH_VIOLATION_CONTROL);
  Resources::get().unregisterCache(cache);
}

void hostCacheChanged(void*, void* cache, int64_t delta) {
  Resources::get().cacheChanged(cache, delta);
}

uint64_t hostCacheBudget(void*) { return Resources::get().budget(); }

void hostReport(void* host, void* instance, int32_t code, uint32_t count) {
  auto* graph = static_cast<AudGraph*>(host);
  if (graph == nullptr) return;
  reportNode(graph, instance, code, count);
}

void hostEmitMessage(void* host, void* instance, const AudMessage* message) {
  auto* graph = static_cast<AudGraph*>(host);
  if (graph == nullptr) return;
  emitMessage(graph, instance, message);
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

// Destroys an instance that no block renders any more, with what it leaves
// behind (ticket 25): the messages its wrapper holds give their slots back,
// its running loads are asked to stop before the node goes and end as
// cancelled after it - also those of a node whose create failed.
void destroyInstance(AudGraph* graph, NodeInstance* instance) {
  releaseWrapped(graph, instance);
  cancelLoadsOf(graph, instance->handle);
  if (instance->instance != nullptr) {
    instance->descriptor->vtable->destroy(instance->instance);
    instance->instance = nullptr;
  }
  endLoadsOf(graph, instance->handle);
}

// Frees what no program references any more: finished instances - unless
// a source that is not yet freed targets them (ticket 26) - and the edges
// that faded out.
void collect(AudGraph* graph) {
  collectSources(graph);
  for (auto it = graph->instances.begin(); it != graph->instances.end();) {
    NodeInstance* instance = it->get();
    if (instance->retired && instance->programs == 0 &&
        instance->done.load(std::memory_order_acquire) &&
        !sourceTargets(graph, instance)) {
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
  // An event route of a retiring emitter goes with either of its ends.
  auto& routes = graph->zombieEventEdges;
  routes.erase(std::remove_if(routes.begin(), routes.end(),
                              [graph](const EventEdgeId& id) {
                                return instanceOf(graph, id.from) == nullptr ||
                                       (id.to != AUD_GRAPH_NODE &&
                                        instanceOf(graph, id.to) == nullptr);
                              }),
               routes.end());
}

// Publishes a compiled program; the realtime thread adopts it at the next
// block start.
void publish(AudGraph* graph, Program* program) {
  program->revision = graph->nextRevision++;
  graph->hasProgram = true;
  graph->outputLatency = program->outputLatency;
  graph->outputTail = program->outputTail;
  for (ProgramNode& node : program->nodes) {
    if (!node.instance->retired) node.instance->committed = true;
  }
  freeRetiredPrograms(graph);
  // A pending program the realtime thread never saw is dropped.
  releaseProgram(graph,
                 graph->pending.exchange(program, std::memory_order_acq_rel));
}

// Reads the tails of the live nodes again: a tail may follow what a node
// loaded after its prepare, like the release of the sampler (ticket 25).
// The realtime thread reads the tail of an instance only once a program
// that retires it is published, after the last write here. True when a
// tail changed.
bool refreshTails(AudGraph* graph) {
  bool changed = false;
  for (auto& instance : graph->instances) {
    if (instance->retired || instance->builtin != Builtin::none) continue;
    const AudNodeDescriptor* d = instance->descriptor;
    if (d == nullptr || instance->instance == nullptr) continue;
    if ((d->capabilities & AUD_NODE_CAP_TAIL) && d->vtable->get_tail) {
      const uint32_t tail = d->vtable->get_tail(instance->instance);
      changed = changed || tail != instance->tail;
      instance->tail = tail;
    }
  }
  return changed;
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

// Runs an [offline] call of the ABI on an instance (ticket 20): while the
// graph runs and a program contains the instance, the instance is parked
// first - the realtime thread skips it from the next block on - and the
// render in flight is waited for, so the call never overlaps a process
// call. The flag and the handshake are sequentially consistent, like the
// state and the in-flight flag they follow.
template <class Call>
int32_t withNodeParked(AudGraph* graph, NodeInstance* instance, Call call) {
  if (graph->state.load(std::memory_order_seq_cst) != AUD_GRAPH_RUNNING ||
      !instance->committed) {
    return call();
  }
  instance->parked.store(true, std::memory_order_seq_cst);
  waitForRender(graph);
  const int32_t result = call();
  instance->parked.store(false, std::memory_order_seq_cst);
  return result;
}

// The checks the state calls share; NULL with the error written.
NodeInstance* stateInstance(AudGraph* graph, int32_t node, int32_t* error) {
  NodeInstance* instance = instanceOf(graph, node);
  if (instance == nullptr) {
    *error = AUD_ERROR_NOT_FOUND;
    return nullptr;
  }
  if (instance->retired) {
    *error = AUD_ERROR_RETIRED;
    return nullptr;
  }
  const AudNodeDescriptor* d = instance->descriptor;
  if ((d->capabilities & AUD_NODE_CAP_STATE) == 0 ||
      instance->instance == nullptr || d->vtable->save_state == nullptr ||
      d->vtable->load_state == nullptr) {
    *error = AUD_ERROR_UNSUPPORTED;
    return nullptr;
  }
  return instance;
}

// Pushes a command into the event queue with the next sequence number; a
// full queue refuses, nothing is dropped silently (interop-002).
int32_t enqueueCommand(AudGraph* graph, Command command) {
  command.sequence = graph->nextSequence;
  if (!graph->commands->push(command)) {
    graph->rejected.fetch_add(1, std::memory_order_relaxed);
    return AUD_ERROR_QUEUE_FULL;
  }
  graph->nextSequence += 1;
  return AUD_OK;
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
    tap.ringFrames = std::max(maxFrames * 2, kMinTapRingFrames);
    const size_t samples = static_cast<size_t>(tap.ringFrames) * tap.channels;
    tap.ring.reset(new std::atomic<float>[samples]);
    for (size_t i = 0; i < samples; ++i) {
      tap.ring[i].store(0.0f, std::memory_order_relaxed);
    }
    tap.writePos.store(0, std::memory_order_relaxed);
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
  // The wrapper of the old block size goes with the block it was filling;
  // no block renders meanwhile, so the control thread frees its messages.
  releaseWrapped(graph, instance);
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

// Creates the package's node of a new instance - the engine renders its
// own types itself - and prepares it.
int32_t createAndPrepare(AudGraph* graph, NodeInstance* instance) {
  if (instance->builtin == Builtin::none) {
    const AudNodeDescriptor* type = instance->descriptor;
    instance->instance = type->vtable->create(type, &graph->hostApi);
    if (instance->instance == nullptr) return AUD_ERROR_FAILED;
  }
  return prepareInstance(graph, instance);
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

// Puts an event of a command into the event queue at `at` (NULL: the next
// block): a place in the scheduler is reserved for an event with a time.
int32_t sendCommandEvent(AudGraph* graph, NodeInstance* instance,
                         const AudEvent& event, const AudTimestamp* at,
                         uint32_t id) {
  Command command;
  command.kind = CommandKind::event;
  command.instance = instance;
  command.id = id;
  command.event = event;
  command.event.struct_size = sizeof(AudEvent);
  command.event.sample_offset = 0;
  if (at == nullptr) {
    command.at.struct_size = sizeof(AudTimestamp);
    command.at.domain = AUD_TIME_IMMEDIATE;
  } else {
    command.at = *at;
    const int32_t checked = checkLookahead(graph, at);
    if (checked != AUD_OK) return checked;
    command.reserved = at->domain != AUD_TIME_IMMEDIATE;
  }
  if (command.reserved) {
    // A place in the scheduler is reserved until the event leaves it, so
    // that a burst within one block cannot overrun the scheduler. The
    // realtime thread only gives places back: the check cannot be late.
    if (graph->schedulerReserved.load(std::memory_order_acquire) >=
        graph->config.scheduler_capacity) {
      graph->rejected.fetch_add(1, std::memory_order_relaxed);
      return AUD_ERROR_CAPACITY;
    }
    graph->schedulerReserved.fetch_add(1, std::memory_order_acq_rel);
  }
  const int32_t result = enqueueCommand(graph, command);
  if (result != AUD_OK && command.reserved) {
    graph->schedulerReserved.fetch_sub(1, std::memory_order_acq_rel);
  }
  return result;
}

// A free message slot, searched from where the last search stopped;
// UINT32_MAX when every slot waits for its node (ticket 25).
uint32_t findMessageSlot(AudGraph* graph) {
  for (uint32_t i = 0; i < graph->numMessageSlots; ++i) {
    const uint32_t index = (graph->nextMessageSlot + i) % graph->numMessageSlots;
    if (!graph->messageSlots[index].busy.load(std::memory_order_acquire)) {
      graph->nextMessageSlot = (index + 1) % graph->numMessageSlots;
      return index;
    }
  }
  return UINT32_MAX;
}

// Writes a message into a slot and counts its bytes in the layout of
// AudGraphMessage - the path, then per argument a tag and the value -
// against AUD_GRAPH_MESSAGE_BYTES (osc-002); the slot keeps the path, the
// texts and the blobs, a part of them. No check takes a sum, which could
// wrap on a 32-bit target: a blob of almost 4 GiB would pass it.
struct MessageWriter {
  explicit MessageWriter(MessageSlot& target) : slot(target) {}

  MessageSlot& slot;
  size_t packed = 0;  // the bytes of the layout
  size_t stored = 0;  // the bytes of the slot

  // Counts `size` bytes of the layout; false when they do not fit.
  bool count(size_t size) {
    if (size > kMessageBytes - packed) return false;
    packed += size;
    return true;
  }

  // Counts and keeps `size` bytes; NULL when they do not fit.
  const char* store(const void* bytes, size_t size) {
    if (!count(size)) return nullptr;
    char* target = slot.data + stored;
    if (size > 0) std::memcpy(target, bytes, size);
    stored += size;
    return target;
  }
};

// Copies an argument into its slot - a text or a blob into the bytes - and
// counts its tag and its value; AUD_ERROR_CAPACITY when they do not fit,
// AUD_ERROR_INVALID_ARGUMENT for an unknown type or a blob without bytes.
int32_t copyArgument(MessageWriter& writer, const AudMessageArg& arg,
                     AudMessageArg& target) {
  target = arg;
  target.struct_size = sizeof(AudMessageArg);
  bool fits = writer.count(1);  // the tag
  switch (arg.type) {
    case AUD_ARG_INT32:
    case AUD_ARG_FLOAT:
      fits = fits && writer.count(4);
      break;
    case AUD_ARG_INT64:
    case AUD_ARG_DOUBLE:
      fits = fits && writer.count(8);
      break;
    case AUD_ARG_TRUE:
    case AUD_ARG_FALSE:
    case AUD_ARG_NIL:
    case AUD_ARG_IMPULSE:
      break;
    case AUD_ARG_STRING: {
      const char* s = arg.value.s == nullptr ? "" : arg.value.s;
      target.value.s = fits ? writer.store(s, std::strlen(s) + 1) : nullptr;
      fits = target.value.s != nullptr;
      break;
    }
    case AUD_ARG_BLOB:
      if (arg.size > 0 && arg.value.b == nullptr) {
        return AUD_ERROR_INVALID_ARGUMENT;
      }
      // Its size, then its bytes.
      fits = fits && writer.count(4);
      target.value.b = fits ? writer.store(arg.value.b, arg.size) : nullptr;
      fits = target.value.b != nullptr;
      break;
    default:
      return AUD_ERROR_INVALID_ARGUMENT;
  }
  return fits ? AUD_OK : AUD_ERROR_CAPACITY;
}

// Copies a message into a slot; AUD_ERROR_CAPACITY for more than
// AUD_GRAPH_MAX_MESSAGE_ARGS arguments or more than AUD_GRAPH_MESSAGE_BYTES,
// AUD_ERROR_INVALID_ARGUMENT for an argument the layout cannot carry.
int32_t copyMessage(MessageSlot& slot, const AudMessage& message) {
  if (message.num_args > kMaxMessageArgs) return AUD_ERROR_CAPACITY;
  MessageWriter writer{slot};
  const char* path = writer.store(message.path, std::strlen(message.path) + 1);
  if (path == nullptr) return AUD_ERROR_CAPACITY;
  for (uint32_t i = 0; i < message.num_args; ++i) {
    const int32_t copied = copyArgument(writer, message.args[i], slot.args[i]);
    if (copied != AUD_OK) return copied;
  }
  slot.message.struct_size = sizeof(AudMessage);
  slot.message.num_args = message.num_args;
  slot.message.path = path;
  slot.message.args = slot.args;
  return AUD_OK;
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
                    hostEmitEvent,
                    hostPostTask,
                    hostBeginLoad,
                    hostLoadProgress,
                    hostEndLoad,
                    hostLoadCancelled,
                    hostRegisterCache,
                    hostUnregisterCache,
                    hostCacheChanged,
                    hostCacheBudget,
                    hostReport,
                    hostEmitMessage};
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
  c.message_capacity = orDefault(c.message_capacity, kDefaultMessageCapacity);
  c.reply_capacity = orDefault(c.reply_capacity, kDefaultReplyCapacity);
  graph->numMessageSlots = c.message_capacity;
  graph->messageSlots.reset(new MessageSlot[c.message_capacity]);
  graph->replies =
      std::make_unique<AudSpscQueue<AudGraphMessage>>(c.reply_capacity);
  graph->scheduler.init(c.scheduler_capacity);
  graph->popped.resize(c.max_events_per_block);
  // The block's events: a budget of queued events, a budget of scheduled
  // ones, a budget of the event sources, the note offs of one full tracker
  // and the pedal offs of one release (ticket 26); what more a block's
  // releases need carries over to the next block.
  graph->blockEvents.resize(3 * c.max_events_per_block + NoteTracker::kCapacity +
                            kMaxPedalOffs);
  // The events the nodes emit in a block, as many as the block's events:
  // a node that hands its events on - a channel split behind a full block
  // - hands on every one of them (ticket 37). An event takes one entry per
  // connection it leaves on; beyond the arena it is dropped and counted.
  graph->emitted.resize(graph->blockEvents.size());
  graph->nodeEvents.resize(graph->blockEvents.size() + graph->emitted.size());
  graph->deferred.resize(graph->nodeEvents.size());
  graph->nodeRanges.resize(c.max_nodes);
  prepareRealtime(graph);
  resetTransport(graph);
  graph->state.store(AUD_GRAPH_CREATED, std::memory_order_release);
  graph->notifier = std::thread(notifierLoop, graph);
  Resources::get().attach();
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
  destroySources(graph);
  for (auto& instance : graph->instances) destroyInstance(graph, instance.get());
  graph->instances.clear();
  for (TransportProvider& provider : graph->providers) {
    if (provider.provider != nullptr) {
      provider.vtable->destroy(provider.provider);
    }
  }
  // The nodes are gone; tasks they posted may still run and report to the
  // graph, so the graph waits until they have run - only its own, the tasks
  // of other graphs go on (ticket 25). The last graph of the process stops
  // the workers.
  Resources::get().waitTasks(graph->tasks, 0);
  Resources::get().detach();
  delete graph;
}

AUD_EXPORT int32_t aud_graph_prepare(AudGraph* graph, double sample_rate,
                                     uint32_t max_frames) {
  if (graph == nullptr || sample_rate < 0) return AUD_ERROR_INVALID_ARGUMENT;
  const uint32_t state = graph->state.load(std::memory_order_acquire);
  if (state == AUD_GRAPH_RUNNING || state == AUD_GRAPH_DISPOSED) {
    return AUD_ERROR_STATE;
  }
  // The beat at the current position, computed with the old rate.
  anchorTransport(graph);
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
  }
  // The tracked notes get their note offs with the first block after the
  // restart; the transport keeps its position and its pending requests.
  graph->closeNotesPending = true;
  resetTime(graph);
  publishTransportState(graph);
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
  // No render is in flight: the instances can be reset here; their tracked
  // notes get note offs with the first block after the restart.
  for (auto& instance : graph->instances) {
    if (instance->instance != nullptr && instance->descriptor->vtable->reset) {
      instance->descriptor->vtable->reset(instance->instance, AUD_RESET_STOP);
    }
  }
  graph->closeNotesPending = true;
  // The transport stops where it is.
  anchorTransport(graph);
  graph->transport.playing = false;
  publishTransportState(graph);
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
  instance->passesEvents =
      (type->capabilities & AUD_NODE_CAP_PASSES_EVENTS) != 0;
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
  }
  // The handle exists before the package creates and prepares the node, so
  // that a load it begins meanwhile carries its node (ticket 25). A node
  // that fails spends its handle: handles are never reused.
  instance->handle = graph->nextHandle++;
  graph->creating = instance.get();
  result = createAndPrepare(graph, instance.get());
  graph->creating = nullptr;
  if (result != AUD_OK) {
    destroyInstance(graph, instance.get());
    return result;
  }
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

AUD_EXPORT uint32_t aud_graph_output_tail(AudGraph* graph) {
  return graph == nullptr ? 0 : graph->outputTail;
}

AUD_EXPORT int32_t aud_graph_node_save_state(AudGraph* graph, int32_t node,
                                             void* buffer, size_t capacity,
                                             size_t* size) {
  if (graph == nullptr || size == nullptr ||
      (buffer == nullptr && capacity > 0)) {
    return AUD_ERROR_INVALID_ARGUMENT;
  }
  int32_t error = AUD_OK;
  NodeInstance* instance = stateInstance(graph, node, &error);
  if (instance == nullptr) return error;
  return withNodeParked(graph, instance, [&] {
    return instance->descriptor->vtable->save_state(instance->instance, buffer,
                                                    capacity, size);
  });
}

AUD_EXPORT int32_t aud_graph_node_load_state(AudGraph* graph, int32_t node,
                                             const void* data, size_t size,
                                             uint32_t version) {
  if (graph == nullptr || (data == nullptr && size > 0)) {
    return AUD_ERROR_INVALID_ARGUMENT;
  }
  int32_t error = AUD_OK;
  NodeInstance* instance = stateInstance(graph, node, &error);
  if (instance == nullptr) return error;
  return withNodeParked(graph, instance, [&] {
    return instance->descriptor->vtable->load_state(instance->instance, data,
                                                    size, version);
  });
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
  graph->workingDroppedEvents.clear();
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
  auto& dropped = graph->workingDroppedEvents;
  events.erase(std::remove_if(events.begin(), events.end(),
                              [node, &dropped](const EventEdgeId& id) {
                                const bool drop =
                                    id.from == node || id.to == node;
                                if (drop) dropped.push_back(id);
                                return drop;
                              }),
               events.end());
  return AUD_OK;
}

AUD_EXPORT int32_t aud_graph_commit(AudGraph* graph) {
  if (graph == nullptr) return AUD_ERROR_INVALID_ARGUMENT;
  if (!graph->transactionOpen) return AUD_ERROR_STATE;
  freeRetiredPrograms(graph);
  refreshTails(graph);
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
  // A retiring emitter keeps the event routes to the nodes that stay
  // which its removal dropped, as its audio edges stay, until it is done:
  // the note offs it forwards while its notes close still arrive (ticket
  // 26). A route the transaction disconnected is gone.
  std::vector<EventEdgeId> zombies;
  for (NodeInstance* instance : retiring) {
    for (const EventEdgeId& id : graph->workingDroppedEvents) {
      if (id.from != instance->handle || id.to == instance->handle) continue;
      const auto& removed = graph->workingRemoved;
      if (std::find(removed.begin(), removed.end(), id.to) != removed.end()) {
        continue;
      }
      zombies.push_back(id);
    }
  }
  const size_t zombiesBefore = graph->zombieEventEdges.size();
  graph->zombieEventEdges.insert(graph->zombieEventEdges.end(), zombies.begin(),
                                 zombies.end());
  int32_t error = AUD_OK;
  Program* program = compileProgram(graph, graph->working, &error);
  if (program == nullptr) {
    for (NodeInstance* instance : retiring) instance->retired = false;
    graph->zombieEventEdges.resize(zombiesBefore);
    return error;
  }
  graph->topology = graph->working;
  graph->workingRemoved.clear();
  graph->workingDroppedEvents.clear();
  graph->transactionOpen = false;
  // What a removed node still loads is heard nowhere (ticket 25).
  for (NodeInstance* instance : retiring) {
    cancelLoadsOf(graph, instance->handle);
  }
  publish(graph, program);
  return static_cast<int32_t>(program->revision);
}

AUD_EXPORT int32_t aud_graph_rollback(AudGraph* graph) {
  if (graph == nullptr) return AUD_ERROR_INVALID_ARGUMENT;
  if (!graph->transactionOpen) return AUD_ERROR_STATE;
  graph->transactionOpen = false;
  graph->workingRemoved.clear();
  graph->workingDroppedEvents.clear();
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
      event->struct_size < sizeof(AudEvent) ||
      event->type == AUD_EVENT_MESSAGE) {
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
  return sendCommandEvent(graph, instance, *event, at, id);
}

// ............................................................................
// Messages of nodes (ticket 25)

AUD_EXPORT int32_t aud_graph_send_message(AudGraph* graph, int32_t node,
                                          const AudMessage* message,
                                          const AudTimestamp* at, uint32_t id) {
  if (graph == nullptr || message == nullptr ||
      message->struct_size < sizeof(AudMessage) || message->path == nullptr ||
      message->path[0] != '/' ||
      (message->num_args > 0 && message->args == nullptr)) {
    return AUD_ERROR_INVALID_ARGUMENT;
  }
  NodeInstance* instance = instanceOf(graph, node);
  if (instance == nullptr) return AUD_ERROR_NOT_FOUND;
  if (instance->retired) return AUD_ERROR_RETIRED;
  if (!instance->committed) return AUD_ERROR_STATE;
  if ((instance->descriptor->capabilities & AUD_NODE_CAP_MESSAGES) == 0) {
    return AUD_ERROR_UNSUPPORTED;
  }
  const uint32_t slotIndex = findMessageSlot(graph);
  if (slotIndex == UINT32_MAX) {
    graph->rejected.fetch_add(1, std::memory_order_relaxed);
    return AUD_ERROR_CAPACITY;
  }
  MessageSlot& slot = graph->messageSlots[slotIndex];
  const int32_t copied = copyMessage(slot, *message);
  if (copied != AUD_OK) return copied;
  AudEvent event = aud_event_message_make(&slot.message, 0, 0);
  event.words[2] = slotIndex;
  // The queue's release publishes the slot to the realtime thread.
  slot.busy.store(true, std::memory_order_relaxed);
  const int32_t result = sendCommandEvent(graph, instance, event, at, id);
  if (result != AUD_OK) slot.busy.store(false, std::memory_order_release);
  return result;
}

AUD_EXPORT int32_t aud_graph_take_messages(AudGraph* graph,
                                           AudGraphMessage* out,
                                           uint32_t capacity) {
  if (graph == nullptr || (out == nullptr && capacity > 0)) {
    return AUD_ERROR_INVALID_ARGUMENT;
  }
  uint32_t count = 0;
  while (count < capacity && graph->replies->pop(out[count])) count += 1;
  return static_cast<int32_t>(count);
}

// ............................................................................
// Loads (ticket 25)

AUD_EXPORT int32_t aud_graph_loads(AudGraph* graph, int32_t* ids,
                                   uint32_t capacity) {
  if (graph == nullptr || (ids == nullptr && capacity > 0)) {
    return AUD_ERROR_INVALID_ARGUMENT;
  }
  std::lock_guard<std::mutex> lock(graph->loadsMutex);
  uint32_t index = 0;
  for (auto& load : graph->loads) {
    if (index < capacity) ids[index] = load->id;
    index += 1;
  }
  return static_cast<int32_t>(index);
}

AUD_EXPORT int32_t aud_graph_load_info(AudGraph* graph, int32_t id,
                                       AudLoadInfo* info) {
  if (graph == nullptr || info == nullptr ||
      info->struct_size < sizeof(AudLoadInfo)) {
    return AUD_ERROR_INVALID_ARGUMENT;
  }
  std::lock_guard<std::mutex> lock(graph->loadsMutex);
  const Load* load = findLoad(graph, id);
  if (load == nullptr) return AUD_ERROR_NOT_FOUND;
  info->id = load->id;
  info->node = load->node;
  info->result = load->result.load(std::memory_order_acquire);
  info->progress = load->progress.load(std::memory_order_acquire);
  info->cancelled = load->cancelled.load(std::memory_order_acquire) ? 1 : 0;
  info->start_ns = load->startNs;
  info->end_ns = load->endNs;
  std::snprintf(info->name, sizeof(info->name), "%s", load->name.c_str());
  std::snprintf(info->message, sizeof(info->message), "%s",
                load->message.c_str());
  return AUD_OK;
}

AUD_EXPORT int32_t aud_graph_cancel_load(AudGraph* graph, int32_t id) {
  if (graph == nullptr) return AUD_ERROR_INVALID_ARGUMENT;
  std::lock_guard<std::mutex> lock(graph->loadsMutex);
  Load* load = runningLoad(graph, id);
  if (load == nullptr) return AUD_ERROR_NOT_FOUND;
  load->cancelled.store(true, std::memory_order_release);
  return AUD_OK;
}

AUD_EXPORT int32_t aud_graph_wait_loads(AudGraph* graph, int32_t node,
                                        uint32_t timeout_ms) {
  if (graph == nullptr) return AUD_ERROR_INVALID_ARGUMENT;
  const auto running = [graph, node] {
    for (auto& load : graph->loads) {
      if ((node == 0 || load->node == node) &&
          load->result.load(std::memory_order_acquire) == AUD_PENDING) {
        return true;
      }
    }
    return false;
  };
  {
    std::unique_lock<std::mutex> lock(graph->loadsMutex);
    if (timeout_ms == 0) {
      graph->loadsChanged.wait(lock, [&] { return !running(); });
    } else if (!graph->loadsChanged.wait_for(
                   lock, std::chrono::milliseconds(timeout_ms),
                   [&] { return !running(); })) {
      return AUD_PENDING;
    }
  }
  // What the loads changed of the tails counts from now on, not only from
  // the next commit: a host asks for the tail once the loads are done.
  if (refreshTails(graph) && !graph->transactionOpen && graph->hasProgram) {
    recompile(graph);
  }
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
  return enqueueCommand(graph, command);
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
  return enqueueCommand(graph, command);
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
    // The watchdog's guard is raised here, outside the nonblocking block
    // render: on Darwin the first access of a thread-local on a new thread
    // allocates its storage through dyld, once per thread (ticket 20).
    WatchdogScope watchdog;
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

AUD_EXPORT int32_t aud_graph_render_host(AudGraph* graph,
                                         AudHostRenderRequest* request) {
  if (graph == nullptr || request == nullptr ||
      request->struct_size < sizeof(AudHostRenderRequest) ||
      request->request == nullptr ||
      (request->max_output_events > 0 && request->output_events == nullptr)) {
    return AUD_ERROR_INVALID_ARGUMENT;
  }
  request->num_output_events = 0;
  request->dropped_output_events = 0;
  // Both fields belong to the rendering thread; aud_graph_render_offline,
  // the other writer of `offline`, never runs while a stream renders.
  graph->hostRequest = request;
  const bool offline = graph->offline;
  if (request->flags & AUD_PROCESS_OFFLINE) graph->offline = true;
  const int32_t result = aud_graph_render(graph, request->request);
  graph->offline = offline;
  graph->hostRequest = nullptr;
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
  // The notifications of the control side and the workers: the loads.
  {
    std::lock_guard<std::mutex> lock(graph->loadsMutex);
    auto& control = graph->controlNotifications;
    size_t taken = 0;
    while (count < capacity && taken < control.size()) {
      out[count++] = control[taken++].notification;
    }
    control.erase(control.begin(), control.begin() + static_cast<long>(taken));
    // What did not fit wakes the listener again.
    if (!control.empty()) wakeListener(graph);
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
  for (uint32_t attempt = 0; attempt < kTapReadAttempts; ++attempt) {
    const uint32_t before = tap.generation.load(std::memory_order_acquire);
    if (before & 1) continue;
    const uint32_t writePos = tap.writePos.load(std::memory_order_relaxed);
    const std::atomic<float>* ring =
        tap.ring.get() + static_cast<size_t>(channel) * tap.ringFrames;
    for (uint32_t i = 0; i < frames; ++i) {
      const uint32_t position =
          (writePos + tap.ringFrames - frames + i) % tap.ringFrames;
      out[i] = ring[position].load(std::memory_order_relaxed);
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
  stats->reserved2 = 0;
  stats->realtime_violations =
      graph->realtimeViolations.load(std::memory_order_relaxed);
  stats->messages_dropped =
      graph->messagesDropped.load(std::memory_order_relaxed);
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
  graph->realtimeViolations.store(0, std::memory_order_relaxed);
  graph->messagesDropped.store(0, std::memory_order_relaxed);
}
