// @license
// Copyright (c) Audanika. All Rights Reserved.
//
// Use of this source code is governed by terms that can be
// found in the LICENSE file in the root of this package.

import 'dart:async';
import 'dart:ffi';
import 'dart:typed_data';

import 'package:aud_audio_core/aud_audio_core.dart';
import 'package:aud_audio_core/aud_audio_core_bindings.dart' as core;
import 'package:ffi/ffi.dart';

import 'aud_audio_graph_bindings_generated.dart' as bindings;
import 'aud_graph_document.dart';
import 'aud_graph_exception.dart';
import 'aud_graph_node.dart';
import 'aud_graph_notification.dart';
import 'aud_graph_options.dart';
import 'aud_graph_state.dart';
import 'aud_graph_stats.dart';
import 'aud_graph_transaction.dart';
import 'aud_graph_transport_state.dart';

// #############################################################################
/// The audio graph engine (ticket 19, S2): persistent node instances
/// edited in transactions that compile into immutable render programs
/// (graph-003), commands through realtime queues (interop-002), events in
/// any time domain (time-001), the internal transport, taps and the
/// notifications of the realtime thread. `aud_audio_io` and the plugin
/// shells call the engine's render function; tests render offline through
/// `AudOfflineRenderer`.
///
/// The graph registers its own node types - oscillator, mixer, filter,
/// feedback and tap - and `aud.core.gain` of `aud_audio_core`; DSP packages
/// register theirs with [hostApi]. The node handle 0, [io], is the graph:
/// its input buses are the outputs of [io], its output buses the inputs.
class AudGraph {
  /// Creates a graph in the created state.
  ///
  /// - [sampleRate] the sample rate in Hz
  /// - [maxFrames] the largest block a render receives
  /// - [inputChannels] the channels of each input bus of the graph
  /// - [outputChannels] the channels of each output bus of the graph
  /// - [options] capacities and policies
  /// - [id] the graph id in OSC addresses (osc-001)
  /// - [registerCoreNodes] whether `aud.core.gain` is registered
  /// - [listen] whether the notification thread wakes [pump]; tests that
  ///   render offline call [pump] themselves
  AudGraph({
    double sampleRate = 48000,
    int maxFrames = 1024,
    List<int> inputChannels = const [],
    List<int> outputChannels = const [2],
    AudGraphOptions options = const AudGraphOptions(),
    this.id = 1,
    bool registerCoreNodes = true,
    bool listen = true,
  }) : inputChannels = List.unmodifiable(inputChannels),
       outputChannels = List.unmodifiable(outputChannels) {
    final config = calloc<bindings.AudGraphConfig>();
    final inputs = calloc<Uint32>(inputChannels.length + 1);
    final outputs = calloc<Uint32>(outputChannels.length + 1);
    try {
      inputs.asTypedList(inputChannels.length).setAll(0, inputChannels);
      outputs.asTypedList(outputChannels.length).setAll(0, outputChannels);
      config.ref
        ..struct_size = sizeOf<bindings.AudGraphConfig>()
        ..sample_rate = sampleRate
        ..max_frames = maxFrames
        ..num_input_buses = inputChannels.length
        ..input_channels = inputs
        ..num_output_buses = outputChannels.length
        ..output_channels = outputs;
      options.writeTo(config.ref);
      _pointer = bindings.aud_graph_create(config);
    } finally {
      calloc.free(inputs);
      calloc.free(outputs);
      calloc.free(config);
    }
    if (_pointer == nullptr) {
      throw const AudGraphException(
        AUD_ERROR_INVALID_ARGUMENT,
        'The graph was not created; check the configuration.',
      );
    }
    _buffer = calloc<bindings.AudGraphNotification>(_batch);
    if (registerCoreNodes) AudCoreGain.register(hostApi);
    io = AudNode(
      handle: bindings.AUD_GRAPH_NODE,
      name: AudGraphDocument.graphId,
      descriptor: _ioDescriptor(),
      inputChannels: this.outputChannels,
      outputChannels: this.inputChannels,
    );
    router.registerGraph(id);
    adapter = AudOscAdapter(router: router);
    if (listen) {
      _listener = NativeCallable<Void Function(Pointer<Void>)>.listener(
        _onWake,
      );
      bindings.aud_graph_set_listener(
        _pointer,
        _listener!.nativeFunction,
        nullptr,
      );
    }
  }

