// @license
// Copyright (c) Audanika. All Rights Reserved.
//
// Use of this source code is governed by terms that can be
// found in the LICENSE file in the root of this package.

import 'dart:convert';
import 'dart:ffi';

import 'aud_audio_graph_bindings_generated.dart' as bindings;

// #############################################################################
/// What `AudHost.inspect` reads from a graph document without a graph: the
/// buses - so that a plugin shell can create the graph that fits - the
/// counts and the name (plugin-002).
class AudHostDocumentInfo {
  /// Creates the description of a document.
  const AudHostDocumentInfo({
    required this.schema,
    required this.name,
    required this.inputChannels,
    required this.outputChannels,
    required this.numNodes,
    required this.numAssets,
  });

  /// The description from its native struct.
  factory AudHostDocumentInfo.fromNative(bindings.AudHostDocumentInfo native) {
    final name = <int>[];
    for (
      var i = 0;
      i < bindings.AUD_HOST_MAX_NAME && native.name[i] != 0;
      i++
    ) {
      name.add(native.name[i] & 0xFF);
    }
    return AudHostDocumentInfo(
      schema: native.schema,
      name: utf8.decode(name, allowMalformed: true),
      inputChannels: [
        for (var b = 0; b < native.num_input_buses; b++)
          native.input_channels[b],
      ],
      outputChannels: [
        for (var b = 0; b < native.num_output_buses; b++)
          native.output_channels[b],
      ],
      numNodes: native.num_nodes,
      numAssets: native.num_assets,
    );
  }

  // ...........................................................................
  /// The version of the document schema.
  final int schema;

  /// The name of the graph; the host keeps its first 127 bytes.
  final String name;

  /// The channels of each input bus of the graph.
  final List<int> inputChannels;

  /// The channels of each output bus of the graph.
  final List<int> outputChannels;

  /// The nodes of the document.
  final int numNodes;

  /// The assets of the document.
  final int numAssets;

  /// The description as JSON.
  Map<String, Object?> toJson() => {
    'schema': schema,
    'name': name,
    'inputChannels': inputChannels,
    'outputChannels': outputChannels,
    'numNodes': numNodes,
    'numAssets': numAssets,
  };

  @override
  bool operator ==(Object other) =>
      other is AudHostDocumentInfo &&
      jsonEncode(other.toJson()) == jsonEncode(toJson());

  @override
  int get hashCode => jsonEncode(toJson()).hashCode;

  @override
  String toString() => 'AudHostDocumentInfo(${toJson()})';
}
