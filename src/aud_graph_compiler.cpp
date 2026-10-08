// @license
// Copyright (c) Audanika. All Rights Reserved.
//
// Use of this source code is governed by terms that can be
// found in the LICENSE file in the root of this package.

// The program compiler (graph-001, graph-003): turns the topology the
// control thread edited into an immutable render program. It orders the
// nodes topologically with feedback nodes split into a reader and a writer,
// rejects cycles, aligns the latency of the paths that meet at a node,
// assigns the channel buffers by lifetime so that they are reused, carries
// the fade and alignment state of every connection in a shared edge object
// and lists what the realtime thread has to do when it adopts the program.
// Runs on the control thread only and may allocate.

#include <algorithm>
#include <cstring>
#include <map>
#include <memory>
#include <set>
#include <tuple>
#include <utility>

#include "aud_graph_internal.hpp"

namespace aud {
namespace {

constexpr uint32_t kGraphVertex = UINT32_MAX;

// A vertex of the dependency graph: a node, or one side of a feedback node.
struct Vertex {
  uint32_t node = 0;
  bool writer = false;
  int32_t handle = 0;
  std::vector<uint32_t> successors;
  uint32_t indegree = 0;
  uint32_t aligned = 0;  // latency at the output of the vertex
  bool reachesOutput = false;
};

// A connection resolved to program nodes and vertices.
struct EdgeUse {
  Edge* edge = nullptr;
  uint32_t srcNode = kGraphVertex;  // program node, or the graph input
  uint32_t srcBus = 0;
  uint32_t srcChannels = 0;
  uint32_t srcVertex = kGraphVertex;
  uint32_t dstNode = kGraphVertex;  // program node, or the graph output
  uint32_t dstBus = 0;
  uint32_t dstChannels = 0;
  uint32_t dstVertex = kGraphVertex;
  uint32_t delay = 0;
};

// A channel buffer with its lifetime in jobs.
struct PoolSlot {
  uint32_t slot = 0;
  uint32_t def = UINT32_MAX;
  uint32_t last = 0;
};

class Compiler {
  // The ends of a connection: the key of the set of the topology.
  using Ends = std::tuple<int32_t, uint32_t, int32_t, uint32_t>;

 public:
  Compiler(AudGraph* graph, const Topology& topology)
      : graph_(graph), topology_(topology), program_(new Program()) {}

  Program* run(int32_t* error) {
    collectNodes();
    if (!resolveEdges(error)) return fail();
    buildVertices();
    if (!sortVertices()) {
      *error = AUD_ERROR_CYCLE;
      return fail();
    }
    alignLatency();
    allocateOutputs();
    emitJobs();
    assignPool();
    buildBuses();
    buildRoutes();
    finish();
    return program_.release();
  }

 private:
  // ..........................................................................
  // Nodes

  void collectNodes() {
    for (auto& owned : graph_->instances) {
      NodeInstance* instance = owned.get();
      if (instance->retired && instance->done.load(std::memory_order_acquire)) {
        continue;
      }
      nodeIndex_[instance->handle] =
          static_cast<uint32_t>(program_->nodes.size());
      ProgramNode node;
      node.instance = instance;
      node.numInputBuses = static_cast<uint32_t>(instance->inputChannels.size());
      node.numOutputBuses =
          static_cast<uint32_t>(instance->outputChannels.size());
      program_->nodes.push_back(node);
    }
  }

  NodeInstance* instanceAt(uint32_t node) const {
    return program_->nodes[node].instance;
  }

  // ..........................................................................
  // Edges

  // Finds the edge object of a connection, or creates it.
  Edge* edgeFor(const AudioEdgeId& id, bool* created) {
    *created = false;
    for (auto& edge : graph_->edges) {
      if (edge->id.sameEnds(id)) return edge.get();
    }
    auto edge = std::make_unique<Edge>();
    edge->id = id;
    Edge* raw = edge.get();
    graph_->edges.push_back(std::move(edge));
    created_.push_back(raw);
    *created = true;
    return raw;
  }

