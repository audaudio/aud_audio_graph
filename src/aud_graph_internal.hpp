// @license
// Copyright (c) Audanika. All Rights Reserved.
//
// Use of this source code is governed by terms that can be
// found in the LICENSE file in the root of this package.

// The structures the engine shares between its translation units: node
// instances, edges, programs, queues, the scheduler, the transport and the
// graph itself. Which thread owns a field is noted on it; the realtime
// thread never allocates, so every container here is sized on the control
// thread before it is used.

#ifndef AUD_GRAPH_INTERNAL_HPP
#define AUD_GRAPH_INTERNAL_HPP

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "aud_abi.h"
#include "aud_audio_graph.h"
#include "aud_semaphore.hpp"
#include "aud_spsc_queue.hpp"
#include "aud_time_filter.h"

// With AUD_GRAPH_RTSAN the realtime path carries Clang's nonblocking
// effect, so that the RealtimeSanitizer checks everything a block calls
// (ticket 20, decision 8); other toolchains see nothing.
#if AUD_GRAPH_RTSAN && defined(__clang__)
#define AUD_NONBLOCKING [[clang::nonblocking]]
#else
#define AUD_NONBLOCKING
#endif

namespace aud {

// The defaults of AudGraphConfig.
constexpr uint32_t kDefaultMaxNodes = 256;
constexpr uint32_t kDefaultMaxConnections = 1024;
constexpr uint32_t kDefaultParamQueueCapacity = 1024;
constexpr uint32_t kDefaultEventQueueCapacity = 4096;
constexpr uint32_t kDefaultSchedulerCapacity = 4096;
constexpr uint32_t kDefaultNotificationCapacity = 1024;
constexpr uint32_t kDefaultMaxEventsPerBlock = 1024;
constexpr double kDefaultFadeSeconds = 0.005;
constexpr double kDefaultMaxTailSeconds = 10.0;
constexpr int64_t kDefaultLookaheadNs = 10000000000LL;
// Segments the transport may split one block into.
constexpr uint32_t kMaxSegments = 16;
// Transport requests waiting for their time.
constexpr uint32_t kMaxPendingRequests = 64;
// Programs the realtime thread can park before the control thread frees
// them; when every slot is taken the adoption waits for the next block.
constexpr size_t kRetiredPrograms = 8;
// The largest channel count of a bus.
constexpr uint32_t kMaxChannels = 64;
// The longest delay line in frames an alignment edge may need.
constexpr uint32_t kMaxAlignmentFrames = 1u << 20;
// The least frames a tap keeps: 8192 are 170 ms at 48 kHz, enough for a
// scope that reads once per frame of the UI.
constexpr uint32_t kMinTapRingFrames = 8192;
// Retries of a tap read while the realtime thread writes the ring.
constexpr uint32_t kTapReadAttempts = 16;
// No reset waits for a node.
constexpr uint32_t kNoReset = UINT32_MAX;

struct NodeInstance;
struct Program;

// ............................................................................
// Commands (interop-002)

enum class CommandKind : uint32_t { event, cancel, transport };

// A command of the event queue: an event with its time, a cancellation or
// a transport request.
struct Command {
  CommandKind kind = CommandKind::event;
  NodeInstance* instance = nullptr;  // resolved on the control thread
  uint32_t id = 0;
  uint64_t sequence = 0;
  AudEvent event{};
  AudTimestamp at{};
  AudTransportRequest request{};
  bool reserved = false;  // holds a place in the scheduler (ticket 20)
};

// A command of the parameter queue.
struct ParamCommand {
  NodeInstance* instance = nullptr;
  uint32_t param = 0;
  uint32_t rampFrames = 0;
  float value = 0;
  uint64_t sequence = 0;
};

// ............................................................................
// Running notes (interop-002): the engine closes every note it delivered.

struct NoteTracker {
  static constexpr uint32_t kCapacity = 256;
  uint32_t count = 0;
  // type << 28 | group << 24 | channel << 16 | note << 8 | port
  uint32_t notes[kCapacity];

  // Tracks a note on; false when full.
  bool add(uint32_t key) {
    for (uint32_t i = 0; i < count; ++i) {
      if (notes[i] == key) return true;
    }
    if (count == kCapacity) return false;
    notes[count++] = key;
    return true;
  }

  // Whether a note sounds.
  bool contains(uint32_t key) const {
    for (uint32_t i = 0; i < count; ++i) {
      if (notes[i] == key) return true;
    }
    return false;
  }

