// @license
// Copyright (c) Audanika. All Rights Reserved.
//
// Use of this source code is governed by terms that can be
// found in the LICENSE file in the root of this package.

// The C API of the audio graph engine of aud_audio_graph (ticket 19, S2).
//
// A graph hosts node types registered through the C ABI of aud_audio_core
// and renders persistent node instances through immutable render programs
// (graph-001, graph-003): the control thread edits the topology in
// transactions, each commit compiles a program and publishes it, the
// realtime thread adopts it at the next block start and acknowledges the
// revision. Removed nodes and connections fade out, new connections fade
// in, nodes with a tail render it out before they are freed. Parameter
// changes, events and transport requests travel through lock-free queues
// with fixed capacities and defined overflow rules (interop-002); events
// carry timestamps of any domain of time-001 and wait in a bounded
// scheduler until their block. The engine never calls into Dart from the
// realtime thread: a notification thread wakes the listener, the control
// thread takes the notifications.
//
// Threads: every function is tagged. [control] functions are called from
// one thread, the control thread; [realtime] from the thread of the stream
// callback or the plugin host. Node handles are positive integers that are
// never reused; the handle 0 names the graph itself: its input buses are
// the outputs of node 0, its output buses the inputs of node 0, the events
// a host hands to aud_graph_render leave node 0's event output 0, and
// events a node emits into node 0's event input 0 reach the control thread
// as notifications.

#ifndef AUD_AUDIO_GRAPH_H
#define AUD_AUDIO_GRAPH_H

#include <stdint.h>

#include "aud_abi.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct AudGraph AudGraph;

// The type ids of the nodes the engine registers itself.
#define AUD_GRAPH_FEEDBACK_TYPE_ID "aud.graph.feedback"
#define AUD_GRAPH_TAP_TYPE_ID "aud.graph.tap"
#define AUD_GRAPH_OSCILLATOR_TYPE_ID "aud.graph.oscillator"
#define AUD_GRAPH_MIXER_TYPE_ID "aud.graph.mixer"
#define AUD_GRAPH_FILTER_TYPE_ID "aud.graph.filter"

// The handle of the graph itself.
#define AUD_GRAPH_NODE 0

// The id of the internal transport provider.
#define AUD_GRAPH_INTERNAL_TRANSPORT_ID "aud.graph.transport"

// Result codes of the graph itself. The ABI owns the codes down to -99;
// the graph's own start at -100 and never cross the ABI.
enum {
  // The realtime thread allocated, freed or logged: the debug watchdog
  // (ticket 20) counted a violation of the realtime contract.
  AUD_GRAPH_ERROR_REALTIME_VIOLATION = -100,
};

// What the watchdog caught, OR-ed into `value` of its diagnostic.
enum {
  AUD_GRAPH_VIOLATION_NEW = 1 << 0,     // operator new
  AUD_GRAPH_VIOLATION_DELETE = 1 << 1,  // operator delete
  AUD_GRAPH_VIOLATION_ALLOC = 1 << 2,   // AudHostApi.alloc
  AUD_GRAPH_VIOLATION_FREE = 1 << 3,    // AudHostApi.free
  AUD_GRAPH_VIOLATION_LOG = 1 << 4,     // AudHostApi.log
  // A control or offline function of AudHostApi, e.g. begin_load.
  AUD_GRAPH_VIOLATION_CONTROL = 1 << 5,
};

// The lifecycle states of a graph (lifecycle-001).
enum {
  AUD_GRAPH_CREATED = 0,
  AUD_GRAPH_PREPARED = 1,
  AUD_GRAPH_RUNNING = 2,
  AUD_GRAPH_SUSPENDED = 3,
  AUD_GRAPH_STOPPED = 4,
  AUD_GRAPH_DISPOSED = 5,
};

// Flags of a graph.
enum {
  // Late events are dropped instead of played at the block start.
  AUD_GRAPH_DROP_LATE_EVENTS = 1 << 0,
};

// Flags of a connection.
enum {
  // The connection opts out of the latency alignment at its destination:
  // the signal arrives as early as possible (graph-001).
  AUD_CONNECTION_LOW_LATENCY = 1 << 0,
};

// The parameters of the reference nodes.
enum {
  AUD_OSCILLATOR_PARAM_FREQUENCY = 0,
  AUD_OSCILLATOR_PARAM_AMPLITUDE = 1,
  AUD_OSCILLATOR_PARAM_WAVEFORM = 2,  // 0 sine, 1 saw, 2 square, 3 triangle
  AUD_MIXER_NUM_INPUTS = 8,
  AUD_MIXER_PARAM_MASTER = 8,  // gains 0..7 are the input gains
  AUD_FILTER_PARAM_CUTOFF = 0,
  AUD_FILTER_PARAM_RESONANCE = 1,
  AUD_FILTER_PARAM_MODE = 2,  // 0 low, 1 high, 2 band, 3 notch
};