  /// A graph built from [document]: its buses, nodes, connections and
  /// transport settings; see [load].
  factory AudGraph.fromDocument(
    AudGraphDocument document, {
    double sampleRate = 48000,
    int maxFrames = 1024,
    AudGraphOptions options = const AudGraphOptions(),
    int id = 1,
    bool listen = true,
  }) {
    final graph = AudGraph(
      sampleRate: sampleRate,
      maxFrames: maxFrames,
      inputChannels: document.inputChannels,
      outputChannels: document.outputChannels,
      options: options,
      id: id,
      listen: listen,
    );
    try {
      graph.load(document);
    } catch (_) {
      graph.dispose();
      rethrow;
    }
    return graph;
  }

  // ...........................................................................
  /// The graph id in OSC addresses.
  final int id;

  /// The channels of each input bus of the graph.
  final List<int> inputChannels;

  /// The channels of each output bus of the graph.
  final List<int> outputChannels;

  /// The graph itself as a node: the handle 0.
  late final AudNode io;

  /// The OSC router the nodes of the graph are registered with.
  final AudOscRouter router = AudOscRouter();

  /// The OSC adapter over [router]; [handleOsc] uses it.
  late final AudOscAdapter adapter;

  /// The native graph.
  Pointer<bindings.AudGraph> get pointer => _pointer;

  /// The `AudHostApi` DSP packages register their node types and
  /// transport providers with.
  Pointer<core.AudHostApi> get hostApi => bindings.aud_graph_host_api(_pointer);

  /// The sample rate in Hz.
  double get sampleRate => bindings.aud_graph_sample_rate(_pointer);

  /// The largest block a render receives.
  int get maxFrames => bindings.aud_graph_max_frames(_pointer);

  /// Whether [dispose] ran.
  bool get isDisposed => _pointer == nullptr;

  // ...........................................................................
  // Node types and nodes

  /// The registered node types.
  List<AudNodeDescriptor> get nodeTypes => [
    for (var i = 0; i < bindings.aud_graph_num_node_types(_pointer); i++)
      AudNodeDescriptor.fromNative(
        bindings.aud_graph_node_type(_pointer, i).ref,
      ),
  ];

  /// The node type [typeId], or null.
  AudNodeDescriptor? nodeType(String typeId) {
    final native = typeId.toNativeUtf8();
    try {
      final descriptor = bindings.aud_graph_node_type_by_id(
        _pointer,
        native.cast(),
      );
      return descriptor == nullptr
          ? null
          : AudNodeDescriptor.fromNative(descriptor.ref);
    } finally {
      calloc.free(native);
    }
  }

  /// The live nodes in creation order.
  List<AudNode> get nodes => List.unmodifiable(_nodes.values);

  /// The live node with [handle], or null.
  AudNode? node(int handle) => _nodes[handle];

  /// The live node named [name], or null; [io] for the graph's own name.
  AudNode? nodeNamed(String name) {
    if (name == io.name) return io;
    for (final node in _nodes.values) {
      if (node.name == name) return node;
    }
    return null;
  }