  // Forgets a note.
  void remove(uint32_t key) {
    for (uint32_t i = 0; i < count; ++i) {
      if (notes[i] != key) continue;
      notes[i] = notes[count - 1];
      count -= 1;
      return;
    }
  }
};

// ............................................................................
// Scheduled events: a pool with one index heap per time domain, each
// ordered by the domain's value so that the first pending event stops the
// pop.

struct ScheduledEvent {
  NodeInstance* instance = nullptr;  // NULL: the graph's event output
  AudEvent event{};
  AudTimestamp at{};
  int64_t key = 0;  // the time minus the lead, in the domain's unit
  uint32_t id = 0;
  uint64_t sequence = 0;
  bool cancelled = false;
};

struct EventHeap {
  std::vector<uint32_t> items;  // pool indices
};

struct Scheduler {
  std::vector<ScheduledEvent> pool;
  std::vector<uint32_t> freeList;
  EventHeap heaps[3];  // sample, host, beat
  uint32_t count = 0;

  void init(uint32_t capacity);
  bool push(const ScheduledEvent& event);
  // The index of the earliest event of a domain, or UINT32_MAX.
  uint32_t top(uint32_t domain) const;
  void pop(uint32_t domain);
  void release(uint32_t index);
  // Marks events cancelled: those of `instance` (or all when NULL) with
  // `id` (or all when 0); returns the number cancelled.
  uint32_t cancel(const NodeInstance* instance, uint32_t id);

 private:
  int64_t keyOf(uint32_t index) const;
  bool less(uint32_t a, uint32_t b) const;
  void siftUp(EventHeap& heap, size_t position);
  void siftDown(EventHeap& heap, size_t position);
};

// ............................................................................
// Nodes and edges

enum class Builtin : uint8_t { none, feedback, tap };

// The state of a tap node, read by the control thread. The ring and its
// write position are relaxed atomics: the control thread reads them while
// the realtime thread writes, the generation (a seqlock) tells a torn read
// apart; plain floats there were a data race (ticket 20, ThreadSanitizer).
struct TapState {
  uint32_t channels = 0;
  uint32_t ringFrames = 0;
  std::unique_ptr<std::atomic<float>[]> ring;  // channels x ringFrames
  std::atomic<uint32_t> writePos{0};
  std::atomic<uint32_t> generation{0};
  std::atomic<float> peak[kMaxChannels];
  std::atomic<float> rms[kMaxChannels];
};

// Re-blocks a node without AUD_NODE_CAP_VARIABLE_BLOCK (graph-002): the
// inner process sees blocks of exactly `blockSize` frames; the output lags
// by one block.
struct FixedBlockWrapper {
  uint32_t blockSize = 0;
  uint32_t fill = 0;
  std::vector<std::vector<float>> inputs;   // channel-major, blockSize each
  std::vector<std::vector<float>> outputs;  // channel-major
  std::vector<float*> inputPointers;
  std::vector<float*> outputPointers;
  std::vector<AudAudioBus> inputBuses;
  std::vector<AudAudioBus> outputBuses;
  std::vector<AudEvent> events;
  uint32_t numEvents = 0;
  uint32_t dropped = 0;
  bool primed = false;
};

// A node instance (graph-003): persistent, addressed by a stable handle.
struct NodeInstance {
  // Control thread.
  int32_t handle = 0;
  const AudNodeDescriptor* descriptor = nullptr;
  Builtin builtin = Builtin::none;
  void* instance = nullptr;
  std::vector<uint32_t> inputChannels;
  std::vector<uint32_t> outputChannels;
  uint32_t latency = 0;  // own latency plus the fixed block
  uint32_t tail = 0;
  std::unique_ptr<FixedBlockWrapper> wrapper;
  bool committed = false;  // part of a published program
  bool retired = false;    // removed by a committed transaction
  uint64_t retireSequence = 0;
  uint32_t programs = 0;  // programs alive that reference the instance
  uint32_t lead = 0;      // of the last compiled program
  // Feedback (graph-001): a delay line of at least max_frames.
  uint32_t delayFrames = 0;
  std::vector<float> delayLine;  // channels x delayFrames
  uint32_t delayPos = 0;
  // Tap.
  TapState tap;
  // Realtime thread.
  uint32_t rtIndex = UINT32_MAX;  // index in the adopted program
  NoteTracker notes;
  bool rtRetired = false;
  bool rtDone = false;
  // A transport reset that came while the node was parked (ticket 20); it
  // runs before the node's next process call.
  uint32_t pendingReset = kNoReset;
  bool rtParked = false;  // the last block skipped the node
  bool rtStopped = false;        // the transport was stopped at the last block
  uint32_t fadeRemaining = 0;    // the incoming fade
  uint32_t tailRemaining = 0;    // the tail, or the outgoing fade
  // Realtime thread writes, control thread reads.
  std::atomic<bool> done{false};
  // Control thread sets for a state call (ticket 20). While it is set the
  // realtime thread makes no call of the vtable: process is skipped,
  // set_param travels as an event, a reset waits; the node's events wait
  // for the next block. Sequentially consistent, like the state.
  std::atomic<bool> parked{false};
};

// The identity of an audio connection.
struct AudioEdgeId {
  int32_t from = 0;
  uint32_t fromBus = 0;
  int32_t to = 0;
  uint32_t toBus = 0;
  uint32_t flags = 0;

