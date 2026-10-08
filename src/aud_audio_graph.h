// @license
// Copyright (c) Audanika. All Rights Reserved.
//
// Use of this source code is governed by terms that can be
// found in the LICENSE file in the root of this package.

// The C API of the spike engine of aud_audio_graph (ticket 5, S0-mobile).
//
// The engine hosts node types registered through the C ABI of
// aud_audio_core, renders a serial chain of node instances block by block on
// the realtime thread, and takes parameter changes and events from a
// lock-free command queue with defined overflow behaviour (interop-002).
// Opaque pointers cross this API as void*: the ABI structs live only in
// aud_audio_core. Ticket S2 replaces the chain by compiled render programs.

#ifndef AUD_AUDIO_GRAPH_H
#define AUD_AUDIO_GRAPH_H

#include <stdint.h>

#include "aud_abi.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct AudEngine AudEngine;

// How an engine is created.
typedef struct AudEngineConfig {
  uint32_t struct_size;
  double sample_rate;
  uint32_t max_frames;  // the largest block aud_engine_render receives
  uint32_t channels;    // channels of every bus and of the output
  uint32_t command_queue_capacity;  // 0 = 1024
  uint32_t max_nodes;               // 0 = 64
} AudEngineConfig;

// Counters the realtime thread keeps; read with aud_engine_get_stats.
typedef struct AudEngineStats {
  uint32_t struct_size;
  uint32_t program_revision;  // the chain revision the realtime thread uses
  uint64_t blocks_rendered;
  uint64_t frames_rendered;
  uint64_t commands_applied;
  uint64_t commands_rejected;  // enqueue refused because the queue was full
  int64_t command_latency_min_ns;  // enqueue to apply at the block start
  int64_t command_latency_max_ns;
  int64_t command_latency_sum_ns;
  uint64_t command_latency_count;
  int64_t render_time_max_ns;  // time spent inside aud_engine_render
  int64_t render_time_sum_ns;
  float output_peak;  // the largest absolute output sample since the reset
} AudEngineStats;

// [control] Creates an engine with the reference node types registered.
AUD_EXPORT AudEngine* aud_engine_create(const AudEngineConfig* config);

// [control] Destroys the engine and every node instance. Stop rendering
// first.
AUD_EXPORT void aud_engine_destroy(AudEngine* engine);

// [control] The AudHostApi* packages register their node types with.
AUD_EXPORT const void* aud_engine_host_api(AudEngine* engine);

// [control] The registered node types.
AUD_EXPORT int32_t aud_engine_num_node_types(AudEngine* engine);
AUD_EXPORT const char* aud_engine_node_type_id(AudEngine* engine, int32_t index);
AUD_EXPORT const char* aud_engine_node_type_name(AudEngine* engine, int32_t index);
AUD_EXPORT uint32_t aud_engine_node_type_capabilities(AudEngine* engine, int32_t index);
AUD_EXPORT int32_t aud_engine_node_type_num_params(AudEngine* engine, int32_t index);
// Fills the parameter's id, unit and range; AUD_OK or an error code.
AUD_EXPORT int32_t aud_engine_node_type_param(AudEngine* engine, int32_t index,
                                              uint32_t param, const char** id,
                                              const char** unit, float* min_value,
                                              float* max_value, float* default_value);

// [control] Creates a node instance of a registered type; returns the node
// id (>= 0) or an error code (< 0).
AUD_EXPORT int32_t aud_engine_create_node(AudEngine* engine, const char* type_id);

// [control] Destroys a node instance that is not part of the published
// chain; AUD_ERROR_STATE while it is, or while the realtime thread has not
// adopted the latest chain yet (it may still render the node).
AUD_EXPORT int32_t aud_engine_destroy_node(AudEngine* engine, int32_t node);

// [control] Publishes a serial chain of node instances as the render
// program; the realtime thread adopts it at the next block start. Returns
// the revision (> 0) or an error code.
AUD_EXPORT int32_t aud_engine_set_chain(AudEngine* engine, const int32_t* nodes,
                                        uint32_t count);

// [control, one producer] Enqueues a parameter change; AUD_ERROR_QUEUE_FULL
// when the queue is full - nothing is dropped silently.
AUD_EXPORT int32_t aud_engine_set_param(AudEngine* engine, int32_t node,
                                        uint32_t param, float value);

// [control, one producer] Enqueues a note on (on != 0) or note off.
AUD_EXPORT int32_t aud_engine_send_note(AudEngine* engine, int32_t node, int32_t on,
                                        uint32_t channel, uint32_t number,
                                        float velocity);

// [control] Applies a string setting to a node on the calling thread; may
// block (loading). AUD_ERROR_STATE if the node type takes no strings.
AUD_EXPORT int32_t aud_engine_set_string(AudEngine* engine, int32_t node,
                                         uint32_t key, const char* value);

// [realtime] Renders `frames` frames of interleaved output. Applies pending
// commands first, adopts a new chain, runs the nodes, interleaves.
AUD_EXPORT void aud_engine_render(AudEngine* engine, float* interleaved_output,
                                  uint32_t frames);

// [realtime] The render callback of aud_audio_io: `user` is the engine. The
// signature matches AudRenderCallback of aud_abi.h.
AUD_EXPORT void aud_engine_io_render(void* user, float* interleaved_output,
                                     uint32_t frames, uint32_t channels);

// [control] Copies the counters.
AUD_EXPORT void aud_engine_get_stats(AudEngine* engine, AudEngineStats* stats);

// [control] Zeroes the counters; the realtime thread keeps counting.
AUD_EXPORT void aud_engine_reset_stats(AudEngine* engine);

#ifdef __cplusplus
}
#endif

#endif  // AUD_AUDIO_GRAPH_H