  bool resolveEnd(int32_t handle, uint32_t bus, bool output, uint32_t* node,
                  uint32_t* channels) {
    if (handle == AUD_GRAPH_NODE) {
      const auto& list = output ? graph_->inputChannels : graph_->outputChannels;
      if (bus >= list.size()) return false;
      *node = kGraphVertex;
      *channels = list[bus];
      return true;
    }
    const auto found = nodeIndex_.find(handle);
    if (found == nodeIndex_.end()) return false;
    const NodeInstance* instance = instanceAt(found->second);
    const auto& list = output ? instance->outputChannels : instance->inputChannels;
    if (bus >= list.size()) return false;
    *node = found->second;
    *channels = list[bus];
    return true;
  }

  bool resolveEdges(int32_t* error) {
    for (const AudioEdgeId& id : topology_.audio) ends_.insert(endsOf(id));
    // The connections of the topology: live edges, created or revived.
    for (const AudioEdgeId& id : topology_.audio) {
      bool created = false;
      Edge* edge = edgeFor(id, &created);
      if (!created && edge->retiring) revived_.push_back(edge);
      if (created || edge->retiring) {
        edge->retiring = false;
        edge->fadeInPending.store(true, std::memory_order_release);
      }
      edge->id.flags = id.flags;
      if (!addUse(edge, error)) return false;
    }
    // Edges no longer in the topology retire; those that faded out go.
    for (auto& edge : graph_->edges) {
      if (inTopology(edge->id)) continue;
      if (edge->inactive.load(std::memory_order_acquire)) continue;
      if (!edge->retiring) {
        edge->retiring = true;
        newlyRetiring_.push_back(edge.get());
      }
      if (!addUse(edge.get(), error)) return false;
    }
    return true;
  }

  static Ends endsOf(const AudioEdgeId& id) {
    return std::make_tuple(id.from, id.fromBus, id.to, id.toBus);
  }

  bool inTopology(const AudioEdgeId& id) const {
    return ends_.count(endsOf(id)) > 0;
  }

  bool addUse(Edge* edge, int32_t* error) {
    EdgeUse use;
    use.edge = edge;
    use.srcBus = edge->id.fromBus;
    use.dstBus = edge->id.toBus;
    if (!resolveEnd(edge->id.from, edge->id.fromBus, true, &use.srcNode,
                    &use.srcChannels) ||
        !resolveEnd(edge->id.to, edge->id.toBus, false, &use.dstNode,
                    &use.dstChannels)) {
      // An end of a retiring edge is gone: the edge goes with it.
      if (edge->retiring) {
        edge->inactive.store(true, std::memory_order_release);
        return true;
      }
      *error = AUD_ERROR_NOT_FOUND;
      return false;
    }
    uses_.push_back(use);
    return true;
  }

  // ..........................................................................
  // Vertices and order

  bool isFeedback(uint32_t node) const {
    return instanceAt(node)->builtin == Builtin::feedback;
  }

  void buildVertices() {
    const uint32_t count = static_cast<uint32_t>(program_->nodes.size());
    readerVertex_.assign(count, 0);
    writerVertex_.assign(count, 0);
    for (uint32_t i = 0; i < count; ++i) {
      readerVertex_[i] = static_cast<uint32_t>(vertices_.size());
      vertices_.push_back(vertex(i, false));
      if (isFeedback(i)) {
        writerVertex_[i] = static_cast<uint32_t>(vertices_.size());
        vertices_.push_back(vertex(i, true));
        link(readerVertex_[i], writerVertex_[i]);
      } else {
        writerVertex_[i] = readerVertex_[i];
      }
    }
    for (EdgeUse& use : uses_) {
      if (use.srcNode != kGraphVertex) use.srcVertex = readerVertex_[use.srcNode];
      if (use.dstNode != kGraphVertex) use.dstVertex = writerVertex_[use.dstNode];
      if (use.srcVertex != kGraphVertex && use.dstVertex != kGraphVertex) {
        link(use.srcVertex, use.dstVertex);
      }
    }
    for (const EventEdgeId& id : topology_.events) {
      const auto from = nodeIndex_.find(id.from);
      const auto to = nodeIndex_.find(id.to);
      if (from == nodeIndex_.end() || to == nodeIndex_.end()) continue;
      link(readerVertex_[from->second], writerVertex_[to->second]);
    }
  }

