// @license
// Copyright (c) Audanika. All Rights Reserved.
//
// Use of this source code is governed by terms that can be
// found in the LICENSE file in the root of this package.

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <new>
#include <vector>

#include "aud_audio_graph.h"
#include "aud_clock.h"
#include "aud_ref_nodes.h"
#include "aud_spsc_queue.hpp"

#ifdef __ANDROID__
#include <android/log.h>
#endif

namespace {

constexpr uint32_t kDefaultQueueCapacity = 1024;
constexpr uint32_t kDefaultMaxNodes = 64;
// The per-block command budget (interop-002); the rest carries over in order.
constexpr uint32_t kMaxCommandsPerBlock = 256;
// Programs the realtime thread can retire before the control thread frees
// them; when all slots are taken the adoption waits for the next block.
constexpr size_t kRetiredSlots = 8;

enum class CommandKind : uint32_t { setParam, event };

// A command from the control thread to the realtime thread.
struct Command {
  CommandKind kind;
  int32_t node;
  uint32_t param;
  float value;
  AudEvent event;
  int64_t enqueueTimeNs;
};

// A node instance: its type and the pointer the package returned.
struct NodeInstance {
  const AudNodeDescriptor* type;
  void* instance;
};

// A render program: a serial chain of node instances with a revision.
struct Program {
  uint32_t revision;
  std::vector<NodeInstance*> nodes;
};

}  // namespace

// ###########################################################################
// The engine

struct AudEngine {
  AudEngineConfig config{};
  AudHostApi hostApi{};
  std::vector<const AudNodeDescriptor*> types;
  // Node slots, never resized: index = node id, null = free or destroyed.
  std::vector<std::atomic<NodeInstance*>> slots;
  std::unique_ptr<AudSpscQueue<Command>> commands;
  // Two planar buses for ping-pong rendering, channels x max_frames each.
  std::vector<std::vector<float>> busStorageA;
  std::vector<std::vector<float>> busStorageB;
  std::vector<float*> busA;
  std::vector<float*> busB;
  // Program hand-over: control publishes `pending`, the realtime thread
  // adopts it and parks the previous program in a retired slot that the
  // control thread frees.
  std::atomic<Program*> pending{nullptr};
  std::atomic<Program*> retired[kRetiredSlots]{};
  Program* current = nullptr;  // owned by the realtime thread
  std::vector<int32_t> controlChain;  // the chain the control thread published
  uint32_t nextRevision = 1;
  // Counters the realtime thread writes and the control thread reads.
  std::atomic<uint32_t> programRevision{0};
  std::atomic<uint64_t> blocksRendered{0};
  std::atomic<uint64_t> framesRendered{0};
  std::atomic<uint64_t> commandsApplied{0};
  std::atomic<uint64_t> commandsRejected{0};
  std::atomic<int64_t> latencyMin{INT64_MAX};
  std::atomic<int64_t> latencyMax{0};
  std::atomic<int64_t> latencySum{0};
  std::atomic<uint64_t> latencyCount{0};
  std::atomic<int64_t> renderMax{0};
  std::atomic<int64_t> renderSum{0};
  std::atomic<float> outputPeak{0.0f};
};

