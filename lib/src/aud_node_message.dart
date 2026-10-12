// @license
// Copyright (c) Audanika. All Rights Reserved.
//
// Use of this source code is governed by terms that can be
// found in the LICENSE file in the root of this package.

import 'package:aud_audio_core/aud_audio_core.dart';

// #############################################################################
/// A message a node sent to the control thread (osc-002, ticket 25), e.g.
/// the answer of a sampler to a message of `AudGraph.sendMessage`: its path
/// below the node's route and its arguments.
class AudNodeMessage {
  /// Creates the message of [node].
  const AudNodeMessage({
    required this.node,
    required this.message,
    this.samplePosition = 0,
  });

  // ...........................................................................
  /// The handle of the node.
  final int node;

  /// The path below the node's route, e.g. `/num_regions`, and the
  /// arguments.
  final AudOscMessage message;

  /// The sample position of the block the node sent it in.
  final int samplePosition;

  /// The path below the node's route.
  String get path => message.address;

  /// The arguments.
  List<Object?> get arguments => message.arguments;

  // ...........................................................................
  /// The message with the full address of the node in [graph], e.g.
  /// `/graph/1/node/2/msg/num_regions`, for an OSC server.
  AudOscMessage toOsc(int graph) => AudOscMessage(
    '${AudOscAddress.message(graphId: graph, nodeId: node).value}$path',
    arguments,
  );

  /// The message as JSON.
  Map<String, Object?> toJson() => {
    'node': node,
    'message': message.toJson(),
    'samplePosition': samplePosition,
  };

  @override
  bool operator ==(Object other) =>
      other is AudNodeMessage &&
      other.node == node &&
      other.message == message &&
      other.samplePosition == samplePosition;

  @override
  int get hashCode => Object.hash(node, message, samplePosition);

  @override
  String toString() =>
      'AudNodeMessage($node ${message.address} '
      '${message.arguments})';
}
