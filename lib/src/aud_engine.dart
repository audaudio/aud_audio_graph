// @license
// Copyright (c) Audanika. All Rights Reserved.
//
// Use of this source code is governed by terms that can be
// found in the LICENSE file in the root of this package.

import 'dart:ffi';
import 'dart:typed_data';

import 'package:aud_audio_core/aud_audio_core.dart';
import 'package:ffi/ffi.dart';

import 'aud_audio_graph_bindings_generated.dart' as bindings;

// #############################################################################
/// Thrown when an engine call returns an error code of the ABI.
class AudEngineException implements Exception {
  /// Creates the exception for a result [code] and a [message].
  const AudEngineException({required this.code, required this.message});

  /// The result code, one of the `AUD_ERROR_*` constants.
  final int code;

  /// What failed and why.
  final String message;

  @override
  String toString() => 'AudEngineException($code): $message';
}

// #############################################################################
/// A parameter of a node type.
class AudParamInfo {
  /// Creates the description of a parameter.
  const AudParamInfo({
    required this.id,
    required this.unit,
    required this.min,
    required this.max,
    required this.defaultValue,
  });

  /// The stable identifier, e.g. `frequency`.
  final String id;

  /// The unit, e.g. `Hz`, or an empty string.
  final String unit;

  /// The smallest value.
  final double min;

  /// The largest value.
  final double max;

  /// The value a new node starts with.
  final double defaultValue;
}

// #############################################################################
/// A node type registered with the engine.
class AudNodeTypeInfo {
  /// Creates the description of a node type.
  const AudNodeTypeInfo({
    required this.id,
    required this.name,
    required this.capabilities,
    required this.params,
  });

  /// The type id, e.g. `aud.ref.sine`.
  final String id;

  /// The display name.
  final String name;

  /// The `AUD_NODE_CAP_*` flags.
  final int capabilities;

  /// The parameters in index order.
  final List<AudParamInfo> params;

  /// Whether the type consumes note events.
  bool get takesEvents => capabilities & AUD_NODE_CAP_EVENTS != 0;
}

// #############################################################################
/// The counters the realtime thread keeps.
class AudEngineStats {
  /// Creates a snapshot of the counters.
  const AudEngineStats({
    required this.programRevision,
    required this.blocksRendered,
    required this.framesRendered,
    required this.commandsApplied,
    required this.commandsRejected,
    required this.commandLatencyMinNs,
    required this.commandLatencyMaxNs,
    required this.commandLatencySumNs,
    required this.commandLatencyCount,
    required this.renderTimeMaxNs,
    required this.renderTimeSumNs,
    required this.outputPeak,
  });

  /// The chain revision the realtime thread renders.
  final int programRevision;

  /// Calls of the render function.
  final int blocksRendered;

  /// Frames rendered in total.
  final int framesRendered;

  /// Commands the realtime thread applied.
  final int commandsApplied;

  /// Commands refused because the queue was full.
  final int commandsRejected;

  /// The shortest time from enqueue to apply, in nanoseconds.
  final int commandLatencyMinNs;

  /// The longest time from enqueue to apply, in nanoseconds.
  final int commandLatencyMaxNs;

  /// The sum of all command latencies, in nanoseconds.
  final int commandLatencySumNs;

  /// The number of latencies summed.
  final int commandLatencyCount;

  /// The longest render call, in nanoseconds.
  final int renderTimeMaxNs;

  /// The sum of all render call durations, in nanoseconds.
  final int renderTimeSumNs;

  /// The largest absolute output sample since the last reset, 0 to 1.
  final double outputPeak;

  /// The mean command latency in nanoseconds.
  double get commandLatencyMeanNs =>
      commandLatencyCount == 0 ? 0 : commandLatencySumNs / commandLatencyCount;

  /// The mean render call duration in nanoseconds.
  double get renderTimeMeanNs =>
      blocksRendered == 0 ? 0 : renderTimeSumNs / blocksRendered;
}

