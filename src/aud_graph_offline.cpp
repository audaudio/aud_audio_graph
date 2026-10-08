// @license
// Copyright (c) Audanika. All Rights Reserved.
//
// Use of this source code is governed by terms that can be
// found in the LICENSE file in the root of this package.

// The offline renderer (plan of ticket 17, S2): drives aud_graph_render on
// the calling thread over a virtual timeline, so that tests and exports
// render deterministically, faster than real time, with any sequence of
// block sizes.

#include <cmath>
#include <vector>

#include "aud_graph_internal.hpp"

using namespace aud;

AUD_EXPORT int32_t aud_graph_render_offline(AudGraph* graph,
                                            const AudOfflineRequest* request) {
  if (graph == nullptr || request == nullptr ||
      request->struct_size < sizeof(AudOfflineRequest) ||
      (graph->outputChannels.size() > 0 && request->outputs == nullptr) ||
      (request->num_block_sizes > 0 && request->block_sizes == nullptr)) {
    return AUD_ERROR_INVALID_ARGUMENT;
  }
  if (graph->state.load(std::memory_order_acquire) != AUD_GRAPH_RUNNING) {
    return AUD_ERROR_STATE;
  }
  const uint32_t fixed =
      request->block_frames == 0 ? graph->maxFrames : request->block_frames;
  if (fixed > graph->maxFrames) return AUD_ERROR_INVALID_ARGUMENT;
  for (uint32_t i = 0; i < request->num_block_sizes; ++i) {
    if (request->block_sizes[i] == 0 ||
        request->block_sizes[i] > graph->maxFrames) {
      return AUD_ERROR_INVALID_ARGUMENT;
    }
  }
  const size_t numInputs = graph->inputChannels.size();
  const size_t numOutputs = graph->outputChannels.size();
  std::vector<std::vector<float*>> inputPointers(numInputs);
  std::vector<std::vector<float*>> outputPointers(numOutputs);
  std::vector<AudAudioBus> inputs(numInputs);
  std::vector<AudAudioBus> outputs(numOutputs);
  for (size_t b = 0; b < numInputs; ++b) {
    inputPointers[b].assign(graph->inputChannels[b], nullptr);
    inputs[b] = {sizeof(AudAudioBus), graph->inputChannels[b],
                 inputPointers[b].data()};
  }
  for (size_t b = 0; b < numOutputs; ++b) {
    outputPointers[b].assign(graph->outputChannels[b], nullptr);
    outputs[b] = {sizeof(AudAudioBus), graph->outputChannels[b],
                  outputPointers[b].data()};
  }
  const double nsPerSample = 1e9 / graph->sampleRate;
  int64_t position = request->start_sample_position;
  uint32_t rendered = 0;
  uint32_t cycle = 0;
  graph->offline = true;
  while (rendered < request->frames) {
    uint32_t block = request->num_block_sizes > 0
                         ? request->block_sizes[cycle++ % request->num_block_sizes]
                         : fixed;
    block = std::min(block, request->frames - rendered);
    for (size_t b = 0; b < numInputs; ++b) {
      for (uint32_t c = 0; c < graph->inputChannels[b]; ++c) {
        inputPointers[b][c] =
            request->inputs == nullptr || request->inputs[b].channels == nullptr
                ? graph->silence.data()
                : request->inputs[b].channels[c] + rendered;
      }
    }
    for (size_t b = 0; b < numOutputs; ++b) {
      for (uint32_t c = 0; c < graph->outputChannels[b]; ++c) {
        outputPointers[b][c] = request->outputs[b].channels[c] + rendered;
      }
    }
    AudStreamTime time{};
    time.struct_size = sizeof(AudStreamTime);
    time.frames = block;
    time.sample_rate = graph->sampleRate;
    time.sample_position = position;
    time.host_time_ns =
        request->start_host_time_ns +
        static_cast<int64_t>(std::llround(rendered * nsPerSample));
    time.host_time_source = AUD_TIME_SOURCE_SYNTHESIZED;
    AudRenderRequest render{};
    render.struct_size = sizeof(AudRenderRequest);
    render.frames = block;
    render.num_input_buses = static_cast<uint32_t>(numInputs);
    render.num_output_buses = static_cast<uint32_t>(numOutputs);
    render.inputs = inputs.data();
    render.outputs = outputs.data();
    render.time = &time;
    const int32_t result = aud_graph_render(graph, &render);
    if (result != AUD_OK) {
      graph->offline = false;
      return result;
    }
    position += block;
    rendered += block;
  }
  graph->offline = false;
  return AUD_OK;
}