  /// Creates and prepares an instance of [typeId]; it renders from the
  /// next [transaction] on.
  ///
  /// - [name] the unique name; `n<handle>` by default
  /// - [inputChannels], [outputChannels] the channels of each bus; null
  ///   takes the type's defaults
  /// - [delayFrames] the delay of a feedback node; 0 takes the largest block
  /// - [preset] parameters and string settings applied at once
  AudNode createNode(
    String typeId, {
    String? name,
    List<int>? inputChannels,
    List<int>? outputChannels,
    int delayFrames = 0,
    AudNodePreset? preset,
  }) {
    if (name != null && nodeNamed(name) != null) {
      throw ArgumentError.value(name, 'name', 'A node of that name exists');
    }
    if (name != null && !AudGraphDocument.idPattern.hasMatch(name)) {
      throw ArgumentError.value(name, 'name', 'Not a node id');
    }
    final native = typeId.toNativeUtf8();
    final config = calloc<bindings.AudNodeConfig>();
    final inputs = calloc<Uint32>((inputChannels?.length ?? 0) + 1);
    final outputs = calloc<Uint32>((outputChannels?.length ?? 0) + 1);
    int handle;
    try {
      inputs
          .asTypedList(inputChannels?.length ?? 0)
          .setAll(0, inputChannels ?? []);
      outputs
          .asTypedList(outputChannels?.length ?? 0)
          .setAll(0, outputChannels ?? []);
      config.ref
        ..struct_size = sizeOf<bindings.AudNodeConfig>()
        ..num_input_buses = inputChannels?.length ?? 0
        ..input_channels = inputs
        ..num_output_buses = outputChannels?.length ?? 0
        ..output_channels = outputs
        ..delay_frames = delayFrames;
      handle = AudGraphException.check(
        bindings.aud_graph_create_node(_pointer, native.cast(), config),
        'create a node of type $typeId',
      );
    } finally {
      calloc.free(native);
      calloc.free(config);
      calloc.free(inputs);
      calloc.free(outputs);
    }
    final descriptor = AudNodeDescriptor.fromNative(
      bindings.aud_graph_node_descriptor(_pointer, handle).ref,
    );
    final node = AudNode(
      handle: handle,
      name: name ?? 'n$handle',
      descriptor: descriptor,
      inputChannels: [
        for (var b = 0; b < descriptor.inputBuses.length; b++)
          bindings.aud_graph_node_channels(_pointer, handle, 0, b),
      ],
      outputChannels: [
        for (var b = 0; b < descriptor.outputBuses.length; b++)
          bindings.aud_graph_node_channels(_pointer, handle, 1, b),
      ],
    );
    _nodes[handle] = node;
    _params[handle] = {};
    _strings[handle] = {};
    _delays[handle] = delayFrames;
    router.registerNode(
      graph: id,
      node: handle,
      handle: handle,
      descriptor: descriptor,
    );
    if (preset != null) applyPreset(node, preset);
    return node;
  }

  /// Applies the parameters and string settings of [preset] to [node]; the
  /// state blob waits for the headless host (ticket 20). Throws an
  /// [ArgumentError] when the preset does not fit the node's type.
  void applyPreset(AudNode node, AudNodePreset preset) {
    final problems = preset.validate(node.descriptor);
    if (problems.isNotEmpty) {
      throw ArgumentError.value(preset, 'preset', problems.join('; '));
    }
    for (final entry in preset.params.entries) {
      setParam(node, entry.key, entry.value);
    }
    for (final entry in preset.strings.entries) {
      setString(node, entry.key, entry.value);
    }
  }

  /// The parameters set from Dart for [node], by id.
  Map<String, double> paramsOf(AudNode node) =>
      Map.unmodifiable(_params[node.handle] ?? const {});

  /// The string settings set from Dart for [node], by id.
  Map<String, String> stringsOf(AudNode node) =>
      Map.unmodifiable(_strings[node.handle] ?? const {});

  /// The audio connections of the published topology.
  List<AudConnection> get connections => List.unmodifiable(_connections);

  /// The event connections of the published topology.
  List<AudEventConnection> get eventConnections =>
      List.unmodifiable(_eventConnections);

  // ...........................................................................
  // Transactions (graph-003)

  /// Runs [edits] inside a transaction and commits them as one revision;
  /// returns the revision. When an edit or the commit fails the
  /// transaction is rolled back and the error rethrown.
  int transaction(void Function(AudGraphTransaction transaction) edits) {
    AudGraphException.check(bindings.aud_graph_begin(_pointer), 'begin');
    final transaction = AudGraphTransaction(this);
    int revision;
    try {
      edits(transaction);
      revision = AudGraphException.check(
        bindings.aud_graph_commit(_pointer),
        'commit',
      );
    } catch (_) {
      bindings.aud_graph_rollback(_pointer);
      rethrow;
    }
    _record(transaction);
    return revision;
  }

  /// Completes once the realtime thread has adopted [revision] or a later
  /// one, or when the graph is disposed.
  Future<void> adopted(int revision) {
    if (this.revision >= revision) return Future.value();
    final completer = Completer<void>();
    _waiting.add((revision, completer));
    return completer.future;
  }

  /// The revision the realtime thread adopted last.
  int get revision => bindings.aud_graph_revision(_pointer);

  /// The latency of the published program from the graph inputs to the
  /// graph outputs, in frames.
  int get outputLatency => bindings.aud_graph_output_latency(_pointer);

  /// The latency of [node] in frames.
  int latencyOf(AudNode node) => AudGraphException.check(
    bindings.aud_graph_node_latency(_pointer, node.handle),
    'read the latency of ${node.name}',
  );