namespace {

// ###########################################################################
// The host API the engine offers to packages

int32_t hostRegisterNodeType(void* host, const AudNodeDescriptor* descriptor) {
  auto* engine = static_cast<AudEngine*>(host);
  if (descriptor == nullptr || descriptor->struct_size < sizeof(AudNodeDescriptor) ||
      descriptor->type_id == nullptr || descriptor->vtable == nullptr) {
    return AUD_ERROR_INVALID_ARGUMENT;
  }
  const AudNodeVTable* vtable = descriptor->vtable;
  if (vtable->struct_size < sizeof(AudNodeVTable) || vtable->create == nullptr ||
      vtable->destroy == nullptr || vtable->prepare == nullptr ||
      vtable->process == nullptr) {
    return AUD_ERROR_INVALID_ARGUMENT;
  }
  if (descriptor->abi_major != AUD_ABI_VERSION_MAJOR) return AUD_ERROR_ABI_MAJOR;
  if (descriptor->abi_minor > AUD_ABI_VERSION_MINOR) return AUD_ERROR_ABI_MINOR;
  for (const AudNodeDescriptor* type : engine->types) {
    if (std::strcmp(type->type_id, descriptor->type_id) == 0) {
      return AUD_ERROR_DUPLICATE_TYPE;
    }
  }
  engine->types.push_back(descriptor);
  return AUD_OK;
}

void* hostAlloc(void*, size_t bytes) { return std::malloc(bytes); }

void hostFree(void*, void* memory) { std::free(memory); }

void hostLog(void*, int32_t level, const char* message) {
#ifdef __ANDROID__
  __android_log_print(level >= AUD_LOG_ERROR ? ANDROID_LOG_ERROR : ANDROID_LOG_INFO,
                      "aud_audio", "%s", message);
#else
  std::fprintf(stderr, "[aud_audio %d] %s\n", level, message);
#endif
}

// ###########################################################################
// Helpers

const AudNodeDescriptor* typeAt(AudEngine* engine, int32_t index) {
  if (engine == nullptr || index < 0 ||
      static_cast<size_t>(index) >= engine->types.size()) {
    return nullptr;
  }
  return engine->types[static_cast<size_t>(index)];
}

NodeInstance* nodeAt(AudEngine* engine, int32_t node) {
  if (engine == nullptr || node < 0 ||
      static_cast<size_t>(node) >= engine->slots.size()) {
    return nullptr;
  }
  return engine->slots[static_cast<size_t>(node)].load(std::memory_order_acquire);
}

void freeRetired(AudEngine* engine) {
  for (auto& slot : engine->retired) {
    delete slot.exchange(nullptr, std::memory_order_acq_rel);
  }
}

int32_t enqueue(AudEngine* engine, const Command& command) {
  if (!engine->commands->push(command)) {
    engine->commandsRejected.fetch_add(1, std::memory_order_relaxed);
    return AUD_ERROR_QUEUE_FULL;
  }
  return AUD_OK;
}

// ###########################################################################
// The realtime path

void adoptPendingProgram(AudEngine* engine) {
  if (engine->pending.load(std::memory_order_acquire) == nullptr) return;
  for (auto& slot : engine->retired) {
    if (slot.load(std::memory_order_acquire) != nullptr) continue;
    Program* program = engine->pending.exchange(nullptr, std::memory_order_acq_rel);
    if (program == nullptr) return;
    slot.store(engine->current, std::memory_order_release);
    engine->current = program;
    engine->programRevision.store(program->revision, std::memory_order_relaxed);
    return;
  }
}

void applyCommand(AudEngine* engine, const Command& command) {
  NodeInstance* node = nodeAt(engine, command.node);
  if (node == nullptr) return;
  const AudNodeVTable* vtable = node->type->vtable;
  if (command.kind == CommandKind::setParam) {
    if (vtable->set_param != nullptr && command.param < node->type->num_params) {
      vtable->set_param(node->instance, command.param, command.value);
    }
    return;
  }
  if (vtable->event != nullptr) vtable->event(node->instance, &command.event);
}

void applyCommands(AudEngine* engine) {
  Command command;
  uint32_t budget = kMaxCommandsPerBlock;
  int64_t minLatency = engine->latencyMin.load(std::memory_order_relaxed);
  int64_t maxLatency = engine->latencyMax.load(std::memory_order_relaxed);
  int64_t sumLatency = 0;
  uint64_t count = 0;
  while (budget > 0 && engine->commands->pop(command)) {
    --budget;
    applyCommand(engine, command);
    const int64_t latency = aud_clock_now_ns() - command.enqueueTimeNs;
    minLatency = std::min(minLatency, latency);
    maxLatency = std::max(maxLatency, latency);
    sumLatency += latency;
    ++count;
  }
  if (count == 0) return;
  engine->commandsApplied.fetch_add(count, std::memory_order_relaxed);
  engine->latencyMin.store(minLatency, std::memory_order_relaxed);
  engine->latencyMax.store(maxLatency, std::memory_order_relaxed);
  engine->latencySum.fetch_add(sumLatency, std::memory_order_relaxed);
  engine->latencyCount.fetch_add(count, std::memory_order_relaxed);
}

// Renders one block of at most max_frames frames into interleaved output.
void renderBlock(AudEngine* engine, float* output, uint32_t frames) {
  const uint32_t channels = engine->config.channels;
  float** current = engine->busA.data();
  float** other = engine->busB.data();
  for (uint32_t channel = 0; channel < channels; ++channel) {
    std::memset(current[channel], 0, sizeof(float) * frames);
  }
  if (engine->current != nullptr) {
    AudProcessContext context{};
    context.struct_size = sizeof(AudProcessContext);
    context.frames = frames;
    context.channels = channels;
    context.sample_rate = engine->config.sample_rate;
    for (NodeInstance* node : engine->current->nodes) {
      const AudNodeDescriptor* type = node->type;
      const bool inPlace = type->num_inputs == 0 ||
                           (type->capabilities & AUD_NODE_CAP_IN_PLACE) != 0;
      context.inputs = type->num_inputs == 0 ? nullptr : current;
      context.outputs = inPlace ? current : other;
      type->vtable->process(node->instance, &context);
      if (!inPlace) std::swap(current, other);
    }
  }
  float peak = engine->outputPeak.load(std::memory_order_relaxed);
  for (uint32_t frame = 0; frame < frames; ++frame) {
    for (uint32_t channel = 0; channel < channels; ++channel) {
      const float sample = current[channel][frame];
      output[frame * channels + channel] = sample;
      peak = std::max(peak, std::fabs(sample));
    }
  }
  engine->outputPeak.store(peak, std::memory_order_relaxed);
}

}  // namespace