// How a graph is created. Zero means the default.
typedef struct AudGraphConfig {
  uint32_t struct_size;
  double sample_rate;
  uint32_t max_frames;  // the largest block aud_graph_render receives
  uint32_t num_input_buses;
  const uint32_t* input_channels;  // channels per graph input bus
  uint32_t num_output_buses;
  const uint32_t* output_channels;  // channels per graph output bus
  uint32_t flags;                   // AUD_GRAPH_*
  uint32_t max_nodes;               // 0 = 256
  uint32_t max_connections;         // 0 = 1024
  uint32_t param_queue_capacity;    // 0 = 1024
  uint32_t event_queue_capacity;    // 0 = 4096
  uint32_t scheduler_capacity;      // 0 = 4096
  uint32_t notification_capacity;   // 0 = 1024
  uint32_t max_events_per_block;    // 0 = 1024, the budget of interop-002
  uint32_t fade_frames;             // 0 = 5 ms at the sample rate
  uint32_t max_tail_frames;         // 0 = 10 s at the sample rate
  int64_t lookahead_ns;             // 0 = 10 s
  uint32_t message_capacity;  // 0 = 256 messages waiting for their node
  uint32_t reply_capacity;    // 0 = 256 messages of nodes for the control
                              // thread
} AudGraphConfig;

// How a node instance is created: its bus formats. NULL or zero bus counts
// take the default channels of the descriptor.
typedef struct AudNodeConfig {
  uint32_t struct_size;
  uint32_t num_input_buses;  // the descriptor's count, or 0
  const uint32_t* input_channels;
  uint32_t num_output_buses;
  const uint32_t* output_channels;
  uint32_t delay_frames;  // aud.graph.feedback: the delay; 0 = max_frames
  uint32_t reserved;
} AudNodeConfig;

// What the engine reports to the control thread.
enum {
  // The realtime thread adopted `revision`.
  AUD_NOTIFY_REVISION = 1,
  // The realtime thread rendered the first block in the state `code`.
  AUD_NOTIFY_STATE = 2,
  // The retired `node` finished its fade or tail and was freed.
  AUD_NOTIFY_NODE_DONE = 3,
  // A diagnostic with the result code `code`, `count` occurrences in the
  // block, for `node` (0 for the graph): AUD_ERROR_LATE (late events
  // played or dropped), AUD_ERROR_RETIRED (events of a retired node
  // dropped), AUD_ERROR_QUEUE_FULL (block, notification or host event
  // output capacity exceeded, entries dropped; for a `node`, its messages
  // the graph dropped - the reply queue was full or a message was beyond
  // its limits; for node 0 also the reports of nodes that found no place
  // of their own in the block, counted together),
  // AUD_ERROR_CAPACITY (the note tracker of `node` - the last one in the
  // block - refused note ons, which are dropped so that no note stays
  // open), AUD_ERROR_STATE (`node` is parked for a state call: its events
  // wait for the block after the call, `count` of them when the park
  // began), AUD_ERROR_OVERLOAD (the block took longer than it lasts;
  // `value` holds the render time in ns), AUD_ERROR_UNSUPPORTED (the
  // transport refused a request), AUD_GRAPH_ERROR_REALTIME_VIOLATION (the
  // watchdog caught `count` violations; `value` holds the
  // AUD_GRAPH_VIOLATION_* kinds). A node reports codes of its own through
  // AudHostApi.report (ticket 25), summed per node and code: e.g.
  // AUD_ERROR_UNDERFLOW, a stream that fell behind, or
  // AUD_ERROR_UNSUPPORTED, a message it cannot handle on the audio thread.
  AUD_NOTIFY_DIAGNOSTIC = 4,
  // `node` emitted `event` into the graph's event input.
  AUD_NOTIFY_EVENT = 5,
  // The transport changed: `code` the request type applied, `value` the
  // beat in ticks, `number` the tempo, `count` 1 while playing.
  AUD_NOTIFY_TRANSPORT = 6,
  // The time filter reset `count` times.
  AUD_NOTIFY_TIME_RESET = 7,
  // A load of `node` began, progressed or ended (ticket 25): `value` the
  // load id, `code` AUD_PENDING while it runs and its result at the end,
  // `number` the progress from 0 to 1. aud_graph_load_info says more. A
  // progress replaces the progress of the same load that still waits; while
  // `notification_capacity` notifications of loads wait, a new progress is
  // dropped and counted in `notifications_dropped` - a begin or an end never.
  AUD_NOTIFY_LOAD = 8,
  // Nodes sent `count` messages in the block (AudHostApi.emit_message);
  // aud_graph_take_messages takes them.
  AUD_NOTIFY_MESSAGE = 9,
};

// A notification for the control thread: of the realtime thread, or of a
// load, which the control thread and the workers report (ticket 25).
typedef struct AudGraphNotification {
  uint32_t struct_size;
  uint32_t type;  // AUD_NOTIFY_*
  int32_t node;
  int32_t code;
  uint32_t revision;
  uint32_t count;
  int64_t sample_position;  // the block it happened in
  int64_t value;
  double number;
  AudEvent event;
} AudGraphNotification;

// The counters of a graph.
typedef struct AudGraphStats {
  uint32_t struct_size;
  uint32_t state;
  uint32_t revision;  // the adopted revision
  uint32_t scheduled;  // events waiting in the scheduler
  uint64_t blocks_rendered;
  uint64_t frames_rendered;
  int64_t render_time_max_ns;
  int64_t render_time_sum_ns;
  uint64_t events_delivered;
  uint64_t events_late;
  uint64_t events_dropped;
  uint64_t params_applied;
  uint64_t rejected;  // enqueues the control thread refused
  uint64_t notifications_dropped;
  uint64_t overloads;
  uint32_t time_filter_resets;
  uint32_t reserved;
  float output_peak;  // the largest absolute output sample since the reset
  uint32_t reserved2;
  uint64_t realtime_violations;  // what the watchdog caught since the reset
  // Messages of nodes dropped: the reply queue was full, or a message was
  // beyond AUD_GRAPH_MESSAGE_BYTES or AUD_GRAPH_MAX_MESSAGE_ARGS.
  uint64_t messages_dropped;
} AudGraphStats;