  bool sameEnds(const AudioEdgeId& other) const {
    return from == other.from && fromBus == other.fromBus && to == other.to &&
           toBus == other.toBus;
  }
};

// The identity of an event connection.
struct EventEdgeId {
  int32_t from = 0;
  uint32_t fromPort = 0;
  int32_t to = 0;
  uint32_t toPort = 0;

  bool operator==(const EventEdgeId& other) const {
    return from == other.from && fromPort == other.fromPort &&
           to == other.to && toPort == other.toPort;
  }
};

// An audio connection with its state (graph-003): shared by every program
// that contains it, so fades and alignment delays survive a recompile.
struct Edge {
  // Control thread.
  AudioEdgeId id;
  bool retiring = false;  // removed, fading out
  uint32_t programs = 0;  // programs alive that reference the edge
  uint32_t delayFrames = 0;
  uint32_t delayChannels = 0;
  std::vector<float> delayLine;  // channels x delayFrames
  // Realtime thread; a fresh edge starts silent and fades in.
  uint32_t delayPos = 0;
  float gain = 0;
  float target = 0;
  uint32_t remaining = 0;
  // Control thread sets, realtime thread clears at the adoption.
  std::atomic<bool> fadeInPending{false};
  // Realtime thread writes, control thread reads.
  std::atomic<bool> inactive{false};
};

// The topology the control thread edits.
struct Topology {
  std::vector<AudioEdgeId> audio;
  std::vector<EventEdgeId> events;
};

// ............................................................................
// Programs (graph-001)

enum class SlotKind : uint8_t { pool, input, output, silence };

// One channel buffer a job reads or writes.
struct Slot {
  SlotKind kind = SlotKind::silence;
  uint32_t index = 0;    // pool buffer, or host bus
  uint32_t channel = 0;  // of the host bus
};

enum class JobKind : uint8_t {
  clear,
  mix,
  node,
  feedbackRead,
  feedbackWrite,
  tap,
};

// A step of the render program.
struct Job {
  JobKind kind = JobKind::clear;
  uint32_t node = 0;  // program node index
  Edge* edge = nullptr;
  bool assign = false;  // mix: write instead of add
  uint32_t srcRef = 0;  // first index into Program::slotRefs
  uint32_t srcChannels = 0;
  uint32_t dstRef = 0;
  uint32_t dstChannels = 0;
};

// A bus of a node inside a program.
struct BusBinding {
  uint32_t ref = 0;  // first index into Program::slotRefs
  uint32_t channels = 0;
};

// Where an event output leads.
struct EventRoute {
  uint32_t fromNode = 0;  // program node index; UINT32_MAX for the graph
  uint32_t fromPort = 0;
  uint32_t toNode = 0;  // UINT32_MAX for the graph
  uint32_t toPort = 0;
};

// A node inside a program.
struct ProgramNode {
  NodeInstance* instance = nullptr;
  uint32_t firstInputBus = 0;  // into Program::buses
  uint32_t numInputBuses = 0;
  uint32_t firstOutputBus = 0;
  uint32_t numOutputBuses = 0;
  uint32_t lead = 0;
  bool skip = false;  // done rendering
};

// An immutable render program; the realtime thread adopts it at a block
// start and the control thread frees it afterwards.
struct Program {
  uint32_t revision = 0;
  bool first = false;  // the first program: edges start at full gain
  uint32_t maxFrames = 0;
  uint32_t outputLatency = 0;
  uint32_t outputTail = 0;  // or AUD_TAIL_INFINITE
  std::vector<ProgramNode> nodes;
  std::vector<Job> jobs;
  std::vector<Slot> slots;
  std::vector<uint32_t> slotRefs;
  std::vector<BusBinding> bindings;
  std::vector<AudAudioBus> buses;
  std::vector<float*> channelPointers;  // the storage the buses point into
  std::vector<float*> slotPointers;     // per slot, patched every block
  std::vector<float> pool;
  uint32_t poolBuffers = 0;
  std::vector<Edge*> edges;    // every edge referenced
  std::vector<Edge*> fadeIn;   // new edges
  std::vector<Edge*> fadeOut;  // retiring edges
  std::vector<NodeInstance*> retire;  // instances newly retired
  std::vector<EventRoute> routes;     // sorted by (fromNode, fromPort)
  // The block events one event of the graph's event output becomes.
  uint32_t graphFanout = 0;
};

// ............................................................................
// Transport (time-001)

struct PendingRequest {
  AudTransportRequest request{};
  bool used = false;
};

// The internal transport provider: an anchor of a beat at a sample
// position and a tempo, so that positions never drift.
struct InternalTransport {
  bool playing = false;
  double tempo = 120;
  uint32_t numerator = 4;
  uint32_t denominator = 4;
  bool looping = false;
  int64_t loopStart = 0;
  int64_t loopEnd = 0;
  int64_t anchorSample = 0;
  int64_t anchorBeat = 0;  // ticks at anchorSample
  PendingRequest pending[kMaxPendingRequests];
};

// One registered transport provider.
struct TransportProvider {
  std::string id;
  const AudTransportProviderVTable* vtable = nullptr;
  void* provider = nullptr;
  uint32_t capabilities = 0;
};

// ............................................................................
// The per-block event table

struct BlockEvent {
  uint32_t node = 0;  // program node index
  uint32_t rank = 0;  // note off 0, other 1, note on 2
  uint64_t sequence = 0;
  AudEvent event{};
};

struct EmittedEvent {
  AudEvent event{};
  uint32_t next = UINT32_MAX;
};

// An event of a parked node, kept for the next block (ticket 20).
struct DeferredEvent {
  NodeInstance* instance = nullptr;
  AudEvent event{};
  uint64_t sequence = 0;
};

struct NodeRange {
  uint32_t begin = 0;
  uint32_t end = 0;
  uint32_t emittedHead = UINT32_MAX;
  uint32_t emittedTail = UINT32_MAX;
};

// Diagnostics counted per block and reported once per block.
struct BlockDiagnostics {
  uint32_t late = 0;
  uint32_t retired = 0;
  uint32_t overflow = 0;  // block table or emitted arena
  uint32_t schedulerFull = 0;
  uint32_t trackerFull = 0;
  int32_t trackerFullNode = 0;  // the last node whose tracker refused
  uint32_t transportRefused = 0;
};

}  // namespace aud

