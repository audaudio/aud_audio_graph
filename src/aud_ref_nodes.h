// @license
// Copyright (c) Audanika. All Rights Reserved.
//
// Use of this source code is governed by terms that can be
// found in the LICENSE file in the root of this package.

// The reference nodes of the engine: a sine oscillator and a gain, used by
// the tests, the example app and the benchmarks until the DSP packages
// exist (S2, S10a).

#ifndef AUD_REF_NODES_H
#define AUD_REF_NODES_H

#include "aud_abi.h"

#ifdef __cplusplus
extern "C" {
#endif

// The type ids of the reference nodes.
#define AUD_REF_SINE_TYPE_ID "aud.ref.sine"
#define AUD_REF_GAIN_TYPE_ID "aud.ref.gain"

// Registers the reference node types with a host.
int32_t aud_ref_nodes_register(const AudHostApi* host);

#ifdef __cplusplus
}
#endif

#endif  // AUD_REF_NODES_H