// ###########################################################################
// The C API

AUD_EXPORT AudEngine* aud_engine_create(const AudEngineConfig* config) {
  if (config == nullptr || config->struct_size < sizeof(AudEngineConfig) ||
      config->sample_rate <= 0.0 || config->max_frames == 0 ||
      config->channels == 0) {
    return nullptr;
  }
  auto* engine = new (std::nothrow) AudEngine();
  if (engine == nullptr) return nullptr;
  engine->config = *config;
  if (engine->config.command_queue_capacity == 0) {
    engine->config.command_queue_capacity = kDefaultQueueCapacity;
  }
  if (engine->config.max_nodes == 0) engine->config.max_nodes = kDefaultMaxNodes;
  engine->hostApi = {sizeof(AudHostApi), AUD_ABI_VERSION_MAJOR, AUD_ABI_VERSION_MINOR,
                     engine,             hostRegisterNodeType,  hostAlloc,
                     hostFree,           hostLog};
  engine->slots = std::vector<std::atomic<NodeInstance*>>(engine->config.max_nodes);
  engine->commands = std::make_unique<AudSpscQueue<Command>>(
      engine->config.command_queue_capacity);
  const uint32_t channels = engine->config.channels;
  const uint32_t maxFrames = engine->config.max_frames;
  engine->busStorageA.assign(channels, std::vector<float>(maxFrames, 0.0f));
  engine->busStorageB.assign(channels, std::vector<float>(maxFrames, 0.0f));
  for (uint32_t channel = 0; channel < channels; ++channel) {
    engine->busA.push_back(engine->busStorageA[channel].data());
    engine->busB.push_back(engine->busStorageB[channel].data());
  }
  if (aud_ref_nodes_register(&engine->hostApi) != AUD_OK) {
    delete engine;
    return nullptr;
  }
  return engine;
}

