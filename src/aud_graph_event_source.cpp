// @license
// Copyright (c) Audanika. All Rights Reserved.
//
// Use of this source code is governed by terms that can be
// found in the LICENSE file in the root of this package.

// The control and producer side of the event sources (interop-003,
// midi-003, ticket 26): creation for a committed target, the producer's
// push with its checks and counters, the hand-over token, release, the
// retirement handshake with the realtime thread and the counters; and the
// timing of the engine for the control side. The drain lives in
// aud_graph_render.cpp.

#include <algorithm>
#include <cstring>

#include "aud_clock.h"
#include "aud_graph_internal.hpp"
#include "aud_ump.h"

using namespace aud;

namespace aud {

namespace {

// The source is retired and the realtime thread let go of it.
bool letGo(const AudEventSource* source) {
  return source->retired.load(std::memory_order_acquire) &&
         source->done.load(std::memory_order_acquire);
}

void waitForRenderOf(AudGraph* graph) {
  while (graph->inFlight.load(std::memory_order_seq_cst)) {
    std::this_thread::yield();
  }
}

// Frees a source the realtime thread does not look at any more.
void freeSource(AudGraph* graph, AudEventSource* source) {
  AudEventSource* expected = source;
  graph->sourceSlots[source->slot].compare_exchange_strong(
      expected, nullptr, std::memory_order_acq_rel);
  auto& sources = graph->sources;
  for (auto it = sources.begin(); it != sources.end(); ++it) {
    if (it->get() != source) continue;
    sources.erase(it);
    return;
  }
}

}  // namespace

bool collectSources(AudGraph* graph) {
  bool freed = false;
  // A source retired while the graph does not run gets no block that lets
  // go of it: it goes here, once no render is in flight.
  const bool idle =
      graph->state.load(std::memory_order_seq_cst) != AUD_GRAPH_RUNNING;
  bool waited = false;
  for (size_t i = 0; i < graph->sources.size();) {
    AudEventSource* source = graph->sources[i].get();
    const bool retiredIdle =
        idle && source->retired.load(std::memory_order_acquire);
    if (retiredIdle && !waited) {
      waitForRenderOf(graph);
      waited = true;
    }
    if (letGo(source) || retiredIdle) {
      freeSource(graph, source);
      freed = true;
    } else {
      i += 1;
    }
  }
  return freed;
}

void destroySources(AudGraph* graph) {
  for (auto& slot : graph->sourceSlots) {
    slot.store(nullptr, std::memory_order_release);
  }
  graph->sources.clear();
}

bool sourceTargets(const AudGraph* graph, const NodeInstance* instance) {
  for (const auto& source : graph->sources) {
    if (source->target == instance) return true;
  }
  return false;
}

}  // namespace aud

// ############################################################################
// The C API

AUD_EXPORT AudEventSource* aud_graph_create_event_source(
    AudGraph* graph, const AudEventSourceConfig* config, int32_t* result) {
  int32_t error = AUD_OK;
  int32_t* out = result == nullptr ? &error : result;
  if (graph == nullptr || config == nullptr ||
      config->struct_size < sizeof(AudEventSourceConfig) ||
      config->node == AUD_GRAPH_NODE ||
      (config->timing != AUD_SOURCE_TIMING_FIXED &&
       config->timing != AUD_SOURCE_TIMING_IMMEDIATE)) {
    *out = AUD_ERROR_INVALID_ARGUMENT;
    return nullptr;
  }
  NodeInstance* instance = instanceOf(graph, config->node);
  if (instance == nullptr) {
    *out = AUD_ERROR_NOT_FOUND;
    return nullptr;
  }
  if (instance->retired) {
    *out = AUD_ERROR_RETIRED;
    return nullptr;
  }
  if (!instance->committed) {
    *out = AUD_ERROR_STATE;
    return nullptr;
  }
  if (config->port >= instance->descriptor->num_event_inputs) {
    *out = AUD_ERROR_INVALID_ARGUMENT;
    return nullptr;
  }
  uint32_t slot = kMaxEventSources;
  for (uint32_t i = 0; i < kMaxEventSources; ++i) {
    if (graph->sourceSlots[i].load(std::memory_order_acquire) == nullptr) {
      slot = i;
      break;
    }
  }
  if (slot == kMaxEventSources) {
    *out = AUD_ERROR_CAPACITY;
    return nullptr;
  }
  auto source = std::make_unique<AudEventSource>();
  source->graph = graph;
  source->target = instance;
  source->node = config->node;
  source->port = config->port;
  source->timing = config->timing;
  source->slot = slot;
  source->latencyNs.store(config->latency_ns, std::memory_order_relaxed);
  const uint32_t capacity =
      config->capacity == 0 ? kDefaultSourceCapacity
                            : std::min(config->capacity, kMaxSourceCapacity);
  source->queue = std::make_unique<SourceQueue>(capacity);
  AudEventSource* raw = source.get();
  graph->sources.push_back(std::move(source));
  AudEventSource* expected = nullptr;
  if (!graph->sourceSlots[slot].compare_exchange_strong(
          expected, raw, std::memory_order_acq_rel)) {
    // The slot was nulled by the realtime thread only; nothing else
    // writes it between the search and here.
    graph->sources.pop_back();
    *out = AUD_ERROR_CAPACITY;
    return nullptr;
  }
  *out = AUD_OK;
  return raw;
}

