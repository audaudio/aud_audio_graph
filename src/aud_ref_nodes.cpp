// @license
// Copyright (c) Audanika. All Rights Reserved.
//
// Use of this source code is governed by terms that can be
// found in the LICENSE file in the root of this package.

#include "aud_ref_nodes.h"

#include <cmath>
#include <cstdint>
#include <new>

namespace {

constexpr double kTwoPi = 6.283185307179586;

// A per-block linear ramp towards a target: no clicks on parameter changes.
struct Ramp {
  float current = 0.0f;
  float target = 0.0f;

  void set(float value) { target = value; }

  // Returns the increment per frame that reaches the target at the block end.
  float stepFor(uint32_t frames) const {
    return frames == 0 ? 0.0f : (target - current) / static_cast<float>(frames);
  }
};

// ###########################################################################
// aud.ref.sine

struct SineNode {
  double sampleRate = 48000.0;
  double phase = 0.0;
  float frequency = 440.0f;
  Ramp gain;
};

const AudParamDescriptor kSineParams[] = {
    {sizeof(AudParamDescriptor), "frequency", "Frequency", "Hz", 20.0f, 20000.0f,
     440.0f},
    {sizeof(AudParamDescriptor), "gain", "Gain", "", 0.0f, 1.0f, 0.2f},
};

void* sineCreate(const AudNodeDescriptor*, const AudHostApi*) {
  auto* node = new (std::nothrow) SineNode();
  if (node != nullptr) node->gain.current = node->gain.target = 0.2f;
  return node;
}

void sineDestroy(void* instance) { delete static_cast<SineNode*>(instance); }

int32_t sinePrepare(void* instance, double sampleRate, uint32_t, uint32_t) {
  auto* node = static_cast<SineNode*>(instance);
  node->sampleRate = sampleRate;
  node->phase = 0.0;
  return AUD_OK;
}

void sineReset(void* instance) { static_cast<SineNode*>(instance)->phase = 0.0; }

void sineSetParam(void* instance, uint32_t index, float value) {
  auto* node = static_cast<SineNode*>(instance);
  if (index == 0) node->frequency = value;
  if (index == 1) node->gain.set(value);
}

void sineProcess(void* instance, const AudProcessContext* context) {
  auto* node = static_cast<SineNode*>(instance);
  const double increment = kTwoPi * node->frequency / node->sampleRate;
  const float gainStep = node->gain.stepFor(context->frames);
  float gain = node->gain.current;
  double phase = node->phase;
  for (uint32_t frame = 0; frame < context->frames; ++frame) {
    const float sample = static_cast<float>(std::sin(phase)) * gain;
    for (uint32_t channel = 0; channel < context->channels; ++channel) {
      context->outputs[channel][frame] = sample;
    }
    phase += increment;
    if (phase >= kTwoPi) phase -= kTwoPi;
    gain += gainStep;
  }
  node->phase = phase;
  node->gain.current = node->gain.target;
}

const AudNodeVTable kSineVTable = {
    sizeof(AudNodeVTable), sineCreate, sineDestroy, sinePrepare,
    sineReset,             sineSetParam, nullptr,  sineProcess,
    nullptr,
};

const AudNodeDescriptor kSineDescriptor = {
    sizeof(AudNodeDescriptor),
    AUD_ABI_VERSION_MAJOR,
    AUD_ABI_VERSION_MINOR,
    AUD_REF_SINE_TYPE_ID,
    "Reference sine oscillator",
    AUD_NODE_CAP_VARIABLE_BLOCK,
    0,
    1,
    2,
    kSineParams,
    &kSineVTable,
};

// ###########################################################################
// aud.ref.gain

struct GainNode {
  Ramp gain;
};

const AudParamDescriptor kGainParams[] = {
    {sizeof(AudParamDescriptor), "gain", "Gain", "", 0.0f, 4.0f, 1.0f},
};

void* gainCreate(const AudNodeDescriptor*, const AudHostApi*) {
  auto* node = new (std::nothrow) GainNode();
  if (node != nullptr) node->gain.current = node->gain.target = 1.0f;
  return node;
}

void gainDestroy(void* instance) { delete static_cast<GainNode*>(instance); }

int32_t gainPrepare(void*, double, uint32_t, uint32_t) { return AUD_OK; }

void gainReset(void*) {}

void gainSetParam(void* instance, uint32_t index, float value) {
  if (index == 0) static_cast<GainNode*>(instance)->gain.set(value);
}

void gainProcess(void* instance, const AudProcessContext* context) {
  auto* node = static_cast<GainNode*>(instance);
  const float gainStep = node->gain.stepFor(context->frames);
  for (uint32_t channel = 0; channel < context->channels; ++channel) {
    float gain = node->gain.current;
    const float* input = context->inputs[channel];
    float* output = context->outputs[channel];
    for (uint32_t frame = 0; frame < context->frames; ++frame) {
      output[frame] = input[frame] * gain;
      gain += gainStep;
    }
  }
  node->gain.current = node->gain.target;
}

const AudNodeVTable kGainVTable = {
    sizeof(AudNodeVTable), gainCreate, gainDestroy, gainPrepare,
    gainReset,             gainSetParam, nullptr,  gainProcess,
    nullptr,
};

const AudNodeDescriptor kGainDescriptor = {
    sizeof(AudNodeDescriptor),
    AUD_ABI_VERSION_MAJOR,
    AUD_ABI_VERSION_MINOR,
    AUD_REF_GAIN_TYPE_ID,
    "Reference gain",
    AUD_NODE_CAP_IN_PLACE | AUD_NODE_CAP_VARIABLE_BLOCK,
    1,
    1,
    1,
    kGainParams,
    &kGainVTable,
};

}  // namespace

int32_t aud_ref_nodes_register(const AudHostApi* host) {
  const int32_t sine = host->register_node_type(host->host, &kSineDescriptor);
  if (sine != AUD_OK) return sine;
  return host->register_node_type(host->host, &kGainDescriptor);
}