AUD_EXPORT void aud_engine_destroy(AudEngine* engine) {
  if (engine == nullptr) return;
  delete engine->pending.exchange(nullptr);
  freeRetired(engine);
  delete engine->current;
  for (auto& slot : engine->slots) {
    NodeInstance* node = slot.exchange(nullptr);
    if (node == nullptr) continue;
    node->type->vtable->destroy(node->instance);
    delete node;
  }
  delete engine;
}

AUD_EXPORT const void* aud_engine_host_api(AudEngine* engine) {
  return engine == nullptr ? nullptr : &engine->hostApi;
}

AUD_EXPORT int32_t aud_engine_num_node_types(AudEngine* engine) {
  return engine == nullptr ? 0 : static_cast<int32_t>(engine->types.size());
}

AUD_EXPORT const char* aud_engine_node_type_id(AudEngine* engine, int32_t index) {
  const AudNodeDescriptor* type = typeAt(engine, index);
  return type == nullptr ? nullptr : type->type_id;
}

AUD_EXPORT const char* aud_engine_node_type_name(AudEngine* engine, int32_t index) {
  const AudNodeDescriptor* type = typeAt(engine, index);
  return type == nullptr ? nullptr : type->name;
}

AUD_EXPORT uint32_t aud_engine_node_type_capabilities(AudEngine* engine,
                                                      int32_t index) {
  const AudNodeDescriptor* type = typeAt(engine, index);
  return type == nullptr ? 0 : type->capabilities;
}

AUD_EXPORT int32_t aud_engine_node_type_num_params(AudEngine* engine, int32_t index) {
  const AudNodeDescriptor* type = typeAt(engine, index);
  return type == nullptr ? AUD_ERROR_INVALID_ARGUMENT
                         : static_cast<int32_t>(type->num_params);
}

AUD_EXPORT int32_t aud_engine_node_type_param(AudEngine* engine, int32_t index,
                                              uint32_t param, const char** id,
                                              const char** unit, float* min_value,
                                              float* max_value, float* default_value) {
  const AudNodeDescriptor* type = typeAt(engine, index);
  if (type == nullptr || param >= type->num_params || id == nullptr ||
      unit == nullptr || min_value == nullptr || max_value == nullptr ||
      default_value == nullptr) {
    return AUD_ERROR_INVALID_ARGUMENT;
  }
  const AudParamDescriptor& descriptor = type->params[param];
  *id = descriptor.id;
  *unit = descriptor.unit;
  *min_value = descriptor.min_value;
  *max_value = descriptor.max_value;
  *default_value = descriptor.default_value;
  return AUD_OK;
}

AUD_EXPORT int32_t aud_engine_create_node(AudEngine* engine, const char* type_id) {
  if (engine == nullptr || type_id == nullptr) return AUD_ERROR_INVALID_ARGUMENT;
  const AudNodeDescriptor* type = nullptr;
  for (const AudNodeDescriptor* candidate : engine->types) {
    if (std::strcmp(candidate->type_id, type_id) == 0) type = candidate;
  }
  if (type == nullptr) return AUD_ERROR_UNKNOWN_TYPE;
  int32_t id = -1;
  for (size_t slot = 0; slot < engine->slots.size(); ++slot) {
    if (engine->slots[slot].load(std::memory_order_acquire) == nullptr) {
      id = static_cast<int32_t>(slot);
      break;
    }
  }
  if (id < 0) return AUD_ERROR_OUT_OF_MEMORY;
  void* instance = type->vtable->create(type, &engine->hostApi);
  if (instance == nullptr) return AUD_ERROR_FAILED;
  const int32_t prepared = type->vtable->prepare(
      instance, engine->config.sample_rate, engine->config.max_frames,
      engine->config.channels);
  if (prepared != AUD_OK) {
    type->vtable->destroy(instance);
    return prepared;
  }
  auto* node = new (std::nothrow) NodeInstance{type, instance};
  if (node == nullptr) {
    type->vtable->destroy(instance);
    return AUD_ERROR_OUT_OF_MEMORY;
  }
  engine->slots[static_cast<size_t>(id)].store(node, std::memory_order_release);
  return id;
}