  Vertex vertex(uint32_t node, bool writer) const {
    Vertex v;
    v.node = node;
    v.writer = writer;
    v.handle = instanceAt(node)->handle;
    return v;
  }

  void link(uint32_t from, uint32_t to) {
    vertices_[from].successors.push_back(to);
    vertices_[to].indegree += 1;
  }

  // Kahn's algorithm; among the ready vertices the smallest handle goes
  // first, so a topology always compiles to the same order.
  bool sortVertices() {
    std::vector<uint32_t> indegree;
    for (const Vertex& vertex : vertices_) indegree.push_back(vertex.indegree);
    std::vector<bool> placed(vertices_.size(), false);
    while (order_.size() < vertices_.size()) {
      uint32_t best = UINT32_MAX;
      for (uint32_t v = 0; v < vertices_.size(); ++v) {
        if (placed[v] || indegree[v] != 0) continue;
        if (best == UINT32_MAX || before(v, best)) best = v;
      }
      if (best == UINT32_MAX) return false;
      placed[best] = true;
      order_.push_back(best);
      for (uint32_t next : vertices_[best].successors) indegree[next] -= 1;
    }
    return true;
  }

  bool before(uint32_t a, uint32_t b) const {
    const Vertex& va = vertices_[a];
    const Vertex& vb = vertices_[b];
    if (va.handle != vb.handle) return va.handle < vb.handle;
    return !va.writer && vb.writer;
  }

  // ..........................................................................
  // Latency (graph-001): paths that meet at a node are delayed to the
  // longest one unless a connection opts out.

  uint32_t arrival(const EdgeUse& use) const {
    return use.srcVertex == kGraphVertex ? 0 : vertices_[use.srcVertex].aligned;
  }

  void alignLatency() {
    for (uint32_t v : order_) {
      Vertex& vertex = vertices_[v];
      const uint32_t own =
          vertex.writer ? 0 : instanceAt(vertex.node)->latency;
      vertex.aligned = own + alignInto(v);
    }
    program_->outputLatency = alignInto(kGraphVertex);
    for (uint32_t v : order_) vertices_[v].reachesOutput = false;
    for (const EdgeUse& use : uses_) {
      if (use.dstVertex == kGraphVertex && use.srcVertex != kGraphVertex) {
        vertices_[use.srcVertex].reachesOutput = true;
      }
    }
    for (auto it = order_.rbegin(); it != order_.rend(); ++it) {
      Vertex& vertex = vertices_[*it];
      for (uint32_t next : vertex.successors) {
        if (vertices_[next].reachesOutput) vertex.reachesOutput = true;
      }
    }
    for (uint32_t i = 0; i < program_->nodes.size(); ++i) {
      const Vertex& vertex = vertices_[readerVertex_[i]];
      NodeInstance* instance = instanceAt(i);
      const uint32_t lead =
          vertex.reachesOutput && program_->outputLatency >= vertex.aligned
              ? program_->outputLatency - vertex.aligned + instance->latency
              : instance->latency;
      program_->nodes[i].lead = lead;
      instance->lead = lead;
    }
    computeTail();
  }

  // The tail of the program (ticket 20): the longest node tail plus the
  // path latency from that node to the output, infinite when a node says
  // so. Only steady paths count: a connection that is fading out after a
  // disconnect is gone within the fade, while a connection that carries
  // the tail of a retired node stays until the node is done.
  void computeTail() {
    std::vector<bool> steady(vertices_.size(), false);
    for (auto it = order_.rbegin(); it != order_.rend(); ++it) {
      const uint32_t v = *it;
      for (const EdgeUse& use : uses_) {
        if (use.srcVertex != v) continue;
        if (use.edge->retiring && fadesOut(use)) continue;
        if (use.dstVertex == kGraphVertex || steady[use.dstVertex]) {
          steady[v] = true;
          break;
        }
      }
    }
    uint32_t tail = 0;
    bool infinite = false;
    for (uint32_t i = 0; i < program_->nodes.size(); ++i) {
      const Vertex& vertex = vertices_[readerVertex_[i]];
      const NodeInstance* instance = instanceAt(i);
      if (!steady[readerVertex_[i]] || instance->tail == 0 ||
          program_->outputLatency < vertex.aligned) {
        continue;
      }
      if (instance->tail == AUD_TAIL_INFINITE) {
        infinite = true;
      } else {
        tail = std::max(tail, instance->tail + program_->outputLatency -
                                  vertex.aligned);
      }
    }
    program_->outputTail = infinite ? AUD_TAIL_INFINITE : tail;
  }