// The transport as the realtime thread last saw it.
typedef struct AudGraphTransportState {
  uint32_t struct_size;
  uint32_t playing;
  int64_t beat;  // ticks
  double tempo;
  uint32_t numerator;
  uint32_t denominator;
  uint32_t looping;
  uint32_t reserved;
  int64_t loop_start;
  int64_t loop_end;
} AudGraphTransportState;

// Rendering offline: `frames` frames in blocks of `block_frames`, or in the
// cycle of `block_sizes`, from `inputs` into `outputs` (planar, `frames`
// long, as many buses as the graph has; NULL inputs are silent) on a
// virtual timeline: the host time runs from `start_host_time_ns` with the
// sample clock.
typedef struct AudOfflineRequest {
  uint32_t struct_size;
  uint32_t frames;
  uint32_t block_frames;  // 0 = max_frames
  uint32_t num_block_sizes;
  const uint32_t* block_sizes;
  const AudAudioBus* inputs;
  const AudAudioBus* outputs;
  int64_t start_sample_position;
  int64_t start_host_time_ns;
} AudOfflineRequest;

// What a host that wants the graph's events hands to one render call
// (plugin-002, ticket 20): the render request of the ABI and a buffer the
// engine fills with the events that reached the graph's event input in the
// block - node 0's input 0, where event connections "to the graph" end -
// in ascending sample offset. A host that renders through
// aud_graph_render_host takes these events; rendered through
// aud_graph_render they reach the control thread as AUD_NOTIFY_EVENT.
// With AUD_PROCESS_OFFLINE in `flags` the block renders freewheeling: the
// nodes see the flag and no overload is reported.
typedef struct AudHostRenderRequest {
  uint32_t struct_size;
  uint32_t flags;                   // AUD_PROCESS_*
  const AudRenderRequest* request;  // the buses, time, events and transport
  AudEvent* output_events;          // written by the engine
  uint32_t max_output_events;       // 0: the events stay notifications
  uint32_t num_output_events;       // written by the engine
  uint32_t dropped_output_events;   // did not fit; also a diagnostic
  uint32_t reserved;
} AudHostRenderRequest;

// Wakes the control thread: notifications wait. Called on the notification
// thread, never on the realtime thread.
typedef void (*AudGraphListener)(void* user);

// ............................................................................
// Lifecycle (lifecycle-001)

// [control] Creates a graph in the created state with the reference nodes
// registered; NULL for an invalid configuration.
AUD_EXPORT AudGraph* aud_graph_create(const AudGraphConfig* config);

// [control] Destroys the graph and every instance. The stream must have
// stopped calling aud_graph_render. The loads the nodes leave running end
// as cancelled (see Loads); the call returns once the tasks the nodes
// posted have run - not those of other graphs (ticket 25).
AUD_EXPORT void aud_graph_destroy(AudGraph* graph);

// [control] Prepares every instance for a sample rate and a largest block
// (0 keeps the current value), resets the time filter and the transport
// snapshot and recompiles the program; AUD_ERROR_STATE while running.
AUD_EXPORT int32_t aud_graph_prepare(AudGraph* graph, double sample_rate,
                                     uint32_t max_frames);

// [control] Starts rendering from the prepared or stopped state.
AUD_EXPORT int32_t aud_graph_start(AudGraph* graph);

// [control] Suspends rendering: blocks render silence, the transport and
// the pending events stay. Returns when no render is in flight.
AUD_EXPORT int32_t aud_graph_suspend(AudGraph* graph);

// [control] Resumes rendering after a suspension.
AUD_EXPORT int32_t aud_graph_resume(AudGraph* graph);

// [control] Stops rendering: running notes are closed, the transport stops
// and the time filter resets. Returns when no render is in flight.
AUD_EXPORT int32_t aud_graph_stop(AudGraph* graph);

// [control] The AUD_GRAPH_* state.
AUD_EXPORT int32_t aud_graph_state(AudGraph* graph);

// [control] The prepared sample rate.
AUD_EXPORT double aud_graph_sample_rate(AudGraph* graph);

// [control] The prepared largest block.
AUD_EXPORT uint32_t aud_graph_max_frames(AudGraph* graph);

// ............................................................................
// Node types and instances

// [control] The AudHostApi* packages register their node types and
// transport providers with.
AUD_EXPORT const AudHostApi* aud_graph_host_api(AudGraph* graph);

// [control] The registered node types.
AUD_EXPORT int32_t aud_graph_num_node_types(AudGraph* graph);
AUD_EXPORT const AudNodeDescriptor* aud_graph_node_type(AudGraph* graph,
                                                        int32_t index);
AUD_EXPORT const AudNodeDescriptor* aud_graph_node_type_by_id(
    AudGraph* graph, const char* type_id);

// [control] Creates and prepares an instance of a registered type; returns
// its handle (> 0) or an error code: AUD_ERROR_UNKNOWN_TYPE,
// AUD_ERROR_FORMAT for channels outside the descriptor's range,
// AUD_ERROR_CAPACITY when max_nodes is reached, the error of a create or
// prepare that failed - which spends a handle. The instance renders from
// the next commit on.
AUD_EXPORT int32_t aud_graph_create_node(AudGraph* graph, const char* type_id,
                                         const AudNodeConfig* config);

