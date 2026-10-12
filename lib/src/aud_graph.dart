// @license
// Copyright (c) Audanika. All Rights Reserved.
//
// Use of this source code is governed by terms that can be
// found in the LICENSE file in the root of this package.

import 'dart:async';
import 'dart:typed_data';

import 'package:aud_audio_core/aud_audio_core.dart';

import 'aud_cache_usage.dart';
import 'aud_event_source.dart';
import 'aud_graph_document.dart';
import 'aud_graph_factory_stub.dart'
    if (dart.library.ffi) 'aud_graph_factory_native.dart'
    as platform;
import 'aud_graph_node.dart';
import 'aud_graph_notification.dart';
import 'aud_graph_options.dart';
import 'aud_graph_state.dart';
import 'aud_graph_stats.dart';
import 'aud_graph_timing.dart';
import 'aud_graph_transaction.dart';
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
/// register theirs with `AudGraphFfi.hostApi`. The node handle 0, [io], is the graph:
/// its input buses are the outputs of [io], its output buses the inputs.
///
/// The platform-neutral API: on native platforms the graph is an
/// `AudGraphFfi` of `aud_audio_graph_ffi.dart`; the web has no graph until
/// S5.
abstract interface class AudGraph {
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
  factory AudGraph({
    double sampleRate = 48000,
    int maxFrames = 1024,
    List<int> inputChannels = const [],
    List<int> outputChannels = const [2],
    AudGraphOptions options = const AudGraphOptions(),
    int id = 1,
    bool registerCoreNodes = true,
    bool listen = true,
  }) => platform.createGraph(
    sampleRate: sampleRate,
    maxFrames: maxFrames,
    inputChannels: inputChannels,
    outputChannels: outputChannels,
    options: options,
    id: id,
    registerCoreNodes: registerCoreNodes,
    listen: listen,
  );

  /// A graph built from [document]: its buses, assets, nodes, connections
  /// and transport settings; see [load].
  factory AudGraph.fromDocument(
    AudGraphDocument document, {
    double sampleRate = 48000,
    int maxFrames = 1024,
    AudGraphOptions options = const AudGraphOptions(),
    int id = 1,
    bool listen = true,
    String? baseDirectory,
  }) => platform.createGraphFromDocument(
    document,
    sampleRate: sampleRate,
    maxFrames: maxFrames,
    options: options,
    id: id,
    listen: listen,
    baseDirectory: baseDirectory,
  );

  /// The [outputTail] of a program with a node whose tail never ends.
  static const int infiniteTail = AUD_TAIL_INFINITE;

  // ...........................................................................
  /// The graph id in OSC addresses.
  int get id;

  /// The channels of each input bus of the graph.
  List<int> get inputChannels;

  /// The channels of each output bus of the graph.
  List<int> get outputChannels;

  /// The graph itself as a node: the handle 0.
  AudNode get io;

  /// The OSC router the nodes of the graph are registered with.
  AudOscRouter get router;

  /// The OSC adapter over [router]; [handleOsc] uses it.
  AudOscAdapter get adapter;

  /// The sample rate in Hz.
  double get sampleRate;

  /// The largest block a render receives.
  int get maxFrames;

  /// Whether [dispose] ran.
  bool get isDisposed;

  /// The registered node types.
  List<AudNodeDescriptor> get nodeTypes;

  /// The node type [typeId], or null.
  AudNodeDescriptor? nodeType(String typeId);

  /// The live nodes in creation order.
  List<AudNode> get nodes;

  /// The live node with [handle], or null.
  AudNode? node(int handle);