  // Computes the delays of the connections into a vertex and returns the
  // latency they arrive with.
  uint32_t alignInto(uint32_t vertex) {
    uint32_t target = 0;
    for (const EdgeUse& use : uses_) {
      if (use.dstVertex != vertex) continue;
      if (use.edge->id.flags & AUD_CONNECTION_LOW_LATENCY) continue;
      target = std::max(target, arrival(use));
    }
    uint32_t arrived = 0;
    for (EdgeUse& use : uses_) {
      if (use.dstVertex != vertex) continue;
      const bool lowLatency = (use.edge->id.flags & AUD_CONNECTION_LOW_LATENCY) != 0;
      use.delay = lowLatency ? 0 : target - arrival(use);
      arrived = std::max(arrived, arrival(use) + use.delay);
    }
    return arrived;
  }

  // ..........................................................................
  // Slots: every output channel of every node gets its buffer before the
  // jobs are emitted; input buffers follow in job order.

  uint32_t newSlot(SlotKind kind, uint32_t index, uint32_t channel) {
    Slot slot;
    slot.kind = kind;
    slot.index = index;
    slot.channel = channel;
    program_->slots.push_back(slot);
    const uint32_t id = static_cast<uint32_t>(program_->slots.size() - 1);
    poolIndex_.push_back(UINT32_MAX);
    if (kind == SlotKind::pool) {
      poolIndex_[id] = static_cast<uint32_t>(poolSlots_.size());
      poolSlots_.push_back({id, UINT32_MAX, 0});
    }
    return id;
  }

  // Creates a binding of `channels` slots of one kind: pool buffers, the
  // channels of the host bus `bus`, or the silence buffer.
  uint32_t makeBinding(SlotKind kind, uint32_t bus, uint32_t channels) {
    BusBinding binding;
    binding.ref = static_cast<uint32_t>(program_->slotRefs.size());
    binding.channels = channels;
    for (uint32_t c = 0; c < channels; ++c) {
      program_->slotRefs.push_back(newSlot(kind, bus, c));
    }
    program_->bindings.push_back(binding);
    return static_cast<uint32_t>(program_->bindings.size() - 1);
  }

  void allocateOutputs() {
    const uint32_t count = static_cast<uint32_t>(program_->nodes.size());
    outputBindings_.assign(count, {});
    for (uint32_t i = 0; i < count; ++i) {
      for (uint32_t channels : instanceAt(i)->outputChannels) {
        outputBindings_[i].push_back(makeBinding(SlotKind::pool, 0, channels));
      }
    }
    for (uint32_t bus = 0; bus < graph_->inputChannels.size(); ++bus) {
      graphInputs_.push_back(
          makeBinding(SlotKind::input, bus, graph_->inputChannels[bus]));
    }
    for (uint32_t bus = 0; bus < graph_->outputChannels.size(); ++bus) {
      graphOutputs_.push_back(
          makeBinding(SlotKind::output, bus, graph_->outputChannels[bus]));
    }
  }

  PoolSlot* poolSlotOf(uint32_t slot) {
    const uint32_t index = poolIndex_[slot];
    return index == UINT32_MAX ? nullptr : &poolSlots_[index];
  }

  // Records that the job writes (def) or reads (use) the binding.
  void touch(uint32_t binding, uint32_t job, bool write) {
    const BusBinding& b = program_->bindings[binding];
    for (uint32_t c = 0; c < b.channels; ++c) {
      PoolSlot* pool = poolSlotOf(program_->slotRefs[b.ref + c]);
      if (pool == nullptr) continue;
      if (write && pool->def == UINT32_MAX) pool->def = job;
      pool->last = std::max(pool->last, job);
    }
  }

  // ..........................................................................
  // Jobs

  uint32_t sourceBinding(const EdgeUse& use) const {
    return use.srcNode == kGraphVertex ? graphInputs_[use.srcBus]
                                       : outputBindings_[use.srcNode][use.srcBus];
  }

