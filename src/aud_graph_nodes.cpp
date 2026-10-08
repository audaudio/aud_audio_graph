// @license
// Copyright (c) Audanika. All Rights Reserved.
//
// Use of this source code is governed by terms that can be
// found in the LICENSE file in the root of this package.

// The reference nodes of the graph (plan of ticket 17, S2): a test
// oscillator that plays notes, a mixer with a gain per input and a
// state-variable filter. They are ordinary ABI nodes on AudNodeBase of
// aud_audio_core, registered through the host api like the nodes of any
// DSP package, so they exercise the contract the packages build on.

#include <algorithm>
#include <cmath>
#include <cstdint>

#include "aud_graph_internal.hpp"
#include "aud_node_base.hpp"
#include "aud_ump.h"

namespace aud {
namespace {

constexpr double kTwoPi = 6.283185307179586;
constexpr uint32_t kMaxChannelsOfNode = 16;
// The damping of the filter is k = 2 - kResonanceRange * resonance: at
// full resonance k stays at 0.05, just short of self-oscillation.
constexpr float kResonanceRange = 1.95f;

// Clamps a channel count to what the reference nodes render.
uint32_t channelsOf(const AudAudioBus& bus) {
  return std::min(bus.num_channels, kMaxChannelsOfNode);
}

// ............................................................................
// aud.graph.oscillator

class Oscillator : public AudNodeBase {
 public:
  Oscillator(const AudNodeDescriptor*, const AudHostApi*) : AudNodeBase(3) {
    setParam(AUD_OSCILLATOR_PARAM_FREQUENCY, 440.0f);
    setParam(AUD_OSCILLATOR_PARAM_AMPLITUDE, 0.5f);
    setParam(AUD_OSCILLATOR_PARAM_WAVEFORM, 0.0f);
  }

  void reset(uint32_t reason) override {
    (void)reason;
    phase_ = 0;
    gate_.set(gated_ ? 0.0f : 1.0f);
    note_ = -1;
  }

 protected:
  void renderRange(const AudProcessContext& context, uint32_t offset,
                   uint32_t frames) override {
    if (context.num_output_buses < 1) return;
    const AudAudioBus& out = context.outputs[0];
    const uint32_t channels = channelsOf(out);
    const double increment = kTwoPi * param(AUD_OSCILLATOR_PARAM_FREQUENCY) /
                             (sampleRate() > 0 ? sampleRate() : 48000.0);
    const int waveform =
        static_cast<int>(param(AUD_OSCILLATOR_PARAM_WAVEFORM) + 0.5f);
    const AudParamRamp& amplitude = ramp(AUD_OSCILLATOR_PARAM_AMPLITUDE);
    for (uint32_t i = 0; i < frames; ++i) {
      const float a = valueAt(amplitude, i) * valueAt(gate_, i);
      const float sample = a * shape(waveform, phase_);
      for (uint32_t c = 0; c < channels; ++c) {
        out.channels[c][offset + i] = sample;
      }
      phase_ += increment;
      if (phase_ >= kTwoPi) phase_ -= kTwoPi;
    }
    gate_.advance(frames);
  }

  void handleEvent(const AudEvent& event) override {
    if (event.type != AUD_EVENT_UMP) return;
    const uint32_t word0 = event.words[0];
    const uint32_t word1 = event.words[1];
    if (aud_ump_is_note_on(word0, word1)) {
      note_ = static_cast<int32_t>(aud_ump_note(word0));
      gated_ = true;
      setParam(AUD_OSCILLATOR_PARAM_FREQUENCY,
               440.0f * std::pow(2.0f, (static_cast<float>(note_) - 69.0f) /
                                           12.0f));
      gate_.setTarget(aud_ump_note_velocity(word0, word1), kGateFrames);
    } else if (aud_ump_is_note_off(word0, word1) &&
               static_cast<int32_t>(aud_ump_note(word0)) == note_) {
      gate_.setTarget(0.0f, kGateFrames);
      note_ = -1;
    }
  }

 private:
  static constexpr uint32_t kGateFrames = 64;

  // The value of a ramp `i` frames into the range.
  static float valueAt(const AudParamRamp& ramp, uint32_t i) {
    return i < ramp.remaining()
               ? ramp.value() + ramp.step() * static_cast<float>(i)
               : ramp.target();
  }

