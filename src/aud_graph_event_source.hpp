// @license
// Copyright (c) Audanika. All Rights Reserved.
//
// Use of this source code is governed by terms that can be
// found in the LICENSE file in the root of this package.

// An event source (interop-003, midi-003, ticket 26): the queue a native
// producer fills with UMP packets and the realtime thread drains into the
// block of its target node, with the state of a release and the counters
// the control side reads. Which thread owns a field is noted on it.

#ifndef AUD_GRAPH_EVENT_SOURCE_HPP
#define AUD_GRAPH_EVENT_SOURCE_HPP

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>

#include "aud_audio_graph.h"

namespace aud {

struct NodeInstance;

constexpr uint32_t kDefaultSourceCapacity = 4096;
constexpr uint32_t kMaxSourceCapacity = 1u << 20;
constexpr uint32_t kMaxEventSources = AUD_GRAPH_MAX_EVENT_SOURCES;
// The pedal offs a release may add to a block: sustain and soft on every
// group and channel.
constexpr uint32_t kMaxPedalOffs = 2 * 16 * 16;

// One packet in the queue of a source.
struct SourceEvent {
  uint32_t words[4] = {0, 0, 0, 0};
  uint32_t numWords = 0;
  int64_t timestampNs = 0;
};

// A fixed-capacity, lock-free single-producer single-consumer queue with
// a peek, the shape of AudSpscQueue of aud_audio_core (interop-002): the
// consumer looks at the front and pops only what it takes, so an event
// whose time has not come stays where it is. The capacity is a power of
// two.
class SourceQueue {
 public:
  explicit SourceQueue(uint32_t capacity)
      : capacity_(nextPowerOfTwo(capacity)),
        mask_(capacity_ - 1),
        slots_(new SourceEvent[capacity_]),
        head_(0),
        tail_(0) {}

  // [producer] Pushes an item; false when the queue is full.
  bool push(const SourceEvent& item) {
    const uint64_t tail = tail_.load(std::memory_order_relaxed);
    const uint64_t head = head_.load(std::memory_order_acquire);
    if (tail - head >= capacity_) return false;
    slots_[tail & mask_] = item;
    tail_.store(tail + 1, std::memory_order_release);
    return true;
  }

  // [consumer] The front item, or NULL when the queue is empty; valid
  // until the pop.
  const SourceEvent* peek() const {
    const uint64_t head = head_.load(std::memory_order_relaxed);
    const uint64_t tail = tail_.load(std::memory_order_acquire);
    if (head == tail) return nullptr;
    return &slots_[head & mask_];
  }

  // [consumer] Drops the front item after a peek.
  void pop() {
    const uint64_t head = head_.load(std::memory_order_relaxed);
    head_.store(head + 1, std::memory_order_release);
  }

  // [any thread] The items waiting; approximate from other threads.
  uint32_t size() const {
    const uint64_t tail = tail_.load(std::memory_order_acquire);
    const uint64_t head = head_.load(std::memory_order_acquire);
    return static_cast<uint32_t>(tail - head);
  }

  uint32_t capacity() const { return capacity_; }

 private:
  static uint32_t nextPowerOfTwo(uint32_t value) {
    uint32_t result = 1;
    while (result < value) result <<= 1;
    return result;
  }

  const uint32_t capacity_;
  const uint32_t mask_;
  std::unique_ptr<SourceEvent[]> slots_;
  alignas(64) std::atomic<uint64_t> head_;
  alignas(64) std::atomic<uint64_t> tail_;
};

}  // namespace aud

// An event source. The control thread creates and retires it, one
// producer pushes, the realtime thread drains; the counters are relaxed
// atomics any thread reads.
struct AudEventSource {
  // Control thread.
  AudGraph* graph = nullptr;
  aud::NodeInstance* target = nullptr;
  int32_t node = 0;
  uint32_t port = 0;
  uint32_t timing = AUD_SOURCE_TIMING_FIXED;
  uint32_t slot = 0;  // index in AudGraph::sourceSlots
  std::unique_ptr<aud::SourceQueue> queue;
  // Control thread writes, realtime thread reads at every block.
  std::atomic<int64_t> latencyNs{0};
  // The producer's token: a CAS from 0 attaches, a CAS back detaches.
  std::atomic<uint64_t> producer{0};
  // Any thread raises; the realtime thread takes it once per block.
  std::atomic<bool> releaseRequested{false};
  // Control thread sets at the destroy; the realtime thread answers with
  // `done` once it released the notes and will not touch the source again.
  std::atomic<bool> retired{false};
  std::atomic<bool> done{false};
  // Producer thread: the timestamp of the last push, so that a timestamp
  // that runs backwards is raised. The hand-over between producers orders
  // it through the token.
  int64_t lastTimestampNs = 0;
  // Realtime thread: a release that the block's table could not finish,
  // and whether the pedals lifted with the retirement of the target.
  bool releasing = false;
  bool targetReleased = false;
  // Realtime thread: the group and channel bits of the pedals delivered.
  uint16_t sustain[16] = {};
  uint16_t soft[16] = {};
  // Counters: relaxed atomics, written by the producer or the realtime
  // thread, read anywhere.
  std::atomic<uint64_t> pushed{0};
  std::atomic<uint64_t> rejected{0};
  std::atomic<uint64_t> delivered{0};
  std::atomic<uint64_t> late{0};
  std::atomic<int64_t> maxLateNs{0};
  std::atomic<uint64_t> dropped{0};
  std::atomic<uint64_t> reordered{0};
  std::atomic<uint64_t> releases{0};
  std::atomic<int64_t> maxDelayNs{0};
  std::atomic<uint32_t> lastWord0{0};
  std::atomic<int64_t> lastTimestamp{0};
  std::atomic<uint64_t> delayHistogram[AUD_SOURCE_DELAY_BINS + 1];
  std::atomic<uint64_t> latencyHistogram[AUD_SOURCE_LATENCY_BINS + 1];

  AudEventSource() {
    for (auto& bin : delayHistogram) bin.store(0, std::memory_order_relaxed);
    for (auto& bin : latencyHistogram) bin.store(0, std::memory_order_relaxed);
  }
};

namespace aud {

// aud_graph_render.cpp: [realtime] drains every source of the graph into
// the block, after the scheduler and before the host's events.
void drainSources(AudGraph* graph);

// aud_graph_event_source.cpp

// [control] Frees the retired sources the realtime thread let go of.
void collectSources(AudGraph* graph);

// [control] Frees every source while no block renders: the destroy of
// the graph.
void destroySources(AudGraph* graph);

// [control] Whether a source that is not yet freed targets the instance.
bool sourceTargets(const AudGraph* graph, const NodeInstance* instance);

}  // namespace aud

#endif  // AUD_GRAPH_EVENT_SOURCE_HPP