  uint32_t jobIndex() const {
    return static_cast<uint32_t>(program_->jobs.size());
  }

  // Emits the jobs that fill an input bus from its connections; returns
  // the binding of the bus.
  uint32_t mixInto(uint32_t dstNode, uint32_t dstBus, uint32_t channels,
                   uint32_t existing) {
    std::vector<EdgeUse*> incoming;
    for (EdgeUse& use : uses_) {
      if (use.dstNode == dstNode && use.dstBus == dstBus) incoming.push_back(&use);
    }
    if (incoming.empty() && existing == UINT32_MAX && dstNode != kGraphVertex) {
      return makeBinding(SlotKind::silence, 0, channels);
    }
    const uint32_t binding = existing == UINT32_MAX ? makeBinding(SlotKind::pool, 0, channels) : existing;
    if (incoming.size() != 1) {
      Job clear;
      clear.kind = JobKind::clear;
      clear.dstRef = program_->bindings[binding].ref;
      clear.dstChannels = channels;
      touch(binding, jobIndex(), true);
      program_->jobs.push_back(clear);
    }
    for (EdgeUse* use : incoming) {
      prepareDelay(*use);
      Job mix;
      mix.kind = JobKind::mix;
      mix.edge = use->edge;
      mix.assign = incoming.size() == 1;
      const uint32_t source = sourceBinding(*use);
      mix.srcRef = program_->bindings[source].ref;
      mix.srcChannels = use->srcChannels;
      mix.dstRef = program_->bindings[binding].ref;
      mix.dstChannels = channels;
      touch(source, jobIndex(), false);
      touch(binding, jobIndex(), true);
      program_->jobs.push_back(mix);
    }
    return binding;
  }

  // Gives an edge the delay line of its alignment; a changed delay takes a
  // fresh edge object.
  void prepareDelay(EdgeUse& use) {
    Edge* edge = use.edge;
    const uint32_t delay = std::min(use.delay, kMaxAlignmentFrames);
    if (edge->delayFrames == delay && edge->delayChannels == use.srcChannels) {
      return;
    }
    if (edge->programs > 0) {
      // Another program renders the edge: replace it.
      auto fresh = std::make_unique<Edge>();
      fresh->id = edge->id;
      fresh->retiring = edge->retiring;
      fresh->fadeInPending.store(true, std::memory_order_release);
      for (auto& owned : graph_->edges) {
        if (owned.get() != edge) continue;
        graph_->zombieEdges.push_back(std::move(owned));
        owned = std::move(fresh);
        edge = owned.get();
        break;
      }
      use.edge = edge;
      created_.push_back(edge);
    }
    edge->delayFrames = delay;
    edge->delayChannels = use.srcChannels;
    edge->delayPos = 0;
    edge->delayLine.assign(static_cast<size_t>(delay) * use.srcChannels, 0.0f);
  }

  void emitJobs() {
    const uint32_t count = static_cast<uint32_t>(program_->nodes.size());
    inputBindings_.assign(count, {});
    for (uint32_t v : order_) {
      const Vertex& vertex = vertices_[v];
      const uint32_t i = vertex.node;
      NodeInstance* instance = instanceAt(i);
      if (instance->builtin == Builtin::feedback) {
        if (!vertex.writer) {
          Job read;
          read.kind = JobKind::feedbackRead;
          read.node = i;
          read.dstRef = program_->bindings[outputBindings_[i][0]].ref;
          read.dstChannels = instance->outputChannels[0];
          touch(outputBindings_[i][0], jobIndex(), true);
          program_->jobs.push_back(read);
        } else {
          const uint32_t binding =
              mixInto(i, 0, instance->inputChannels[0], UINT32_MAX);
          inputBindings_[i].push_back(binding);
          Job write;
          write.kind = JobKind::feedbackWrite;
          write.node = i;
          write.srcRef = program_->bindings[binding].ref;
          write.srcChannels = instance->inputChannels[0];
          touch(binding, jobIndex(), false);
          program_->jobs.push_back(write);
        }
        continue;
      }
      emitNodeJobs(i);
    }
    for (uint32_t bus = 0; bus < graphOutputs_.size(); ++bus) {
      mixInto(kGraphVertex, bus, graph_->outputChannels[bus], graphOutputs_[bus]);
    }
  }

