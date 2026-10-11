// @license
// Copyright (c) Audanika. All Rights Reserved.
//
// Use of this source code is governed by terms that can be
// found in the LICENSE file in the root of this package.

import 'package:aud_audio_core/aud_audio_core.dart';

// #############################################################################
/// A load of a node on a worker thread (lifecycle-001, ticket 25): e.g. the
/// SFZ file a sampler loads after a string setting named it.
class AudLoad {
  /// Creates the load.
  const AudLoad({
    required this.id,
    required this.node,
    this.name = '',
    this.result = AUD_PENDING,
    this.progress = 0,
    this.message = '',
    this.cancelled = false,
    this.startNs = 0,
    this.endNs = 0,
  });

  /// A load from [toJson].
  factory AudLoad.fromJson(Map<String, Object?> json) => AudLoad(
    id: (json['id']! as num).toInt(),
    node: (json['node']! as num).toInt(),
    name: json['name'] as String? ?? '',
    result: (json['result'] as num?)?.toInt() ?? AUD_PENDING,
    progress: (json['progress'] as num?)?.toDouble() ?? 0,
    message: json['message'] as String? ?? '',
    cancelled: json['cancelled'] == true,
    startNs: (json['startNs'] as num?)?.toInt() ?? 0,
    endNs: (json['endNs'] as num?)?.toInt() ?? 0,
  );

  // ...........................................................................
  /// The id, unique in the process.
  final int id;

  /// The handle of the node that loads.
  final int node;

  /// What loads, e.g. the file.
  final String name;

  /// `AUD_PENDING` while the load runs, then its result: `AUD_OK`,
  /// `AUD_ERROR_CANCELLED` or an error code.
  final int result;

  /// The progress from 0 to 1.
  final double progress;

  /// Why the load failed; empty otherwise.
  final String message;

  /// Whether the load was asked to stop.
  final bool cancelled;

  /// The host time of the start in nanoseconds.
  final int startNs;

  /// The host time of the end in nanoseconds; 0 while the load runs.
  final int endNs;

  /// Whether the load runs.
  bool get isRunning => result == AUD_PENDING;

  /// Whether the load ended well.
  bool get succeeded => result == AUD_OK;

  /// The name of [result], e.g. `AUD_ERROR_CANCELLED`.
  String get resultName => AudAbi.resultName(result);

  /// How long the load took; null while it runs.
  Duration? get duration =>
      isRunning ? null : Duration(microseconds: (endNs - startNs) ~/ 1000);

  // ...........................................................................
  /// The load as JSON.
  Map<String, Object?> toJson() => {
    'id': id,
    'node': node,
    'name': name,
    'result': result,
    'progress': progress,
    'message': message,
    'cancelled': cancelled,
    'startNs': startNs,
    'endNs': endNs,
  };

  @override
  bool operator ==(Object other) =>
      other is AudLoad &&
      other.id == id &&
      other.node == node &&
      other.name == name &&
      other.result == result &&
      other.progress == progress &&
      other.message == message &&
      other.cancelled == cancelled &&
      other.startNs == startNs &&
      other.endNs == endNs;

  @override
  int get hashCode => Object.hash(
    id,
    node,
    name,
    result,
    progress,
    message,
    cancelled,
    startNs,
    endNs,
  );

  @override
  String toString() => 'AudLoad($id of node $node: $resultName, $name)';
}