  /// The frames by which scheduled events of [node] are pre-delivered so
  /// that they are heard at their time (graph-001).
  int leadOf(AudNode node) => AudGraphException.check(
    bindings.aud_graph_node_lead(_pointer, node.handle),
    'read the lead of ${node.name}',
  );

  // ...........................................................................
  // Commands (interop-002)

  /// Sets a parameter of [node]: [param] is its index or its id. An
  /// immediate [at] takes the parameter queue, any other time travels as a
  /// parameter event; [rampFrames] ramps the change.
  void setParam(
    AudNode node,
    Object param,
    double value, {
    int rampFrames = 0,
    AudTimestamp at = const AudTimestamp.immediate(),
  }) {
    final index = param is int ? param : node.paramIndex(param as String);
    if (index < 0 || index >= node.descriptor.params.length) {
      throw ArgumentError.value(
        param,
        'param',
        'No parameter of ${node.typeId}',
      );
    }
    _params[node.handle]?[node.descriptor.params[index].id] = value;
    if (at.isImmediate) {
      AudGraphException.check(
        bindings.aud_graph_set_param(
          _pointer,
          node.handle,
          index,
          value,
          rampFrames,
        ),
        'set parameter $index of ${node.name}',
      );
      return;
    }
    sendEvent(
      node,
      AudParamEvent(paramIndex: index, value: value, rampFrames: rampFrames),
      at: at,
    );
  }

  /// Sends [event] to [node] at [at]; the event's port names the event
  /// input. [io] as the node sends the event through the graph's event
  /// output to the nodes connected to it. An [id] above zero lets the event
  /// be cancelled while it waits.
  void sendEvent(
    AudNode node,
    AudEvent event, {
    AudTimestamp at = const AudTimestamp.immediate(),
    int id = 0,
  }) {
    final native = calloc<core.AudEvent>();
    final time = calloc<core.AudTimestamp>();
    try {
      event.writeTo(native);
      at.writeTo(time);
      AudGraphException.check(
        bindings.aud_graph_send_event(
          _pointer,
          node.handle,
          native,
          at.isImmediate ? nullptr : time,
          id,
        ),
        'send an event to ${node.name}',
      );
    } finally {
      calloc.free(native);
      calloc.free(time);
    }
  }

  /// Cancels waiting events: those with [id], or all of [node] when [id] is
  /// 0, or every waiting event without both.
  void cancel({AudNode? node, int id = 0}) => AudGraphException.check(
    bindings.aud_graph_cancel(_pointer, node?.handle ?? 0, id),
    'cancel events',
  );

  /// Applies the string setting [key] (its key or its id) to [node] on the
  /// calling thread; may block while the node loads.
  void setString(AudNode node, Object key, String value) {
    final index = key is int ? key : node.stringKey(key as String);
    final native = value.toNativeUtf8();
    try {
      AudGraphException.check(
        bindings.aud_graph_set_string(
          _pointer,
          node.handle,
          index,
          native.cast(),
        ),
        'set string $index of ${node.name}',
      );
    } finally {
      calloc.free(native);
    }
    for (final descriptor in node.descriptor.stringKeys) {
      if (descriptor.key == index) {
        _strings[node.handle]?[descriptor.id] = value;
      }
    }
  }

  /// Sends [request] to the transport.
  void transport(AudTransportRequest request) {
    final native = calloc<core.AudTransportRequest>();
    try {
      request.writeTo(native);
      AudGraphException.check(
        bindings.aud_graph_transport(_pointer, native),
        'send a transport request',
      );
    } finally {
      calloc.free(native);
    }
  }

  /// The transport as the realtime thread published it last.
  AudTransportState get transportState {
    final native = calloc<bindings.AudGraphTransportState>();
    try {
      native.ref.struct_size = sizeOf<bindings.AudGraphTransportState>();
      AudGraphException.check(
        bindings.aud_graph_transport_state(_pointer, native),
        'read the transport',
      );
      return AudTransportState.fromNative(native.ref);
    } finally {
      calloc.free(native);
    }
  }

  /// Sends a typed [command] of `aud_audio_core` (osc-001); node handles in
  /// it name the nodes of this graph.
  void send(AudCommand command) {
    switch (command) {
      case AudSetParamCommand():
        setParam(
          _nodeOf(command.node),
          command.paramIndex,
          command.value,
          rampFrames: command.rampFrames,
          at: command.at,
        );
      case AudEventCommand():
        sendEvent(
          _nodeOf(command.node),
          command.event,
          at: command.at,
          id: command.id,
        );
      case AudCancelCommand():
        cancel(
          node: command.node == null ? null : _nodeOf(command.node!),
          id: command.id ?? 0,
        );
      case AudSetStringCommand():
        setString(_nodeOf(command.node), command.key, command.value);
      case AudTransportCommand():
        transport(command.request);
    }
  }