AUD_EXPORT int32_t aud_engine_destroy_node(AudEngine* engine, int32_t node) {
  NodeInstance* instance = nodeAt(engine, node);
  if (instance == nullptr) return AUD_ERROR_INVALID_ARGUMENT;
  const auto& chain = engine->controlChain;
  if (std::find(chain.begin(), chain.end(), node) != chain.end()) {
    return AUD_ERROR_STATE;
  }
  // The realtime thread may still render an older chain that holds the
  // node: it must have adopted the latest published chain first.
  const uint32_t latest = engine->nextRevision - 1;
  if (latest > 0 && engine->programRevision.load(std::memory_order_acquire) != latest) {
    return AUD_ERROR_STATE;
  }
  engine->slots[static_cast<size_t>(node)].store(nullptr, std::memory_order_release);
  instance->type->vtable->destroy(instance->instance);
  delete instance;
  return AUD_OK;
}

AUD_EXPORT int32_t aud_engine_set_chain(AudEngine* engine, const int32_t* nodes,
                                        uint32_t count) {
  if (engine == nullptr || (nodes == nullptr && count > 0)) {
    return AUD_ERROR_INVALID_ARGUMENT;
  }
  auto program = std::make_unique<Program>();
  program->revision = engine->nextRevision;
  for (uint32_t index = 0; index < count; ++index) {
    NodeInstance* instance = nodeAt(engine, nodes[index]);
    if (instance == nullptr) return AUD_ERROR_INVALID_ARGUMENT;
    program->nodes.push_back(instance);
  }
  engine->nextRevision += 1;
  engine->controlChain.assign(nodes, nodes + count);
  freeRetired(engine);
  const int32_t revision = static_cast<int32_t>(program->revision);
  // A pending program the realtime thread never saw can be freed here.
  delete engine->pending.exchange(program.release(), std::memory_order_acq_rel);
  return revision;
}

AUD_EXPORT int32_t aud_engine_set_param(AudEngine* engine, int32_t node,
                                        uint32_t param, float value) {
  NodeInstance* instance = nodeAt(engine, node);
  if (instance == nullptr || param >= instance->type->num_params) {
    return AUD_ERROR_INVALID_ARGUMENT;
  }
  Command command{};
  command.kind = CommandKind::setParam;
  command.node = node;
  command.param = param;
  command.value = value;
  command.enqueueTimeNs = aud_clock_now_ns();
  return enqueue(engine, command);
}

AUD_EXPORT int32_t aud_engine_send_note(AudEngine* engine, int32_t node, int32_t on,
                                        uint32_t channel, uint32_t number,
                                        float velocity) {
  NodeInstance* instance = nodeAt(engine, node);
  if (instance == nullptr || (instance->type->capabilities & AUD_NODE_CAP_EVENTS) == 0) {
    return AUD_ERROR_INVALID_ARGUMENT;
  }
  Command command{};
  command.kind = CommandKind::event;
  command.node = node;
  command.event.struct_size = sizeof(AudEvent);
  command.event.type = on != 0 ? AUD_EVENT_NOTE_ON : AUD_EVENT_NOTE_OFF;
  command.event.channel = channel;
  command.event.number = number;
  command.event.value = velocity;
  command.enqueueTimeNs = aud_clock_now_ns();
  return enqueue(engine, command);
}

AUD_EXPORT int32_t aud_engine_set_string(AudEngine* engine, int32_t node,
                                         uint32_t key, const char* value) {
  NodeInstance* instance = nodeAt(engine, node);
  if (instance == nullptr || value == nullptr) return AUD_ERROR_INVALID_ARGUMENT;
  const AudNodeVTable* vtable = instance->type->vtable;
  if ((instance->type->capabilities & AUD_NODE_CAP_STRINGS) == 0 ||
      vtable->set_string == nullptr) {
    return AUD_ERROR_STATE;
  }
  return vtable->set_string(instance->instance, key, value);
}