// [control] The descriptor of an instance; NULL for an unknown handle.
AUD_EXPORT const AudNodeDescriptor* aud_graph_node_descriptor(AudGraph* graph,
                                                              int32_t node);

// [control] The channels of a bus of an instance (`direction` 0 input, 1
// output), or an error code.
AUD_EXPORT int32_t aud_graph_node_channels(AudGraph* graph, int32_t node,
                                           uint32_t direction, uint32_t bus);

// [control] The live handles in creation order; returns the number of
// nodes and writes at most `capacity` of them.
AUD_EXPORT int32_t aud_graph_nodes(AudGraph* graph, int32_t* handles,
                                   uint32_t capacity);

// [control] The latency of an instance in frames (its own, plus the fixed
// block when the engine re-blocks it), or an error code.
AUD_EXPORT int32_t aud_graph_node_latency(AudGraph* graph, int32_t node);

// [control] The frames by which scheduled events of the node are
// pre-delivered so that they are heard at their time: the path latency
// from the node to the graph output (graph-001).
AUD_EXPORT int32_t aud_graph_node_lead(AudGraph* graph, int32_t node);

// [control] The latency of the published program from the graph inputs to
// the graph outputs in frames.
AUD_EXPORT int32_t aud_graph_output_latency(AudGraph* graph);

// [control] The tail of the published program in frames: the longest
// node tail plus the path latency from that node to the graph outputs,
// AUD_TAIL_INFINITE when a contributing node reports an infinite tail.
// Retired nodes still rendering their tails count until they finish.
AUD_EXPORT uint32_t aud_graph_output_tail(AudGraph* graph);

// [control] Writes the state of an instance with AUD_NODE_CAP_STATE into
// `buffer` and its size into `size`; AUD_ERROR_BUFFER_TOO_SMALL with the
// needed size when `capacity` is too small (`capacity` 0 asks for the
// size), AUD_ERROR_UNSUPPORTED for a node without state. The ABI tags
// save_state as [offline]: while the graph runs, the node is parked for
// the call so that no realtime call of it overlaps - a block rendered
// meanwhile skips its process call and clears its outputs; its parameter
// changes, events and transport resets wait for its next block. Not
// running, the call goes straight through.
AUD_EXPORT int32_t aud_graph_node_save_state(AudGraph* graph, int32_t node,
                                             void* buffer, size_t capacity,
                                             size_t* size);

// [control] Restores a state blob saved by `version` of the node's state
// format, parked like aud_graph_node_save_state; AUD_ERROR_STATE_VERSION
// when the node cannot read it.
AUD_EXPORT int32_t aud_graph_node_load_state(AudGraph* graph, int32_t node,
                                             const void* data, size_t size,
                                             uint32_t version);

// ............................................................................
// Transactions (graph-003)

// [control] Opens a transaction; AUD_ERROR_STATE while one is open.
AUD_EXPORT int32_t aud_graph_begin(AudGraph* graph);

// [control] Connects an output bus to an input bus inside the open
// transaction; `flags` are AUD_CONNECTION_*. Channels are mapped: equal
// counts one to one, mono is broadcast, a wider source is averaged into a
// mono input, otherwise the first channels are mapped.
AUD_EXPORT int32_t aud_graph_connect(AudGraph* graph, int32_t from,
                                     uint32_t from_bus, int32_t to,
                                     uint32_t to_bus, uint32_t flags);

// [control] Removes an audio connection inside the open transaction.
AUD_EXPORT int32_t aud_graph_disconnect(AudGraph* graph, int32_t from,
                                        uint32_t from_bus, int32_t to,
                                        uint32_t to_bus);

// [control] Connects an event output to an event input.
AUD_EXPORT int32_t aud_graph_connect_events(AudGraph* graph, int32_t from,
                                            uint32_t from_port, int32_t to,
                                            uint32_t to_port);

// [control] Removes an event connection.
AUD_EXPORT int32_t aud_graph_disconnect_events(AudGraph* graph, int32_t from,
                                               uint32_t from_port, int32_t to,
                                               uint32_t to_port);

// [control] Removes a node inside the open transaction: its connections
// go with it, the instance retires at the commit and is freed once its
// fade or tail has passed (AUD_NOTIFY_NODE_DONE). A node that never
// rendered is freed at once. The commit cancels the loads of the node
// (ticket 25; see Loads).
AUD_EXPORT int32_t aud_graph_remove_node(AudGraph* graph, int32_t node);

// [control] Compiles the topology and publishes the program; returns the
// revision (> 0) or an error code, e.g. AUD_ERROR_CYCLE. On an error the
// transaction stays open.
AUD_EXPORT int32_t aud_graph_commit(AudGraph* graph);

// [control] Discards the open transaction.
AUD_EXPORT int32_t aud_graph_rollback(AudGraph* graph);

// [control] The revision the realtime thread adopted last.
AUD_EXPORT uint32_t aud_graph_revision(AudGraph* graph);

// ............................................................................
// Commands (interop-002)

// [control, one producer] Enqueues a parameter change for the next block;
// a node that has not rendered yet takes it at once. AUD_ERROR_QUEUE_FULL
// when the queue is full - nothing is dropped silently.
AUD_EXPORT int32_t aud_graph_set_param(AudGraph* graph, int32_t node,
                                       uint32_t param, float value,
                                       uint32_t ramp_frames);