// #############################################################################
/// The spike engine of ticket 5: hosts node types registered through the C
/// ABI of `aud_audio_core`, renders a serial chain of node instances block
/// by block on the realtime thread and takes parameter changes and events
/// from a lock-free command queue.
class AudEngine {
  /// Creates an engine with the reference node types `aud.ref.sine` and
  /// `aud.ref.gain` registered.
  ///
  /// - [sampleRate] the sample rate in Hz
  /// - [maxFrames] the largest block [render] receives
  /// - [channels] the channels of every bus and of the output
  /// - [commandQueueCapacity] the commands the queue holds before it refuses
  /// - [maxNodes] the node instances the engine can hold
  AudEngine({
    this.sampleRate = 48000,
    this.maxFrames = 1024,
    this.channels = 2,
    int commandQueueCapacity = 1024,
    int maxNodes = 64,
  }) {
    final config = calloc<bindings.AudEngineConfig>();
    config.ref
      ..struct_size = sizeOf<bindings.AudEngineConfig>()
      ..sample_rate = sampleRate
      ..max_frames = maxFrames
      ..channels = channels
      ..command_queue_capacity = commandQueueCapacity
      ..max_nodes = maxNodes;
    _engine = bindings.aud_engine_create(config);
    calloc.free(config);
    if (_engine == nullptr) {
      throw const AudEngineException(
        code: AUD_ERROR_INVALID_ARGUMENT,
        message: 'The engine was not created; check the configuration.',
      );
    }
    _output = calloc<Float>(maxFrames * channels);
  }

  /// The sample rate in Hz.
  final double sampleRate;

  /// The largest block [render] receives.
  final int maxFrames;

  /// The channels of every bus and of the output.
  final int channels;

  // ...........................................................................
  /// The native engine, the user pointer of [renderCallback].
  Pointer<Void> get handle => _engine.cast();

  /// The `AudHostApi` DSP packages register their node types with.
  Pointer<Void> get hostApi => bindings.aud_engine_host_api(_engine);

  /// The render callback for an `aud_audio_io` stream; pass [handle] as the
  /// user pointer.
  static Pointer<NativeFunction<AudRenderCallbackFunction>>
  get renderCallback =>
      Native.addressOf<NativeFunction<AudRenderCallbackFunction>>(
        bindings.aud_engine_io_render,
      );

  // ...........................................................................
  /// The registered node types.
  List<AudNodeTypeInfo> get nodeTypes {
    final count = bindings.aud_engine_num_node_types(_engine);
    return [for (var index = 0; index < count; index++) _nodeType(index)];
  }

  /// Creates a node instance of [typeId] and returns its id.
  int createNode(String typeId) {
    final native = typeId.toNativeUtf8();
    try {
      return _check(
        bindings.aud_engine_create_node(_engine, native.cast()),
        'create a node of type $typeId',
      );
    } finally {
      calloc.free(native);
    }
  }

  /// Destroys the node instance [node]; it must not be part of the chain,
  /// and the realtime thread must have adopted the latest chain (render a
  /// block or wait for the stream) - the engine refuses otherwise.
  void destroyNode(int node) => _check(
    bindings.aud_engine_destroy_node(_engine, node),
    'destroy node $node',
  );

  /// Publishes [nodes] as the serial chain the realtime thread renders and
  /// returns the revision; the chain is adopted at the next block start.
  int setChain(List<int> nodes) {
    final native = calloc<Int32>(nodes.length);
    try {
      native.asTypedList(nodes.length).setAll(0, nodes);
      return _check(
        bindings.aud_engine_set_chain(_engine, native, nodes.length),
        'set the chain $nodes',
      );
    } finally {
      calloc.free(native);
    }
  }

  // ...........................................................................
  /// Enqueues a parameter change for [node]; throws when the queue is full.
  void setParam(int node, int param, double value) => _check(
    bindings.aud_engine_set_param(_engine, node, param, value),
    'set parameter $param of node $node',
  );

  /// Enqueues a note on for [node].
  void noteOn(
    int node, {
    required int number,
    int channel = 0,
    double velocity = 1.0,
  }) => _check(
    bindings.aud_engine_send_note(_engine, node, 1, channel, number, velocity),
    'send a note on to node $node',
  );