  static float shape(int waveform, double phase) {
    const double t = phase / kTwoPi;  // 0 .. 1
    switch (waveform) {
      case 1:
        return static_cast<float>(2.0 * t - 1.0);
      case 2:
        return t < 0.5 ? 1.0f : -1.0f;
      case 3:
        return static_cast<float>(t < 0.5 ? 4.0 * t - 1.0 : 3.0 - 4.0 * t);
      default:
        return static_cast<float>(std::sin(phase));
    }
  }

  double phase_ = 0;
  AudParamRamp gate_{1.0f};
  bool gated_ = false;
  int32_t note_ = -1;
};

const AudBusDescriptor kOscillatorOutputs[] = {
    {sizeof(AudBusDescriptor), "out", "Output", AUD_BUS_MAIN, 1,
     kMaxChannelsOfNode, 1},
};

const AudEventPortDescriptor kOscillatorEventInputs[] = {
    {sizeof(AudEventPortDescriptor), "midi", "MIDI",
     AUD_EVENT_PORT_MIDI | AUD_EVENT_PORT_CONTROL, 0},
};

const AudParamDescriptor kOscillatorParams[] = {
    {sizeof(AudParamDescriptor), "frequency", "Frequency", "Hz", 20.0f,
     20000.0f, 440.0f, AUD_PARAM_AUTOMATABLE | AUD_PARAM_RAMPED |
                           AUD_PARAM_LOGARITHMIC, 0, 0},
    {sizeof(AudParamDescriptor), "amplitude", "Amplitude", "", 0.0f, 1.0f,
     0.5f, AUD_PARAM_AUTOMATABLE | AUD_PARAM_RAMPED, 0, 0},
    {sizeof(AudParamDescriptor), "waveform", "Waveform", "", 0.0f, 3.0f, 0.0f,
     AUD_PARAM_STEPPED, 4, 0},
};

const AudNodeVTable kOscillatorVTable = AudNodeVTableFor<Oscillator>::vtable();

const AudNodeDescriptor kOscillatorDescriptor = {
    sizeof(AudNodeDescriptor),
    AUD_ABI_VERSION_MAJOR,
    AUD_ABI_VERSION_MINOR,
    1,
    AUD_GRAPH_OSCILLATOR_TYPE_ID,
    "Oscillator",
    "Audanika",
    AUD_NODE_CAP_VARIABLE_BLOCK | AUD_NODE_CAP_EVENTS | AUD_NODE_CAP_LATENCY |
        AUD_NODE_CAP_TAIL | AUD_NODE_CAP_RESET_ON_STOP,
    0,
    0,
    1,
    nullptr,
    kOscillatorOutputs,
    1,
    0,
    kOscillatorEventInputs,
    nullptr,
    3,
    0,
    kOscillatorParams,
    nullptr,
    &kOscillatorVTable,
};

// ............................................................................
// aud.graph.mixer

class Mixer : public AudNodeBase {
 public:
  Mixer(const AudNodeDescriptor*, const AudHostApi*)
      : AudNodeBase(AUD_MIXER_NUM_INPUTS + 1) {
    for (uint32_t i = 0; i <= AUD_MIXER_NUM_INPUTS; ++i) setParam(i, 1.0f);
  }

 protected:
  void renderRange(const AudProcessContext& context, uint32_t offset,
                   uint32_t frames) override {
    if (context.num_output_buses < 1) return;
    const AudAudioBus& out = context.outputs[0];
    const uint32_t channels = channelsOf(out);
    for (uint32_t c = 0; c < channels; ++c) {
      float* target = out.channels[c] + offset;
      for (uint32_t i = 0; i < frames; ++i) target[i] = 0;
    }
    const uint32_t inputs =
        std::min(context.num_input_buses, uint32_t{AUD_MIXER_NUM_INPUTS});
    for (uint32_t b = 0; b < inputs; ++b) {
      const AudAudioBus& in = context.inputs[b];
      const AudParamRamp& gain = ramp(b);
      const uint32_t shared = std::min(channels, channelsOf(in));
      for (uint32_t c = 0; c < shared; ++c) {
        const float* source = in.channels[c] + offset;
        float* target = out.channels[c] + offset;
        for (uint32_t i = 0; i < frames; ++i) {
          target[i] += source[i] * valueAt(gain, i);
        }
      }
    }
    const AudParamRamp& master = ramp(AUD_MIXER_PARAM_MASTER);
    for (uint32_t c = 0; c < channels; ++c) {
      float* target = out.channels[c] + offset;
      for (uint32_t i = 0; i < frames; ++i) target[i] *= valueAt(master, i);
    }
  }