// [control, one producer] Enqueues an event for `node` at `at` (NULL means
// the next block). The event's sample offset is ignored; its port names
// the event input. An `id` above zero lets the event be cancelled while it
// waits. AUD_ERROR_LOOKAHEAD beyond the lookahead, AUD_ERROR_CAPACITY when
// the scheduler is full - an event with a time holds its place from the
// enqueue until it leaves the scheduler, so a burst within one block
// cannot overrun it -, AUD_ERROR_STATE for a node that has not been
// committed, AUD_ERROR_RETIRED for a removed node.
AUD_EXPORT int32_t aud_graph_send_event(AudGraph* graph, int32_t node,
                                        const AudEvent* event,
                                        const AudTimestamp* at, uint32_t id);

// [control, one producer] Cancels waiting events: those with `id`, or all
// of `node` when `id` is 0, or every waiting event when both are 0.
AUD_EXPORT int32_t aud_graph_cancel(AudGraph* graph, int32_t node,
                                    uint32_t id);

// [control] Applies a string setting on the calling thread; returns AUD_OK,
// an error code, or the id (> 0) of a load the node started on a worker
// (ticket 25; see Loads). A node that loads on the calling thread may
// block.
AUD_EXPORT int32_t aud_graph_set_string(AudGraph* graph, int32_t node,
                                        uint32_t key, const char* value);

// [control, one producer] Enqueues a transport request; the provider
// applies it on the realtime thread at the request's time.
AUD_EXPORT int32_t aud_graph_transport(AudGraph* graph,
                                       const AudTransportRequest* request);

// [control] Reads the transport as the realtime thread last published it.
AUD_EXPORT int32_t aud_graph_transport_state(AudGraph* graph,
                                             AudGraphTransportState* state);

// [control] Selects a registered transport provider by id (NULL or the
// internal id selects the internal transport); AUD_ERROR_STATE while
// running.
AUD_EXPORT int32_t aud_graph_set_transport_provider(AudGraph* graph,
                                                    const char* id);

// [control] The sample position of the next block.
AUD_EXPORT int64_t aud_graph_sample_position(AudGraph* graph);

// ............................................................................
// Rendering

// [realtime] Renders one block into the host's buses: the render function
// of plugin-002, `user` is the graph. Blocks longer than the prepared
// maximum are refused with AUD_ERROR_INVALID_ARGUMENT and silence; a graph
// that is not running renders silence and returns AUD_OK.
AUD_EXPORT int32_t aud_graph_render(void* user, const AudRenderRequest* request);

// [realtime] Renders one block like aud_graph_render and hands the events
// that reached the graph's event input to the host: the render call of
// the plugin shells (plugin-002). `num_output_events` and
// `dropped_output_events` are written before the call returns.
AUD_EXPORT int32_t aud_graph_render_host(AudGraph* graph,
                                         AudHostRenderRequest* request);

// [control] Renders offline on the calling thread while no stream renders;
// the graph has to be running.
AUD_EXPORT int32_t aud_graph_render_offline(AudGraph* graph,
                                            const AudOfflineRequest* request);

// ............................................................................
// Notifications and taps

// [control] Sets the listener the notification thread wakes; NULL removes
// it.
AUD_EXPORT int32_t aud_graph_set_listener(AudGraph* graph,
                                          AudGraphListener listener,
                                          void* user);

// [control] Takes the waiting notifications, at most `capacity`, and frees
// the instances that finished; returns the number taken.
AUD_EXPORT int32_t aud_graph_take_notifications(AudGraph* graph,
                                                AudGraphNotification* out,
                                                uint32_t capacity);

// [control] Copies the most recent `frames` frames of `channel` of a tap
// node into `out`; frames the tap has not seen yet are 0.
AUD_EXPORT int32_t aud_graph_tap_read(AudGraph* graph, int32_t node,
                                      uint32_t channel, float* out,
                                      uint32_t frames);

// [control] The peak and root mean square of the last block a tap saw.
AUD_EXPORT int32_t aud_graph_tap_meter(AudGraph* graph, int32_t node,
                                       uint32_t channel, float* peak,
                                       float* rms);

// [control] Copies the counters.
AUD_EXPORT int32_t aud_graph_get_stats(AudGraph* graph, AudGraphStats* stats);

// [control] Zeroes the counters; the realtime thread keeps counting.
AUD_EXPORT void aud_graph_reset_stats(AudGraph* graph);

// ............................................................................
// Loads (lifecycle-002, ticket 25)
//
// A node that loads on a worker thread - e.g. the sampler loading an SFZ
// file named by a string setting - opens a load through
// AudHostApi.begin_load: aud_graph_set_string returns its id (> 0), the
// load reports its progress and ends with a result. The graph keeps the
// loads that run and the last 64 that ended. A load the package begins in
// the create or the first prepare of aud_graph_create_node belongs to the
// node being created. The commit that removes a node cancels its loads, and
// a load its node leaves running when it is destroyed - or when its create
// or prepare fails - ends as AUD_ERROR_CANCELLED with the message "the node
// was removed"; an end_load that comes later is ignored.

#define AUD_GRAPH_MAX_TEXT 256

