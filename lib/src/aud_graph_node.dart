// @license
// Copyright (c) Audanika. All Rights Reserved.
//
// Use of this source code is governed by terms that can be
// found in the LICENSE file in the root of this package.

import 'package:aud_audio_core/aud_audio_core.dart';

// #############################################################################
/// A node instance of a graph (graph-003): a stable handle, a name the
/// documents use, its type and the formats of its buses. The handle 0 is
/// the graph itself: its input buses are the outputs of this node, its
/// output buses the inputs.
class AudNode {
  /// Creates the description of an instance.
  const AudNode({
    required this.handle,
    required this.name,
    required this.descriptor,
    required this.inputChannels,
    required this.outputChannels,
  });

  /// The handle the engine knows the instance by; 0 for the graph.
  final int handle;

  /// The name, unique in the graph; the id in the graph document.
  final String name;

  /// The node type.
  final AudNodeDescriptor descriptor;

  /// The channels of each input bus.
  final List<int> inputChannels;

  /// The channels of each output bus.
  final List<int> outputChannels;

  /// The type id, e.g. `aud.graph.oscillator`.
  String get typeId => descriptor.typeId;

  /// Whether this is the graph itself.
  bool get isGraph => handle == 0;

  // ...........................................................................
  /// The index of the parameter [id]; throws an [ArgumentError] when the
  /// type has no such parameter.
  int paramIndex(String id) {
    final index = descriptor.paramIndex(id);
    if (index == null) {
      throw ArgumentError.value(id, 'id', 'No parameter of $typeId');
    }
    return index;
  }

  /// The key of the string setting [id]; throws an [ArgumentError] when
  /// the type has no such setting.
  int stringKey(String id) {
    final key = descriptor.stringKey(id);
    if (key == null) {
      throw ArgumentError.value(id, 'id', 'No string setting of $typeId');
    }
    return key.key;
  }

  /// The node as JSON.
  Map<String, Object?> toJson() => {
    'handle': handle,
    'name': name,
    'type': typeId,
    'inputChannels': inputChannels,
    'outputChannels': outputChannels,
  };

  @override
  bool operator ==(Object other) => other is AudNode && other.handle == handle;

  @override
  int get hashCode => handle.hashCode;

  @override
  String toString() => 'AudNode($name, $typeId, #$handle)';
}