AUD_EXPORT int32_t aud_graph_destroy_event_source(AudGraph* graph,
                                                  AudEventSource* source) {
  if (graph == nullptr || source == nullptr || source->graph != graph) {
    return AUD_ERROR_INVALID_ARGUMENT;
  }
  if (source->retired.load(std::memory_order_acquire)) return AUD_ERROR_STATE;
  if (source->producer.load(std::memory_order_acquire) != 0) {
    return AUD_GRAPH_ERROR_BUSY;
  }
  const uint32_t state = graph->state.load(std::memory_order_seq_cst);
  if (state == AUD_GRAPH_RUNNING) {
    // The realtime thread releases the notes and lets go at its next
    // block; the collector frees the source then.
    source->retired.store(true, std::memory_order_release);
    return AUD_OK;
  }
  // No block renders: the source goes at once. The notes of its target
  // are closed by the first block after the restart (lifecycle-001).
  waitForRenderOf(graph);
  freeSource(graph, source);
  return AUD_OK;
}

AUD_EXPORT int32_t aud_event_source_set_latency(AudEventSource* source,
                                                int64_t latency_ns) {
  if (source == nullptr) return AUD_ERROR_INVALID_ARGUMENT;
  source->latencyNs.store(latency_ns, std::memory_order_relaxed);
  return AUD_OK;
}

AUD_EXPORT int32_t aud_event_source_attach(AudEventSource* source,
                                           uint64_t token) {
  if (source == nullptr || token == 0) return AUD_ERROR_INVALID_ARGUMENT;
  uint64_t expected = 0;
  // Acquire: the pushes of the previous producer, released by its detach,
  // are visible to this one.
  if (!source->producer.compare_exchange_strong(expected, token,
                                                std::memory_order_acq_rel)) {
    return AUD_GRAPH_ERROR_BUSY;
  }
  return AUD_OK;
}

AUD_EXPORT int32_t aud_event_source_detach(AudEventSource* source,
                                           uint64_t token) {
  if (source == nullptr || token == 0) return AUD_ERROR_INVALID_ARGUMENT;
  uint64_t expected = token;
  // Release: the pushes of this producer happen before the next attach.
  if (!source->producer.compare_exchange_strong(expected, 0,
                                                std::memory_order_acq_rel)) {
    return AUD_ERROR_INVALID_ARGUMENT;
  }
  return AUD_OK;
}

AUD_EXPORT int32_t aud_event_source_push(AudEventSource* source,
                                         const uint32_t* words,
                                         uint32_t count, int64_t host_time_ns) {
  if (source == nullptr) return AUD_ERROR_INVALID_ARGUMENT;
  if (words == nullptr || count == 0 || count > 4 ||
      count != aud_ump_word_count(words[0])) {
    source->rejected.fetch_add(1, std::memory_order_relaxed);
    return AUD_ERROR_INVALID_ARGUMENT;
  }
  const int64_t now = aud_clock_now_ns();
  SourceEvent packet;
  packet.numWords = count;
  for (uint32_t i = 0; i < count; ++i) packet.words[i] = words[i];
  packet.timestampNs = host_time_ns == 0 ? now : host_time_ns;
  if (packet.timestampNs < source->lastTimestampNs) {
    packet.timestampNs = source->lastTimestampNs;
    source->reordered.fetch_add(1, std::memory_order_relaxed);
  }
  source->lastTimestampNs = packet.timestampNs;
  // The delivery delay: how long the packet took from its timestamp to
  // here, on the producer's thread.
  const int64_t delay = std::max<int64_t>(0, now - packet.timestampNs);
  if (delay > source->maxDelayNs.load(std::memory_order_relaxed)) {
    source->maxDelayNs.store(delay, std::memory_order_relaxed);
  }
  const int64_t index = delay / AUD_SOURCE_DELAY_BIN_NS;
  const uint32_t bin = index < AUD_SOURCE_DELAY_BINS
                           ? static_cast<uint32_t>(index)
                           : AUD_SOURCE_DELAY_BINS;
  source->delayHistogram[bin].fetch_add(1, std::memory_order_relaxed);
  if (!source->queue->push(packet)) {
    source->rejected.fetch_add(1, std::memory_order_relaxed);
    // A note off that is lost would hang its note: the release closes it.
    if (aud_ump_is_note_off(words[0], count > 1 ? words[1] : 0)) {
      aud_event_source_release(source);
    }
    return AUD_ERROR_QUEUE_FULL;
  }
  source->pushed.fetch_add(1, std::memory_order_relaxed);
  return AUD_OK;
}