  void emitNodeJobs(uint32_t i) {
    NodeInstance* instance = instanceAt(i);
    const bool inPlace =
        instance->builtin == Builtin::none &&
        (instance->descriptor->capabilities & AUD_NODE_CAP_IN_PLACE) != 0 &&
        !instance->inputChannels.empty() && !instance->outputChannels.empty() &&
        instance->inputChannels[0] == instance->outputChannels[0];
    for (uint32_t bus = 0; bus < instance->inputChannels.size(); ++bus) {
      const uint32_t channels = instance->inputChannels[bus];
      uint32_t binding;
      if (inPlace && bus == 0) {
        // The output buffer doubles as the input buffer; without a
        // connection mixInto clears it.
        binding = mixInto(i, bus, channels, outputBindings_[i][0]);
      } else {
        binding = mixInto(i, bus, channels, UINT32_MAX);
      }
      inputBindings_[i].push_back(binding);
    }
    Job job;
    job.kind = instance->builtin == Builtin::tap ? JobKind::tap : JobKind::node;
    job.node = i;
    const uint32_t index = jobIndex();
    for (uint32_t binding : inputBindings_[i]) touch(binding, index, false);
    for (uint32_t binding : outputBindings_[i]) touch(binding, index, true);
    if (instance->builtin == Builtin::tap) {
      job.srcRef = program_->bindings[inputBindings_[i][0]].ref;
      job.srcChannels = instance->inputChannels[0];
      job.dstRef = program_->bindings[outputBindings_[i][0]].ref;
      job.dstChannels = instance->outputChannels[0];
    }
    program_->jobs.push_back(job);
  }

  // ..........................................................................
  // The pool: buffers are reused once their last reader ran.

  void assignPool() {
    std::vector<uint32_t> busyUntil;  // per buffer: the last job that reads it
    std::sort(poolSlots_.begin(), poolSlots_.end(),
              [](const PoolSlot& a, const PoolSlot& b) {
                return a.def < b.def || (a.def == b.def && a.slot < b.slot);
              });
    for (const PoolSlot& pool : poolSlots_) {
      const uint32_t def = pool.def == UINT32_MAX ? 0 : pool.def;
      uint32_t buffer = UINT32_MAX;
      for (uint32_t b = 0; b < busyUntil.size(); ++b) {
        if (busyUntil[b] < def) {
          buffer = b;
          break;
        }
      }
      if (buffer == UINT32_MAX) {
        buffer = static_cast<uint32_t>(busyUntil.size());
        busyUntil.push_back(0);
      }
      busyUntil[buffer] = std::max(pool.last, def);
      program_->slots[pool.slot].index = buffer;
    }
    program_->poolBuffers = static_cast<uint32_t>(busyUntil.size());
    program_->maxFrames = graph_->maxFrames;
    program_->pool.assign(
        static_cast<size_t>(program_->poolBuffers) * graph_->maxFrames, 0.0f);
  }

  // ..........................................................................
  // The bus structs of the process contexts

  void buildBuses() {
    program_->channelPointers.assign(program_->slotRefs.size(), nullptr);
    program_->slotPointers.assign(program_->slots.size(), nullptr);
    for (uint32_t i = 0; i < program_->nodes.size(); ++i) {
      ProgramNode& node = program_->nodes[i];
      node.firstInputBus = static_cast<uint32_t>(program_->buses.size());
      for (uint32_t binding : inputBindings_[i]) pushBus(binding);
      node.firstOutputBus = static_cast<uint32_t>(program_->buses.size());
      for (uint32_t binding : outputBindings_[i]) pushBus(binding);
    }
  }

  void pushBus(uint32_t binding) {
    const BusBinding& b = program_->bindings[binding];
    AudAudioBus bus;
    bus.struct_size = sizeof(AudAudioBus);
    bus.num_channels = b.channels;
    bus.channels = program_->channelPointers.data() + b.ref;
    program_->buses.push_back(bus);
  }

  // ..........................................................................
  // Event routes