// A load as the graph knows it.
typedef struct AudLoadInfo {
  uint32_t struct_size;
  int32_t id;
  int32_t node;    // the handle of the node that loads
  int32_t result;  // AUD_PENDING while it runs, then the result
  float progress;  // 0 to 1
  uint32_t cancelled;
  int64_t start_ns;  // host time of the begin
  int64_t end_ns;    // host time of the end, 0 while it runs
  char name[AUD_GRAPH_MAX_TEXT];     // what loads, e.g. the file
  // Why a load failed, or what a load that ended well could not do.
  char message[AUD_GRAPH_MAX_TEXT];
} AudLoadInfo;

// [control] The ids of the loads the graph keeps, the oldest first; returns
// their number and writes at most `capacity`.
AUD_EXPORT int32_t aud_graph_loads(AudGraph* graph, int32_t* ids,
                                   uint32_t capacity);

// [control] A load by its id; AUD_ERROR_NOT_FOUND when the graph keeps it
// no longer.
AUD_EXPORT int32_t aud_graph_load_info(AudGraph* graph, int32_t id,
                                       AudLoadInfo* info);

// [control] Asks a running load to stop; the node ends it with
// AUD_ERROR_CANCELLED as soon as it looks. AUD_ERROR_NOT_FOUND for a load
// that ended or is unknown.
AUD_EXPORT int32_t aud_graph_cancel_load(AudGraph* graph, int32_t id);

// [control] Waits until no load of `node` (0: of any node) runs; AUD_OK, or
// AUD_PENDING after `timeout_ms` (0 waits as long as it takes). Then the
// tails of the nodes are read again, so that the output tail counts what
// the loads changed.
AUD_EXPORT int32_t aud_graph_wait_loads(AudGraph* graph, int32_t node,
                                        uint32_t timeout_ms);

// ............................................................................
// Messages of nodes (osc-002, ticket 25)
//
// A node with AUD_NODE_CAP_MESSAGES takes OSC messages: the graph copies a
// message into a slot of its own and delivers it as an AUD_EVENT_MESSAGE
// event - queued, scheduled, ordered and budgeted like every event -, the
// node answers through AudHostApi.emit_message on the audio thread, and the
// control thread takes the answers here.

// The bytes a message to or from a node may take: its path and its
// arguments in the layout of AudGraphMessage.
#define AUD_GRAPH_MESSAGE_BYTES 1000

// The arguments a message to or from a node may carry.
#define AUD_GRAPH_MAX_MESSAGE_ARGS 16

// A message a node sent. `data` holds the path with its terminator, then
// per argument its OSC type tag (one byte) and its value in little endian:
// i and f 4 bytes, h and d 8 bytes, s the text with its terminator, b the
// size in 4 bytes and the bytes; T, F, N and I carry no value.
typedef struct AudGraphMessage {
  uint32_t struct_size;
  int32_t node;
  int64_t sample_position;  // the block it was sent in
  uint32_t num_args;
  uint32_t size;  // bytes used in `data`
  uint8_t data[AUD_GRAPH_MESSAGE_BYTES];
} AudGraphMessage;

// [control, one producer] Sends a message to a node that takes messages,
// at `at` (NULL: the next block); an `id` above zero lets it be cancelled
// while it waits (aud_graph_cancel). The graph copies the message:
// AUD_ERROR_CAPACITY when every slot waits for its node, or the message
// takes more than AUD_GRAPH_MESSAGE_BYTES or carries more than
// AUD_GRAPH_MAX_MESSAGE_ARGS arguments; AUD_ERROR_INVALID_ARGUMENT for a
// path that does not start with a slash or an argument of an unknown type,
// AUD_ERROR_UNSUPPORTED for a node without messages; the rest as
// aud_graph_send_event.
AUD_EXPORT int32_t aud_graph_send_message(AudGraph* graph, int32_t node,
                                          const AudMessage* message,
                                          const AudTimestamp* at, uint32_t id);

// [control] Takes the messages nodes sent, at most `capacity`; returns the
// number taken.
AUD_EXPORT int32_t aud_graph_take_messages(AudGraph* graph,
                                           AudGraphMessage* out,
                                           uint32_t capacity);

// ............................................................................
// The resources of the process (lifecycle-002, ticket 25)
//
// The graphs of a process share two worker threads, which run the tasks of
// the packages (AudHostApi.post_task), and one sample cache budget for the
// caches the packages register: when the caches together hold more, the
// engine asks the largest to free what nobody uses, the least recently
// used first. The budget belongs to the process because the packages share
// what they load between graphs.

typedef struct AudCacheUsage {
  uint32_t struct_size;
  uint32_t caches;   // registered caches
  uint64_t budget;   // bytes
  int64_t bytes;     // the caches hold together
  uint64_t trims;    // trim calls so far
  uint64_t trimmed;  // bytes the trims freed so far
  uint32_t workers;  // worker threads running
  uint32_t tasks;    // tasks queued or running
} AudCacheUsage;

// [control] Sets the sample cache budget of the process in bytes (default
// 256 MB); a smaller budget trims at once, on the calling thread. A budget
// above INT64_MAX is INT64_MAX: no limit.
AUD_EXPORT int32_t aud_cache_set_budget(uint64_t bytes);

// [any thread] The sample cache budget of the process in bytes, at most
// INT64_MAX.
AUD_EXPORT uint64_t aud_cache_budget(void);

// [any thread] The caches, the budget and the workers.
AUD_EXPORT int32_t aud_cache_usage(AudCacheUsage* usage);

