// @license
// Copyright (c) Audanika. All Rights Reserved.
//
// Use of this source code is governed by terms that can be
// found in the LICENSE file in the root of this package.

// The load gauge of the engine (ticket 29): the reading side. The realtime
// thread records every live render inline (aud_graph_load.hpp); a reader
// walks back from the newest render, copies the shares of its window onto
// its stack, forms the runs of renders that fill the device buffer and
// selects the 99th percentiles of both by nearest rank.

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

#include "aud_graph_internal.hpp"
#include "aud_graph_load.hpp"

namespace aud {

namespace {

// The nearest rank of `count` values: the smallest one that at least 99 %
// of them do not exceed, the ceil(0.99 * count)-th in ascending order. The
// values are reordered.
float p99Of(float* values, uint32_t count) {
  if (count == 0) return 0;
  const uint32_t rank = (99 * count + 99) / 100;
  std::nth_element(values, values + (rank - 1), values + count);
  return values[rank - 1];
}

}  // namespace

void LoadGauge::readFrom(uint64_t renders, uint64_t overloads,
                         int64_t windowNs, uint32_t bufferFrames,
                         AudGraphLoad* load) const {
  const double sampleRate = sampleRate_.load(std::memory_order_relaxed);
  // The frames that cover the window; without a window or a rate the ring.
  const double wanted =
      windowNs > 0 && sampleRate > 0
          ? static_cast<double>(windowNs) * sampleRate / 1e9
          : std::numeric_limits<double>::infinity();
  // The window, newest first.
  std::array<float, kSlots> shares;
  std::array<uint32_t, kSlots> framesOfRender;
  uint32_t count = 0;
  uint64_t frames = 0;
  double weighted = 0;
  float max = 0;
  for (uint64_t index = renders;
       index > 0 && count < kSlots && static_cast<double>(frames) < wanted;) {
    index -= 1;
    const uint64_t slot =
        slots_[index % kSlots].load(std::memory_order_relaxed);
    // The writer has lapped the reader: this render and the older ones are
    // gone.
    if (!holds(slot, index)) break;
    const float share = shareOf(slot);
    const uint32_t slotFrames = framesOf(slot);
    shares[count] = share;
    framesOfRender[count] = slotFrames;
    count += 1;
    frames += slotFrames;
    weighted += static_cast<double>(share) * slotFrames;
    max = std::max(max, share);
  }
  // The runs that fill the device buffer: each render and the older ones
  // before it until their frames reach it, within the window.
  std::array<float, kSlots> runs;
  uint32_t numRuns = 0;
  uint32_t bufferOverloads = 0;
  float bufferMax = 0;
  for (uint32_t first = 0; first < count; ++first) {
    uint64_t runFrames = 0;
    double runWeighted = 0;
    uint32_t last = first;
    for (; last < count; ++last) {
      runFrames += framesOfRender[last];
      runWeighted += static_cast<double>(shares[last]) * framesOfRender[last];
      if (runFrames >= bufferFrames) break;
    }
    if (last == count || runFrames == 0) break;
    const float run =
        static_cast<float>(runWeighted / static_cast<double>(runFrames));
    runs[numRuns++] = run;
    bufferMax = std::max(bufferMax, run);
    if (run > 1.0f) bufferOverloads += 1;
  }
  load->renders = count;
  load->frames = frames;
  load->window_ns =
      sampleRate > 0 ? static_cast<int64_t>(std::llround(
                           static_cast<double>(frames) * 1e9 / sampleRate))
                     : 0;
  load->mean = frames > 0 ? weighted / static_cast<double>(frames) : 0;
  load->max = max;
  load->p99 = p99Of(shares.data(), count);
  load->renders_total = renders;
  load->overloads_total = overloads;
  load->buffer_frames = bufferFrames;
  load->buffer_overloads = bufferOverloads;
  load->buffer_p99 = p99Of(runs.data(), numRuns);
  load->buffer_max = bufferMax;
}

}  // namespace aud

// ############################################################################
// The render load of the engine

AUD_EXPORT int32_t aud_graph_render_load(const AudGraph* graph,
                                         int64_t window_ns,
                                         uint32_t buffer_frames,
                                         AudGraphLoad* load) {
  if (graph == nullptr || load == nullptr ||
      load->struct_size < sizeof(AudGraphLoad)) {
    return AUD_ERROR_INVALID_ARGUMENT;
  }
  graph->loadGauge.read(window_ns, buffer_frames, load);
  return AUD_OK;
}