  void buildRoutes() {
    for (const EventEdgeId& id : topology_.events) {
      EventRoute route;
      if (id.from == AUD_GRAPH_NODE) {
        route.fromNode = kGraphVertex;
      } else {
        const auto from = nodeIndex_.find(id.from);
        if (from == nodeIndex_.end()) continue;
        route.fromNode = from->second;
      }
      route.fromPort = id.fromPort;
      if (id.to == AUD_GRAPH_NODE) {
        route.toNode = kGraphVertex;
      } else {
        const auto to = nodeIndex_.find(id.to);
        if (to == nodeIndex_.end()) continue;
        route.toNode = to->second;
      }
      route.toPort = id.toPort;
      program_->routes.push_back(route);
    }
    std::sort(program_->routes.begin(), program_->routes.end(),
              [](const EventRoute& a, const EventRoute& b) {
                return a.fromNode < b.fromNode ||
                       (a.fromNode == b.fromNode && a.fromPort < b.fromPort);
              });
    for (const EventRoute& route : program_->routes) {
      if (route.fromNode == kGraphVertex && route.toNode != kGraphVertex) {
        program_->graphFanout += 1;
      }
    }
  }

  // ..........................................................................
  // The adoption lists and the references

  void finish() {
    program_->first = !graph_->hasProgram;
    for (const EdgeUse& use : uses_) {
      Edge* edge = use.edge;
      if (std::find(program_->edges.begin(), program_->edges.end(), edge) !=
          program_->edges.end()) {
        continue;
      }
      program_->edges.push_back(edge);
      edge->programs += 1;
      if (edge->retiring) {
        // A connection removed before it was ever heard stays silent.
        edge->fadeInPending.store(false, std::memory_order_release);
        if (fadesOut(use)) program_->fadeOut.push_back(edge);
      } else if (edge->fadeInPending.load(std::memory_order_acquire)) {
        program_->fadeIn.push_back(edge);
      }
    }
    for (ProgramNode& node : program_->nodes) {
      node.instance->programs += 1;
      if (node.instance->retired) program_->retire.push_back(node.instance);
    }
  }

  // A retiring edge fades out unless it carries the tail of a retired
  // source into a live destination (graph-003).
  bool fadesOut(const EdgeUse& use) const {
    if (use.srcNode == kGraphVertex) return true;
    const NodeInstance* source = instanceAt(use.srcNode);
    if (!source->retired || source->tail == 0) return true;
    if (use.dstNode == kGraphVertex) return false;
    return instanceAt(use.dstNode)->retired;
  }

  // Undoes what the compile did to the edge objects.
  Program* fail() {
    for (Edge* edge : revived_) {
      edge->retiring = true;
      edge->fadeInPending.store(false, std::memory_order_release);
    }
    for (Edge* edge : newlyRetiring_) edge->retiring = false;
    for (Edge* edge : created_) {
      for (auto it = graph_->edges.begin(); it != graph_->edges.end(); ++it) {
        if (it->get() != edge) continue;
        graph_->edges.erase(it);
        break;
      }
    }
    return nullptr;
  }

  AudGraph* graph_;
  const Topology& topology_;
  std::unique_ptr<Program> program_;
  std::map<int32_t, uint32_t> nodeIndex_;
  std::set<Ends> ends_;
  std::vector<EdgeUse> uses_;
  std::vector<Edge*> created_;
  std::vector<Edge*> revived_;
  std::vector<Edge*> newlyRetiring_;
  std::vector<uint32_t> poolIndex_;
  std::vector<Vertex> vertices_;
  std::vector<uint32_t> readerVertex_;
  std::vector<uint32_t> writerVertex_;
  std::vector<uint32_t> order_;
  std::vector<PoolSlot> poolSlots_;
  std::vector<std::vector<uint32_t>> outputBindings_;
  std::vector<std::vector<uint32_t>> inputBindings_;
  std::vector<uint32_t> graphInputs_;
  std::vector<uint32_t> graphOutputs_;
};

}  // namespace

Program* compileProgram(AudGraph* graph, const Topology& topology,
                        int32_t* error) {
  *error = AUD_OK;
  Compiler compiler(graph, topology);
  return compiler.run(error);
}

}  // namespace aud