// [control] Waits until no task of the workers waits or runs - of any
// graph; AUD_OK, or AUD_PENDING after `timeout_ms` (0 waits as long as it
// takes). The offline renderer waits like this before its first block and
// after every block.
AUD_EXPORT int32_t aud_tasks_wait_idle(uint32_t timeout_ms);

// ............................................................................
// The headless host (plugin-002, ticket 20)
//
// A host over a graph that works without Dart: it loads a graph document
// (doc/schemas/aud_graph_document.schema.json with the node presets of
// aud_node_preset.schema.json of the core) in one transaction, applies the
// presets, resolves the assets the document references, saves the whole
// state back as a document and gives the plugin shells what they need:
// stable parameter ids, latency, tail and the events of the graph per
// block. Every function is [control] except aud_host_render. A failed call
// leaves a message in aud_host_last_error.
//
// Presets apply in the order strings, state, parameters: the strings load
// what a node needs, the state blob restores what parameters cannot hold,
// the parameters come last. The host keeps the value of every parameter -
// the engine has none to report - and assumes the descriptor's default for
// one that no preset names; a saved document names them all. A node whose
// state blob holds parameter values therefore has them overridden by the
// parameters of the preset.
//
// Assets: the document's `assets` table maps an id to a path; a string
// setting whose value is `asset:<id>` names one. The host resolves the
// path against its base directory, checks that the file exists before
// anything is applied and hands the resolved path to the node's
// set_string, which loads the file. The paths are not confined to the base
// directory - a sample library lives elsewhere -, so a document names any
// file the process may read; a host that loads documents it does not trust
// checks them with aud_host_asset first.
//
// Parameter ids: FNV-1a over `<node id>/<parameter id>` with the top bit
// cleared - 31 bits, as VST3 hosts expect (JUCE clears the same bit), and
// never the invalid id 0xFFFFFFFF of VST3 and CLAP. A document whose ids
// collide is refused.

typedef struct AudHost AudHost;

#define AUD_HOST_MAX_BUSES 16
#define AUD_HOST_MAX_NAME 128

// How a host is created. NULL or zero means the default.
typedef struct AudHostOptions {
  uint32_t struct_size;
  // Relative asset paths resolve against it; NULL leaves them relative to
  // the working directory.
  const char* base_directory;
} AudHostOptions;

// What aud_host_inspect reads from a document without a graph: its buses,
// so that a shell can create the graph that fits, and its counts.
typedef struct AudHostDocumentInfo {
  uint32_t struct_size;
  uint32_t schema;
  uint32_t num_input_buses;
  uint32_t num_output_buses;
  uint32_t input_channels[AUD_HOST_MAX_BUSES];
  uint32_t output_channels[AUD_HOST_MAX_BUSES];
  uint32_t num_nodes;
  uint32_t num_assets;
  char name[AUD_HOST_MAX_NAME];
} AudHostDocumentInfo;

// One parameter of the loaded document. The strings stay valid until the
// next load.
typedef struct AudHostParam {
  uint32_t struct_size;
  uint32_t id;     // the stable id
  int32_t node;    // the node handle
  uint32_t index;  // the index in the node's descriptor
  const char* node_id;
  const char* param_id;
  const AudParamDescriptor* descriptor;
  float value;  // the current value as the host set it
} AudHostParam;

// One asset of the loaded document. The strings stay valid until the next
// load or aud_host_set_asset_path.
typedef struct AudHostAsset {
  uint32_t struct_size;
  uint32_t exists;       // whether the file was found at the last check
  const char* id;
  const char* path;      // as the document names it
  const char* resolved;  // what the nodes receive
} AudHostAsset;

// [control] Creates a host over a graph; the host never destroys the
// graph. NULL for an invalid argument.
AUD_EXPORT AudHost* aud_host_create(AudGraph* graph,
                                    const AudHostOptions* options);

// [control] Destroys the host; the graph and its nodes stay.
AUD_EXPORT void aud_host_destroy(AudHost* host);

// [control] The graph of the host.
AUD_EXPORT AudGraph* aud_host_graph(AudHost* host);

// [control] What the last failed call of the host complained about, or an
// empty string; valid until the next call.
AUD_EXPORT const char* aud_host_last_error(AudHost* host);

// [control] Reads the buses, the counts and the name of a document text
// without a graph; AUD_ERROR_INVALID_ARGUMENT for text that is no
// document, AUD_ERROR_CAPACITY for more buses than the info holds.
AUD_EXPORT int32_t aud_host_inspect(const char* json, size_t length,
                                    AudHostDocumentInfo* info);

// [control] Loads a document: validates all of it - the schema, the types
// registered with the graph, the ids, the buses against the graph's, the
// parameters and string keys, the state versions, the assets on disk, the
// parameter ids - and only then creates the nodes, applies their presets
// and connects them in one transaction while the nodes of the previous
// document retire. On an error nothing of the document is applied; the
// error code names the first problem and aud_host_last_error says which.
// The transport settings are sent after the commit: AUD_ERROR_QUEUE_FULL
// then means the document is loaded but the event queue refused them.
AUD_EXPORT int32_t aud_host_load(AudHost* host, const char* json,
                                 size_t length);

// [control] Writes the loaded document with the current state - the
// parameters, the string settings, the state blobs of the nodes and the
// asset table - as JSON text into `buffer` and its length into `size`;
// AUD_ERROR_BUFFER_TOO_SMALL with the needed length when `capacity` is
// smaller than the length plus the terminator. The connections are those
// of the loaded document; edits made on the graph directly are not part
// of it.
AUD_EXPORT int32_t aud_host_save(AudHost* host, char* buffer, size_t capacity,
                                 size_t* size);