// ............................................................................
// The graph

struct AudGraph {
  // Configuration, control thread.
  AudGraphConfig config{};
  std::vector<uint32_t> inputChannels;
  std::vector<uint32_t> outputChannels;
  double sampleRate = 48000;
  uint32_t maxFrames = 0;
  uint32_t fadeFrames = 0;
  uint32_t maxTailFrames = 0;
  int64_t lookaheadNs = 0;
  AudHostApi hostApi{};
  std::vector<const AudNodeDescriptor*> types;
  std::vector<aud::TransportProvider> providers;
  int32_t selectedProvider = -1;  // index, -1 internal

  // Lifecycle.
  std::atomic<uint32_t> state{AUD_GRAPH_CREATED};
  std::atomic<bool> inFlight{false};

  // Instances and topology, control thread.
  std::vector<std::unique_ptr<aud::NodeInstance>> instances;
  int32_t nextHandle = 1;
  std::vector<std::unique_ptr<aud::Edge>> edges;
  std::vector<std::unique_ptr<aud::Edge>> zombieEdges;
  aud::Topology topology;
  aud::Topology working;
  std::vector<int32_t> workingRemoved;
  bool transactionOpen = false;
  uint32_t nextRevision = 1;
  uint32_t outputLatency = 0;
  uint32_t outputTail = 0;
  bool hasProgram = false;

  // Program hand-over.
  std::atomic<aud::Program*> pending{nullptr};
  std::atomic<aud::Program*> retired[aud::kRetiredPrograms]{};
  aud::Program* current = nullptr;  // realtime thread
  std::atomic<uint32_t> adoptedRevision{0};

  // Queues.
  std::unique_ptr<AudSpscQueue<aud::Command>> commands;
  std::unique_ptr<AudSpscQueue<aud::ParamCommand>> params;
  uint64_t nextSequence = 1;  // control thread
  std::atomic<uint64_t> poppedSequence{0};
  aud::Scheduler scheduler;  // realtime thread
  std::atomic<uint32_t> scheduledCount{0};
  // Places of the scheduler taken by events with a time: the control thread
  // reserves one per event before the enqueue, the realtime thread gives it
  // back when the event leaves the queue or the scheduler (ticket 20).
  std::atomic<uint32_t> schedulerReserved{0};