AUD_EXPORT void aud_engine_render(AudEngine* engine, float* interleaved_output,
                                  uint32_t frames) {
  if (engine == nullptr || interleaved_output == nullptr) return;
  const int64_t start = aud_clock_now_ns();
  adoptPendingProgram(engine);
  applyCommands(engine);
  const uint32_t channels = engine->config.channels;
  uint32_t rendered = 0;
  while (rendered < frames) {
    const uint32_t block = std::min(frames - rendered, engine->config.max_frames);
    renderBlock(engine, interleaved_output + static_cast<size_t>(rendered) * channels,
                block);
    rendered += block;
  }
  const int64_t elapsed = aud_clock_now_ns() - start;
  engine->blocksRendered.fetch_add(1, std::memory_order_relaxed);
  engine->framesRendered.fetch_add(frames, std::memory_order_relaxed);
  engine->renderSum.fetch_add(elapsed, std::memory_order_relaxed);
  if (elapsed > engine->renderMax.load(std::memory_order_relaxed)) {
    engine->renderMax.store(elapsed, std::memory_order_relaxed);
  }
}

AUD_EXPORT void aud_engine_io_render(void* user, float* interleaved_output,
                                     uint32_t frames, uint32_t channels) {
  auto* engine = static_cast<AudEngine*>(user);
  if (engine == nullptr || channels != engine->config.channels) return;
  aud_engine_render(engine, interleaved_output, frames);
}

AUD_EXPORT void aud_engine_get_stats(AudEngine* engine, AudEngineStats* stats) {
  if (engine == nullptr || stats == nullptr ||
      stats->struct_size < sizeof(AudEngineStats)) {
    return;
  }
  stats->program_revision = engine->programRevision.load(std::memory_order_relaxed);
  stats->blocks_rendered = engine->blocksRendered.load(std::memory_order_relaxed);
  stats->frames_rendered = engine->framesRendered.load(std::memory_order_relaxed);
  stats->commands_applied = engine->commandsApplied.load(std::memory_order_relaxed);
  stats->commands_rejected = engine->commandsRejected.load(std::memory_order_relaxed);
  const uint64_t count = engine->latencyCount.load(std::memory_order_relaxed);
  stats->command_latency_min_ns =
      count == 0 ? 0 : engine->latencyMin.load(std::memory_order_relaxed);
  stats->command_latency_max_ns = engine->latencyMax.load(std::memory_order_relaxed);
  stats->command_latency_sum_ns = engine->latencySum.load(std::memory_order_relaxed);
  stats->command_latency_count = count;
  stats->render_time_max_ns = engine->renderMax.load(std::memory_order_relaxed);
  stats->render_time_sum_ns = engine->renderSum.load(std::memory_order_relaxed);
  stats->output_peak = engine->outputPeak.load(std::memory_order_relaxed);
}

AUD_EXPORT void aud_engine_reset_stats(AudEngine* engine) {
  if (engine == nullptr) return;
  engine->blocksRendered.store(0, std::memory_order_relaxed);
  engine->framesRendered.store(0, std::memory_order_relaxed);
  engine->commandsApplied.store(0, std::memory_order_relaxed);
  engine->commandsRejected.store(0, std::memory_order_relaxed);
  engine->latencyMin.store(INT64_MAX, std::memory_order_relaxed);
  engine->latencyMax.store(0, std::memory_order_relaxed);
  engine->latencySum.store(0, std::memory_order_relaxed);
  engine->latencyCount.store(0, std::memory_order_relaxed);
  engine->renderMax.store(0, std::memory_order_relaxed);
  engine->renderSum.store(0, std::memory_order_relaxed);
  engine->outputPeak.store(0.0f, std::memory_order_relaxed);
}