  /// Converts an OSC [message] through [adapter] and sends the commands;
  /// returns them. Throws an `AudOscException` for a message that fits no
  /// target.
  List<AudCommand> handleOsc(
    AudOscMessage message, {
    AudOscTimetag timetag = AudOscTimetag.immediate,
  }) {
    final commands = adapter.convert(message, timetag: timetag);
    commands.forEach(send);
    return commands;
  }

  /// Converts an OSC [bundle] through [adapter] and sends the commands.
  List<AudCommand> handleOscBundle(AudOscBundle bundle) {
    final commands = adapter.convertBundle(bundle);
    commands.forEach(send);
    return commands;
  }

  // ...........................................................................
  // Lifecycle (lifecycle-001)

  /// The lifecycle state.
  AudGraphState get state => isDisposed
      ? AudGraphState.disposed
      : AudGraphState.fromCode(bindings.aud_graph_state(_pointer));

  /// Prepares every instance for a new [sampleRate] or [maxFrames] (null
  /// keeps the current value); not while running.
  void prepare({double? sampleRate, int? maxFrames}) => AudGraphException.check(
    bindings.aud_graph_prepare(_pointer, sampleRate ?? 0, maxFrames ?? 0),
    'prepare',
  );

  /// Starts rendering.
  void start() =>
      AudGraphException.check(bindings.aud_graph_start(_pointer), 'start');

  /// Suspends rendering: blocks render silence, the transport and the
  /// pending events stay.
  void suspend() =>
      AudGraphException.check(bindings.aud_graph_suspend(_pointer), 'suspend');

  /// Resumes rendering after a suspension.
  void resume() =>
      AudGraphException.check(bindings.aud_graph_resume(_pointer), 'resume');

  /// Stops rendering: running notes are closed, the transport stops.
  void stop() =>
      AudGraphException.check(bindings.aud_graph_stop(_pointer), 'stop');

  /// Destroys the graph and every instance; the stream has to be stopped.
  void dispose() {
    if (isDisposed) return;
    if (_listener != null) {
      bindings.aud_graph_set_listener(_pointer, nullptr, nullptr);
    }
    bindings.aud_graph_destroy(_pointer);
    _pointer = nullptr;
    _listener?.close();
    _listener = null;
    calloc.free(_buffer);
    for (final (_, completer) in _waiting) {
      completer.complete();
    }
    _waiting.clear();
    unawaited(_notifications.close());
  }

  // ...........................................................................
  // Notifications

  /// The notifications of the realtime thread, as [pump] takes them.
  Stream<AudGraphNotification> get notifications => _notifications.stream;

  /// Takes the waiting notifications, completes the futures of [adopted]
  /// and adds them to [notifications]; returns their number. The listener
  /// calls this when the realtime thread wakes it; tests call it after
  /// rendering offline.
  int pump() {
    if (isDisposed) return 0;
    var total = 0;
    while (true) {
      final count = AudGraphException.check(
        bindings.aud_graph_take_notifications(_pointer, _buffer, _batch),
        'take notifications',
      );
      for (var i = 0; i < count; i++) {
        _dispatch(AudGraphNotification.fromNative(_buffer[i]));
      }
      total += count;
      if (count < _batch) return total;
    }
  }

  // ...........................................................................
  // Taps and counters

  /// The most recent [frames] frames of [channel] of the tap node [tap];
  /// frames the tap has not seen yet are 0.
  Float32List readTap(AudNode tap, {int channel = 0, required int frames}) {
    final native = calloc<Float>(frames + 1);
    try {
      AudGraphException.check(
        bindings.aud_graph_tap_read(
          _pointer,
          tap.handle,
          channel,
          native,
          frames,
        ),
        'read tap ${tap.name}',
      );
      return Float32List.fromList(native.asTypedList(frames));
    } finally {
      calloc.free(native);
    }
  }