  // Notifications.
  std::unique_ptr<AudSpscQueue<AudGraphNotification>> notifications;
  std::atomic<uint64_t> notificationsDropped{0};
  AudSemaphore wake;
  std::thread notifier;
  std::atomic<bool> stopNotifier{false};
  std::atomic<bool> wakePending{false};
  std::atomic<AudGraphListener> listener{nullptr};
  std::atomic<void*> listenerUser{nullptr};
  bool blockNotified = false;  // realtime: a notification was pushed

  // Time, realtime thread.
  AudTimeFilter filter{};
  bool synthesizedValid = false;
  int64_t synthesizedOrigin = 0;
  int64_t synthesizedSample = 0;
  std::atomic<int64_t> samplePosition{0};
  std::atomic<uint32_t> filterResets{0};
  AudStreamTime streamTime{};
  AudTransportSegment segments[aud::kMaxSegments];
  AudTransportSnapshot snapshot{};
  bool lastPlaying = false;
  bool pendingSeek = false;  // an immediate seek flags the first segment
  // A stop or a prepare reset the nodes while no block rendered; the first
  // block after the restart closes their tracked notes (ticket 20). The
  // control thread writes it before the state becomes running again.
  bool closeNotesPending = false;

  // Transport, realtime thread, published through atomics.
  aud::InternalTransport transport;
  std::atomic<uint32_t> publishedPlaying{0};
  std::atomic<int64_t> publishedBeat{0};
  std::atomic<double> publishedTempo{120};
  std::atomic<uint32_t> publishedNumerator{4};
  std::atomic<uint32_t> publishedDenominator{4};
  std::atomic<uint32_t> publishedLooping{0};
  std::atomic<int64_t> publishedLoopStart{0};
  std::atomic<int64_t> publishedLoopEnd{0};

  // Per-block scratch, sized by the configuration.
  std::vector<aud::Command> popped;
  uint32_t numPopped = 0;
  std::vector<aud::BlockEvent> blockEvents;
  uint32_t numBlockEvents = 0;
  std::vector<AudEvent> nodeEvents;
  std::vector<aud::EmittedEvent> emitted;
  uint32_t numEmitted = 0;
  std::vector<aud::DeferredEvent> deferred;  // of parked nodes
  uint32_t numDeferred = 0;
  std::vector<aud::NodeRange> nodeRanges;
  std::vector<float> silence;
  aud::BlockDiagnostics diagnostics;
  uint32_t currentNode = UINT32_MAX;  // the node job that runs
  bool offline = false;
  bool stateObserved = false;
  // The host's event output of the block, realtime thread (ticket 20).
  AudHostRenderRequest* hostRequest = nullptr;

  // Counters the realtime thread writes and the control thread reads.
  std::atomic<uint64_t> blocksRendered{0};
  std::atomic<uint64_t> framesRendered{0};
  std::atomic<int64_t> renderMax{0};
  std::atomic<int64_t> renderSum{0};
  std::atomic<uint64_t> eventsDelivered{0};
  std::atomic<uint64_t> eventsLate{0};
  std::atomic<uint64_t> eventsDropped{0};
  std::atomic<uint64_t> paramsApplied{0};
  std::atomic<uint64_t> rejected{0};
  std::atomic<uint64_t> overloads{0};
  std::atomic<float> outputPeak{0};
  std::atomic<uint64_t> realtimeViolations{0};
};

namespace aud {

// ............................................................................
// Functions the translation units share

// aud_graph.cpp
NodeInstance* instanceOf(AudGraph* graph, int32_t handle);

// aud_graph_compiler.cpp: compiles the topology into a program; returns
// NULL with the error code written.
Program* compileProgram(AudGraph* graph, const Topology& topology,
                        int32_t* error);

// aud_graph_render.cpp
void prepareRealtime(AudGraph* graph);
void resetTime(AudGraph* graph);
void resetTransport(AudGraph* graph);
void anchorTransport(AudGraph* graph);
void publishTransportState(AudGraph* graph);
int32_t renderBlock(AudGraph* graph,
                    const AudRenderRequest* request) AUD_NONBLOCKING;
void emitEvent(AudGraph* graph, void* instance, const AudEvent* event);
int64_t beatNow(AudGraph* graph);

// aud_graph_nodes.cpp: registers the reference nodes.
int32_t registerReferenceNodes(const AudHostApi* host);

}  // namespace aud

#endif  // AUD_GRAPH_INTERNAL_HPP