// [control] The nodes of the loaded document, and the handle of the node at
// `index` in document order (AUD_ERROR_NOT_FOUND beyond them).
AUD_EXPORT int32_t aud_host_num_nodes(AudHost* host);
AUD_EXPORT int32_t aud_host_node_at(AudHost* host, uint32_t index);

// [control] The handle of the node `id` of the loaded document, or
// AUD_ERROR_NOT_FOUND.
AUD_EXPORT int32_t aud_host_node(AudHost* host, const char* id);

// [control] The document id of a node handle; NULL for a handle the
// document does not know.
AUD_EXPORT const char* aud_host_node_id(AudHost* host, int32_t node);

// [control] Applies a node preset (JSON of aud_node_preset.schema.json) to
// the node `node_id`: the strings on the calling thread, the state blob
// through the node park, the parameters through the queue. A preset the
// validation refuses changes nothing; a part the node or a full queue
// refuses while it is applied stops it there - the parts before stay, and
// the host's table holds what the node has.
AUD_EXPORT int32_t aud_host_apply_preset(AudHost* host, const char* node_id,
                                         const char* json, size_t length);

// [control] Writes the preset of the node `node_id` with its current
// parameters, strings and state as JSON text, like aud_host_save.
AUD_EXPORT int32_t aud_host_node_preset(AudHost* host, const char* node_id,
                                        char* buffer, size_t capacity,
                                        size_t* size);

// [control] The parameters of the loaded document, ordered by id.
AUD_EXPORT int32_t aud_host_num_params(AudHost* host);
AUD_EXPORT int32_t aud_host_param(AudHost* host, uint32_t index,
                                  AudHostParam* out);

// [control] The index of the parameter with the stable `id`, or
// AUD_ERROR_NOT_FOUND.
AUD_EXPORT int32_t aud_host_param_index(AudHost* host, uint32_t id);

// [any thread] The stable id of a parameter: FNV-1a over the node id, a
// slash and the parameter id, the top bit cleared; 0 for a NULL argument.
AUD_EXPORT uint32_t aud_host_param_id(const char* node_id,
                                      const char* param_id);

// [control, one producer] Sets a parameter by its stable id like
// aud_graph_set_param; AUD_ERROR_INVALID_ARGUMENT for a value outside the
// parameter's range, which no document could load again.
AUD_EXPORT int32_t aud_host_set_param(AudHost* host, uint32_t id, float value,
                                      uint32_t ramp_frames);

// [control] The current value of a parameter by its stable id.
AUD_EXPORT int32_t aud_host_get_param(AudHost* host, uint32_t id,
                                      float* value);

// [control] The latency and the tail of the published program:
// aud_graph_output_latency and aud_graph_output_tail.
AUD_EXPORT int32_t aud_host_latency(AudHost* host);
AUD_EXPORT uint32_t aud_host_tail(AudHost* host);

// [control] The assets of the loaded document.
AUD_EXPORT int32_t aud_host_num_assets(AudHost* host);
AUD_EXPORT int32_t aud_host_asset(AudHost* host, uint32_t index,
                                  AudHostAsset* out);

// [control] Relinks an asset: the new path is resolved and checked, every
// string setting that names the asset is applied again, and the saved
// document carries the new path (relative to the base directory when it
// lies inside it).
AUD_EXPORT int32_t aud_host_set_asset_path(AudHost* host, const char* id,
                                           const char* path);

// [realtime] Renders one block: aud_graph_render_host on the host's graph.
AUD_EXPORT int32_t aud_host_render(AudHost* host,
                                   AudHostRenderRequest* request);

// [control] Waits until the loads the document started have ended, e.g.
// the instruments a sampler loads on a worker (ticket 25): aud_graph_wait_loads
// on the host's graph. AUD_OK, or AUD_PENDING after `timeout_ms`.
AUD_EXPORT int32_t aud_host_wait_loads(AudHost* host, uint32_t timeout_ms);

// ............................................................................
// The debug watchdog (ticket 20)
//
// Compiled in with AUD_GRAPH_WATCHDOG: the native tests always, a build
// hook when the app sets the user define `watchdog: true`. While a block
// renders, the global operator new and delete and the host api's alloc,
// free and log count every call from the rendering thread as a violation
// of the realtime contract, then do their work anyway. The block reports
// the hits as AUD_GRAPH_ERROR_REALTIME_VIOLATION and the counters keep
// them. A release build without the define carries none of this.
//
// What it sees: the native tests link the engine and their nodes into one
// program and see every allocation. In an app the replaced operators serve
// the library of aud_audio_graph only - the engine and its reference nodes;
// a DSP package is a library of its own whose allocations the watchdog
// cannot see, only its calls of the host api. RealtimeSanitizer sees every
// library (S22).

// Whether the watchdog is compiled in.
AUD_EXPORT int32_t aud_graph_watchdog_enabled(void);

// [any thread] The violations every graph of the process counted since
// the last reset; 0 without the watchdog.
AUD_EXPORT uint64_t aud_graph_watchdog_violations(void);

// [any thread] Zeroes the process-wide count.
AUD_EXPORT void aud_graph_watchdog_reset(void);

#ifdef __cplusplus
}
#endif

#endif  // AUD_AUDIO_GRAPH_H