 private:
  static float valueAt(const AudParamRamp& ramp, uint32_t i) {
    return i < ramp.remaining()
               ? ramp.value() + ramp.step() * static_cast<float>(i)
               : ramp.target();
  }
};

#define AUD_MIXER_INPUT(id, name)                                        \
  {sizeof(AudBusDescriptor), id, name, AUD_BUS_MAIN | AUD_BUS_OPTIONAL, 1, \
   kMaxChannelsOfNode, 2}

const AudBusDescriptor kMixerInputs[] = {
    AUD_MIXER_INPUT("in0", "Input 1"), AUD_MIXER_INPUT("in1", "Input 2"),
    AUD_MIXER_INPUT("in2", "Input 3"), AUD_MIXER_INPUT("in3", "Input 4"),
    AUD_MIXER_INPUT("in4", "Input 5"), AUD_MIXER_INPUT("in5", "Input 6"),
    AUD_MIXER_INPUT("in6", "Input 7"), AUD_MIXER_INPUT("in7", "Input 8"),
};

const AudBusDescriptor kMixerOutputs[] = {
    {sizeof(AudBusDescriptor), "out", "Output", AUD_BUS_MAIN, 1,
     kMaxChannelsOfNode, 2},
};

const AudEventPortDescriptor kMixerEventInputs[] = {
    {sizeof(AudEventPortDescriptor), "events", "Events",
     AUD_EVENT_PORT_CONTROL, 0},
};

#define AUD_MIXER_GAIN(id, name)                                            \
  {sizeof(AudParamDescriptor), id, name, "", 0.0f, 4.0f, 1.0f,              \
   AUD_PARAM_AUTOMATABLE | AUD_PARAM_RAMPED, 0, 0}

const AudParamDescriptor kMixerParams[] = {
    AUD_MIXER_GAIN("gain0", "Gain 1"),   AUD_MIXER_GAIN("gain1", "Gain 2"),
    AUD_MIXER_GAIN("gain2", "Gain 3"),   AUD_MIXER_GAIN("gain3", "Gain 4"),
    AUD_MIXER_GAIN("gain4", "Gain 5"),   AUD_MIXER_GAIN("gain5", "Gain 6"),
    AUD_MIXER_GAIN("gain6", "Gain 7"),   AUD_MIXER_GAIN("gain7", "Gain 8"),
    AUD_MIXER_GAIN("master", "Master"),
};

const AudNodeVTable kMixerVTable = AudNodeVTableFor<Mixer>::vtable();

const AudNodeDescriptor kMixerDescriptor = {
    sizeof(AudNodeDescriptor),
    AUD_ABI_VERSION_MAJOR,
    AUD_ABI_VERSION_MINOR,
    1,
    AUD_GRAPH_MIXER_TYPE_ID,
    "Mixer",
    "Audanika",
    AUD_NODE_CAP_VARIABLE_BLOCK | AUD_NODE_CAP_EVENTS | AUD_NODE_CAP_LATENCY |
        AUD_NODE_CAP_TAIL,
    0,
    AUD_MIXER_NUM_INPUTS,
    1,
    kMixerInputs,
    kMixerOutputs,
    1,
    0,
    kMixerEventInputs,
    nullptr,
    AUD_MIXER_NUM_INPUTS + 1,
    0,
    kMixerParams,
    nullptr,
    &kMixerVTable,
};

// ............................................................................
// aud.graph.filter: the linear trapezoidal state-variable filter after
// Andrew Simper (Cytomic), stable at every cutoff.

class Filter : public AudNodeBase {
 public:
  Filter(const AudNodeDescriptor*, const AudHostApi*) : AudNodeBase(3) {
    setParam(AUD_FILTER_PARAM_CUTOFF, 1000.0f);
    setParam(AUD_FILTER_PARAM_RESONANCE, 0.2f);
    setParam(AUD_FILTER_PARAM_MODE, 0.0f);
  }

  void reset(uint32_t reason) override {
    (void)reason;
    for (uint32_t c = 0; c < kMaxChannelsOfNode; ++c) ic1_[c] = ic2_[c] = 0;
  }