  /// The peak and root mean square of the last block [tap] saw on
  /// [channel].
  ({double peak, double rms}) tapMeter(AudNode tap, {int channel = 0}) {
    final values = calloc<Float>(2);
    try {
      AudGraphException.check(
        bindings.aud_graph_tap_meter(
          _pointer,
          tap.handle,
          channel,
          values,
          values + 1,
        ),
        'read the meter of ${tap.name}',
      );
      return (peak: values[0], rms: values[1]);
    } finally {
      calloc.free(values);
    }
  }

  /// A snapshot of the counters.
  AudGraphStats get stats {
    final native = calloc<bindings.AudGraphStats>();
    try {
      native.ref.struct_size = sizeOf<bindings.AudGraphStats>();
      AudGraphException.check(
        bindings.aud_graph_get_stats(_pointer, native),
        'read the counters',
      );
      return AudGraphStats.fromNative(native.ref);
    } finally {
      calloc.free(native);
    }
  }

  /// Zeroes the counters.
  void resetStats() => bindings.aud_graph_reset_stats(_pointer);

  /// The sample position of the next block.
  int get samplePosition => bindings.aud_graph_sample_position(_pointer);

  // ...........................................................................
  // Documents

  /// Builds the nodes, connections and transport settings of [document]
  /// in one transaction and returns the nodes by their ids. The buses of
  /// the document have to match the graph's.
  Map<String, AudNode> load(AudGraphDocument document) {
    if (document.inputChannels.join(',') != inputChannels.join(',') ||
        document.outputChannels.join(',') != outputChannels.join(',')) {
      throw ArgumentError.value(
        document,
        'document',
        'The buses of the document do not match the graph',
      );
    }
    final problems = document.validate();
    if (problems.isNotEmpty) {
      throw ArgumentError.value(document, 'document', problems.join('; '));
    }
    final nodes = <String, AudNode>{};
    for (final entry in document.nodes) {
      nodes[entry.id] = createNode(
        entry.typeId,
        name: entry.id,
        inputChannels: entry.inputChannels,
        outputChannels: entry.outputChannels,
        delayFrames: entry.delayFrames,
        preset: entry.preset,
      );
    }
    AudNode of(String name) =>
        name == AudGraphDocument.graphId ? io : nodes[name]!;
    transaction((tx) {
      for (final c in document.connections) {
        tx.connect(
          of(c.from),
          of(c.to),
          fromBus: c.fromBus,
          toBus: c.toBus,
          lowLatency: c.lowLatency,
        );
      }
      for (final c in document.eventConnections) {
        tx.connectEvents(
          of(c.from),
          of(c.to),
          fromPort: c.fromPort,
          toPort: c.toPort,
        );
      }
    });
    final t = document.transport;
    transport(AudTransportRequest.setTempo(t.tempo));
    transport(
      AudTransportRequest.setTimeSignature(
        numerator: t.numerator,
        denominator: t.denominator,
      ),
    );
    if (t.looping) {
      transport(
        AudTransportRequest.setLoop(start: t.loopStart!, end: t.loopEnd!),
      );
    }
    return nodes;
  }

  /// The graph as a document: the live nodes with the parameters and
  /// strings set from Dart, the published connections and the transport.
  AudGraphDocument toDocument({String name = ''}) {
    String nameOf(int handle) =>
        handle == io.handle ? AudGraphDocument.graphId : _nodes[handle]!.name;
    final t = transportState;
    return AudGraphDocument(
      name: name,
      inputChannels: inputChannels,
      outputChannels: outputChannels,
      nodes: [
        for (final node in _nodes.values)
          AudGraphDocumentNode(
            id: node.name,
            typeId: node.typeId,
            inputChannels: node.inputChannels,
            outputChannels: node.outputChannels,
            delayFrames: _delays[node.handle] ?? 0,
            preset: AudNodePreset(
              typeId: node.typeId,
              nodeVersion: node.descriptor.version,
              params: paramsOf(node),
              strings: stringsOf(node),
            ),
          ),
      ],
      connections: [
        for (final c in _connections)
          if (_nodes.containsKey(c.from) || c.from == io.handle)
            if (_nodes.containsKey(c.to) || c.to == io.handle)
              AudGraphConnection(
                from: nameOf(c.from),
                fromBus: c.fromBus,
                to: nameOf(c.to),
                toBus: c.toBus,
                lowLatency: c.lowLatency,
              ),
      ],
      eventConnections: [
        for (final c in _eventConnections)
          if (_nodes.containsKey(c.from) || c.from == io.handle)
            if (_nodes.containsKey(c.to) || c.to == io.handle)
              AudGraphEventConnection(
                from: nameOf(c.from),
                fromPort: c.fromPort,
                to: nameOf(c.to),
                toPort: c.toPort,
              ),
      ],
      transport: AudGraphTransportSettings(
        tempo: t.tempo,
        numerator: t.numerator,
        denominator: t.denominator,
        loopStart: t.looping ? t.loopStart : null,
        loopEnd: t.looping ? t.loopEnd : null,
      ),
    );
  }

