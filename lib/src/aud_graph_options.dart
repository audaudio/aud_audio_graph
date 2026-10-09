// @license
// Copyright (c) Audanika. All Rights Reserved.
//
// Use of this source code is governed by terms that can be
// found in the LICENSE file in the root of this package.

// #############################################################################
/// The capacities and policies of a graph (decisions interop-002 and
/// graph-003). Zero takes the default of the engine.
class AudGraphOptions {
  /// Creates the options.
  const AudGraphOptions({
    this.maxNodes = 0,
    this.maxConnections = 0,
    this.paramQueueCapacity = 0,
    this.eventQueueCapacity = 0,
    this.schedulerCapacity = 0,
    this.notificationCapacity = 0,
    this.maxEventsPerBlock = 0,
    this.fadeFrames = 0,
    this.maxTailFrames = 0,
    this.lookahead = Duration.zero,
    this.dropLateEvents = false,
  });

  /// The options from [toJson].
  factory AudGraphOptions.fromJson(Map<String, Object?> json) =>
      AudGraphOptions(
        maxNodes: (json['maxNodes'] as num?)?.toInt() ?? 0,
        maxConnections: (json['maxConnections'] as num?)?.toInt() ?? 0,
        paramQueueCapacity: (json['paramQueueCapacity'] as num?)?.toInt() ?? 0,
        eventQueueCapacity: (json['eventQueueCapacity'] as num?)?.toInt() ?? 0,
        schedulerCapacity: (json['schedulerCapacity'] as num?)?.toInt() ?? 0,
        notificationCapacity:
            (json['notificationCapacity'] as num?)?.toInt() ?? 0,
        maxEventsPerBlock: (json['maxEventsPerBlock'] as num?)?.toInt() ?? 0,
        fadeFrames: (json['fadeFrames'] as num?)?.toInt() ?? 0,
        maxTailFrames: (json['maxTailFrames'] as num?)?.toInt() ?? 0,
        lookahead: Duration(
          microseconds: (json['lookaheadUs'] as num?)?.toInt() ?? 0,
        ),
        dropLateEvents: json['dropLateEvents'] == true,
      );

  // ...........................................................................
  /// The instances the graph can hold, retired ones included; 0 = 256.
  final int maxNodes;

  /// The audio and event connections; 0 = 1024.
  final int maxConnections;

  /// The parameter changes the queue holds; 0 = 1024.
  final int paramQueueCapacity;

  /// The events, cancellations and transport requests the queue holds;
  /// 0 = 4096.
  final int eventQueueCapacity;

  /// The events that wait for their time; 0 = 4096.
  final int schedulerCapacity;

  /// The notifications that wait for the control thread; 0 = 1024.
  final int notificationCapacity;

  /// The budget of queue entries one block applies; 0 = 1024.
  final int maxEventsPerBlock;

  /// The fade of removed and new connections in frames; 0 = 5 ms.
  final int fadeFrames;

  /// The longest tail a retired node renders out, in frames; 0 = 10 s.
  final int maxTailFrames;

  /// How far ahead events may be scheduled; zero = 10 s.
  final Duration lookahead;

  /// Whether late events are dropped instead of played at the block start.
  final bool dropLateEvents;

  // ...........................................................................

  /// The options as JSON.
  Map<String, Object?> toJson() => {
    'maxNodes': maxNodes,
    'maxConnections': maxConnections,
    'paramQueueCapacity': paramQueueCapacity,
    'eventQueueCapacity': eventQueueCapacity,
    'schedulerCapacity': schedulerCapacity,
    'notificationCapacity': notificationCapacity,
    'maxEventsPerBlock': maxEventsPerBlock,
    'fadeFrames': fadeFrames,
    'maxTailFrames': maxTailFrames,
    'lookaheadUs': lookahead.inMicroseconds,
    'dropLateEvents': dropLateEvents,
  };

  @override
  bool operator ==(Object other) =>
      other is AudGraphOptions && other.toString() == toString();

  @override
  int get hashCode => toString().hashCode;

  @override
  String toString() => 'AudGraphOptions(${toJson()})';
}
