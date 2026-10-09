// @license
// Copyright (c) Audanika. All Rights Reserved.
//
// Use of this source code is governed by terms that can be
// found in the LICENSE file in the root of this package.

import 'dart:ffi';

import 'package:aud_audio_core/aud_audio_core_ffi.dart';
import 'package:ffi/ffi.dart';

import 'aud_audio_graph_bindings_generated.dart' as bindings;

// #############################################################################
/// A parameter of the document an [AudHost] loaded: its stable id, the node
/// it belongs to, its descriptor and the value the host set last
/// (plugin-002).
class AudHostParam {
  /// Creates the description of a parameter.
  const AudHostParam({
    required this.id,
    required this.node,
    required this.nodeId,
    required this.index,
    required this.descriptor,
    required this.value,
  });

  /// The parameter from its native struct.
  factory AudHostParam.fromNative(bindings.AudHostParam native) => AudHostParam(
    id: native.id,
    node: native.node,
    nodeId: native.node_id.cast<Utf8>().toDartString(),
    index: native.index,
    descriptor: native.descriptor.ref.toDart(),
    value: native.value,
  );

  // ...........................................................................
  /// The stable id: FNV-1a over `<node id>/<parameter id>`, 31 bits.
  final int id;

  /// The handle of the node.
  final int node;

  /// The id of the node in the document.
  final String nodeId;

  /// The index of the parameter in the node's descriptor.
  final int index;

  /// The parameter's descriptor.
  final AudParamDescriptor descriptor;

  /// The value the host set last, or the default.
  final double value;

  /// The id of the parameter, e.g. `cutoff`.
  String get paramId => descriptor.id;

  /// The parameter as JSON.
  Map<String, Object?> toJson() => {
    'id': id,
    'node': node,
    'nodeId': nodeId,
    'paramId': paramId,
    'index': index,
    'value': value,
  };

  @override
  bool operator ==(Object other) =>
      other is AudHostParam &&
      other.id == id &&
      other.node == node &&
      other.nodeId == nodeId &&
      other.index == index &&
      other.descriptor == descriptor &&
      other.value == value;

  @override
  int get hashCode => Object.hash(id, node, nodeId, index, descriptor, value);

  @override
  String toString() => 'AudHostParam(${toJson()})';
}
