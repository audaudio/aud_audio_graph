// @license
// Copyright (c) Audanika. All Rights Reserved.
//
// Use of this source code is governed by terms that can be
// found in the LICENSE file in the root of this package.

import 'dart:async';
import 'dart:ffi';
import 'dart:io';
import 'dart:typed_data';

import 'package:aud_audio_core/aud_audio_core_ffi.dart';
import 'package:aud_audio_core/aud_audio_core_bindings.dart' as core;
import 'package:ffi/ffi.dart';

import 'aud_audio_graph_bindings_generated.dart' as bindings;
import 'aud_cache_usage.dart';
import 'aud_event_source.dart';
import 'aud_event_source_ffi.dart';
import 'aud_graph.dart';
import 'aud_graph_document.dart';
import 'aud_graph_exception.dart';
import 'aud_graph_node.dart';
import 'aud_graph_notification.dart';
import 'aud_graph_options.dart';
import 'aud_graph_state.dart';
import 'aud_graph_stats.dart';
import 'aud_graph_transaction.dart';
import 'aud_graph_native_conversions.dart';
import 'aud_graph_timing.dart';
import 'aud_graph_transport_state.dart';
import 'aud_load.dart';
import 'aud_node_message.dart';

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
class AudGraphFfi implements AudGraph {
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
  AudGraphFfi({
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
    _messageBuffer = calloc<bindings.AudGraphMessage>(_batch);
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

  /// A graph built from [document]: its buses, assets, nodes, connections
  /// and transport settings; see [load].
  factory AudGraphFfi.fromDocument(
    AudGraphDocument document, {
    double sampleRate = 48000,
    int maxFrames = 1024,
    AudGraphOptions options = const AudGraphOptions(),
    int id = 1,
    bool listen = true,
    String? baseDirectory,
  }) {
    final graph = AudGraphFfi(
      sampleRate: sampleRate,
      maxFrames: maxFrames,
      inputChannels: document.inputChannels,
      outputChannels: document.outputChannels,
      options: options,
      id: id,
      listen: listen,
    );
    try {
      graph.load(document, baseDirectory: baseDirectory);
    } catch (_) {
      graph.dispose();
      rethrow;
    }
    return graph;
  }

  // ...........................................................................
  /// The graph id in OSC addresses.
  @override
  final int id;

  /// The channels of each input bus of the graph.
  @override
  final List<int> inputChannels;

  /// The channels of each output bus of the graph.
  @override
  final List<int> outputChannels;

  /// The graph itself as a node: the handle 0.
  @override
  late final AudNode io;

  /// The OSC router the nodes of the graph are registered with.
  @override
  final AudOscRouter router = AudOscRouter();

  /// The OSC adapter over [router]; [handleOsc] uses it.
  @override
  late final AudOscAdapter adapter;

  /// The native graph.
  Pointer<bindings.AudGraph> get pointer => _pointer;

  /// The `AudHostApi` DSP packages register their node types and
  /// transport providers with.
  Pointer<core.AudHostApi> get hostApi => bindings.aud_graph_host_api(_pointer);

  /// The sample rate in Hz.
  @override
  double get sampleRate => bindings.aud_graph_sample_rate(_pointer);

  /// The largest block a render receives.
  @override
  int get maxFrames => bindings.aud_graph_max_frames(_pointer);

  /// Whether [dispose] ran.
  @override
  bool get isDisposed => _pointer == nullptr;

  // ...........................................................................
  // Node types and nodes

  /// The registered node types.
  @override
  List<AudNodeDescriptor> get nodeTypes => [
    for (var i = 0; i < bindings.aud_graph_num_node_types(_pointer); i++)
      bindings.aud_graph_node_type(_pointer, i).ref.toDart(),
  ];

  /// The node type [typeId], or null.
  @override
  AudNodeDescriptor? nodeType(String typeId) {
    final native = typeId.toNativeUtf8();
    try {
      final descriptor = bindings.aud_graph_node_type_by_id(
        _pointer,
        native.cast(),
      );
      return descriptor == nullptr ? null : descriptor.ref.toDart();
    } finally {
      calloc.free(native);
    }
  }

  /// The live nodes in creation order.
  @override
  List<AudNode> get nodes => List.unmodifiable(_nodes.values);

  /// The live node with [handle], or null.
  @override
  AudNode? node(int handle) => _nodes[handle];

  /// The live node named [name], or null; [io] for the graph's own name.
  @override
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
  @override
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
    final descriptor = bindings
        .aud_graph_node_descriptor(_pointer, handle)
        .ref
        .toDart();
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

  /// Applies [preset] to [node] in the order of the headless host:
  /// strings, state, parameters; the strings in the order of the node's
  /// string keys, since a key may depend on one before it (the sampler's
  /// text on its virtual path). A string `asset:<id>` reaches the node as
  /// the path of the asset (see [addAsset]); the state blob loads through
  /// [loadState]. Throws an [ArgumentError] when the preset does not fit the
  /// node's type or names an unknown asset.
  @override
  void applyPreset(AudNode node, AudNodePreset preset) {
    final problems = preset.validate(node.descriptor);
    for (final value in preset.strings.values) {
      final asset = AudGraphAsset.idOf(value);
      if (asset != null && !_assets.containsKey(asset)) {
        problems.add('unknown asset $asset');
      }
    }
    if (problems.isNotEmpty) {
      throw ArgumentError.value(preset, 'preset', problems.join('; '));
    }
    for (final key in node.descriptor.stringKeys) {
      final value = preset.strings[key.id];
      if (value != null) setString(node, key.id, value);
    }
    if (preset.state != null) {
      loadState(node, preset.state!, version: preset.stateVersion);
    }
    for (final entry in preset.params.entries) {
      setParam(node, entry.key, entry.value);
    }
  }

  // ...........................................................................
  // State and assets (plugin-002)

  /// The state blob of [node], whose type saves its state; throws an
  /// [AudGraphException] otherwise. While the graph runs, the node is
  /// parked for the call: a block rendered meanwhile skips it.
  @override
  Uint8List saveState(AudNode node) {
    final size = calloc<Size>();
    Pointer<Uint8> buffer = nullptr;
    try {
      final probe = bindings.aud_graph_node_save_state(
        _pointer,
        node.handle,
        nullptr,
        0,
        size,
      );
      if (probe != AUD_ERROR_BUFFER_TOO_SMALL) {
        AudGraphException.check(probe, 'save the state of ${node.name}');
      }
      buffer = calloc<Uint8>(size.value + 1);
      AudGraphException.check(
        bindings.aud_graph_node_save_state(
          _pointer,
          node.handle,
          buffer.cast(),
          size.value + 1,
          size,
        ),
        'save the state of ${node.name}',
      );
      return Uint8List.fromList(buffer.asTypedList(size.value));
    } finally {
      calloc.free(size);
      if (buffer != nullptr) calloc.free(buffer);
    }
  }

  /// Restores the state blob [data] of [node], written by [version] of the
  /// node's state format - its current one by default; parked like
  /// [saveState].
  @override
  void loadState(AudNode node, Uint8List data, {int? version}) {
    final native = calloc<Uint8>(data.length + 1);
    try {
      native.asTypedList(data.length).setAll(0, data);
      AudGraphException.check(
        bindings.aud_graph_node_load_state(
          _pointer,
          node.handle,
          native.cast(),
          data.length,
          version ?? node.descriptor.stateVersion,
        ),
        'load the state of ${node.name}',
      );
    } finally {
      calloc.free(native);
    }
  }

  /// The files the nodes reference, as [addAsset] added them.
  @override
  List<AudGraphAsset> get assets =>
      List.unmodifiable([for (final entry in _assets.values) entry.asset]);

  /// The path of the asset [id] on disk, or null for an unknown id.
  @override
  String? assetPath(String id) => _assets[id]?.resolved;

  /// Adds [asset]: its path, resolved against [baseDirectory] (or the
  /// working directory), must name an existing file; throws an
  /// [ArgumentError] otherwise. String settings name it `asset:<id>`.
  @override
  void addAsset(AudGraphAsset asset, {String? baseDirectory}) {
    final resolved = File(asset.path).isAbsolute || baseDirectory == null
        ? asset.path
        : '$baseDirectory${Platform.pathSeparator}${asset.path}';
    if (!File(resolved).existsSync()) {
      throw ArgumentError.value(
        asset.path,
        'asset',
        'Asset ${asset.id} not found: $resolved',
      );
    }
    _assets[asset.id] = (asset: asset, resolved: resolved);
  }

  /// The parameters set from Dart for [node], by id.
  @override
  Map<String, double> paramsOf(AudNode node) =>
      Map.unmodifiable(_params[node.handle] ?? const {});

  /// The string settings set from Dart for [node], by id.
  @override
  Map<String, String> stringsOf(AudNode node) =>
      Map.unmodifiable(_strings[node.handle] ?? const {});

  /// The audio connections of the published topology.
  @override
  List<AudConnection> get connections => List.unmodifiable(_connections);

  /// The event connections of the published topology.
  @override
  List<AudEventConnection> get eventConnections =>
      List.unmodifiable(_eventConnections);

  // ...........................................................................
  // Transactions (graph-003)

  /// Runs [edits] inside a transaction and commits them as one revision;
  /// returns the revision. When an edit or the commit fails the
  /// transaction is rolled back and the error rethrown.
  @override
  int transaction(void Function(AudGraphTransaction transaction) edits) {
    AudGraphException.check(bindings.aud_graph_begin(_pointer), 'begin');
    final transaction = AudGraphTransaction(this, _FfiEdits(this));
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
  @override
  Future<void> adopted(int revision) {
    if (this.revision >= revision) return Future.value();
    final completer = Completer<void>();
    _waiting.add((revision, completer));
    return completer.future;
  }

  /// The revision the realtime thread adopted last.
  @override
  int get revision => bindings.aud_graph_revision(_pointer);

  /// The latency of the published program from the graph inputs to the
  /// graph outputs, in frames.
  @override
  int get outputLatency => bindings.aud_graph_output_latency(_pointer);

  /// The tail of the published program in frames: the longest node tail
  /// plus its path to the outputs, [AudGraph.infiniteTail] when a node never ends.
  @override
  int get outputTail => bindings.aud_graph_output_tail(_pointer);

  /// The latency of [node] in frames.
  @override
  int latencyOf(AudNode node) => AudGraphException.check(
    bindings.aud_graph_node_latency(_pointer, node.handle),
    'read the latency of ${node.name}',
  );

  /// The frames by which scheduled events of [node] are pre-delivered so
  /// that they are heard at their time (graph-001).
  @override
  int leadOf(AudNode node) => AudGraphException.check(
    bindings.aud_graph_node_lead(_pointer, node.handle),
    'read the lead of ${node.name}',
  );

  // ...........................................................................
  // Commands (interop-002)

  /// Sets a parameter of [node]: [param] is its index or its id. An
  /// immediate [at] takes the parameter queue, any other time travels as a
  /// parameter event; [rampFrames] ramps the change.
  @override
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
  @override
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
  @override
  void cancel({AudNode? node, int id = 0}) => AudGraphException.check(
    bindings.aud_graph_cancel(_pointer, node?.handle ?? 0, id),
    'cancel events',
  );

  // ...........................................................................
  // Event sources (ticket 26)

  /// Creates an event source for [node]; [AudGraph.createEventSource] names
  /// the arguments and what throws. The source is an [AudEventSourceFfi]
  /// with the addresses a native producer needs.
  @override
  AudEventSourceFfi createEventSource(
    AudNode node, {
    int port = 0,
    AudSourceTiming timing = AudSourceTiming.fixed,
    int latencyNs = 0,
    int capacity = 0,
  }) {
    final config = calloc<bindings.AudEventSourceConfig>();
    final result = calloc<Int32>();
    try {
      config.ref
        ..struct_size = sizeOf<bindings.AudEventSourceConfig>()
        ..capacity = capacity
        ..node = node.handle
        ..port = port
        ..timing = timing.code
        ..latency_ns = latencyNs;
      final pointer = bindings.aud_graph_create_event_source(
        _pointer,
        config,
        result,
      );
      if (pointer == nullptr) {
        AudGraphException.check(
          result.value,
          'create an event source for ${node.name}',
        );
      }
      final source = AudEventSourceFfi(
        graph: _pointer,
        pointer: pointer,
        node: node,
        port: port,
        timing: timing,
        latencyNs: latencyNs,
        onDispose: _sources.remove,
      );
      _sources.add(source);
      return source;
    } finally {
      calloc.free(config);
      calloc.free(result);
    }
  }

  /// The event sources that are not disposed.
  @override
  List<AudEventSource> get eventSources => List.unmodifiable(_sources);

  /// The timing of the engine as the last block saw it; `valid` is false
  /// before a block rendered.
  @override
  AudGraphTiming get timing {
    final native = calloc<bindings.AudGraphTiming>();
    try {
      native.ref.struct_size = sizeOf<bindings.AudGraphTiming>();
      final result = bindings.aud_graph_timing(_pointer, native);
      if (result != AUD_ERROR_STATE) {
        AudGraphException.check(result, 'read the timing');
      }
      return native.ref.toDart();
    } finally {
      calloc.free(native);
    }
  }

  /// Applies the string setting [key] (its key or its id) to [node] on the
  /// calling thread. A value `asset:<id>` reaches the node as the path of
  /// the asset; an unknown asset throws an [ArgumentError]. Returns the id
  /// of the load the node started on a worker thread, or 0.
  @override
  int setString(AudNode node, Object key, String value) {
    final index = key is int ? key : node.stringKey(key as String);
    final asset = AudGraphAsset.idOf(value);
    final resolved = asset == null
        ? value
        : _assets[asset]?.resolved ??
              (throw ArgumentError.value(value, 'value', 'Unknown asset'));
    final native = resolved.toNativeUtf8();
    final int load;
    try {
      load = AudGraphException.check(
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
    return load;
  }

  // ...........................................................................
  // Loads (ticket 25)

  /// The loads the graph keeps: those that run and the last 64 that ended,
  /// the oldest first.
  @override
  List<AudLoad> get loads {
    final count = AudGraphException.check(
      bindings.aud_graph_loads(_pointer, nullptr, 0),
      'count the loads',
    );
    final ids = calloc<Int32>(count + 1);
    try {
      final taken = bindings.aud_graph_loads(_pointer, ids, count);
      return [for (var i = 0; i < taken && i < count; i++) ?loadOf(ids[i])];
    } finally {
      calloc.free(ids);
    }
  }

  /// The load [id], or null when the graph keeps it no longer.
  @override
  AudLoad? loadOf(int id) {
    final info = calloc<bindings.AudLoadInfo>();
    try {
      info.ref.struct_size = sizeOf<bindings.AudLoadInfo>();
      final result = bindings.aud_graph_load_info(_pointer, id, info);
      if (result == AUD_ERROR_NOT_FOUND) return null;
      AudGraphException.check(result, 'read load $id');
      return info.ref.toDart();
    } finally {
      calloc.free(info);
    }
  }

  /// Asks the running load [id] to stop; false when it ended or is unknown.
  @override
  bool cancelLoad(int id) =>
      bindings.aud_graph_cancel_load(_pointer, id) == AUD_OK;

  /// Completes with the load [id] once it has ended.
  @override
  Future<AudLoad> loadEnded(int id) {
    final current = loadOf(id);
    if (current == null) {
      throw ArgumentError.value(id, 'id', 'The graph keeps no such load');
    }
    if (!current.isRunning) return Future.value(current);
    final completer = Completer<AudLoad>();
    _loadWaiters.putIfAbsent(id, () => []).add(completer);
    return completer.future;
  }

  /// Waits until no load of [node] (null: of any node) runs; false after
  /// [timeout], rounded up to milliseconds; a negative one throws an
  /// [ArgumentError]. Blocks the calling isolate.
  @override
  bool waitLoads({AudNode? node, Duration timeout = Duration.zero}) =>
      bindings.aud_graph_wait_loads(
        _pointer,
        node?.handle ?? 0,
        timeout.timeoutMs,
      ) ==
      AUD_OK;

  // ...........................................................................
  // Messages of nodes (ticket 25)

  /// Sends [message] to [node] at [at]; the answers arrive on [messages].
  /// [AudGraph.sendMessage] names the limits of a message and what throws.
  @override
  void sendMessage(
    AudNode node,
    AudOscMessage message, {
    AudTimestamp at = const AudTimestamp.immediate(),
    int id = 0,
  }) => using((arena) {
    final time = arena<core.AudTimestamp>();
    at.writeTo(time);
    AudGraphException.check(
      bindings.aud_graph_send_message(
        _pointer,
        node.handle,
        message.writeNative(arena),
        at.isImmediate ? nullptr : time,
        id,
      ),
      'send ${message.address} to ${node.name}',
    );
  });

  /// The messages the nodes sent, as [pump] takes them.
  @override
  Stream<AudNodeMessage> get messages => _messages.stream;

  // ...........................................................................
  // The resources of the process (ticket 25)

  /// The sample cache budget of the process in bytes.
  @override
  int get cacheBudget => bindings.aud_cache_budget();

  @override
  set cacheBudget(int bytes) {
    if (bytes < 0) throw ArgumentError.value(bytes, 'bytes', 'Negative');
    bindings.aud_cache_set_budget(bytes);
  }

  /// The caches, the budget and the worker threads of the process.
  @override
  AudCacheUsage get cacheUsage {
    final usage = calloc<bindings.AudCacheUsage>();
    try {
      usage.ref.struct_size = sizeOf<bindings.AudCacheUsage>();
      AudGraphException.check(
        bindings.aud_cache_usage(usage),
        'read the cache usage',
      );
      return usage.ref.toDart();
    } finally {
      calloc.free(usage);
    }
  }

  /// Waits until no task of the worker threads waits or runs; [timeout]
  /// counts like the timeout of [waitLoads].
  @override
  bool waitTasks({Duration timeout = Duration.zero}) =>
      bindings.aud_tasks_wait_idle(timeout.timeoutMs) == AUD_OK;

  /// Sends [request] to the transport.
  @override
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
  @override
  AudTransportState get transportState {
    final native = calloc<bindings.AudGraphTransportState>();
    try {
      native.ref.struct_size = sizeOf<bindings.AudGraphTransportState>();
      AudGraphException.check(
        bindings.aud_graph_transport_state(_pointer, native),
        'read the transport',
      );
      return native.ref.toDart();
    } finally {
      calloc.free(native);
    }
  }

  /// Sends a typed [command] of `aud_audio_core` (osc-001); node handles in
  /// it name the nodes of this graph.
  @override
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
      case AudMessageCommand():
        sendMessage(
          _nodeOf(command.node),
          command.message,
          at: command.at,
          id: command.id,
        );
    }
  }

  /// Converts an OSC [message] through [adapter] and sends the commands;
  /// returns them. Throws an `AudOscException` for a message that fits no
  /// target.
  @override
  List<AudCommand> handleOsc(
    AudOscMessage message, {
    AudOscTimetag timetag = AudOscTimetag.immediate,
  }) {
    final commands = adapter.convert(message, timetag: timetag);
    commands.forEach(send);
    return commands;
  }

  /// Converts an OSC [bundle] through [adapter] and sends the commands.
  @override
  List<AudCommand> handleOscBundle(AudOscBundle bundle) {
    final commands = adapter.convertBundle(bundle);
    commands.forEach(send);
    return commands;
  }

  // ...........................................................................
  // Lifecycle (lifecycle-001)

  /// The lifecycle state.
  @override
  AudGraphState get state => isDisposed
      ? AudGraphState.disposed
      : AudGraphState.fromCode(bindings.aud_graph_state(_pointer));

  /// Prepares every instance for a new [sampleRate] or [maxFrames] (null
  /// keeps the current value); not while running.
  @override
  void prepare({double? sampleRate, int? maxFrames}) => AudGraphException.check(
    bindings.aud_graph_prepare(_pointer, sampleRate ?? 0, maxFrames ?? 0),
    'prepare',
  );

  /// Starts rendering.
  @override
  void start() =>
      AudGraphException.check(bindings.aud_graph_start(_pointer), 'start');

  /// Suspends rendering: blocks render silence, the transport and the
  /// pending events stay.
  @override
  void suspend() =>
      AudGraphException.check(bindings.aud_graph_suspend(_pointer), 'suspend');

  /// Resumes rendering after a suspension.
  @override
  void resume() =>
      AudGraphException.check(bindings.aud_graph_resume(_pointer), 'resume');

  /// Stops rendering: running notes are closed, the transport stops.
  @override
  void stop() =>
      AudGraphException.check(bindings.aud_graph_stop(_pointer), 'stop');

  /// Destroys the graph and every instance; the stream has to be stopped.
  @override
  void dispose() {
    if (isDisposed) return;
    if (_listener != null) {
      bindings.aud_graph_set_listener(_pointer, nullptr, nullptr);
    }
    // The engine frees the sources with the graph.
    for (final source in _sources) {
      source.markDisposed();
    }
    _sources.clear();
    bindings.aud_graph_destroy(_pointer);
    _pointer = nullptr;
    _listener?.close();
    _listener = null;
    calloc.free(_buffer);
    calloc.free(_messageBuffer);
    for (final (_, completer) in _waiting) {
      completer.complete();
    }
    _waiting.clear();
    // The loads of the nodes ended with them.
    for (final MapEntry(key: id, value: completers) in _loadWaiters.entries) {
      for (final completer in completers) {
        completer.complete(
          AudLoad(
            id: id,
            node: 0,
            result: AUD_ERROR_CANCELLED,
            message: 'The graph was disposed',
          ),
        );
      }
    }
    _loadWaiters.clear();
    unawaited(_notifications.close());
    unawaited(_messages.close());
  }

  // ...........................................................................
  // Notifications

  /// The notifications of the engine - of the realtime thread and of the
  /// loads -, as [pump] takes them.
  @override
  Stream<AudGraphNotification> get notifications => _notifications.stream;

  /// Takes the waiting notifications, completes the futures of [adopted]
  /// and of [loadEnded] and adds them to [notifications], takes the
  /// messages of the nodes for [messages]; returns the number of
  /// notifications. The listener calls this when the engine wakes it;
  /// tests call it after rendering offline.
  @override
  int pump() {
    if (isDisposed) return 0;
    var total = 0;
    while (true) {
      final count = AudGraphException.check(
        bindings.aud_graph_take_notifications(_pointer, _buffer, _batch),
        'take notifications',
      );
      for (var i = 0; i < count; i++) {
        _dispatch(_buffer[i].toDart());
      }
      total += count;
      if (count < _batch) break;
    }
    // The messages of the nodes (ticket 25).
    while (true) {
      final count = AudGraphException.check(
        bindings.aud_graph_take_messages(_pointer, _messageBuffer, _batch),
        'take messages',
      );
      for (var i = 0; i < count; i++) {
        _messages.add(_messageBuffer[i].toDart());
      }
      if (count < _batch) return total;
    }
  }

  // ...........................................................................
  // Taps and counters

  /// The most recent [frames] frames of [channel] of the tap node [tap];
  /// frames the tap has not seen yet are 0.
  @override
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
  @override
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
  @override
  AudGraphStats get stats {
    final native = calloc<bindings.AudGraphStats>();
    try {
      native.ref.struct_size = sizeOf<bindings.AudGraphStats>();
      AudGraphException.check(
        bindings.aud_graph_get_stats(_pointer, native),
        'read the counters',
      );
      return native.ref.toDart();
    } finally {
      calloc.free(native);
    }
  }

  /// Zeroes the counters.
  @override
  void resetStats() => bindings.aud_graph_reset_stats(_pointer);

  /// Whether the native library carries the debug watchdog of the realtime
  /// thread: a build hook compiles it in when the app sets the user define
  /// `watchdog: true` of aud_audio_graph (ticket 20).
  static bool get watchdogEnabled => bindings.aud_graph_watchdog_enabled() != 0;

  /// The allocations, frees, log calls and control calls of the host api
  /// the watchdog caught on rendering threads of the process since
  /// [resetWatchdog]; 0 without the watchdog.
  static int get watchdogViolations => bindings.aud_graph_watchdog_violations();

  /// Zeroes [watchdogViolations].
  static void resetWatchdog() => bindings.aud_graph_watchdog_reset();

  /// The sample position of the next block.
  @override
  int get samplePosition => bindings.aud_graph_sample_position(_pointer);

  // ...........................................................................
  // Documents

  /// Builds the assets, nodes, connections and transport settings of
  /// [document] in one transaction and returns the nodes by their ids. The
  /// buses of the document have to match the graph's; the asset paths
  /// resolve against [baseDirectory] (see [addAsset]).
  @override
  Map<String, AudNode> load(
    AudGraphDocument document, {
    String? baseDirectory,
  }) {
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
    for (final asset in document.assets) {
      addAsset(asset, baseDirectory: baseDirectory);
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

  /// The graph as a document: the assets, the live nodes with the
  /// parameters and strings set from Dart and - with [includeState] - the
  /// state blobs of the nodes that save one, the published connections and
  /// the transport.
  @override
  AudGraphDocument toDocument({String name = '', bool includeState = true}) {
    String nameOf(int handle) =>
        handle == io.handle ? AudGraphDocument.graphId : _nodes[handle]!.name;
    final t = transportState;
    return AudGraphDocument(
      name: name,
      inputChannels: inputChannels,
      outputChannels: outputChannels,
      assets: assets,
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
              state: includeState && node.descriptor.capabilities.state
                  ? saveState(node)
                  : null,
              stateVersion: includeState && node.descriptor.capabilities.state
                  ? node.descriptor.stateVersion
                  : null,
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
  late final Pointer<bindings.AudGraphMessage> _messageBuffer;
  NativeCallable<Void Function(Pointer<Void>)>? _listener;
  final Map<int, AudNode> _nodes = {};
  final Map<int, Map<String, double>> _params = {};
  final Map<int, Map<String, String>> _strings = {};
  final Map<int, int> _delays = {};
  final Map<String, ({AudGraphAsset asset, String resolved})> _assets = {};
  final List<AudConnection> _connections = [];
  final List<AudEventConnection> _eventConnections = [];
  final List<(int, Completer<void>)> _waiting = [];
  final List<AudEventSourceFfi> _sources = [];
  // Synchronous, so that a pump() delivers before it returns.
  final StreamController<AudGraphNotification> _notifications =
      StreamController.broadcast(sync: true);
  final StreamController<AudNodeMessage> _messages = StreamController.broadcast(
    sync: true,
  );
  final Map<int, List<Completer<AudLoad>>> _loadWaiters = {};

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
    if (notification is AudLoadNotification && !notification.isRunning) {
      final completers = _loadWaiters.remove(notification.load);
      if (completers != null) {
        final ended =
            loadOf(notification.load) ??
            AudLoad(
              id: notification.load,
              node: notification.node,
              result: notification.result,
              progress: notification.progress,
            );
        for (final completer in completers) {
          completer.complete(ended);
        }
      }
    }
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

// #############################################################################
/// The edits of a transaction over the C API of [AudGraphFfi].
class _FfiEdits implements AudGraphEdits {
  _FfiEdits(this._graph);

  final AudGraphFfi _graph;

  @override
  int connect(
    int from,
    int fromBus,
    int to,
    int toBus, {
    required bool lowLatency,
  }) => bindings.aud_graph_connect(
    _graph.pointer,
    from,
    fromBus,
    to,
    toBus,
    lowLatency ? bindings.AUD_CONNECTION_LOW_LATENCY : 0,
  );

  @override
  int disconnect(int from, int fromBus, int to, int toBus) =>
      bindings.aud_graph_disconnect(_graph.pointer, from, fromBus, to, toBus);

  @override
  int connectEvents(int from, int fromPort, int to, int toPort) => bindings
      .aud_graph_connect_events(_graph.pointer, from, fromPort, to, toPort);

  @override
  int disconnectEvents(int from, int fromPort, int to, int toPort) => bindings
      .aud_graph_disconnect_events(_graph.pointer, from, fromPort, to, toPort);

  @override
  int remove(int handle) =>
      bindings.aud_graph_remove_node(_graph.pointer, handle);
}
