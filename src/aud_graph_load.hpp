// @license
// Copyright (c) Audanika. All Rights Reserved.
//
// Use of this source code is governed by terms that can be
// found in the LICENSE file in the root of this package.

// The load gauge of the engine (ticket 29): the share of its block that
// each of the last AUD_GRAPH_LOAD_RENDERS live renders took, in a ring the
// realtime thread writes and any other thread reads without a lock.
//
// One writer: the thread that renders. A slot is one 64-bit atomic - the
// share as the bits of a float, the frames and the low 16 bits of the
// render's index as a tag -, so a slot is never torn and a reader tells a
// slot the writer overwrote since from the render it looks for: the tags
// of a render and of the render a lap later differ. The writer stores the
// slot, then the count of the renders with release order; a reader that
// loads the count with acquire order finds every render below it in its
// slot or overwritten by a later one, and stops at the first overwritten
// one. Only a reader that sleeps through 64 laps of the ring - 65536
// renders - could take a newer render for the one it looks for. An
// overload is counted after the count with release order and read before
// it with acquire order, so a reading never holds more overloads than
// renders.

#ifndef AUD_GRAPH_LOAD_HPP
#define AUD_GRAPH_LOAD_HPP

#include <atomic>
#include <cstdint>
#include <cstring>

#include "aud_audio_graph.h"

namespace aud {

// ############################################################################
// The gauge

class LoadGauge {
 public:
  // [realtime] Records a live render of `frames` frames at `sampleRate`
  // that took `elapsedNs` of the `budgetNs` its block lasts: its share,
  // and an overload when it took longer. No allocation, lock or I/O.
  void record(int64_t elapsedNs, int64_t budgetNs, uint32_t frames,
              double sampleRate) {
    const double share = budgetNs > 0 ? static_cast<double>(elapsedNs) /
                                            static_cast<double>(budgetNs)
                                      : 0.0;
    const uint64_t index = renders_.load(std::memory_order_relaxed);
    slots_[index % kSlots].store(pack(static_cast<float>(share), frames, index),
                                 std::memory_order_relaxed);
    sampleRate_.store(sampleRate, std::memory_order_relaxed);
    renders_.store(index + 1, std::memory_order_release);
    if (elapsedNs > budgetNs) {
      overloads_.fetch_add(1, std::memory_order_release);
    }
  }

  // [any thread but the realtime thread] Reads the window of `windowNs`
  // (<= 0: the whole ring) and the runs of renders that fill
  // `bufferFrames` into `load`, whose struct_size the caller checked; see
  // aud_graph_render_load.
  void read(int64_t windowNs, uint32_t bufferFrames, AudGraphLoad* load) const {
    const uint64_t overloads = overloads_.load(std::memory_order_acquire);
    const uint64_t renders = renders_.load(std::memory_order_acquire);
    readFrom(renders, overloads, windowNs, bufferFrames, load);
  }

  // Reads the window as a reader that found `renders` renders and
  // `overloads` overloads: walks back from the render `renders` - 1. read()
  // passes what it loads; the tests pass an older count to see a reader
  // stop at a slot the writer overwrote since.
  void readFrom(uint64_t renders, uint64_t overloads, int64_t windowNs,
                uint32_t bufferFrames, AudGraphLoad* load) const;

 private:
  static constexpr uint32_t kSlots = AUD_GRAPH_LOAD_RENDERS;
  static constexpr uint64_t kTagMask = 0xFFFF;
  static constexpr uint32_t kMaxFrames = 0xFFFF;
  static_assert(std::atomic<uint64_t>::is_always_lock_free &&
                    std::atomic<double>::is_always_lock_free,
                "the realtime thread takes no lock (process-005)");
  static_assert(kSlots % (kTagMask + 1) != 0,
                "a tag tells a render from the one a lap later");

  // The bits of the share, the frames and the tag of the render `index`.
  static uint64_t pack(float share, uint32_t frames, uint64_t index) {
    uint32_t bits = 0;
    std::memcpy(&bits, &share, sizeof(bits));
    const uint64_t clamped = frames < kMaxFrames ? frames : kMaxFrames;
    return (static_cast<uint64_t>(bits) << 32) | (clamped << 16) |
           (index & kTagMask);
  }

  static float shareOf(uint64_t slot) {
    const uint32_t bits = static_cast<uint32_t>(slot >> 32);
    float share = 0;
    std::memcpy(&share, &bits, sizeof(share));
    return share;
  }

  static uint32_t framesOf(uint64_t slot) {
    return static_cast<uint32_t>((slot >> 16) & kMaxFrames);
  }

  static bool holds(uint64_t slot, uint64_t index) {
    return (slot & kTagMask) == (index & kTagMask);
  }

  std::atomic<uint64_t> slots_[kSlots]{};
  // The live renders since the graph was created: the next render's index.
  std::atomic<uint64_t> renders_{0};
  // Of them, the ones that took longer than their block.
  std::atomic<uint64_t> overloads_{0};
  // The sample rate of the newest render: the window's frames to time.
  std::atomic<double> sampleRate_{0};
};

}  // namespace aud

#endif  // AUD_GRAPH_LOAD_HPP