 protected:
  void renderRange(const AudProcessContext& context, uint32_t offset,
                   uint32_t frames) override {
    if (context.num_input_buses < 1 || context.num_output_buses < 1) return;
    const AudAudioBus& in = context.inputs[0];
    const AudAudioBus& out = context.outputs[0];
    const uint32_t channels = std::min(channelsOf(in), channelsOf(out));
    const double rate = sampleRate() > 0 ? sampleRate() : 48000.0;
    const double cutoff = std::min<double>(param(AUD_FILTER_PARAM_CUTOFF),
                                           rate * 0.49);
    const float g = static_cast<float>(std::tan(3.141592653589793 * cutoff /
                                                rate));
    const float k = 2.0f - kResonanceRange * std::min(1.0f, std::max(0.0f, param(
                                         AUD_FILTER_PARAM_RESONANCE)));
    const float a1 = 1.0f / (1.0f + g * (g + k));
    const float a2 = g * a1;
    const float a3 = g * a2;
    const int mode = static_cast<int>(param(AUD_FILTER_PARAM_MODE) + 0.5f);
    for (uint32_t c = 0; c < channels; ++c) {
      const float* source = in.channels[c] + offset;
      float* target = out.channels[c] + offset;
      float ic1 = ic1_[c];
      float ic2 = ic2_[c];
      for (uint32_t i = 0; i < frames; ++i) {
        const float v0 = source[i];
        const float v3 = v0 - ic2;
        const float v1 = a1 * ic1 + a2 * v3;
        const float v2 = ic2 + a2 * ic1 + a3 * v3;
        ic1 = 2.0f * v1 - ic1;
        ic2 = 2.0f * v2 - ic2;
        switch (mode) {
          case 1:
            target[i] = v0 - k * v1 - v2;
            break;
          case 2:
            target[i] = v1;
            break;
          case 3:
            target[i] = v0 - k * v1;
            break;
          default:
            target[i] = v2;
        }
      }
      ic1_[c] = ic1;
      ic2_[c] = ic2;
    }
  }

 private:
  float ic1_[kMaxChannelsOfNode] = {};
  float ic2_[kMaxChannelsOfNode] = {};
};

const AudBusDescriptor kFilterInputs[] = {
    {sizeof(AudBusDescriptor), "in", "Input", AUD_BUS_MAIN, 1,
     kMaxChannelsOfNode, 2},
};

const AudBusDescriptor kFilterOutputs[] = {
    {sizeof(AudBusDescriptor), "out", "Output", AUD_BUS_MAIN, 1,
     kMaxChannelsOfNode, 2},
};

const AudEventPortDescriptor kFilterEventInputs[] = {
    {sizeof(AudEventPortDescriptor), "events", "Events",
     AUD_EVENT_PORT_CONTROL, 0},
};

const AudParamDescriptor kFilterParams[] = {
    {sizeof(AudParamDescriptor), "cutoff", "Cutoff", "Hz", 20.0f, 20000.0f,
     1000.0f, AUD_PARAM_AUTOMATABLE | AUD_PARAM_RAMPED | AUD_PARAM_LOGARITHMIC,
     0, 0},
    {sizeof(AudParamDescriptor), "resonance", "Resonance", "", 0.0f, 1.0f,
     0.2f, AUD_PARAM_AUTOMATABLE | AUD_PARAM_RAMPED, 0, 0},
    {sizeof(AudParamDescriptor), "mode", "Mode", "", 0.0f, 3.0f, 0.0f,
     AUD_PARAM_STEPPED, 4, 0},
};

const AudNodeVTable kFilterVTable = AudNodeVTableFor<Filter>::vtable();

const AudNodeDescriptor kFilterDescriptor = {
    sizeof(AudNodeDescriptor),
    AUD_ABI_VERSION_MAJOR,
    AUD_ABI_VERSION_MINOR,
    1,
    AUD_GRAPH_FILTER_TYPE_ID,
    "Filter",
    "Audanika",
    AUD_NODE_CAP_IN_PLACE | AUD_NODE_CAP_VARIABLE_BLOCK | AUD_NODE_CAP_EVENTS |
        AUD_NODE_CAP_LATENCY | AUD_NODE_CAP_TAIL,
    0,
    1,
    1,
    kFilterInputs,
    kFilterOutputs,
    1,
    0,
    kFilterEventInputs,
    nullptr,
    3,
    0,
    kFilterParams,
    nullptr,
    &kFilterVTable,
};

}  // namespace

int32_t registerReferenceNodes(const AudHostApi* host) {
  const AudNodeDescriptor* descriptors[] = {
      &kOscillatorDescriptor, &kMixerDescriptor, &kFilterDescriptor};
  for (const AudNodeDescriptor* descriptor : descriptors) {
    const int32_t result = host->register_node_type(host->host, descriptor);
    if (result != AUD_OK) return result;
  }
  return AUD_OK;
}

}  // namespace aud