  /// Enqueues a note off for [node].
  void noteOff(int node, {required int number, int channel = 0}) => _check(
    bindings.aud_engine_send_note(_engine, node, 0, channel, number, 0),
    'send a note off to node $node',
  );

  /// Applies the string setting [key] to [node] on the calling thread; may
  /// block while the node loads.
  void setString(int node, int key, String value) {
    final native = value.toNativeUtf8();
    try {
      _check(
        bindings.aud_engine_set_string(_engine, node, key, native.cast()),
        'set string $key of node $node',
      );
    } finally {
      calloc.free(native);
    }
  }

  // ...........................................................................
  /// Renders [frames] frames of interleaved output on the calling thread;
  /// for tests and offline rendering. Audio streams call [renderCallback]
  /// instead.
  Float32List render(int frames) {
    if (frames > maxFrames) {
      throw ArgumentError.value(frames, 'frames', 'at most $maxFrames');
    }
    bindings.aud_engine_render(_engine, _output, frames);
    return Float32List.fromList(_output.asTypedList(frames * channels));
  }

  /// A snapshot of the realtime counters.
  AudEngineStats get stats {
    final native = calloc<bindings.AudEngineStats>();
    try {
      native.ref.struct_size = sizeOf<bindings.AudEngineStats>();
      bindings.aud_engine_get_stats(_engine, native);
      final s = native.ref;
      return AudEngineStats(
        programRevision: s.program_revision,
        blocksRendered: s.blocks_rendered,
        framesRendered: s.frames_rendered,
        commandsApplied: s.commands_applied,
        commandsRejected: s.commands_rejected,
        commandLatencyMinNs: s.command_latency_min_ns,
        commandLatencyMaxNs: s.command_latency_max_ns,
        commandLatencySumNs: s.command_latency_sum_ns,
        commandLatencyCount: s.command_latency_count,
        renderTimeMaxNs: s.render_time_max_ns,
        renderTimeSumNs: s.render_time_sum_ns,
        outputPeak: s.output_peak,
      );
    } finally {
      calloc.free(native);
    }
  }

  /// Zeroes the realtime counters.
  void resetStats() => bindings.aud_engine_reset_stats(_engine);

  /// Destroys the engine and its node instances; stop the stream first.
  void dispose() {
    if (_engine == nullptr) return;
    bindings.aud_engine_destroy(_engine);
    _engine = nullptr;
    calloc.free(_output);
  }

  // ...........................................................................
  late Pointer<bindings.AudEngine> _engine;
  late final Pointer<Float> _output;

  AudNodeTypeInfo _nodeType(int index) {
    final numParams = bindings.aud_engine_node_type_num_params(_engine, index);
    return AudNodeTypeInfo(
      id: _string(bindings.aud_engine_node_type_id(_engine, index)),
      name: _string(bindings.aud_engine_node_type_name(_engine, index)),
      capabilities: bindings.aud_engine_node_type_capabilities(_engine, index),
      params: [for (var p = 0; p < numParams; p++) _param(index, p)],
    );
  }

  AudParamInfo _param(int type, int param) {
    final id = calloc<Pointer<Char>>();
    final unit = calloc<Pointer<Char>>();
    final values = calloc<Float>(3);
    try {
      _check(
        bindings.aud_engine_node_type_param(
          _engine,
          type,
          param,
          id,
          unit,
          values,
          values + 1,
          values + 2,
        ),
        'read parameter $param of node type $type',
      );
      return AudParamInfo(
        id: _string(id.value),
        unit: _string(unit.value),
        min: values[0],
        max: values[1],
        defaultValue: values[2],
      );
    } finally {
      calloc.free(id);
      calloc.free(unit);
      calloc.free(values);
    }
  }

  static String _string(Pointer<Char> native) =>
      native.cast<Utf8>().toDartString();

  int _check(int result, String what) {
    if (result < 0) {
      throw AudEngineException(
        code: result,
        message: 'Could not $what: ${AudAbi.resultName(result)}',
      );
    }
    return result;
  }
}