  /// The live node named [name], or null; [io] for the graph's own name.
  AudNode? nodeNamed(String name);

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
  });

  /// Applies [preset] to [node] in the order of the headless host:
  /// strings, state, parameters. A string `asset:<id>` reaches the node as
  /// the path of the asset (see [addAsset]); the state blob loads through
  /// [loadState]. Throws an [ArgumentError] when the preset does not fit the
  /// node's type or names an unknown asset.
  void applyPreset(AudNode node, AudNodePreset preset);

  /// The state blob of [node], whose type saves its state; throws an
  /// [AudGraphException] otherwise. While the graph runs, the node is
  /// parked for the call: a block rendered meanwhile skips it.
  Uint8List saveState(AudNode node);

  /// Restores the state blob [data] of [node], written by [version] of the
  /// node's state format - its current one by default; parked like
  /// [saveState].
  void loadState(AudNode node, Uint8List data, {int? version});

  /// The files the nodes reference, as [addAsset] added them.
  List<AudGraphAsset> get assets;

  /// The path of the asset [id] on disk, or null for an unknown id.
  String? assetPath(String id);

  /// Adds [asset]: its path, resolved against [baseDirectory] (or the
  /// working directory), must name an existing file; throws an
  /// [ArgumentError] otherwise. String settings name it `asset:<id>`.
  void addAsset(AudGraphAsset asset, {String? baseDirectory});

  /// The parameters set from Dart for [node], by id.
  Map<String, double> paramsOf(AudNode node);

  /// The string settings set from Dart for [node], by id.
  Map<String, String> stringsOf(AudNode node);

  /// The audio connections of the published topology.
  List<AudConnection> get connections;

  /// The event connections of the published topology.
  List<AudEventConnection> get eventConnections;

  /// Runs [edits] inside a transaction and commits them as one revision;
  /// returns the revision. When an edit or the commit fails the
  /// transaction is rolled back and the error rethrown.
  int transaction(void Function(AudGraphTransaction transaction) edits);

  /// Completes once the realtime thread has adopted [revision] or a later
  /// one, or when the graph is disposed.
  Future<void> adopted(int revision);

  /// The revision the realtime thread adopted last.
  int get revision;

  /// The latency of the published program from the graph inputs to the
  /// graph outputs, in frames.
  int get outputLatency;

  /// The tail of the published program in frames: the longest node tail
  /// plus its path to the outputs, [infiniteTail] when a node never ends.
  int get outputTail;

  /// The latency of [node] in frames.
  int latencyOf(AudNode node);

  /// The frames by which scheduled events of [node] are pre-delivered so
  /// that they are heard at their time (graph-001).
  int leadOf(AudNode node);

  /// Sets a parameter of [node]: [param] is its index or its id. An
  /// immediate [at] takes the parameter queue, any other time travels as a
  /// parameter event; [rampFrames] ramps the change.
  void setParam(
    AudNode node,
    Object param,
    double value, {
    int rampFrames = 0,
    AudTimestamp at = const AudTimestamp.immediate(),
  });

  /// Sends [event] to [node] at [at]; the event's port names the event
  /// input. [io] as the node sends the event through the graph's event
  /// output to the nodes connected to it. An [id] above zero lets the event
  /// be cancelled while it waits.
  void sendEvent(
    AudNode node,
    AudEvent event, {
    AudTimestamp at = const AudTimestamp.immediate(),
    int id = 0,
  });

  /// Cancels waiting events: those with [id], or all of [node] when [id] is
  /// 0, or every waiting event without both.
  void cancel({AudNode? node, int id = 0});

  // ...........................................................................
  // Event sources (interop-003, midi-003, ticket 26)

  /// Creates an event source for the event input [port] of the committed
  /// [node]: a lock-free queue one producer at a time fills with UMP
  /// packets and host timestamps, which the realtime thread drains every
  /// block under [timing], with the latency [latencyNs] (C of
  /// [AudSourceTiming.fixed]) and a queue of [capacity] packets (0: 4096,
  /// rounded up to a power of two). Throws an [AudGraphException]:
  /// `AUD_ERROR_STATE` for a node that has not been committed,
  /// `AUD_ERROR_RETIRED` for a removed one, `AUD_ERROR_INVALID_ARGUMENT`
  /// for [io] or a port beyond the node's event inputs, `AUD_ERROR_CAPACITY`
  /// beyond 64 sources.
  AudEventSource createEventSource(
    AudNode node, {
    int port = 0,
    AudSourceTiming timing = AudSourceTiming.fixed,
    int latencyNs = 0,
    int capacity = 0,
  });

  /// The event sources that are not disposed.
  List<AudEventSource> get eventSources;

  /// The timing of the engine as the last block saw it; `valid` is false
  /// before a block rendered.
  AudGraphTiming get timing;

  /// Applies the string setting [key] (its key or its id) to [node] on the
  /// calling thread. A value `asset:<id>` reaches the node as the path of
  /// the asset; an unknown asset throws an [ArgumentError]. Returns the id
  /// of the load the node started on a worker thread (ticket 25), which
  /// [loadEnded] awaits, or 0 when the node applied the setting at once - a
  /// node that loads on the calling thread blocks it meanwhile.
  int setString(AudNode node, Object key, String value);

  // ...........................................................................
  // Loads (lifecycle-002, ticket 25)

  /// The loads the graph keeps: those that run and the last 64 that ended,
  /// the oldest first.
  List<AudLoad> get loads;

  /// The load [id], or null when the graph keeps it no longer.
  AudLoad? loadOf(int id);

  /// Asks the running load [id] to stop; the node ends it with
  /// `AUD_ERROR_CANCELLED` as soon as it looks. False when the load ended
  /// or is unknown. Removing a node cancels its loads too; a load its node
  /// leaves running when it goes ends as `AUD_ERROR_CANCELLED`.
  bool cancelLoad(int id);

  /// Completes with the load [id] once it has ended - done, failed or
  /// cancelled -, at once for a load that ended already; throws an
  /// [ArgumentError] for a load the graph does not keep. The notifications
  /// complete it: with `listen: false` it needs [pump].
  Future<AudLoad> loadEnded(int id);

  /// Waits until no load of [node] (null: of any node) runs; false after
  /// [timeout]. Zero waits as long as it takes, a timeout shorter than a
  /// millisecond waits one, a negative one throws an [ArgumentError]. It
  /// blocks the calling isolate - an app awaits [loadEnded] instead; tests
  /// and offline renders use it.
  bool waitLoads({AudNode? node, Duration timeout = Duration.zero});

  // ...........................................................................
  // Messages of nodes (osc-002, ticket 25)

  /// Sends [message] to [node], which takes messages: its address is the
  /// path below the node's route, e.g. `/region0/ampeg_attack` for the
  /// route `/graph/<g>/node/<n>/msg/region0/ampeg_attack`. The node gets it
  /// on the audio thread at [at], ordered with its events; an [id] above
  /// zero lets it be cancelled while it waits. The answers arrive on
  /// [messages].
  ///
  /// A message carries at most 16 arguments (`AUD_GRAPH_MAX_MESSAGE_ARGS`)
  /// and takes at most 1000 bytes (`AUD_GRAPH_MESSAGE_BYTES`): its address
  /// with a terminator, then per argument a type tag and the value - 4
  /// bytes of an int that fits 32 bits or of a double, 8 of a larger int, a
  /// text with its terminator, a blob with 4 bytes of size. Throws an
  /// [AudGraphException]: `AUD_ERROR_CAPACITY` beyond these limits or while
  /// every slot of the graph waits for its node, `AUD_ERROR_UNSUPPORTED` for
  /// a node that takes no messages, `AUD_ERROR_INVALID_ARGUMENT` for an
  /// address without a leading slash, the codes of [sendEvent] otherwise;
  /// an [ArgumentError] for an argument a node message cannot carry, e.g. a
  /// timetag.
  void sendMessage(
    AudNode node,
    AudOscMessage message, {
    AudTimestamp at = const AudTimestamp.immediate(),
    int id = 0,
  });

  /// The messages the nodes sent, as [pump] takes them.
  Stream<AudNodeMessage> get messages;

  // ...........................................................................
  // The resources of the process (lifecycle-002, ticket 25)

  /// The sample cache budget of the process in bytes, which every graph of
  /// the process shares; 256 MB until an app sets one. A smaller budget
  /// trims the caches at once; a negative one throws an [ArgumentError].
  int get cacheBudget;
  set cacheBudget(int bytes);

  /// The caches, the budget and the worker threads of the process.
  AudCacheUsage get cacheUsage;

  /// Waits until no task of the worker threads waits or runs - of any graph
  /// of the process; false after [timeout], which counts like the timeout
  /// of [waitLoads]. Blocks like [waitLoads].
  bool waitTasks({Duration timeout = Duration.zero});

  /// Sends [request] to the transport.
  void transport(AudTransportRequest request);

  /// The transport as the realtime thread published it last.
  AudTransportState get transportState;

  /// Sends a typed [command] of `aud_audio_core` (osc-001); node handles in
  /// it name the nodes of this graph.
  void send(AudCommand command);

  /// Converts an OSC [message] through [adapter] and sends the commands;
  /// returns them. Throws an `AudOscException` for a message that fits no
  /// target.
  List<AudCommand> handleOsc(
    AudOscMessage message, {
    AudOscTimetag timetag = AudOscTimetag.immediate,
  });

  /// Converts an OSC [bundle] through [adapter] and sends the commands.
  List<AudCommand> handleOscBundle(AudOscBundle bundle);

  /// The lifecycle state.
  AudGraphState get state;

  /// Prepares every instance for a new [sampleRate] or [maxFrames] (null
  /// keeps the current value); not while running.
  void prepare({double? sampleRate, int? maxFrames});

  /// Starts rendering.
  void start();

  /// Suspends rendering: blocks render silence, the transport and the
  /// pending events stay.
  void suspend();

  /// Resumes rendering after a suspension.
  void resume();

  /// Stops rendering: running notes are closed, the transport stops.
  void stop();

  /// Destroys the graph and every instance; the stream has to be stopped.
  void dispose();

  /// The notifications of the engine - of the realtime thread and of the
  /// loads -, as [pump] takes them.
  Stream<AudGraphNotification> get notifications;

  /// Takes the waiting notifications, completes the futures of [adopted]
  /// and of [loadEnded] and adds them to [notifications], takes the
  /// messages of the nodes for [messages]; returns the number of
  /// notifications. The listener calls this when the engine wakes it;
  /// tests call it after rendering offline.
  int pump();

  /// The most recent [frames] frames of [channel] of the tap node [tap];
  /// frames the tap has not seen yet are 0.
  Float32List readTap(AudNode tap, {int channel = 0, required int frames});

  /// The peak and root mean square of the last block [tap] saw on
  /// [channel].
  ({double peak, double rms}) tapMeter(AudNode tap, {int channel = 0});

  /// A snapshot of the counters.
  AudGraphStats get stats;

  /// Zeroes the counters.
  void resetStats();

  /// The sample position of the next block.
  int get samplePosition;

  /// Builds the assets, nodes, connections and transport settings of
  /// [document] in one transaction and returns the nodes by their ids. The
  /// buses of the document have to match the graph's; the asset paths
  /// resolve against [baseDirectory] (see [addAsset]).
  Map<String, AudNode> load(AudGraphDocument document, {String? baseDirectory});

  /// The graph as a document: the assets, the live nodes with the
  /// parameters and strings set from Dart and - with [includeState] - the
  /// state blobs of the nodes that save one, the published connections and
  /// the transport.
  AudGraphDocument toDocument({String name = '', bool includeState = true});
}
