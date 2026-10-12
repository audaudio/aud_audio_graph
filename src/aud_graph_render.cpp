// @license
// Copyright (c) Audanika. All Rights Reserved.
//
// Use of this source code is governed by terms that can be
// found in the LICENSE file in the root of this package.

// The realtime side of the graph: one block at a time, without allocation,
// lock or I/O. A block adopts a pending program (graph-003), feeds the
// time filter and captures the transport (time-001), drains the queues and
// the scheduler into the event table of the block (interop-002), runs the
// jobs of the program in order (graph-001) and advances the fades of the
// connections and the retirement of removed nodes. Diagnostics are counted
// per block and reported once.

#include <algorithm>
#include <cmath>
#include <cstring>

#include "aud_clock.h"
#include "aud_graph_internal.hpp"
#include "aud_graph_watchdog.hpp"
#include "aud_transport.h"
#include "aud_ump.h"

namespace aud {

constexpr uint32_t kGraphTarget = UINT32_MAX;

// ............................................................................
// The scheduler

namespace {

uint32_t domainIndex(uint32_t domain) {
  switch (domain) {
    case AUD_TIME_SAMPLE:
      return 0;
    case AUD_TIME_HOST:
      return 1;
    case AUD_TIME_BEAT:
      return 2;
    default:
      return UINT32_MAX;
  }
}

}  // namespace

void Scheduler::init(uint32_t capacity) {
  pool.assign(capacity, ScheduledEvent{});
  freeList.clear();
  freeList.reserve(capacity);
  for (uint32_t i = capacity; i > 0; --i) freeList.push_back(i - 1);
  for (EventHeap& heap : heaps) {
    heap.items.clear();
    heap.items.reserve(capacity);
  }
  count = 0;
}

int64_t Scheduler::keyOf(uint32_t index) const { return pool[index].key; }

bool Scheduler::less(uint32_t a, uint32_t b) const {
  const int64_t ka = keyOf(a);
  const int64_t kb = keyOf(b);
  if (ka != kb) return ka < kb;
  return pool[a].sequence < pool[b].sequence;
}

void Scheduler::siftUp(EventHeap& heap, size_t position) {
  while (position > 0) {
    const size_t parent = (position - 1) / 2;
    if (!less(heap.items[position], heap.items[parent])) return;
    std::swap(heap.items[position], heap.items[parent]);
    position = parent;
  }
}

void Scheduler::siftDown(EventHeap& heap, size_t position) {
  const size_t size = heap.items.size();
  while (true) {
    const size_t left = 2 * position + 1;
    const size_t right = left + 1;
    size_t smallest = position;
    if (left < size && less(heap.items[left], heap.items[smallest])) {
      smallest = left;
    }
    if (right < size && less(heap.items[right], heap.items[smallest])) {
      smallest = right;
    }
    if (smallest == position) return;
    std::swap(heap.items[position], heap.items[smallest]);
    position = smallest;
  }
}

bool Scheduler::push(const ScheduledEvent& event) {
  const uint32_t domain = domainIndex(event.at.domain);
  if (domain == UINT32_MAX || freeList.empty()) return false;
  const uint32_t index = freeList.back();
  freeList.pop_back();
  pool[index] = event;
  EventHeap& heap = heaps[domain];
  heap.items.push_back(index);
  siftUp(heap, heap.items.size() - 1);
  count += 1;
  return true;
}

uint32_t Scheduler::top(uint32_t domain) const {
  const EventHeap& heap = heaps[domain];
  return heap.items.empty() ? UINT32_MAX : heap.items[0];
}

void Scheduler::pop(uint32_t domain) {
  EventHeap& heap = heaps[domain];
  heap.items[0] = heap.items.back();
  heap.items.pop_back();
  if (!heap.items.empty()) siftDown(heap, 0);
}

void Scheduler::release(uint32_t index) {
  freeList.push_back(index);
  count -= 1;
}

uint32_t Scheduler::cancel(const NodeInstance* instance, uint32_t id,
                           void (*removed)(void* user, const AudEvent& event),
                           void* user) {
  uint32_t cancelled = 0;
  for (EventHeap& heap : heaps) {
    size_t kept = 0;
    for (size_t i = 0; i < heap.items.size(); ++i) {
      const uint32_t index = heap.items[i];
      const ScheduledEvent& event = pool[index];
      const bool matches = (instance == nullptr || event.instance == instance) &&
                           (id == 0 || event.id == id);
      if (matches) {
        if (removed != nullptr) removed(user, event.event);
        release(index);
        cancelled += 1;
      } else {
        heap.items[kept++] = index;
      }
    }
    if (kept == heap.items.size()) continue;
    heap.items.resize(kept);
    // Rebuild the heap from the back (Floyd).
    for (size_t i = kept / 2; i > 0; --i) siftDown(heap, i - 1);
  }
  return cancelled;
}

// ............................................................................
// Notifications

namespace {

AudGraphNotification makeNotification(AudGraph* graph, uint32_t type) {
  AudGraphNotification n{};
  n.struct_size = sizeof(AudGraphNotification);
  n.type = type;
  n.sample_position = graph->streamTime.sample_position;
  n.event.struct_size = sizeof(AudEvent);
  return n;
}

void notify(AudGraph* graph, const AudGraphNotification& notification) {
  if (graph->notifications->push(notification)) {
    graph->blockNotified = true;
  } else {
    graph->notificationsDropped.fetch_add(1, std::memory_order_relaxed);
  }
}

void notifyDiagnostic(AudGraph* graph, int32_t code, uint32_t count,
                      int32_t node = 0, int64_t value = 0) {
  if (count == 0) return;
  AudGraphNotification n = makeNotification(graph, AUD_NOTIFY_DIAGNOSTIC);
  n.code = code;
  n.count = count;
  n.node = node;
  n.value = value;
  notify(graph, n);
}

// ............................................................................
// The event table of the block

uint32_t noteKey(const AudEvent& event) {
  const uint32_t word0 = event.words[0];
  return (aud_ump_message_type(word0) << 28) | (aud_ump_group(word0) << 24) |
         (aud_ump_channel(word0) << 16) | (aud_ump_note(word0) << 8) |
         (event.port & 0xff);
}

// The rank of an event among the events of its node at one offset: a note
// off before the rest, the rest before note ons, each rank in the order the
// events came in (interop-002). A note off ranks first only when its pitch
// sounds - it can close an earlier note, so that a note on of the same
// pitch retriggers. The note off of a pitch that does not sound belongs to
// a note on of this block and keeps its order behind it: swapped, a note on
// and its own note off at the same time would hang the note (ticket 20).
uint32_t rankOf(const AudEvent& event, const NodeInstance* instance) {
  if (event.type != AUD_EVENT_UMP) return 1;
  if (aud_ump_is_note_on(event.words[0], event.words[1])) return 2;
  if (aud_ump_is_note_off(event.words[0], event.words[1])) {
    return instance->notes.contains(noteKey(event)) ? 0 : 2;
  }
  return 1;
}

// Whether the block's event table takes another event.
bool blockHasRoom(const AudGraph* graph) {
  return graph->numBlockEvents < graph->blockEvents.size();
}

bool addBlockEvent(AudGraph* graph, uint32_t node, const AudEvent& event,
                   uint32_t offset, uint64_t sequence) {
  if (graph->numBlockEvents >= graph->blockEvents.size()) {
    graph->diagnostics.overflow += 1;
    graph->eventsDropped.fetch_add(1, std::memory_order_relaxed);
    releaseMessage(graph, event);
    return false;
  }
  BlockEvent& entry = graph->blockEvents[graph->numBlockEvents++];
  entry.node = node;
  entry.rank = rankOf(event, graph->current->nodes[node].instance);
  entry.sequence = sequence;
  entry.event = event;
  entry.event.struct_size = sizeof(AudEvent);
  entry.event.sample_offset = offset;
  return true;
}

// An event reached the graph's event input: the host's event output takes
// it when the block renders through aud_graph_render_host (ticket 20),
// otherwise the control thread gets a notification.
void outputEvent(AudGraph* graph, int32_t node, const AudEvent& event,
                 uint32_t offset) {
  AudHostRenderRequest* host = graph->hostRequest;
  if (host != nullptr && host->max_output_events > 0) {
    if (host->num_output_events >= host->max_output_events) {
      host->dropped_output_events += 1;
      graph->diagnostics.overflow += 1;
      graph->eventsDropped.fetch_add(1, std::memory_order_relaxed);
      return;
    }
    AudEvent& out = host->output_events[host->num_output_events++];
    out = event;
    out.struct_size = sizeof(AudEvent);
    out.sample_offset = offset;
    return;
  }
  AudGraphNotification n = makeNotification(graph, AUD_NOTIFY_EVENT);
  n.node = node;
  n.event = event;
  n.event.struct_size = sizeof(AudEvent);
  n.event.sample_offset = offset;
  notify(graph, n);
}

// Orders the host's event output by offset; stable, so events of one
// node keep their order. Insertion sort - nothing may allocate here: every
// source - the host's events, each node - adds its events in ascending
// offset, so the cost grows with the events times the sources that
// interleave, not with their square.
void sortOutputEvents(AudHostRenderRequest* host) {
  AudEvent* events = host->output_events;
  for (uint32_t i = 1; i < host->num_output_events; ++i) {
    const AudEvent event = events[i];
    uint32_t k = i;
    while (k > 0 && events[k - 1].sample_offset > event.sample_offset) {
      events[k] = events[k - 1];
      k -= 1;
    }
    events[k] = event;
  }
}

// Routes an event that leaves the graph's event output.
void routeGraphEvent(AudGraph* graph, const AudEvent& event, uint32_t offset,
                     uint64_t sequence) {
  const Program* program = graph->current;
  if (program == nullptr) return;
  for (const EventRoute& route : program->routes) {
    if (route.fromNode != kGraphTarget || route.fromPort != event.port) continue;
    if (route.toNode == kGraphTarget) {
      AudEvent routed = event;
      routed.port = route.toPort;
      outputEvent(graph, AUD_GRAPH_NODE, routed, offset);
      continue;
    }
    AudEvent routed = event;
    routed.port = route.toPort;
    addBlockEvent(graph, route.toNode, routed, offset, sequence);
  }
}


// Tracks the note of an event; false for a note on the tracker cannot
// hold: it is not delivered, since it could never be closed (interop-002).
bool trackNote(AudGraph* graph, NodeInstance* instance, const AudEvent& event) {
  if (event.type != AUD_EVENT_UMP) return true;
  const uint32_t word0 = event.words[0];
  const uint32_t word1 = event.words[1];
  if (aud_ump_is_note_on(word0, word1)) {
    if (instance->notes.add(noteKey(event))) return true;
    graph->diagnostics.trackerFull += 1;
    graph->diagnostics.trackerFullNode = instance->handle;
    graph->eventsDropped.fetch_add(1, std::memory_order_relaxed);
    return false;
  }
  if (aud_ump_is_note_off(word0, word1)) {
    instance->notes.remove(noteKey(event));
  }
  return true;
}

// Closes the running notes of a node with note offs at the block start.
// Bounded, it stops at a full table and keeps the notes still open for
// the next block, as a release of a source needs (ticket 26); otherwise
// every note goes, a note off that finds no room is dropped and counted.
// True once every note is closed.
bool closeNotes(AudGraph* graph, NodeInstance* instance, uint32_t node,
                bool bounded = false) {
  for (uint32_t i = 0; i < instance->notes.count; ++i) {
    if (bounded && !blockHasRoom(graph)) {
      NoteTracker& notes = instance->notes;
      std::memmove(notes.notes, notes.notes + i,
                   sizeof(uint32_t) * (notes.count - i));
      notes.count -= i;
      return false;
    }
    const uint32_t key = instance->notes.notes[i];
    const uint32_t type = key >> 28;
    const uint32_t group = (key >> 24) & 0xf;
    const uint32_t channel = (key >> 16) & 0xf;
    const uint32_t note = (key >> 8) & 0x7f;
    const uint32_t port = key & 0xff;
    uint32_t words[4] = {0, 0, 0, 0};
    uint32_t numWords = 1;
    if (type == AUD_UMP_TYPE_MIDI2_CHANNEL_VOICE) {
      aud_ump_midi2_note(group, channel, 0, note, 0x8000, 0, 0, words);
      numWords = 2;
    } else {
      words[0] = aud_ump_midi1_word(group, 0x80 | channel, note, 64);
    }
    AudEvent event = aud_event_ump(words, numWords, 0, port);
    addBlockEvent(graph, node, event, 0, 0);
  }
  instance->notes.count = 0;
  return true;
}

// ............................................................................
// Adoption (graph-003)

// The callback of Scheduler::cancel: a removed message frees its slot.
void releaseRemoved(void* user, const AudEvent& event) {
  releaseMessage(static_cast<AudGraph*>(user), event);
}

// Gives places of the scheduler back to the control thread (ticket 20).
void releaseScheduled(AudGraph* graph, uint32_t count) {
  if (count > 0) {
    graph->schedulerReserved.fetch_sub(count, std::memory_order_acq_rel);
  }
}

void adoptPending(AudGraph* graph) {
  if (graph->pending.load(std::memory_order_acquire) == nullptr) return;
  std::atomic<Program*>* free = nullptr;
  for (auto& slot : graph->retired) {
    if (slot.load(std::memory_order_acquire) == nullptr) {
      free = &slot;
      break;
    }
  }
  if (free == nullptr) return;  // every slot is taken: next block
  Program* program = graph->pending.exchange(nullptr, std::memory_order_acq_rel);
  if (program == nullptr) return;
  // Nothing played before the first adopted program: its connections
  // start at full gain instead of fading in.
  const bool first = graph->current == nullptr;
  free->store(graph->current, std::memory_order_release);
  graph->current = program;
  for (uint32_t i = 0; i < program->nodes.size(); ++i) {
    ProgramNode& node = program->nodes[i];
    node.instance->rtIndex = i;
    node.skip = node.instance->rtDone;
  }
  for (NodeInstance* instance : program->retire) {
    if (instance->rtRetired) continue;
    instance->rtRetired = true;
    instance->fadeRemaining = graph->fadeFrames;
    instance->tailRemaining =
        instance->tail > 0 ? std::min(instance->tail, graph->maxTailFrames)
                           : graph->fadeFrames;
    closeNotes(graph, instance, instance->rtIndex);
    const uint32_t purged =
        graph->scheduler.cancel(instance, 0, releaseRemoved, graph);
    releaseScheduled(graph, purged);
    graph->diagnostics.retired += purged;
    graph->eventsDropped.fetch_add(purged, std::memory_order_relaxed);
  }
  for (Edge* edge : program->fadeIn) {
    if (!edge->fadeInPending.exchange(false, std::memory_order_acq_rel)) continue;
    edge->inactive.store(false, std::memory_order_release);
    if (first) {
      edge->gain = 1;
      edge->target = 1;
      edge->remaining = 0;
    } else {
      edge->target = 1;
      edge->remaining = edge->gain < 1 ? graph->fadeFrames : 0;
    }
  }
  for (Edge* edge : program->fadeOut) {
    if (edge->target == 0) continue;
    edge->target = 0;
    edge->remaining = graph->fadeFrames;
  }
  graph->adoptedRevision.store(program->revision, std::memory_order_release);
  AudGraphNotification n = makeNotification(graph, AUD_NOTIFY_REVISION);
  n.revision = program->revision;
  notify(graph, n);
}

// ............................................................................
// Time (time-001)

double nsPerSample(const AudGraph* graph) { return 1e9 / graph->sampleRate; }

// Starts the measure of the period anew (ticket 26): the longest gap and
// the histogram of the gaps, and the previous render start, so that the
// gap of the render that resets - the discontinuity of a reset of the time
// filter, often - is no sample of the new measure. Relaxed stores, so the
// realtime thread may call it on a reset of the time filter.
void resetPeriod(AudGraph* graph) {
  graph->maxPeriodNs = 0;
  graph->lastRenderValid = false;
  for (auto& bin : graph->periodHistogram) {
    bin.store(0, std::memory_order_relaxed);
  }
}

void computeStreamTime(AudGraph* graph, const AudRenderRequest* request) {
  AudStreamTime time{};
  if (request->time != nullptr &&
      request->time->struct_size >= sizeof(AudStreamTime)) {
    time = *request->time;
  } else {
    time.frames = request->frames;
    time.sample_rate = graph->sampleRate;
    time.sample_position = graph->synthesizedSample;
    time.host_time_source = AUD_TIME_SOURCE_NONE;
  }
  time.struct_size = sizeof(AudStreamTime);
  time.frames = request->frames;
  if (time.sample_rate <= 0) time.sample_rate = graph->sampleRate;
  if (time.host_time_source == AUD_TIME_SOURCE_NONE) {
    if (!graph->synthesizedValid) {
      graph->synthesizedOrigin =
          aud_clock_now_ns() -
          static_cast<int64_t>(std::llround(
              static_cast<double>(time.sample_position) * nsPerSample(graph)));
      graph->synthesizedValid = true;
    }
    time.host_time_ns =
        graph->synthesizedOrigin +
        static_cast<int64_t>(std::llround(
            static_cast<double>(time.sample_position) * nsPerSample(graph)));
    time.host_time_source = AUD_TIME_SOURCE_SYNTHESIZED;
    time.host_time_accuracy_ns = 0;
  } else {
    if (aud_time_filter_add(&graph->filter, time.sample_position, time.frames,
                            time.host_time_ns)) {
      graph->filterResets.fetch_add(1, std::memory_order_relaxed);
      // The period is measured anew after a reset (ticket 26).
      resetPeriod(graph);
      AudGraphNotification n = makeNotification(graph, AUD_NOTIFY_TIME_RESET);
      n.count = 1;
      n.sample_position = time.sample_position;
      notify(graph, n);
    }
    time.host_time_ns =
        aud_time_filter_host_time_at(&graph->filter, time.sample_position);
  }
  graph->streamTime = time;
  graph->synthesizedSample = time.sample_position + time.frames;
}

// ............................................................................
// The internal transport

int64_t beatAt(const AudGraph* graph, int64_t sample) {
  const InternalTransport& t = graph->transport;
  if (!t.playing) return t.anchorBeat;
  const double beats = static_cast<double>(sample - t.anchorSample) * t.tempo /
                       (60.0 * graph->sampleRate);
  return t.anchorBeat + aud_ticks_from_beats(beats);
}

int64_t barStartOf(const AudGraph* graph, int64_t beat) {
  const InternalTransport& t = graph->transport;
  const double barBeats = 4.0 * t.numerator / t.denominator;
  const int64_t barTicks = aud_ticks_from_beats(barBeats);
  if (barTicks <= 0) return beat;
  const int64_t remainder = ((beat % barTicks) + barTicks) % barTicks;
  return beat - remainder;
}

void fillSegment(AudGraph* graph, AudTransportSegment* segment, uint32_t offset,
                 uint32_t frames, uint32_t flags) {
  const InternalTransport& t = graph->transport;
  const int64_t beat = beatAt(graph, graph->streamTime.sample_position + offset);
  segment->struct_size = sizeof(AudTransportSegment);
  segment->sample_offset = offset;
  segment->frames = frames;
  segment->flags = flags | (t.playing ? AUD_SEGMENT_PLAYING : 0) |
                   (t.looping ? AUD_SEGMENT_LOOPING : 0);
  segment->beat = beat;
  segment->tempo = t.tempo;
  segment->tempo_increment = 0;
  segment->bar_start = barStartOf(graph, beat);
  segment->time_signature_numerator = t.numerator;
  segment->time_signature_denominator = t.denominator;
  segment->loop_start = t.loopStart;
  segment->loop_end = t.loopEnd;
}

void notifyTransport(AudGraph* graph, uint32_t type) {
  const InternalTransport& t = graph->transport;
  AudGraphNotification n = makeNotification(graph, AUD_NOTIFY_TRANSPORT);
  n.code = static_cast<int32_t>(type);
  n.value = t.anchorBeat;
  n.number = t.tempo;
  n.count = t.playing ? 1 : 0;
  notify(graph, n);
}

// Applies a request to the internal transport at a sample position; the
// anchor moves there so that the position never drifts.
bool applyInternalRequest(AudGraph* graph, const AudTransportRequest& request,
                          int64_t sample, bool* seek) {
  InternalTransport& t = graph->transport;
  const int64_t beat = beatAt(graph, sample);
  t.anchorSample = sample;
  t.anchorBeat = beat;
  switch (request.type) {
    case AUD_TRANSPORT_REQUEST_START:
      t.playing = true;
      break;
    case AUD_TRANSPORT_REQUEST_STOP:
      t.playing = false;
      break;
    case AUD_TRANSPORT_REQUEST_SEEK:
      t.anchorBeat = request.beat;
      *seek = true;
      break;
    case AUD_TRANSPORT_REQUEST_SET_TEMPO:
      if (!(request.value > 0)) return false;
      t.tempo = request.value;
      break;
    case AUD_TRANSPORT_REQUEST_SET_TIME_SIGNATURE:
      if (request.numerator == 0 || request.denominator == 0) return false;
      t.numerator = request.numerator;
      t.denominator = request.denominator;
      break;
    case AUD_TRANSPORT_REQUEST_SET_LOOP:
      t.looping = request.beat_end > request.beat;
      t.loopStart = request.beat;
      t.loopEnd = request.beat_end;
      break;
    default:
      return false;  // the quantum belongs to Link (link-001)
  }
  notifyTransport(graph, request.type);
  return true;
}

// Resolves the time of a pending request against the remainder of the
// block; returns the offset, or UINT32_MAX when it lies beyond the block.
uint32_t requestOffset(AudGraph* graph, const AudTransportRequest& request,
                       uint32_t from, uint32_t frames) {
  AudTransportSegment segment;
  fillSegment(graph, &segment, from, frames - from, 0);
  segment.sample_offset = 0;
  AudStreamTime time = graph->streamTime;
  time.sample_position += from;
  time.frames = frames - from;
  time.host_time_ns += static_cast<int64_t>(
      std::llround(static_cast<double>(from) * nsPerSample(graph)));
  AudTransportSnapshot snapshot{};
  snapshot.struct_size = sizeof(AudTransportSnapshot);
  snapshot.num_segments = 1;
  snapshot.segments = &segment;
  snapshot.time = time;
  int64_t offset = 0;
  const int32_t result = aud_transport_resolve(&snapshot, &request.at, &offset);
  if (result == AUD_OK) return from + static_cast<uint32_t>(offset);
  if (result == AUD_ERROR_LATE) return from;
  return UINT32_MAX;
}

// The pending request that takes effect first in [from, frames), or -1;
// `offset` receives where it takes effect, or `frames`.
int earliestRequest(AudGraph* graph, uint32_t from, uint32_t frames,
                    uint32_t* offset) {
  const InternalTransport& t = graph->transport;
  *offset = frames;
  int best = -1;
  for (uint32_t i = 0; i < kMaxPendingRequests; ++i) {
    if (!t.pending[i].used) continue;
    const uint32_t at = requestOffset(graph, t.pending[i].request, from, frames);
    if (at < *offset) {
      *offset = at;
      best = static_cast<int>(i);
    }
  }
  return best;
}

// Where the loop wraps in [from, frames), or `frames` when it does not.
uint32_t loopWrapOffset(AudGraph* graph, int64_t position, uint32_t from,
                        uint32_t frames) {
  const InternalTransport& t = graph->transport;
  if (!t.playing || !t.looping || !(t.tempo > 0)) return frames;
  const int64_t beat = beatAt(graph, position + from);
  if (beat >= t.loopEnd) return from;
  const double beats = aud_beats_from_ticks(t.loopEnd - beat);
  const double toEnd =
      std::ceil(beats * 60.0 * graph->sampleRate / t.tempo - 1e-9);
  if (toEnd >= static_cast<double>(frames - from)) return frames;
  return from + static_cast<uint32_t>(toEnd);
}

// Captures the segments of the block from the internal transport: the
// pending requests and the loop split the block where they take effect.
void captureInternal(AudGraph* graph, uint32_t frames) {
  InternalTransport& t = graph->transport;
  const int64_t position = graph->streamTime.sample_position;
  uint32_t numSegments = 0;
  uint32_t offset = 0;
  uint32_t flags = graph->pendingSeek ? AUD_SEGMENT_SEEK : 0;
  while (true) {
    uint32_t bestOffset = frames;
    const int best = earliestRequest(graph, offset, frames, &bestOffset);
    const uint32_t wrapOffset = loopWrapOffset(graph, position, offset, frames);
    const uint32_t next = std::min(bestOffset, wrapOffset);
    if (next > offset) {
      fillSegment(graph, &graph->segments[numSegments++], offset, next - offset,
                  flags);
      flags = 0;
      if (numSegments == kMaxSegments) break;
    }
    if (next >= frames) break;
    if (wrapOffset == next && wrapOffset <= bestOffset) {
      t.anchorSample = position + next;
      t.anchorBeat = t.loopStart;
      flags |= AUD_SEGMENT_DISCONTINUITY;
    } else {
      bool seek = false;
      PendingRequest& pending = t.pending[best];
      pending.used = false;
      if (!applyInternalRequest(graph, pending.request, position + next, &seek)) {
        graph->diagnostics.transportRefused += 1;
      }
      if (seek) flags |= AUD_SEGMENT_SEEK;
    }
    offset = next;
  }
  if (numSegments == 0) {
    fillSegment(graph, &graph->segments[numSegments++], 0, frames, flags);
  }
  graph->snapshot.struct_size = sizeof(AudTransportSnapshot);
  graph->snapshot.capabilities =
      AUD_TRANSPORT_CAP_TEMPO | AUD_TRANSPORT_CAP_SEEK |
      AUD_TRANSPORT_CAP_START_STOP | AUD_TRANSPORT_CAP_TIME_SIGNATURE |
      AUD_TRANSPORT_CAP_HOST_TIME | AUD_TRANSPORT_CAP_LOOP;
  graph->snapshot.num_segments = numSegments;
  graph->snapshot.segments = graph->segments;
  graph->snapshot.time = graph->streamTime;
}

// A single stopped segment over the block, when no transport says more.
void stoppedSegment(AudGraph* graph, uint32_t frames) {
  fillSegment(graph, &graph->segments[0], 0, frames, 0);
  graph->segments[0].flags &= ~uint32_t{AUD_SEGMENT_PLAYING};
}

void captureTransport(AudGraph* graph, const AudRenderRequest* request) {
  const uint32_t frames = request->frames;
  if (request->transport != nullptr &&
      request->transport->struct_size >= sizeof(AudTransportSnapshot)) {
    // The plugin host's transport takes precedence.
    const AudTransportSnapshot& host = *request->transport;
    const uint32_t count = std::min(host.num_segments, kMaxSegments);
    for (uint32_t i = 0; i < count; ++i) graph->segments[i] = host.segments[i];
    graph->snapshot.struct_size = sizeof(AudTransportSnapshot);
    graph->snapshot.capabilities = host.capabilities;
    graph->snapshot.num_segments = count;
    graph->snapshot.segments = graph->segments;
    graph->snapshot.time = graph->streamTime;
    if (count == 0) {
      stoppedSegment(graph, frames);
      graph->snapshot.num_segments = 1;
    }
    return;
  }
  if (graph->selectedProvider >= 0) {
    const TransportProvider& provider =
        graph->providers[static_cast<size_t>(graph->selectedProvider)];
    uint32_t count = 0;
    const int32_t result = provider.vtable->capture(
        provider.provider, &graph->streamTime, graph->segments, kMaxSegments,
        &count);
    if (result != AUD_OK || count == 0) {
      stoppedSegment(graph, frames);
      count = 1;
    }
    graph->snapshot.struct_size = sizeof(AudTransportSnapshot);
    graph->snapshot.capabilities = provider.capabilities;
    graph->snapshot.num_segments = count;
    graph->snapshot.segments = graph->segments;
    graph->snapshot.time = graph->streamTime;
    return;
  }
  captureInternal(graph, frames);
}

// Publishes the transport at the end of the block.
void publishTransport(AudGraph* graph, uint32_t frames) {
  const AudTransportSegment& last =
      graph->segments[graph->snapshot.num_segments - 1];
  const bool playing = (last.flags & AUD_SEGMENT_PLAYING) != 0;
  int64_t beat = last.beat;
  if (playing) {
    beat += aud_ticks_from_beats(aud_transport_beats_after(
        &last, graph->sampleRate,
        static_cast<double>(frames - last.sample_offset)));
  }
  graph->publishedPlaying.store(playing ? 1 : 0, std::memory_order_relaxed);
  graph->publishedBeat.store(beat, std::memory_order_relaxed);
  graph->publishedTempo.store(last.tempo, std::memory_order_relaxed);
  graph->publishedNumerator.store(last.time_signature_numerator,
                                  std::memory_order_relaxed);
  graph->publishedDenominator.store(last.time_signature_denominator,
                                    std::memory_order_relaxed);
  graph->publishedLooping.store((last.flags & AUD_SEGMENT_LOOPING) ? 1 : 0,
                                std::memory_order_relaxed);
  graph->publishedLoopStart.store(last.loop_start, std::memory_order_relaxed);
  graph->publishedLoopEnd.store(last.loop_end, std::memory_order_release);
}

// Resets the nodes that asked for it when the transport stops or seeks.
void applyTransportResets(AudGraph* graph) {
  Program* program = graph->current;
  const AudTransportSnapshot& snapshot = graph->snapshot;
  bool stopped = false;
  bool seeked = false;
  bool playing = graph->lastPlaying;
  for (uint32_t i = 0; i < snapshot.num_segments; ++i) {
    const AudTransportSegment& segment = snapshot.segments[i];
    const bool now = (segment.flags & AUD_SEGMENT_PLAYING) != 0;
    if (playing && !now) stopped = true;
    if (segment.flags & AUD_SEGMENT_SEEK) seeked = true;
    playing = now;
  }
  graph->lastPlaying = playing;
  if (program == nullptr || (!stopped && !seeked)) return;
  for (uint32_t i = 0; i < program->nodes.size(); ++i) {
    ProgramNode& node = program->nodes[i];
    NodeInstance* instance = node.instance;
    if (node.skip || instance->instance == nullptr) continue;
    const uint32_t caps = instance->descriptor->capabilities;
    const bool onStop = stopped && (caps & AUD_NODE_CAP_RESET_ON_STOP);
    const bool onSeek = seeked && (caps & AUD_NODE_CAP_RESET_ON_SEEK);
    if (!onStop && !onSeek) continue;
    const uint32_t reason = onStop ? AUD_RESET_STOP : AUD_RESET_SEEK;
    if (instance->parked.load(std::memory_order_seq_cst)) {
      // A state call runs: the reset waits for the node's next block.
      instance->pendingReset = reason;
    } else if (instance->descriptor->vtable->reset != nullptr) {
      instance->descriptor->vtable->reset(instance->instance, reason);
    }
    closeNotes(graph, instance, i);
  }
}

// ............................................................................
// Resolving event times with the lead of their node (graph-001)

double currentTempo(const AudGraph* graph) {
  const AudTransportSnapshot& snapshot = graph->snapshot;
  if (snapshot.num_segments == 0) return graph->transport.tempo;
  return snapshot.segments[snapshot.num_segments - 1].tempo;
}

// The key a scheduled event is ordered by: its time minus the lead.
int64_t keyOf(const AudGraph* graph, const AudTimestamp& at, uint32_t lead) {
  switch (at.domain) {
    case AUD_TIME_SAMPLE:
      return at.value - lead;
    case AUD_TIME_HOST:
      return at.value - static_cast<int64_t>(std::llround(
                            static_cast<double>(lead) * nsPerSample(graph)));
    case AUD_TIME_BEAT:
      return at.value - aud_ticks_from_beats(static_cast<double>(lead) /
                                             graph->sampleRate *
                                             currentTempo(graph) / 60.0);
    default:
      return at.value;
  }
}

// Resolves a timestamp to an offset of the block, `lead` frames early.
int32_t resolveLead(AudGraph* graph, const AudTimestamp& at, uint32_t lead,
                    uint32_t* offset) {
  int64_t resolved = 0;
  if (lead == 0) {
    const int32_t result = aud_transport_resolve(&graph->snapshot, &at, &resolved);
    *offset = static_cast<uint32_t>(resolved);
    return result;
  }
  AudTransportSegment segments[kMaxSegments];
  AudTransportSnapshot shifted = graph->snapshot;
  for (uint32_t i = 0; i < shifted.num_segments; ++i) {
    segments[i] = graph->snapshot.segments[i];
    segments[i].beat += aud_ticks_from_beats(aud_transport_beats_after(
        &segments[i], graph->sampleRate, static_cast<double>(lead)));
  }
  shifted.segments = segments;
  shifted.time.sample_position += lead;
  shifted.time.host_time_ns += static_cast<int64_t>(
      std::llround(static_cast<double>(lead) * nsPerSample(graph)));
  const int32_t result = aud_transport_resolve(&shifted, &at, &resolved);
  if (result == AUD_OK) {
    *offset = static_cast<uint32_t>(resolved);
    return AUD_OK;
  }
  if (result != AUD_ERROR_LATE) return result;
  // Too close to be pre-delivered in full: as early as possible, unless
  // the time itself has passed.
  const int32_t plain = aud_transport_resolve(&graph->snapshot, &at, &resolved);
  if (plain == AUD_OK || plain == AUD_PENDING) {
    *offset = 0;
    return AUD_OK;
  }
  return plain;
}

uint32_t leadOf(const AudGraph* graph, const NodeInstance* instance,
                const AudEvent& event) {
  if (instance == nullptr || (event.flags & AUD_EVENT_FLAG_LIVE)) return 0;
  return graph->current->nodes[instance->rtIndex].lead;
}

// Whether an instance can take events in this block.
bool acceptsEvents(const NodeInstance* instance) {
  return instance->rtIndex != UINT32_MAX && !instance->rtDone &&
         !instance->rtRetired;
}

void deliverNow(AudGraph* graph, NodeInstance* instance, const AudEvent& event,
                uint32_t offset, uint64_t sequence) {
  if (instance == nullptr) {
    routeGraphEvent(graph, event, offset, sequence);
    return;
  }
  addBlockEvent(graph, instance->rtIndex, event, offset, sequence);
}

void dropLate(AudGraph* graph, NodeInstance* instance, const AudEvent& event,
              uint64_t sequence) {
  graph->diagnostics.late += 1;
  if (graph->config.flags & AUD_GRAPH_DROP_LATE_EVENTS) {
    graph->eventsDropped.fetch_add(1, std::memory_order_relaxed);
    releaseMessage(graph, event);
    return;
  }
  graph->eventsLate.fetch_add(1, std::memory_order_relaxed);
  deliverNow(graph, instance, event, 0, sequence);
}

// Places an event of the queue: into the block, into the scheduler, or
// nowhere; true when the scheduler keeps it.
bool deliverEvent(AudGraph* graph, const Command& command) {
  NodeInstance* instance = command.instance;
  if (instance != nullptr && (graph->current == nullptr || !acceptsEvents(instance))) {
    graph->diagnostics.retired += 1;
    graph->eventsDropped.fetch_add(1, std::memory_order_relaxed);
    releaseMessage(graph, command.event);
    return false;
  }
  if (command.at.domain == AUD_TIME_IMMEDIATE) {
    deliverNow(graph, instance, command.event, 0, command.sequence);
    return false;
  }
  const uint32_t lead = leadOf(graph, instance, command.event);
  uint32_t offset = 0;
  const int32_t result = resolveLead(graph, command.at, lead, &offset);
  if (result == AUD_OK) {
    deliverNow(graph, instance, command.event, offset, command.sequence);
    return false;
  }
  if (result == AUD_ERROR_LATE) {
    dropLate(graph, instance, command.event, command.sequence);
    return false;
  }
  if (result == AUD_ERROR_INVALID_ARGUMENT) {
    graph->eventsDropped.fetch_add(1, std::memory_order_relaxed);
    releaseMessage(graph, command.event);
    return false;
  }
  ScheduledEvent scheduled;
  scheduled.instance = instance;
  scheduled.event = command.event;
  scheduled.at = command.at;
  scheduled.key = keyOf(graph, command.at, lead);
  scheduled.id = command.id;
  scheduled.sequence = command.sequence;
  // The reservation of the control thread keeps a place free for it; the
  // check stays for a scheduler that is full anyway.
  if (!graph->scheduler.push(scheduled)) {
    graph->diagnostics.schedulerFull += 1;
    graph->eventsDropped.fetch_add(1, std::memory_order_relaxed);
    releaseMessage(graph, command.event);
    return false;
  }
  return true;
}

// Moves the events whose time has come from the scheduler into the block:
// at most the budget of a block (interop-002); the rest waits in the
// scheduler, in order, and follows in the next block.
void popScheduled(AudGraph* graph) {
  Scheduler& scheduler = graph->scheduler;
  uint32_t budget = graph->config.max_events_per_block;
  for (uint32_t domain = 0; domain < 3; ++domain) {
    while (budget > 0) {
      const uint32_t index = scheduler.top(domain);
      if (index == UINT32_MAX) break;
      const ScheduledEvent& event = scheduler.pool[index];
      NodeInstance* instance = event.instance;
      // An event of the graph's event output becomes one block event per
      // route; without room for all of them it waits for the next block.
      const uint32_t needed =
          instance == nullptr && graph->current != nullptr
              ? graph->current->graphFanout
              : 1;
      if (graph->numBlockEvents + needed > graph->blockEvents.size()) break;
      if (instance != nullptr && (graph->current == nullptr || !acceptsEvents(instance))) {
        releaseMessage(graph, event.event);
        scheduler.pop(domain);
        scheduler.release(index);
        releaseScheduled(graph, 1);
        graph->diagnostics.retired += 1;
        graph->eventsDropped.fetch_add(1, std::memory_order_relaxed);
        continue;
      }
      const uint32_t lead = leadOf(graph, instance, event.event);
      uint32_t offset = 0;
      const int32_t result = resolveLead(graph, event.at, lead, &offset);
      if (result == AUD_PENDING || result == AUD_ERROR_UNSUPPORTED) break;
      const ScheduledEvent copy = event;
      scheduler.pop(domain);
      scheduler.release(index);
      releaseScheduled(graph, 1);
      budget -= 1;
      if (result == AUD_OK) {
        deliverNow(graph, copy.instance, copy.event, offset, copy.sequence);
      } else if (result == AUD_ERROR_LATE) {
        dropLate(graph, copy.instance, copy.event, copy.sequence);
      } else {
        graph->eventsDropped.fetch_add(1, std::memory_order_relaxed);
        releaseMessage(graph, copy.event);
      }
    }
  }
}

// ............................................................................
// The event sources (interop-003, midi-003, ticket 26)

// The event of a packet of a source.
AudEvent sourceEvent(const SourceEvent& packet, uint32_t port, uint32_t flags) {
  AudEvent event = aud_event_ump(packet.words, packet.numWords, 0, port);
  event.flags = flags;
  return event;
}

// The control change of a MIDI 1.0 or 2.0 packet: true with the group, the
// channel and the controller number.
bool controlChangeOf(const SourceEvent& packet, uint32_t* group,
                     uint32_t* channel, uint32_t* controller) {
  const uint32_t word0 = packet.words[0];
  if (!aud_ump_is_channel_voice(word0)) return false;
  if (aud_ump_status(word0) != AUD_UMP_STATUS_CONTROL_CHANGE) return false;
  *group = aud_ump_group(word0);
  *channel = aud_ump_channel(word0);
  *controller = (word0 >> 8) & 0x7f;  // data1 of MIDI 1.0, index of MIDI 2.0
  return true;
}

// Remembers a sustain or soft pedal the source delivered, so that a
// release lifts it.
void notePedal(AudEventSource* source, const SourceEvent& packet) {
  uint32_t group = 0;
  uint32_t channel = 0;
  uint32_t controller = 0;
  if (!controlChangeOf(packet, &group, &channel, &controller)) return;
  if (controller == 64) source->sustain[group] |= uint16_t(1u << channel);
  if (controller == 66) source->soft[group] |= uint16_t(1u << channel);
}

// Counts a delivered packet: the activity and the input to output time,
// the time at which the frame it was placed at - `lead` frames before its
// time - reaches the output, minus its timestamp.
void countDelivered(AudGraph* graph, AudEventSource* source,
                    const SourceEvent& packet, uint32_t offset, uint32_t lead) {
  source->delivered.fetch_add(1, std::memory_order_relaxed);
  source->lastWord0.store(packet.words[0], std::memory_order_relaxed);
  source->lastTimestamp.store(packet.timestampNs, std::memory_order_relaxed);
  const int64_t outputNs =
      graph->streamTime.host_time_ns +
      static_cast<int64_t>(std::llround(static_cast<double>(offset + lead) *
                                        nsPerSample(graph)));
  const int64_t latency = outputNs - packet.timestampNs;
  uint32_t bin = AUD_SOURCE_LATENCY_BINS;
  if (latency >= 0) {
    const int64_t index = latency / AUD_SOURCE_LATENCY_BIN_NS;
    if (index < AUD_SOURCE_LATENCY_BINS) bin = static_cast<uint32_t>(index);
  } else {
    bin = 0;
  }
  source->latencyHistogram[bin].fetch_add(1, std::memory_order_relaxed);
}

// Places a packet of a source into the block at `offset`; the caller made
// sure the table has room.
void deliverSourcePacket(AudGraph* graph, AudEventSource* source,
                         const SourceEvent& packet, uint32_t offset,
                         uint32_t lead, uint32_t flags) {
  const AudEvent event = sourceEvent(packet, source->port, flags);
  addBlockEvent(graph, source->target->rtIndex, event, offset,
                graph->sourceSequence++);
  notePedal(source, packet);
  countDelivered(graph, source, packet, offset, lead);
}

// The offset of a packet under AUD_SOURCE_TIMING_FIXED, with the lead of
// the target: AUD_OK, AUD_ERROR_LATE with the offset 0 and the lateness,
// or AUD_PENDING when the packet belongs to a later block.
int32_t fixedOffset(AudGraph* graph, const AudEventSource* source,
                    const SourceEvent& packet, uint32_t lead, uint32_t* offset,
                    int64_t* lateNs) {
  AudTimestamp at{};
  at.struct_size = sizeof(AudTimestamp);
  at.domain = AUD_TIME_HOST;
  at.value = packet.timestampNs + source->latencyNs.load(std::memory_order_relaxed);
  const int32_t result = resolveLead(graph, at, lead, offset);
  if (result == AUD_OK) return AUD_OK;
  if (result == AUD_PENDING) return AUD_PENDING;
  // Late, or a time the snapshot cannot resolve: the block start.
  *offset = 0;
  *lateNs = std::max<int64_t>(0, graph->streamTime.host_time_ns - at.value);
  return AUD_ERROR_LATE;
}

// Empties the queue of a source for a release: note offs and controllers
// at the block start, note ons dropped. A drop spends one of the block's
// `budget` like a delivery of the drain, so that at most
// max_events_per_block packets leave the queues per block. False when the
// block's table filled or the budget ran out before the queue was empty;
// the release then goes on in the next block.
bool flushSource(AudGraph* graph, AudEventSource* source, bool accepts,
                 uint32_t* budget) {
  NodeInstance* target = source->target;
  while (const SourceEvent* packet = source->queue->peek()) {
    const bool noteOn = aud_ump_is_note_on(packet->words[0], packet->words[1]);
    if (!accepts || noteOn) {
      if (*budget == 0) return false;
      source->dropped.fetch_add(1, std::memory_order_relaxed);
      source->queue->pop();
      *budget -= 1;
      continue;
    }
    if (!blockHasRoom(graph)) return false;
    deliverSourcePacket(graph, source, *packet, 0, 0, 0);
    // A note off the flush delivers closes its note here - after its rank
    // was taken, so that it keeps its place before the controllers - and
    // the closing of the target's notes does not repeat it.
    if (aud_ump_is_note_off(packet->words[0], packet->words[1])) {
      target->notes.remove(noteKey(sourceEvent(*packet, source->port, 0)));
    }
    source->queue->pop();
  }
  return true;
}

// Lifts one pedal of a source - CC `controller` = 0 on `channel` of
// `group`, as a MIDI 1.0 control change - when its bit is set in
// `channels`, and clears the bit; false when the block's table has no
// room, the pedal then stays noted for the next block.
bool liftPedal(AudGraph* graph, AudEventSource* source, uint32_t group,
               uint32_t channel, uint32_t controller, uint16_t* channels) {
  const uint16_t bit = uint16_t(1u << channel);
  if ((*channels & bit) == 0) return true;
  if (!blockHasRoom(graph)) return false;
  const uint32_t word = aud_ump_midi1_word(group, 0xB0 | channel, controller, 0);
  addBlockEvent(graph, source->target->rtIndex,
                aud_event_ump(&word, 1, 0, source->port), 0,
                graph->sourceSequence++);
  *channels &= uint16_t(~bit);
  return true;
}

// Lifts the pedals the source delivered: CC64 = 0 and CC66 = 0 on every
// group and channel that saw one, as far as the block's table has room.
// True once every pedal is lifted.
bool liftPedals(AudGraph* graph, AudEventSource* source) {
  for (uint32_t group = 0; group < 16; ++group) {
    for (uint32_t channel = 0; channel < 16; ++channel) {
      if (!liftPedal(graph, source, group, channel, 64,
                     &source->sustain[group])) {
        return false;
      }
      if (!liftPedal(graph, source, group, channel, 66, &source->soft[group])) {
        return false;
      }
    }
  }
  return true;
}

// The target retired in this block: the adoption closed its notes, the
// pedals the source delivered lift with them, once - over several blocks
// when the table is full; the node still runs its fade and forwards what
// it gets.
void releaseRetiredTarget(AudGraph* graph, AudEventSource* source) {
  const NodeInstance* target = source->target;
  if (source->targetReleased || graph->current == nullptr ||
      target->rtIndex == UINT32_MAX || !target->rtRetired || target->rtDone) {
    return;
  }
  if (!liftPedals(graph, source)) return;
  source->targetReleased = true;
  source->releases.fetch_add(1, std::memory_order_relaxed);
}

// Carries out a release: the queue empties - its drops within the block's
// `budget` -, then the target's notes close and its pedals lift, each as
// far as the block's table has room - the table takes the release of one
// source after full budgets, several sources in one block carry the rest
// over. True once it is complete.
bool releaseSource(AudGraph* graph, AudEventSource* source, bool accepts,
                   uint32_t* budget) {
  if (!flushSource(graph, source, accepts, budget)) return false;
  if (accepts) {
    if (!closeNotes(graph, source->target, source->target->rtIndex, true)) {
      return false;
    }
    if (!liftPedals(graph, source)) return false;
  } else {
    for (uint32_t group = 0; group < 16; ++group) {
      source->sustain[group] = 0;
      source->soft[group] = 0;
    }
  }
  source->releases.fetch_add(1, std::memory_order_relaxed);
  return true;
}

// Drains one source with what is left of the budget: peek and pop until a
// packet belongs to a later block or the table is full. A packet dropped
// for a target that takes no events spends the budget like a delivered
// one, so that at most max_events_per_block packets leave per block.
void drainSource(AudGraph* graph, AudEventSource* source, bool accepts,
                 uint32_t* budget) {
  NodeInstance* target = source->target;
  const bool immediate = source->timing == AUD_SOURCE_TIMING_IMMEDIATE;
  while (*budget > 0) {
    const SourceEvent* packet = source->queue->peek();
    if (packet == nullptr) return;
    if (!accepts) {
      source->dropped.fetch_add(1, std::memory_order_relaxed);
      source->queue->pop();
      *budget -= 1;
      continue;
    }
    if (!blockHasRoom(graph)) return;
    uint32_t offset = 0;
    uint32_t lead = 0;
    uint32_t flags = 0;
    if (immediate) {
      flags = AUD_EVENT_FLAG_LIVE;
    } else {
      lead = graph->current->nodes[target->rtIndex].lead;
      int64_t lateNs = 0;
      const int32_t result =
          fixedOffset(graph, source, *packet, lead, &offset, &lateNs);
      if (result == AUD_PENDING) return;
      if (result == AUD_ERROR_LATE) {
        source->late.fetch_add(1, std::memory_order_relaxed);
        if (lateNs > source->maxLateNs.load(std::memory_order_relaxed)) {
          source->maxLateNs.store(lateNs, std::memory_order_relaxed);
        }
      }
    }
    deliverSourcePacket(graph, source, *packet, offset, lead, flags);
    source->queue->pop();
    *budget -= 1;
  }
}

}  // namespace

// [realtime] Drains the sources in the order of their slots: a release
// first, then the packets whose time has come, within one budget for all
// sources together, delivered or dropped (interop-002); a retired source
// is released and let go.
void drainSources(AudGraph* graph) {
  uint32_t budget = graph->config.max_events_per_block;
  for (std::atomic<AudEventSource*>& slot : graph->sourceSlots) {
    AudEventSource* source = slot.load(std::memory_order_acquire);
    if (source == nullptr) continue;
    const bool accepts =
        graph->current != nullptr && acceptsEvents(source->target);
    if (!accepts) releaseRetiredTarget(graph, source);
    if (source->retired.load(std::memory_order_acquire)) {
      // The last act: the notes of the target close, then the source is
      // the control thread's again. A table too full for the flush waits
      // for the next block, as a release does.
      if (!releaseSource(graph, source, accepts, &budget)) continue;
      AudGraphNotification n = makeNotification(graph, AUD_NOTIFY_SOURCE_DONE);
      n.node = source->node;
      n.value = static_cast<int64_t>(reinterpret_cast<intptr_t>(source));
      notify(graph, n);
      // The slot clears before `done` is stored: the control thread frees
      // the source on `done` and may create the next one into the same
      // slot at once, so the slot must be no one's by then.
      AudEventSource* expected = source;
      slot.compare_exchange_strong(expected, nullptr,
                                   std::memory_order_acq_rel);
      source->done.store(true, std::memory_order_release);
      continue;
    }
    if (source->releaseRequested.exchange(false, std::memory_order_acq_rel)) {
      source->releasing = true;
    }
    if (source->releasing) {
      source->releasing = !releaseSource(graph, source, accepts, &budget);
      continue;
    }
    drainSource(graph, source, accepts, &budget);
  }
}

namespace {

// ............................................................................
// The queues (interop-002)

void handleTransportRequest(AudGraph* graph, const AudTransportRequest& request) {
  if (graph->selectedProvider >= 0) {
    const TransportProvider& provider =
        graph->providers[static_cast<size_t>(graph->selectedProvider)];
    if (provider.vtable->request(provider.provider, &request) != AUD_OK) {
      graph->diagnostics.transportRefused += 1;
    }
    return;
  }
  if (request.at.domain == AUD_TIME_IMMEDIATE) {
    bool seek = false;
    if (!applyInternalRequest(graph, request, graph->streamTime.sample_position,
                              &seek)) {
      graph->diagnostics.transportRefused += 1;
    }
    if (seek) graph->pendingSeek = true;
    return;
  }
  for (PendingRequest& pending : graph->transport.pending) {
    if (pending.used) continue;
    pending.used = true;
    pending.request = request;
    return;
  }
  graph->diagnostics.transportRefused += 1;
}

void drainParams(AudGraph* graph) {
  uint32_t budget = graph->config.max_events_per_block;
  ParamCommand command;
  uint64_t popped = graph->poppedSequence.load(std::memory_order_relaxed);
  while (budget > 0 && graph->params->pop(command)) {
    budget -= 1;
    popped = std::max(popped, command.sequence);
    NodeInstance* instance = command.instance;
    if (instance->rtIndex == UINT32_MAX || instance->rtDone ||
        graph->current == nullptr) {
      graph->eventsDropped.fetch_add(1, std::memory_order_relaxed);
      continue;
    }
    if (command.rampFrames == 0 &&
        !instance->parked.load(std::memory_order_seq_cst)) {
      instance->descriptor->vtable->set_param(instance->instance, command.param,
                                              command.value);
    } else {
      // A ramp, or a node parked for a state call: the change travels as an
      // event of the block, postponed with the node's other events.
      addBlockEvent(graph, instance->rtIndex,
                    aud_event_param(command.param, command.value,
                                    command.rampFrames, 0),
                    0, command.sequence);
    }
    graph->paramsApplied.fetch_add(1, std::memory_order_relaxed);
  }
  graph->poppedSequence.store(popped, std::memory_order_release);
}

// Pops the commands of the block: transport requests apply at once so
// that the block's segments reflect them; events and cancellations wait
// in their order until the transport of the block is known.
void drainCommands(AudGraph* graph) {
  uint32_t budget = graph->config.max_events_per_block;
  graph->numPopped = 0;
  uint64_t popped = graph->poppedSequence.load(std::memory_order_relaxed);
  Command command;
  while (budget > 0 && graph->commands->pop(command)) {
    budget -= 1;
    popped = std::max(popped, command.sequence);
    if (command.kind == CommandKind::transport) {
      handleTransportRequest(graph, command.request);
    } else {
      graph->popped[graph->numPopped++] = command;
    }
  }
  graph->poppedSequence.store(popped, std::memory_order_release);
}

// Closes the notes a stop or a prepare left open (lifecycle-001): the
// control thread reset the nodes while no block rendered, the note offs
// reach them at the start of the first block after the restart.
void closePendingNotes(AudGraph* graph) {
  if (!graph->closeNotesPending) return;
  graph->closeNotesPending = false;
  Program* program = graph->current;
  if (program == nullptr) return;
  for (uint32_t i = 0; i < program->nodes.size(); ++i) {
    NodeInstance* instance = program->nodes[i].instance;
    if (instance->notes.count > 0) closeNotes(graph, instance, i);
  }
}

// Hands the events postponed for parked nodes to the block, at its start
// and in their order, to the nodes that still take events (ticket 20).
void deliverDeferred(AudGraph* graph) {
  const uint32_t count = graph->numDeferred;
  graph->numDeferred = 0;
  for (uint32_t i = 0; i < count; ++i) {
    const DeferredEvent& deferred = graph->deferred[i];
    if (graph->current == nullptr || !acceptsEvents(deferred.instance)) {
      graph->diagnostics.retired += 1;
      graph->eventsDropped.fetch_add(1, std::memory_order_relaxed);
      releaseMessage(graph, deferred.event);
      continue;
    }
    addBlockEvent(graph, deferred.instance->rtIndex, deferred.event, 0,
                  deferred.sequence);
  }
}

// Places the popped events and applies the cancellations in their order.
void deliverPopped(AudGraph* graph) {
  for (uint32_t i = 0; i < graph->numPopped; ++i) {
    const Command& command = graph->popped[i];
    if (command.kind == CommandKind::cancel) {
      releaseScheduled(graph,
                       graph->scheduler.cancel(command.instance, command.id,
                                               releaseRemoved, graph));
    } else if (!deliverEvent(graph, command) && command.reserved) {
      releaseScheduled(graph, 1);
    }
  }
}

void sortBlockEvents(AudGraph* graph) {
  BlockEvent* begin = graph->blockEvents.data();
  std::sort(begin, begin + graph->numBlockEvents,
            [](const BlockEvent& a, const BlockEvent& b) {
              if (a.node != b.node) return a.node < b.node;
              if (a.event.sample_offset != b.event.sample_offset) {
                return a.event.sample_offset < b.event.sample_offset;
              }
              if (a.rank != b.rank) return a.rank < b.rank;
              return a.sequence < b.sequence;
            });
  const Program* program = graph->current;
  const uint32_t nodes = program == nullptr
                             ? 0
                             : static_cast<uint32_t>(program->nodes.size());
  for (uint32_t i = 0; i < nodes; ++i) {
    graph->nodeRanges[i].begin = 0;
    graph->nodeRanges[i].end = 0;
  }
  uint32_t k = 0;
  while (k < graph->numBlockEvents) {
    const uint32_t node = begin[k].node;
    const uint32_t start = k;
    while (k < graph->numBlockEvents && begin[k].node == node) ++k;
    if (node < nodes) {
      graph->nodeRanges[node].begin = start;
      graph->nodeRanges[node].end = k;
    }
  }
}

// Gathers the events of a node in ascending offset: the sorted table and
// the chain of events emitted into it during this block.
uint32_t gatherEvents(AudGraph* graph, uint32_t node, NodeInstance* instance) {
  const NodeRange& range = graph->nodeRanges[node];
  uint32_t count = 0;
  for (uint32_t k = range.begin; k < range.end; ++k) {
    graph->nodeEvents[count++] = graph->blockEvents[k].event;
  }
  uint32_t index = range.emittedHead;
  while (index != UINT32_MAX) {
    const AudEvent& event = graph->emitted[index].event;
    uint32_t position = count;
    while (position > 0 &&
           graph->nodeEvents[position - 1].sample_offset > event.sample_offset) {
      graph->nodeEvents[position] = graph->nodeEvents[position - 1];
      position -= 1;
    }
    graph->nodeEvents[position] = event;
    count += 1;
    index = graph->emitted[index].next;
  }
  uint32_t kept = 0;
  for (uint32_t i = 0; i < count; ++i) {
    if (!trackNote(graph, instance, graph->nodeEvents[i])) continue;
    if (kept != i) graph->nodeEvents[kept] = graph->nodeEvents[i];
    kept += 1;
  }
  return kept;
}

// ............................................................................
// The jobs

void patchPointers(AudGraph* graph, const AudRenderRequest* request) {
  Program* program = graph->current;
  for (uint32_t s = 0; s < program->slots.size(); ++s) {
    const Slot& slot = program->slots[s];
    float* pointer = nullptr;
    switch (slot.kind) {
      case SlotKind::pool:
        pointer = program->pool.data() +
                  static_cast<size_t>(slot.index) * program->maxFrames;
        break;
      case SlotKind::input:
        pointer = request->inputs[slot.index].channels[slot.channel];
        break;
      case SlotKind::output:
        pointer = request->outputs[slot.index].channels[slot.channel];
        break;
      case SlotKind::silence:
        pointer = graph->silence.data();
        break;
    }
    program->slotPointers[s] = pointer;
  }
  for (uint32_t k = 0; k < program->slotRefs.size(); ++k) {
    program->channelPointers[k] = program->slotPointers[program->slotRefs[k]];
  }
}

void clearChannels(float* const* channels, uint32_t count, uint32_t frames) {
  for (uint32_t c = 0; c < count; ++c) {
    std::memset(channels[c], 0, sizeof(float) * frames);
  }
}

// Mixes `x` into the destination channels of a frame with the mapping of
// aud_graph_connect.
inline void mapFrame(float* const* dst, uint32_t dstChannels, const float* x,
                     uint32_t srcChannels, float gain, bool assign,
                     uint32_t i) {
  if (srcChannels == dstChannels) {
    for (uint32_t c = 0; c < dstChannels; ++c) {
      const float v = gain * x[c];
      dst[c][i] = assign ? v : dst[c][i] + v;
    }
  } else if (srcChannels == 1) {
    const float v = gain * x[0];
    for (uint32_t c = 0; c < dstChannels; ++c) {
      dst[c][i] = assign ? v : dst[c][i] + v;
    }
  } else if (dstChannels == 1) {
    float sum = 0;
    for (uint32_t c = 0; c < srcChannels; ++c) sum += x[c];
    const float v = gain * sum / static_cast<float>(srcChannels);
    dst[0][i] = assign ? v : dst[0][i] + v;
  } else {
    const uint32_t shared = std::min(srcChannels, dstChannels);
    for (uint32_t c = 0; c < shared; ++c) {
      const float v = gain * x[c];
      dst[c][i] = assign ? v : dst[c][i] + v;
    }
    if (assign) {
      for (uint32_t c = shared; c < dstChannels; ++c) dst[c][i] = 0;
    }
  }
}

void runMix(AudGraph* graph, const Job& job, uint32_t frames) {
  Program* program = graph->current;
  Edge* edge = job.edge;
  float* const* src = program->channelPointers.data() + job.srcRef;
  float* const* dst = program->channelPointers.data() + job.dstRef;
  if (edge->inactive.load(std::memory_order_relaxed)) {
    if (job.assign) clearChannels(dst, job.dstChannels, frames);
    return;
  }
  const float gain = edge->gain;
  float step = 0;
  uint32_t ramp = 0;
  if (edge->remaining > 0) {
    ramp = std::min(frames, edge->remaining);
    step = (edge->target - edge->gain) / static_cast<float>(edge->remaining);
  }
  const float after = ramp > 0 && edge->remaining <= frames ? edge->target : gain;
  const bool delayed = edge->delayFrames > 0;
  if (!delayed && ramp == 0 && job.srcChannels == job.dstChannels) {
    // The common case: a steady connection between equal buses.
    for (uint32_t c = 0; c < job.dstChannels; ++c) {
      const float* s = src[c];
      float* d = dst[c];
      if (job.assign) {
        if (gain == 1) {
          std::memcpy(d, s, sizeof(float) * frames);
        } else {
          for (uint32_t i = 0; i < frames; ++i) d[i] = gain * s[i];
        }
      } else {
        for (uint32_t i = 0; i < frames; ++i) d[i] += gain * s[i];
      }
    }
    return;
  }
  float x[kMaxChannels];
  const uint32_t srcChannels = std::min(job.srcChannels, kMaxChannels);
  float* line = edge->delayLine.data();
  const uint32_t delay = edge->delayFrames;
  uint32_t pos = edge->delayPos;
  for (uint32_t i = 0; i < frames; ++i) {
    const float g = i < ramp ? gain + step * static_cast<float>(i) : after;
    if (delayed) {
      for (uint32_t c = 0; c < srcChannels; ++c) {
        float* cell = line + static_cast<size_t>(c) * delay + pos;
        x[c] = *cell;
        *cell = src[c][i];
      }
      pos = pos + 1 == delay ? 0 : pos + 1;
    } else {
      for (uint32_t c = 0; c < srcChannels; ++c) x[c] = src[c][i];
    }
    mapFrame(dst, job.dstChannels, x, srcChannels, g, job.assign, i);
  }
  edge->delayPos = pos;
  if (ramp > 0) {
    if (edge->remaining <= frames) {
      edge->gain = edge->target;
      edge->remaining = 0;
    } else {
      edge->gain += step * static_cast<float>(frames);
      edge->remaining -= frames;
    }
  }
}

// Re-blocks a node without AUD_NODE_CAP_VARIABLE_BLOCK (graph-002).
void runWrapped(AudGraph* graph, NodeInstance* instance,
                const AudProcessContext& context) {
  FixedBlockWrapper& w = *instance->wrapper;
  const uint32_t block = w.blockSize;
  uint32_t processed = 0;
  uint32_t nextEvent = 0;
  while (processed < context.frames) {
    const uint32_t chunk = std::min(context.frames - processed, block - w.fill);
    size_t channel = 0;
    for (uint32_t b = 0; b < context.num_input_buses; ++b) {
      for (uint32_t c = 0; c < context.inputs[b].num_channels; ++c) {
        std::memcpy(w.inputs[channel++].data() + w.fill,
                    context.inputs[b].channels[c] + processed,
                    sizeof(float) * chunk);
      }
    }
    channel = 0;
    for (uint32_t b = 0; b < context.num_output_buses; ++b) {
      for (uint32_t c = 0; c < context.outputs[b].num_channels; ++c) {
        std::memcpy(context.outputs[b].channels[c] + processed,
                    w.outputs[channel++].data() + w.fill, sizeof(float) * chunk);
      }
    }
    while (nextEvent < context.num_events &&
           context.events[nextEvent].sample_offset < processed + chunk) {
      if (w.numEvents < w.events.size()) {
        AudEvent event = context.events[nextEvent];
        event.sample_offset = w.fill + (event.sample_offset - processed);
        w.events[w.numEvents++] = event;
      } else {
        w.dropped += 1;
        releaseMessage(graph, context.events[nextEvent]);
      }
      nextEvent += 1;
    }
    w.fill += chunk;
    processed += chunk;
    if (w.fill < block) continue;
    AudProcessContext inner = context;
    inner.frames = block;
    inner.sample_position = context.sample_position + processed - block;
    inner.inputs = w.inputBuses.empty() ? nullptr : w.inputBuses.data();
    inner.outputs = w.outputBuses.empty() ? nullptr : w.outputBuses.data();
    inner.num_events = w.numEvents;
    inner.events = w.events.data();
    instance->descriptor->vtable->process(instance->instance, &inner);
    for (uint32_t i = 0; i < w.numEvents; ++i) releaseMessage(graph, w.events[i]);
    w.fill = 0;
    w.numEvents = 0;
  }
}

// Keeps an event of a parked node for the next block; 0 when the buffer
// is full and the event is dropped.
uint32_t postpone(AudGraph* graph, NodeInstance* instance,
                  const AudEvent& event, uint64_t sequence) {
  if (graph->numDeferred >= graph->deferred.size()) {
    graph->diagnostics.overflow += 1;
    graph->eventsDropped.fetch_add(1, std::memory_order_relaxed);
    releaseMessage(graph, event);
    return 0;
  }
  DeferredEvent& deferred = graph->deferred[graph->numDeferred++];
  deferred.instance = instance;
  deferred.event = event;
  deferred.sequence = sequence;
  return 1;
}

// A parked node (ticket 20): the control thread runs one of its state
// calls, so the block leaves it alone and clears its outputs; its events
// wait for the next block - a note off dropped here would hang a note.
void skipParked(AudGraph* graph, const Job& job, uint32_t frames) {
  Program* program = graph->current;
  ProgramNode& node = program->nodes[job.node];
  for (uint32_t b = 0; b < node.numOutputBuses; ++b) {
    const AudAudioBus& bus = program->buses[node.firstOutputBus + b];
    clearChannels(bus.channels, bus.num_channels, frames);
  }
  const NodeRange& range = graph->nodeRanges[job.node];
  uint32_t postponed = 0;
  for (uint32_t k = range.begin; k < range.end; ++k) {
    const BlockEvent& entry = graph->blockEvents[k];
    postponed += postpone(graph, node.instance, entry.event, entry.sequence);
  }
  for (uint32_t index = range.emittedHead; index != UINT32_MAX;
       index = graph->emitted[index].next) {
    postponed += postpone(graph, node.instance, graph->emitted[index].event, 0);
  }
  // One diagnostic when the park begins; the blocks after it postpone the
  // same events again.
  if (!node.instance->rtParked) {
    notifyDiagnostic(graph, AUD_ERROR_STATE, postponed, node.instance->handle);
    node.instance->rtParked = true;
  }
}

void runNode(AudGraph* graph, const Job& job, uint32_t frames) {
  Program* program = graph->current;
  ProgramNode& node = program->nodes[job.node];
  if (node.skip) {
    // No message reaches a node that is done; should one be in the table,
    // its slot goes back.
    const NodeRange& range = graph->nodeRanges[job.node];
    for (uint32_t k = range.begin; k < range.end; ++k) {
      releaseMessage(graph, graph->blockEvents[k].event);
    }
    return;
  }
  NodeInstance* instance = node.instance;
  if (instance->parked.load(std::memory_order_seq_cst)) {
    skipParked(graph, job, frames);
    return;
  }
  instance->rtParked = false;
  if (instance->pendingReset != kNoReset) {
    if (instance->descriptor->vtable->reset != nullptr) {
      instance->descriptor->vtable->reset(instance->instance,
                                          instance->pendingReset);
    }
    instance->pendingReset = kNoReset;
  }
  const uint32_t numEvents = gatherEvents(graph, job.node, instance);
  AudProcessContext context{};
  context.struct_size = sizeof(AudProcessContext);
  context.frames = frames;
  context.sample_rate = graph->sampleRate;
  context.sample_position = graph->streamTime.sample_position;
  context.flags = graph->offline ? AUD_PROCESS_OFFLINE : 0;
  context.num_input_buses = node.numInputBuses;
  context.inputs =
      node.numInputBuses == 0 ? nullptr : &program->buses[node.firstInputBus];
  context.num_output_buses = node.numOutputBuses;
  context.outputs = node.numOutputBuses == 0
                        ? nullptr
                        : &program->buses[node.firstOutputBus];
  context.num_events = numEvents;
  context.events = graph->nodeEvents.data();
  context.transport =
      (instance->descriptor->capabilities & AUD_NODE_CAP_TRANSPORT)
          ? &graph->snapshot
          : nullptr;
  graph->currentNode = job.node;
  if (instance->wrapper) {
    runWrapped(graph, instance, context);
  } else {
    instance->descriptor->vtable->process(instance->instance, &context);
    for (uint32_t i = 0; i < numEvents; ++i) {
      releaseMessage(graph, graph->nodeEvents[i]);
    }
  }
  graph->currentNode = UINT32_MAX;
  graph->eventsDelivered.fetch_add(numEvents, std::memory_order_relaxed);
}

void runFeedbackRead(AudGraph* graph, const Job& job, uint32_t frames) {
  Program* program = graph->current;
  NodeInstance* instance = program->nodes[job.node].instance;
  float* const* dst = program->channelPointers.data() + job.dstRef;
  const uint32_t delay = instance->delayFrames;
  const float* line = instance->delayLine.data();
  for (uint32_t c = 0; c < job.dstChannels; ++c) {
    const float* channel = line + static_cast<size_t>(c) * delay;
    uint32_t pos = instance->delayPos;
    for (uint32_t i = 0; i < frames; ++i) {
      dst[c][i] = channel[pos];
      pos = pos + 1 == delay ? 0 : pos + 1;
    }
  }
}

void runFeedbackWrite(AudGraph* graph, const Job& job, uint32_t frames) {
  Program* program = graph->current;
  NodeInstance* instance = program->nodes[job.node].instance;
  float* const* src = program->channelPointers.data() + job.srcRef;
  const uint32_t delay = instance->delayFrames;
  float* line = instance->delayLine.data();
  uint32_t pos = instance->delayPos;
  for (uint32_t c = 0; c < job.srcChannels; ++c) {
    float* channel = line + static_cast<size_t>(c) * delay;
    pos = instance->delayPos;
    for (uint32_t i = 0; i < frames; ++i) {
      channel[pos] = src[c][i];
      pos = pos + 1 == delay ? 0 : pos + 1;
    }
  }
  instance->delayPos = pos;
}

void runTap(AudGraph* graph, const Job& job, uint32_t frames) {
  Program* program = graph->current;
  ProgramNode& node = program->nodes[job.node];
  if (node.skip) return;
  NodeInstance* instance = node.instance;
  TapState& tap = instance->tap;
  float* const* src = program->channelPointers.data() + job.srcRef;
  float* const* dst = program->channelPointers.data() + job.dstRef;
  const uint32_t shared = std::min(job.srcChannels, job.dstChannels);
  for (uint32_t c = 0; c < shared; ++c) {
    std::memcpy(dst[c], src[c], sizeof(float) * frames);
  }
  for (uint32_t c = shared; c < job.dstChannels; ++c) {
    std::memset(dst[c], 0, sizeof(float) * frames);
  }
  tap.generation.fetch_add(1, std::memory_order_acq_rel);
  const uint32_t ringFrames = tap.ringFrames;
  for (uint32_t c = 0; c < tap.channels && c < job.srcChannels; ++c) {
    std::atomic<float>* ring = tap.ring.get() + static_cast<size_t>(c) * ringFrames;
    float peak = 0;
    double sum = 0;
    uint32_t pos = tap.writePos.load(std::memory_order_relaxed);
    for (uint32_t i = 0; i < frames; ++i) {
      const float v = src[c][i];
      ring[pos].store(v, std::memory_order_relaxed);
      pos = pos + 1 == ringFrames ? 0 : pos + 1;
      peak = std::max(peak, std::fabs(v));
      sum += static_cast<double>(v) * v;
    }
    tap.peak[c].store(peak, std::memory_order_relaxed);
    tap.rms[c].store(
        frames == 0 ? 0.0f : static_cast<float>(std::sqrt(sum / frames)),
        std::memory_order_relaxed);
  }
  tap.writePos.store(
      (tap.writePos.load(std::memory_order_relaxed) + frames) % ringFrames,
      std::memory_order_relaxed);
  tap.generation.fetch_add(1, std::memory_order_acq_rel);
}

void runJobs(AudGraph* graph, uint32_t frames) {
  Program* program = graph->current;
  for (const Job& job : program->jobs) {
    switch (job.kind) {
      case JobKind::clear:
        clearChannels(program->channelPointers.data() + job.dstRef,
                      job.dstChannels, frames);
        break;
      case JobKind::mix:
        runMix(graph, job, frames);
        break;
      case JobKind::node:
        runNode(graph, job, frames);
        break;
      case JobKind::feedbackRead:
        runFeedbackRead(graph, job, frames);
        break;
      case JobKind::feedbackWrite:
        runFeedbackWrite(graph, job, frames);
        break;
      case JobKind::tap:
        runTap(graph, job, frames);
        break;
    }
  }
}

// ............................................................................
// After the jobs: retirement and the fades (graph-003)

uint32_t saturatingSubtract(uint32_t value, uint32_t amount) {
  return value > amount ? value - amount : 0;
}

bool touches(const Edge* edge, const NodeInstance* instance) {
  return edge->id.from == instance->handle || edge->id.to == instance->handle;
}

void finishRetirement(AudGraph* graph, uint32_t frames) {
  Program* program = graph->current;
  const uint64_t popped = graph->poppedSequence.load(std::memory_order_relaxed);
  for (ProgramNode& node : program->nodes) {
    NodeInstance* instance = node.instance;
    if (!instance->rtRetired || instance->rtDone) continue;
    instance->fadeRemaining = saturatingSubtract(instance->fadeRemaining, frames);
    instance->tailRemaining = saturatingSubtract(instance->tailRemaining, frames);
    if (instance->tailRemaining > 0 || popped < instance->retireSequence) continue;
    instance->rtDone = true;
    node.skip = true;
    // The fade can end before the wrapper fills its block: the messages
    // it holds never reach the node, their slots come back now.
    releaseWrapped(graph, instance);
    for (Edge* edge : program->edges) {
      if (touches(edge, instance)) {
        edge->inactive.store(true, std::memory_order_release);
      }
    }
    instance->done.store(true, std::memory_order_release);
    AudGraphNotification n = makeNotification(graph, AUD_NOTIFY_NODE_DONE);
    n.node = instance->handle;
    notify(graph, n);
  }
  for (Edge* edge : program->edges) {
    if (edge->target == 0 && edge->remaining == 0 && edge->gain == 0 &&
        !edge->fadeInPending.load(std::memory_order_relaxed)) {
      edge->inactive.store(true, std::memory_order_release);
    }
  }
}

void resetScratch(AudGraph* graph) {
  graph->numBlockEvents = 0;
  graph->numEmitted = 0;
  graph->numPopped = 0;
  graph->diagnostics = BlockDiagnostics{};
  graph->blockNotified = false;
  graph->pendingSeek = false;
  graph->blockMessages = 0;
  graph->blockMessagesDropped = 0;
  graph->blockMessagesDroppedNode = 0;
  graph->numNodeReports = 0;
  graph->nodeReportsMerged = 0;
}

// Clears the event ranges of the nodes of the adopted program. Only the
// program the realtime thread owns is read: a pending one may be freed by
// the control thread at any moment when it publishes a newer one - reading
// its size here was a use after free that ThreadSanitizer found (ticket 20).
void resetNodeRanges(AudGraph* graph) {
  const Program* program = graph->current;
  const size_t count = program == nullptr ? 0 : program->nodes.size();
  for (size_t i = 0; i < count && i < graph->nodeRanges.size(); ++i) {
    graph->nodeRanges[i] = NodeRange{};
  }
}

void reportDiagnostics(AudGraph* graph) {
  const BlockDiagnostics& d = graph->diagnostics;
  notifyDiagnostic(graph, AUD_ERROR_LATE, d.late);
  notifyDiagnostic(graph, AUD_ERROR_RETIRED, d.retired);
  notifyDiagnostic(graph, AUD_ERROR_QUEUE_FULL, d.overflow);
  notifyDiagnostic(graph, AUD_ERROR_CAPACITY, d.schedulerFull + d.trackerFull,
                   d.trackerFull > 0 ? d.trackerFullNode : 0);
  notifyDiagnostic(graph, AUD_ERROR_UNSUPPORTED, d.transportRefused);
  // What the nodes reported (ticket 25): once per node and code.
  for (uint32_t i = 0; i < graph->numNodeReports; ++i) {
    const NodeReport& report = graph->nodeReports[i];
    notifyDiagnostic(graph, report.code, report.count, report.node);
  }
  notifyDiagnostic(graph, AUD_ERROR_QUEUE_FULL, graph->nodeReportsMerged);
  notifyDiagnostic(graph, AUD_ERROR_QUEUE_FULL, graph->blockMessagesDropped,
                   graph->blockMessagesDroppedNode);
  if (graph->blockMessages > 0) {
    AudGraphNotification n = makeNotification(graph, AUD_NOTIFY_MESSAGE);
    n.count = graph->blockMessages;
    notify(graph, n);
  }
}

float peakOf(const AudRenderRequest* request) {
  float peak = 0;
  for (uint32_t b = 0; b < request->num_output_buses; ++b) {
    const AudAudioBus& bus = request->outputs[b];
    for (uint32_t c = 0; c < bus.num_channels; ++c) {
      for (uint32_t i = 0; i < request->frames; ++i) {
        peak = std::max(peak, std::fabs(bus.channels[c][i]));
      }
    }
  }
  return peak;
}

bool validRequest(const AudGraph* graph, const AudRenderRequest* request) {
  if (request->frames == 0 || request->frames > graph->maxFrames) return false;
  if (request->num_input_buses != graph->inputChannels.size() ||
      request->num_output_buses != graph->outputChannels.size()) {
    return false;
  }
  for (uint32_t b = 0; b < request->num_input_buses; ++b) {
    if (request->inputs == nullptr ||
        request->inputs[b].num_channels != graph->inputChannels[b]) {
      return false;
    }
  }
  for (uint32_t b = 0; b < request->num_output_buses; ++b) {
    if (request->outputs == nullptr ||
        request->outputs[b].num_channels != graph->outputChannels[b]) {
      return false;
    }
  }
  if (request->time != nullptr &&
      request->time->struct_size >= sizeof(AudStreamTime) &&
      request->time->frames != request->frames) {
    return false;
  }
  return true;
}

void clearOutputs(const AudRenderRequest* request) {
  for (uint32_t b = 0; b < request->num_output_buses; ++b) {
    const AudAudioBus& bus = request->outputs[b];
    clearChannels(bus.channels, bus.num_channels, request->frames);
  }
}

}  // namespace

// ............................................................................
// The functions the control thread calls

void prepareRealtime(AudGraph* graph) {
  aud_time_filter_init(&graph->filter, graph->sampleRate,
                       AUD_TIME_FILTER_DEFAULT_WINDOW);
  graph->silence.assign(graph->maxFrames, 0.0f);
  resetTime(graph);
}

void resetTime(AudGraph* graph) {
  aud_time_filter_reset(&graph->filter);
  graph->synthesizedValid = false;
  resetPeriod(graph);
}

int64_t beatNow(AudGraph* graph) {
  return beatAt(graph, graph->samplePosition.load(std::memory_order_relaxed));
}

// Moves the anchor of the internal transport to the current sample
// position with the current rate and tempo, so that a following change of
// the sample rate or a stop keeps the beat (lifecycle-001).
void anchorTransport(AudGraph* graph) {
  InternalTransport& t = graph->transport;
  const int64_t position = graph->samplePosition.load(std::memory_order_relaxed);
  t.anchorBeat = beatAt(graph, position);
  t.anchorSample = position;
}

void resetTransport(AudGraph* graph) {
  anchorTransport(graph);
  for (PendingRequest& pending : graph->transport.pending) pending.used = false;
  publishTransportState(graph);
}

// Publishes the internal transport as it stands, while no block renders.
void publishTransportState(AudGraph* graph) {
  const InternalTransport& t = graph->transport;
  graph->lastPlaying = t.playing;
  graph->publishedPlaying.store(t.playing ? 1 : 0, std::memory_order_relaxed);
  graph->publishedBeat.store(t.anchorBeat, std::memory_order_relaxed);
  graph->publishedTempo.store(t.tempo, std::memory_order_relaxed);
  graph->publishedNumerator.store(t.numerator, std::memory_order_relaxed);
  graph->publishedDenominator.store(t.denominator, std::memory_order_relaxed);
  graph->publishedLooping.store(t.looping ? 1 : 0, std::memory_order_relaxed);
  graph->publishedLoopStart.store(t.loopStart, std::memory_order_relaxed);
  graph->publishedLoopEnd.store(t.loopEnd, std::memory_order_release);
}

// [realtime] A node emits an event: it goes to the nodes connected to the
// output, or to the control thread.
void emitEvent(AudGraph* graph, void* instance, const AudEvent* event) {
  const uint32_t current = graph->currentNode;
  const Program* program = graph->current;
  if (current == UINT32_MAX || program == nullptr) return;
  // Only the engine creates message events; a node answers through
  // emit_message.
  if (event->type == AUD_EVENT_MESSAGE) return;
  const ProgramNode& node = program->nodes[current];
  if (node.instance->instance != instance) return;
  const uint32_t frames = graph->streamTime.frames;
  const uint32_t offset =
      std::min(event->sample_offset, frames == 0 ? 0 : frames - 1);
  // The routes are sorted by (node, port): the first match is found by
  // binary search, the loop stops at the first other port.
  EventRoute key;
  key.fromNode = current;
  key.fromPort = event->port;
  const auto routesEnd = program->routes.end();
  auto it = std::lower_bound(
      program->routes.begin(), routesEnd, key,
      [](const EventRoute& a, const EventRoute& b) {
        return a.fromNode < b.fromNode ||
               (a.fromNode == b.fromNode && a.fromPort < b.fromPort);
      });
  for (; it != routesEnd && it->fromNode == current &&
         it->fromPort == event->port;
       ++it) {
    const EventRoute& route = *it;
    if (route.toNode == kGraphTarget) {
      AudEvent routed = *event;
      routed.port = route.toPort;
      outputEvent(graph, node.instance->handle, routed, offset);
      continue;
    }
    if (graph->numEmitted >= graph->emitted.size()) {
      graph->diagnostics.overflow += 1;
      graph->eventsDropped.fetch_add(1, std::memory_order_relaxed);
      continue;
    }
    const uint32_t index = graph->numEmitted++;
    EmittedEvent& emitted = graph->emitted[index];
    emitted.event = *event;
    emitted.event.struct_size = sizeof(AudEvent);
    emitted.event.port = route.toPort;
    emitted.event.sample_offset = offset;
    emitted.next = UINT32_MAX;
    NodeRange& range = graph->nodeRanges[route.toNode];
    if (range.emittedTail == UINT32_MAX) {
      range.emittedHead = index;
    } else {
      graph->emitted[range.emittedTail].next = index;
    }
    range.emittedTail = index;
  }
}

namespace {

// Appends `size` bytes to a message record; false when they do not fit.
// The check takes no sum, which could wrap on a 32-bit target: a blob of
// almost 4 GiB would pass it.
bool appendBytes(AudGraphMessage& record, const void* bytes, size_t size) {
  if (size > sizeof(record.data) - record.size) return false;
  std::memcpy(record.data + record.size, bytes, size);
  record.size += static_cast<uint32_t>(size);
  return true;
}

// Packs a message into a record in the layout of AudGraphMessage; false
// for a message beyond the limits of osc-002 - more than
// AUD_GRAPH_MAX_MESSAGE_ARGS arguments or AUD_GRAPH_MESSAGE_BYTES - or with
// an argument of an unknown type.
bool packMessage(AudGraphMessage& record, const AudMessage& message) {
  if (message.num_args > kMaxMessageArgs) return false;
  const char* path = message.path == nullptr ? "" : message.path;
  if (!appendBytes(record, path, std::strlen(path) + 1)) return false;
  for (uint32_t i = 0; i < message.num_args; ++i) {
    const AudMessageArg& arg = message.args[i];
    const uint8_t tag = static_cast<uint8_t>(arg.type);
    if (!appendBytes(record, &tag, 1)) return false;
    bool fits = true;
    switch (arg.type) {
      case AUD_ARG_INT32:
        fits = appendBytes(record, &arg.value.i, 4);
        break;
      case AUD_ARG_FLOAT:
        fits = appendBytes(record, &arg.value.f, 4);
        break;
      case AUD_ARG_INT64:
        fits = appendBytes(record, &arg.value.h, 8);
        break;
      case AUD_ARG_DOUBLE:
        fits = appendBytes(record, &arg.value.d, 8);
        break;
      case AUD_ARG_STRING: {
        const char* s = arg.value.s == nullptr ? "" : arg.value.s;
        fits = appendBytes(record, s, std::strlen(s) + 1);
        break;
      }
      case AUD_ARG_BLOB: {
        // A blob without bytes goes as an empty one.
        const uint32_t size = arg.value.b == nullptr ? 0 : arg.size;
        fits = appendBytes(record, &size, 4) &&
               (size == 0 || appendBytes(record, arg.value.b, size));
        break;
      }
      case AUD_ARG_TRUE:
      case AUD_ARG_FALSE:
      case AUD_ARG_NIL:
      case AUD_ARG_IMPULSE:
        break;
      default:
        return false;
    }
    if (!fits) return false;
  }
  record.num_args = message.num_args;
  return true;
}

// The handle of the node whose process call runs and owns `instance`, 0
// for a call outside of one.
int32_t runningHandle(const AudGraph* graph, const void* instance) {
  const uint32_t current = graph->currentNode;
  const Program* program = graph->current;
  if (current == UINT32_MAX || program == nullptr) return 0;
  const NodeInstance* node = program->nodes[current].instance;
  return node->instance == instance ? node->handle : 0;
}

}  // namespace

// [realtime] A node sends a message to the control thread (ticket 25): the
// reply queue takes a copy; a full queue or a message beyond the limits of
// a record drops it and counts.
void emitMessage(AudGraph* graph, void* instance, const AudMessage* message) {
  const int32_t handle = runningHandle(graph, instance);
  if (handle == 0 || message == nullptr ||
      message->struct_size < sizeof(AudMessage) ||
      (message->num_args > 0 && message->args == nullptr)) {
    return;
  }
  AudGraphMessage record;
  record.struct_size = sizeof(AudGraphMessage);
  record.node = handle;
  record.sample_position = graph->streamTime.sample_position;
  record.num_args = 0;
  record.size = 0;
  if (!packMessage(record, *message) || !graph->replies->push(record)) {
    graph->blockMessagesDropped += 1;
    graph->blockMessagesDroppedNode = handle;
    graph->messagesDropped.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  graph->blockMessages += 1;
}

// [realtime] A node reports a diagnostic; the block sums them per node and
// code.
void reportNode(AudGraph* graph, void* instance, int32_t code, uint32_t count) {
  const int32_t handle = runningHandle(graph, instance);
  if (handle == 0 || count == 0) return;
  for (uint32_t i = 0; i < graph->numNodeReports; ++i) {
    NodeReport& report = graph->nodeReports[i];
    if (report.node == handle && report.code == code) {
      report.count += count;
      return;
    }
  }
  if (graph->numNodeReports == kMaxNodeReports) {
    graph->nodeReportsMerged += count;
    return;
  }
  graph->nodeReports[graph->numNodeReports++] = NodeReport{handle, code, count};
}

void releaseMessage(AudGraph* graph, const AudEvent& event) {
  if (event.type != AUD_EVENT_MESSAGE) return;
  const uint32_t slot = event.words[2];
  if (slot < graph->numMessageSlots) {
    graph->messageSlots[slot].busy.store(false, std::memory_order_release);
  }
}

// The wrapper holds the events of the block it fills until the block is
// full; a node that is done, prepared again or destroyed never gets them.
void releaseWrapped(AudGraph* graph, NodeInstance* instance) {
  FixedBlockWrapper* wrapper = instance->wrapper.get();
  if (wrapper == nullptr) return;
  for (uint32_t i = 0; i < wrapper->numEvents; ++i) {
    releaseMessage(graph, wrapper->events[i]);
  }
  // A second call frees nothing: a slot may hold another message by then.
  wrapper->numEvents = 0;
  wrapper->fill = 0;
}

// The flag keeps the semaphore at one post per wake; the control thread
// clears it before it takes the notifications.
void wakeListener(AudGraph* graph) {
  if (!graph->wakePending.exchange(true, std::memory_order_acq_rel)) {
    graph->wake.post();
  }
}

// Measures the gap since the previous render for the timing of the control
// side (ticket 26): live, the gap between two render starts, so the first
// render of a stream counts nothing; offline, the block.
// The longest gap and one bin of the histogram, a relaxed add: O(1), no
// lock, since the control side reads the bins while the block renders.
void measurePeriod(AudGraph* graph, int64_t start, uint32_t frames) {
  int64_t period = 0;
  bool measured = false;
  if (graph->offline) {
    period = static_cast<int64_t>(
        std::llround(static_cast<double>(frames) * nsPerSample(graph)));
    measured = true;
  } else if (graph->lastRenderValid) {
    period = start - graph->lastRenderStartNs;
    measured = true;
  }
  graph->lastRenderStartNs = start;
  graph->lastRenderValid = true;
  if (!measured) return;
  graph->maxPeriodNs = std::max(graph->maxPeriodNs, period);
  uint32_t bin = AUD_GRAPH_PERIOD_BINS;
  if (period < 0) {
    bin = 0;
  } else {
    const int64_t index = period / AUD_GRAPH_PERIOD_BIN_NS;
    if (index < AUD_GRAPH_PERIOD_BINS) bin = static_cast<uint32_t>(index);
  }
  graph->periodHistogram[bin].fetch_add(1, std::memory_order_relaxed);
}

// Publishes the timing of the block under the seqlock.
void publishTiming(AudGraph* graph, uint32_t frames) {
  TimingSnapshot& t = graph->timing;
  t.generation.fetch_add(1, std::memory_order_acq_rel);
  t.flags.store(AUD_TIMING_VALID | (graph->offline ? AUD_TIMING_OFFLINE : 0),
                std::memory_order_relaxed);
  t.sampleRate.store(graph->sampleRate, std::memory_order_relaxed);
  t.samplePosition.store(graph->streamTime.sample_position,
                         std::memory_order_relaxed);
  t.hostTimeNs.store(graph->streamTime.host_time_ns, std::memory_order_relaxed);
  t.frames.store(frames, std::memory_order_relaxed);
  t.outputLatencyFrames.store(graph->streamTime.output_latency_frames,
                              std::memory_order_relaxed);
  t.maxPeriodNs.store(graph->maxPeriodNs, std::memory_order_relaxed);
  t.nominalPeriodNs.store(
      static_cast<int64_t>(std::llround(static_cast<double>(graph->maxFrames) *
                                        nsPerSample(graph))),
      std::memory_order_relaxed);
  t.timeFilterResets.store(graph->filterResets.load(std::memory_order_relaxed),
                           std::memory_order_relaxed);
  t.generation.fetch_add(1, std::memory_order_acq_rel);
}

// Reports what the watchdog caught in the block (ticket 20).
void reportWatchdog(AudGraph* graph) {
  const uint32_t hits = watchdogBlockHits();
  if (hits == 0) return;
  graph->realtimeViolations.fetch_add(hits, std::memory_order_relaxed);
  notifyDiagnostic(graph, AUD_GRAPH_ERROR_REALTIME_VIOLATION, hits, 0,
                   watchdogBlockKinds());
}

// [realtime] Renders one block.
int32_t renderBlock(AudGraph* graph,
                    const AudRenderRequest* request) AUD_NONBLOCKING {
  if (!validRequest(graph, request)) {
    clearOutputs(request);
    return AUD_ERROR_INVALID_ARGUMENT;
  }
  const int64_t start = aud_clock_now_ns();
  const uint32_t frames = request->frames;
  resetScratch(graph);
  computeStreamTime(graph, request);
  measurePeriod(graph, start, frames);
  adoptPending(graph);
  resetNodeRanges(graph);
  if (!graph->stateObserved) {
    AudGraphNotification n = makeNotification(graph, AUD_NOTIFY_STATE);
    n.code = AUD_GRAPH_RUNNING;
    notify(graph, n);
    graph->stateObserved = true;
  }
  closePendingNotes(graph);
  deliverDeferred(graph);
  drainParams(graph);
  drainCommands(graph);
  captureTransport(graph, request);
  applyTransportResets(graph);
  deliverPopped(graph);
  popScheduled(graph);
  drainSources(graph);
  for (uint32_t i = 0; i < request->num_events; ++i) {
    const AudEvent& event = request->events[i];
    // Only the engine creates message events (osc-002).
    if (event.type == AUD_EVENT_MESSAGE) continue;
    routeGraphEvent(graph, event, std::min(event.sample_offset, frames - 1), 0);
  }
  sortBlockEvents(graph);
  // A program compiled for a smaller block would overflow its buffers; a
  // prepare publishes a new one, adopted above unless every retired slot
  // is taken, so this block stays silent then.
  if (graph->current != nullptr && frames <= graph->current->maxFrames) {
    patchPointers(graph, request);
    runJobs(graph, frames);
    finishRetirement(graph, frames);
  } else {
    clearOutputs(request);
    // The events of the block reach no node; their message slots go back.
    for (uint32_t i = 0; i < graph->numBlockEvents; ++i) {
      releaseMessage(graph, graph->blockEvents[i].event);
    }
  }
  publishTransport(graph, frames);
  const float peak = peakOf(request);
  if (peak > graph->outputPeak.load(std::memory_order_relaxed)) {
    graph->outputPeak.store(peak, std::memory_order_relaxed);
  }
  graph->samplePosition.store(graph->streamTime.sample_position + frames,
                              std::memory_order_release);
  publishTiming(graph, frames);
  graph->scheduledCount.store(graph->scheduler.count, std::memory_order_release);
  graph->blocksRendered.fetch_add(1, std::memory_order_relaxed);
  graph->framesRendered.fetch_add(frames, std::memory_order_relaxed);
  if (graph->hostRequest != nullptr) sortOutputEvents(graph->hostRequest);
  reportDiagnostics(graph);
  reportWatchdog(graph);
  const int64_t elapsed = aud_clock_now_ns() - start;
  graph->renderSum.fetch_add(elapsed, std::memory_order_relaxed);
  if (elapsed > graph->renderMax.load(std::memory_order_relaxed)) {
    graph->renderMax.store(elapsed, std::memory_order_relaxed);
  }
  const int64_t budget = static_cast<int64_t>(
      std::llround(static_cast<double>(frames) * nsPerSample(graph)));
  if (!graph->offline && elapsed > budget) {
    graph->overloads.fetch_add(1, std::memory_order_relaxed);
    AudGraphNotification n = makeNotification(graph, AUD_NOTIFY_DIAGNOSTIC);
    n.code = AUD_ERROR_OVERLOAD;
    n.count = 1;
    n.value = elapsed;
    notify(graph, n);
  }
  if (graph->blockNotified) wakeListener(graph);
  return AUD_OK;
}

}  // namespace aud