AUD_EXPORT uint32_t aud_event_source_size(const AudEventSource* source) {
  return source == nullptr ? 0 : source->queue->size();
}

AUD_EXPORT uint32_t aud_event_source_capacity(const AudEventSource* source) {
  return source == nullptr ? 0 : source->queue->capacity();
}

AUD_EXPORT void aud_event_source_release(AudEventSource* source) {
  if (source == nullptr) return;
  source->releaseRequested.store(true, std::memory_order_release);
}

AUD_EXPORT int32_t aud_event_source_stats(const AudEventSource* source,
                                          AudEventSourceStats* stats) {
  if (source == nullptr || stats == nullptr ||
      stats->struct_size < sizeof(AudEventSourceStats)) {
    return AUD_ERROR_INVALID_ARGUMENT;
  }
  stats->attached = source->producer.load(std::memory_order_acquire) != 0;
  stats->pushed = source->pushed.load(std::memory_order_relaxed);
  stats->rejected = source->rejected.load(std::memory_order_relaxed);
  stats->delivered = source->delivered.load(std::memory_order_relaxed);
  stats->late = source->late.load(std::memory_order_relaxed);
  stats->max_late_ns = source->maxLateNs.load(std::memory_order_relaxed);
  stats->dropped = source->dropped.load(std::memory_order_relaxed);
  stats->reordered = source->reordered.load(std::memory_order_relaxed);
  stats->releases = source->releases.load(std::memory_order_relaxed);
  stats->max_delay_ns = source->maxDelayNs.load(std::memory_order_relaxed);
  stats->last_word0 = source->lastWord0.load(std::memory_order_relaxed);
  stats->reserved = 0;
  stats->last_timestamp_ns =
      source->lastTimestamp.load(std::memory_order_relaxed);
  for (uint32_t i = 0; i <= AUD_SOURCE_DELAY_BINS; ++i) {
    stats->delay_histogram[i] =
        source->delayHistogram[i].load(std::memory_order_relaxed);
  }
  for (uint32_t i = 0; i <= AUD_SOURCE_LATENCY_BINS; ++i) {
    stats->latency_histogram[i] =
        source->latencyHistogram[i].load(std::memory_order_relaxed);
  }
  return AUD_OK;
}

// ############################################################################
// The timing of the engine

AUD_EXPORT int32_t aud_graph_timing(const AudGraph* graph,
                                    AudGraphTiming* timing) {
  if (graph == nullptr || timing == nullptr ||
      timing->struct_size < sizeof(AudGraphTiming)) {
    return AUD_ERROR_INVALID_ARGUMENT;
  }
  const TimingSnapshot& t = graph->timing;
  // A seqlock: the generation is odd while the realtime thread writes.
  // The realtime thread writes once per block, so a few attempts are
  // enough; the last one is taken as it is.
  for (uint32_t attempt = 0; attempt < kTapReadAttempts; ++attempt) {
    const uint32_t before = t.generation.load(std::memory_order_acquire);
    if (before & 1) continue;
    timing->flags = t.flags.load(std::memory_order_relaxed);
    timing->sample_rate = t.sampleRate.load(std::memory_order_relaxed);
    timing->sample_position = t.samplePosition.load(std::memory_order_relaxed);
    timing->host_time_ns = t.hostTimeNs.load(std::memory_order_relaxed);
    timing->frames = t.frames.load(std::memory_order_relaxed);
    timing->output_latency_frames =
        t.outputLatencyFrames.load(std::memory_order_relaxed);
    timing->max_period_ns = t.maxPeriodNs.load(std::memory_order_relaxed);
    timing->nominal_period_ns = t.nominalPeriodNs.load(std::memory_order_relaxed);
    timing->time_filter_resets =
        t.timeFilterResets.load(std::memory_order_relaxed);
    timing->reserved = 0;
    std::atomic_thread_fence(std::memory_order_acquire);
    if (t.generation.load(std::memory_order_acquire) != before) continue;
    return (timing->flags & AUD_TIMING_VALID) ? AUD_OK : AUD_ERROR_STATE;
  }
  return AUD_ERROR_FAILED;
}
