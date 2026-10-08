// @license
// Copyright (c) Audanika. All Rights Reserved.
//
// Use of this source code is governed by terms that can be
// found in the LICENSE file in the root of this package.

import 'package:aud_audio_core/aud_audio_core.dart';

import 'aud_audio_graph_bindings_generated.dart' as bindings;
import 'aud_graph_state.dart';

// #############################################################################
/// What the realtime thread reports to the control thread:
/// `AudGraphNotification` of the engine. [toOsc] gives the notification of
/// the OSC vocabulary of `aud_audio_core` where one exists (osc-001).
sealed class AudGraphNotification {
  const AudGraphNotification({required this.samplePosition});

  /// A notification from its native struct.
  factory AudGraphNotification.fromNative(
    bindings.AudGraphNotification native,
  ) {
    final position = native.sample_position;
    return switch (native.type) {
      bindings.AUD_NOTIFY_REVISION => AudRevisionAdoptedNotification(
        revision: native.revision,
        samplePosition: position,
      ),
      bindings.AUD_NOTIFY_STATE => AudStateNotification(
        state: AudGraphState.fromCode(native.code),
        samplePosition: position,
      ),
      bindings.AUD_NOTIFY_NODE_DONE => AudNodeDoneNotification(
        node: native.node,
        samplePosition: position,
      ),
      bindings.AUD_NOTIFY_DIAGNOSTIC => AudDiagnosticNotification(
        code: native.code,
        count: native.count,
        node: native.node,
        value: native.value,
        samplePosition: position,
      ),
      bindings.AUD_NOTIFY_EVENT => AudEventNotification(
        node: native.node,
        event: AudEvent.fromNative(native.event),
        samplePosition: position,
      ),
      bindings.AUD_NOTIFY_TRANSPORT => AudTransportNotification(
        type: AudTransportRequestType.fromCode(native.code),
        beatTicks: native.value,
        tempo: native.number,
        playing: native.count != 0,
        samplePosition: position,
      ),
      bindings.AUD_NOTIFY_TIME_RESET => AudTimeResetNotification(
        count: native.count,
        samplePosition: position,
      ),
      _ => throw ArgumentError.value(
        native.type,
        'type',
        'Unknown notification',
      ),
    };
  }

  // ...........................................................................
  /// The sample position of the block the notification comes from.
  final int samplePosition;

  /// The notification as JSON.
  Map<String, Object?> toJson();

  /// The notification of the OSC vocabulary for the graph [graph], or null
  /// when the vocabulary has none for it.
  AudNotification? toOsc(int graph) => null;

  @override
  String toString() => '$runtimeType(${toJson()})';
}

// #############################################################################
/// The realtime thread adopted a revision (graph-003).
final class AudRevisionAdoptedNotification extends AudGraphNotification {
  /// Creates the notification.
  const AudRevisionAdoptedNotification({
    required this.revision,
    required super.samplePosition,
  });

  /// The revision.
  final int revision;

  @override
  Map<String, Object?> toJson() => {
    'type': 'revision',
    'revision': revision,
    'samplePosition': samplePosition,
  };

  @override
  AudNotification toOsc(int graph) =>
      AudRevisionAdopted(graph: graph, revision: revision);
}

// #############################################################################
/// The realtime thread rendered its first block in a state.
final class AudStateNotification extends AudGraphNotification {
  /// Creates the notification.
  const AudStateNotification({
    required this.state,
    required super.samplePosition,
  });

  /// The state.
  final AudGraphState state;

  @override
  Map<String, Object?> toJson() => {
    'type': 'state',
    'state': state.name,
    'samplePosition': samplePosition,
  };
}

// #############################################################################
/// A retired node finished its fade or tail and was freed.
final class AudNodeDoneNotification extends AudGraphNotification {
  /// Creates the notification.
  const AudNodeDoneNotification({
    required this.node,
    required super.samplePosition,
  });

  /// The handle of the node.
  final int node;

  @override
  Map<String, Object?> toJson() => {
    'type': 'nodeDone',
    'node': node,
    'samplePosition': samplePosition,
  };

  @override
  AudNotification toOsc(int graph) => AudNodeRetired(graph: graph, node: node);
}

// #############################################################################
/// A diagnostic of a block: late events, dropped entries, an overload.
final class AudDiagnosticNotification extends AudGraphNotification {
  /// Creates the notification.
  const AudDiagnosticNotification({
    required this.code,
    required this.count,
    this.node = 0,
    this.value = 0,
    required super.samplePosition,
  });

  /// The result code of the ABI that names the diagnostic.
  final int code;

  /// How often it happened in the block.
  final int count;

  /// The node it concerns; 0 for the graph.
  final int node;

  /// A value: the render time in nanoseconds of an overload.
  final int value;

  /// The name of the code, e.g. `AUD_ERROR_LATE`.
  String get name => AudAbi.resultName(code);

  /// What happened, in words.
  String get message => switch (code) {
    AUD_ERROR_LATE => 'events arrived late',
    AUD_ERROR_RETIRED => 'events of a retired node were dropped',
    AUD_ERROR_QUEUE_FULL => 'entries were dropped: a capacity was exceeded',
    AUD_ERROR_CAPACITY => 'the scheduler or a note tracker was full',
    AUD_ERROR_OVERLOAD => 'the block took $value ns, longer than it lasts',
    AUD_ERROR_UNSUPPORTED => 'the transport refused a request',
    _ => name,
  };

  @override
  Map<String, Object?> toJson() => {
    'type': 'diagnostic',
    'code': code,
    'name': name,
    'count': count,
    'node': node,
    'value': value,
    'samplePosition': samplePosition,
  };

  @override
  AudNotification toOsc(int graph) => AudDiagnostic(
    code: code,
    message: '$message ($count)',
    address: node == 0
        ? null
        : AudOscAddress.node(graphId: graph, nodeId: node).value,
  );
}

// #############################################################################
/// A node emitted an event into the graph's event input.
final class AudEventNotification extends AudGraphNotification {
  /// Creates the notification.
  const AudEventNotification({
    required this.node,
    required this.event,
    required super.samplePosition,
  });

  /// The handle of the node that emitted the event.
  final int node;

  /// The event.
  final AudEvent event;

  @override
  Map<String, Object?> toJson() => {
    'type': 'event',
    'node': node,
    'event': event.toJson(),
    'samplePosition': samplePosition,
  };
}

// #############################################################################
/// The internal transport applied a request.
final class AudTransportNotification extends AudGraphNotification {
  /// Creates the notification.
  const AudTransportNotification({
    required this.type,
    required this.beatTicks,
    required this.tempo,
    required this.playing,
    required super.samplePosition,
  });

  /// The request that was applied.
  final AudTransportRequestType type;

  /// The musical position after the request, in ticks.
  final int beatTicks;

  /// The tempo after the request.
  final double tempo;

  /// Whether the transport plays after the request.
  final bool playing;

  /// The musical position after the request, in beats.
  double get beat => AudBeats.beats(beatTicks);

  @override
  Map<String, Object?> toJson() => {
    'type': 'transport',
    'request': type.name,
    'beat': beat,
    'tempo': tempo,
    'playing': playing,
    'samplePosition': samplePosition,
  };
}

// #############################################################################
/// The time filter reset (time-001).
final class AudTimeResetNotification extends AudGraphNotification {
  /// Creates the notification.
  const AudTimeResetNotification({
    required this.count,
    required super.samplePosition,
  });

  /// The resets in the block.
  final int count;

  @override
  Map<String, Object?> toJson() => {
    'type': 'timeReset',
    'count': count,
    'samplePosition': samplePosition,
  };
}