  // ...........................................................................
  static const int _batch = 64;

  late Pointer<bindings.AudGraph> _pointer;
  late final Pointer<bindings.AudGraphNotification> _buffer;
  NativeCallable<Void Function(Pointer<Void>)>? _listener;
  final Map<int, AudNode> _nodes = {};
  final Map<int, Map<String, double>> _params = {};
  final Map<int, Map<String, String>> _strings = {};
  final Map<int, int> _delays = {};
  final List<AudConnection> _connections = [];
  final List<AudEventConnection> _eventConnections = [];
  final List<(int, Completer<void>)> _waiting = [];
  // Synchronous, so that a pump() delivers before it returns.
  final StreamController<AudGraphNotification> _notifications =
      StreamController.broadcast(sync: true);

  void _onWake(Pointer<Void> _) => pump();

  AudNode _nodeOf(int handle) {
    final node = handle == io.handle ? io : _nodes[handle];
    if (node == null) throw ArgumentError.value(handle, 'node', 'Unknown node');
    return node;
  }

  // Replays the committed edits in their order on the shadow topology.
  void _record(AudGraphTransaction transaction) {
    bool sameEnds(AudConnection a, AudConnection b) =>
        a.from == b.from &&
        a.fromBus == b.fromBus &&
        a.to == b.to &&
        a.toBus == b.toBus;
    for (final edit in transaction.edits) {
      switch (edit.kind) {
        case AudEditKind.connect:
          final c = edit.what as AudConnection;
          _connections.removeWhere((e) => sameEnds(e, c));
          _connections.add(c);
        case AudEditKind.disconnect:
          final c = edit.what as AudConnection;
          _connections.removeWhere((e) => sameEnds(e, c));
        case AudEditKind.connectEvents:
          final c = edit.what as AudEventConnection;
          if (!_eventConnections.contains(c)) _eventConnections.add(c);
        case AudEditKind.disconnectEvents:
          _eventConnections.remove(edit.what as AudEventConnection);
        case AudEditKind.remove:
          final node = edit.what as AudNode;
          _nodes.remove(node.handle);
          _params.remove(node.handle);
          _strings.remove(node.handle);
          _delays.remove(node.handle);
          router.unregisterNode(graph: id, node: node.handle);
          _connections.removeWhere(
            (c) => c.from == node.handle || c.to == node.handle,
          );
          _eventConnections.removeWhere(
            (c) => c.from == node.handle || c.to == node.handle,
          );
      }
    }
  }

  void _dispatch(AudGraphNotification notification) {
    if (notification is AudRevisionAdoptedNotification) {
      _waiting.removeWhere((entry) {
        if (entry.$1 > notification.revision) return false;
        entry.$2.complete();
        return true;
      });
    }
    _notifications.add(notification);
  }

  AudNodeDescriptor _ioDescriptor() => AudNodeDescriptor(
    typeId: 'aud.graph.io',
    name: 'Graph',
    vendor: 'Audanika',
    inputBuses: [
      for (var b = 0; b < outputChannels.length; b++)
        AudBusDescriptor(
          id: 'out$b',
          name: 'Output ${b + 1}',
          minChannels: outputChannels[b],
          maxChannels: outputChannels[b],
          defaultChannels: outputChannels[b],
        ),
    ],
    outputBuses: [
      for (var b = 0; b < inputChannels.length; b++)
        AudBusDescriptor(
          id: 'in$b',
          name: 'Input ${b + 1}',
          minChannels: inputChannels[b],
          maxChannels: inputChannels[b],
          defaultChannels: inputChannels[b],
        ),
    ],
    eventInputs: const [
      AudEventPortDescriptor(
        id: 'events',
        name: 'Events',
        midi: true,
        control: true,
      ),
    ],
    eventOutputs: const [
      AudEventPortDescriptor(
        id: 'events',
        name: 'Events',
        midi: true,
        control: true,
      ),
    ],
  );
}
