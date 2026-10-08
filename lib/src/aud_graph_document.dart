// @license
// Copyright (c) Audanika. All Rights Reserved.
//
// Use of this source code is governed by terms that can be
// found in the LICENSE file in the root of this package.

import 'dart:convert';

import 'package:aud_audio_core/aud_audio_core.dart';

// #############################################################################
/// A node of a graph document: its id, its type, the formats of its buses
/// and its preset (the node preset schema of `aud_audio_core`).
class AudGraphDocumentNode {
  /// Creates a node entry.
  const AudGraphDocumentNode({
    required this.id,
    required this.typeId,
    this.inputChannels,
    this.outputChannels,
    this.delayFrames = 0,
    this.preset,
  });

  /// A node entry from [toJson]; throws a [FormatException] when it does
  /// not follow the schema.
  factory AudGraphDocumentNode.fromJson(Map<String, Object?> json) {
    final id = json['id'];
    if (id is! String || !AudGraphDocument.idPattern.hasMatch(id)) {
      throw FormatException('A node needs an id like osc1', json);
    }
    final typeId = json['type'];
    if (typeId is! String || !AudNodePreset.typeIdPattern.hasMatch(typeId)) {
      throw FormatException(
        'Node $id needs a type like aud.graph.filter',
        json,
      );
    }
    final delayFrames = json['delayFrames'] ?? 0;
    if (delayFrames is! int || delayFrames < 0) {
      throw FormatException('delayFrames of $id must be a count', json);
    }
    return AudGraphDocumentNode(
      id: id,
      typeId: typeId,
      inputChannels: _channels(json['inputChannels'], 'inputChannels of $id'),
      outputChannels: _channels(
        json['outputChannels'],
        'outputChannels of $id',
      ),
      delayFrames: delayFrames,
      preset: json['preset'] == null
          ? null
          : AudNodePreset.fromJson(json['preset']! as Map<String, Object?>),
    );
  }

  static List<int>? _channels(Object? json, String what) {
    if (json == null) return null;
    if (json is! List || json.any((c) => c is! int || c < 1)) {
      throw FormatException('$what must list channel counts', json);
    }
    return [for (final c in json) c as int];
  }

  // ...........................................................................
  /// The id, unique in the document; the name of the node in the graph.
  final String id;

  /// The type id, e.g. `aud.graph.oscillator`.
  final String typeId;

  /// The channels of each input bus; null takes the type's defaults.
  final List<int>? inputChannels;

  /// The channels of each output bus; null takes the type's defaults.
  final List<int>? outputChannels;

  /// The delay of a feedback node in frames; 0 takes the largest block.
  final int delayFrames;

  /// The parameters, string settings and state of the node.
  final AudNodePreset? preset;

  /// The entry as JSON.
  Map<String, Object?> toJson() => {
    'id': id,
    'type': typeId,
    if (inputChannels != null) 'inputChannels': inputChannels,
    if (outputChannels != null) 'outputChannels': outputChannels,
    if (delayFrames != 0) 'delayFrames': delayFrames,
    if (preset != null) 'preset': preset!.toJson(),
  };

  @override
  bool operator ==(Object other) =>
      other is AudGraphDocumentNode && jsonEncode(other) == jsonEncode(this);

  @override
  int get hashCode => jsonEncode(this).hashCode;

  @override
  String toString() => 'AudGraphDocumentNode(${toJson()})';
}

// #############################################################################
/// An audio connection of a graph document between two node ids; the
/// graph itself is [AudGraphDocument.graphId].
class AudGraphConnection {
  /// Creates a connection.
  const AudGraphConnection({
    required this.from,
    required this.to,
    this.fromBus = 0,
    this.toBus = 0,
    this.lowLatency = false,
  });

  /// A connection from [toJson].
  factory AudGraphConnection.fromJson(Map<String, Object?> json) {
    final from = json['from'];
    final to = json['to'];
    if (from is! String || to is! String) {
      throw FormatException('A connection needs from and to', json);
    }
    return AudGraphConnection(
      from: from,
      to: to,
      fromBus: _bus(json['fromBus'], json),
      toBus: _bus(json['toBus'], json),
      lowLatency: json['lowLatency'] == true,
    );
  }

  static int _bus(Object? json, Map<String, Object?> connection) {
    final value = json ?? 0;
    if (value is! int || value < 0) {
      throw FormatException('A bus is an index', connection);
    }
    return value;
  }

  // ...........................................................................
  /// The id of the source node.
  final String from;

  /// The output bus of the source.
  final int fromBus;

  /// The id of the destination node.
  final String to;

  /// The input bus of the destination.
  final int toBus;

  /// Whether the connection opts out of the latency alignment.
  final bool lowLatency;

  /// The connection as JSON.
  Map<String, Object?> toJson() => {
    'from': from,
    if (fromBus != 0) 'fromBus': fromBus,
    'to': to,
    if (toBus != 0) 'toBus': toBus,
    if (lowLatency) 'lowLatency': true,
  };

  @override
  bool operator ==(Object other) =>
      other is AudGraphConnection && jsonEncode(other) == jsonEncode(this);

  @override
  int get hashCode => jsonEncode(this).hashCode;

  @override
  String toString() => 'AudGraphConnection(${toJson()})';
}

// #############################################################################
/// An event connection of a graph document between two node ids.
class AudGraphEventConnection {
  /// Creates an event connection.
  const AudGraphEventConnection({
    required this.from,
    required this.to,
    this.fromPort = 0,
    this.toPort = 0,
  });

  /// An event connection from [toJson].
  factory AudGraphEventConnection.fromJson(Map<String, Object?> json) {
    final from = json['from'];
    final to = json['to'];
    if (from is! String || to is! String) {
      throw FormatException('An event connection needs from and to', json);
    }
    return AudGraphEventConnection(
      from: from,
      to: to,
      fromPort: AudGraphConnection._bus(json['fromPort'], json),
      toPort: AudGraphConnection._bus(json['toPort'], json),
    );
  }

  // ...........................................................................
  /// The id of the source node.
  final String from;

  /// The event output of the source.
  final int fromPort;

  /// The id of the destination node.
  final String to;

  /// The event input of the destination.
  final int toPort;

  /// The connection as JSON.
  Map<String, Object?> toJson() => {
    'from': from,
    if (fromPort != 0) 'fromPort': fromPort,
    'to': to,
    if (toPort != 0) 'toPort': toPort,
  };

  @override
  bool operator ==(Object other) =>
      other is AudGraphEventConnection && jsonEncode(other) == jsonEncode(this);

  @override
  int get hashCode => jsonEncode(this).hashCode;

  @override
  String toString() => 'AudGraphEventConnection(${toJson()})';
}

// #############################################################################
/// The transport settings of a graph document.
class AudGraphTransportSettings {
  /// Creates the settings.
  const AudGraphTransportSettings({
    this.tempo = 120,
    this.numerator = 4,
    this.denominator = 4,
    this.loopStart,
    this.loopEnd,
  }) : assert(
         (loopStart == null) == (loopEnd == null),
         'A loop needs a start and an end',
       );

  /// The settings from [toJson].
  factory AudGraphTransportSettings.fromJson(Map<String, Object?> json) {
    final tempo = json['tempo'] ?? 120;
    final numerator = json['numerator'] ?? 4;
    final denominator = json['denominator'] ?? 4;
    final loopStart = json['loopStart'];
    final loopEnd = json['loopEnd'];
    if (tempo is! num ||
        tempo <= 0 ||
        numerator is! int ||
        numerator < 1 ||
        denominator is! int ||
        denominator < 1 ||
        loopStart is! num? ||
        loopEnd is! num? ||
        (loopStart == null) != (loopEnd == null)) {
      throw FormatException('Invalid transport settings', json);
    }
    return AudGraphTransportSettings(
      tempo: tempo.toDouble(),
      numerator: numerator,
      denominator: denominator,
      loopStart: loopStart?.toDouble(),
      loopEnd: loopEnd?.toDouble(),
    );
  }

  // ...........................................................................
  /// The tempo in beats per minute.
  final double tempo;

  /// The numerator of the time signature.
  final int numerator;

  /// The denominator of the time signature.
  final int denominator;

  /// The loop start in beats, with [loopEnd]; null without a loop.
  final double? loopStart;

  /// The loop end in beats.
  final double? loopEnd;

  /// Whether the transport loops.
  bool get looping => loopStart != null;

  /// The settings as JSON.
  Map<String, Object?> toJson() => {
    'tempo': tempo,
    'numerator': numerator,
    'denominator': denominator,
    if (loopStart != null) 'loopStart': loopStart,
    if (loopEnd != null) 'loopEnd': loopEnd,
  };

  @override
  bool operator ==(Object other) =>
      other is AudGraphTransportSettings &&
      jsonEncode(other) == jsonEncode(this);

  @override
  int get hashCode => jsonEncode(this).hashCode;

  @override
  String toString() => 'AudGraphTransportSettings(${toJson()})';
}

// #############################################################################
/// A graph as JSON (plan of ticket 17, S2): the buses of the graph, its
/// nodes with their presets, the audio and event connections and the
/// transport settings. The document is what the editor, the presets and
/// the plugin shells share; `AudGraph.load` builds a graph from it and
/// `AudGraph.toDocument` writes one. The schema is [jsonSchema]
/// (`doc/schemas/aud_graph_document.schema.json`).
class AudGraphDocument {
  /// Creates a document.
  const AudGraphDocument({
    this.name = '',
    this.inputChannels = const [],
    this.outputChannels = const [2],
    this.nodes = const [],
    this.connections = const [],
    this.eventConnections = const [],
    this.transport = const AudGraphTransportSettings(),
  });

  /// A document from its JSON; throws a [FormatException] when the JSON
  /// does not follow the schema.
  factory AudGraphDocument.fromJson(Map<String, Object?> json) {
    if (json['schema'] != schemaVersion) {
      throw FormatException('schema must be $schemaVersion', json);
    }
    for (final key in json.keys) {
      if (!_keys.contains(key)) throw FormatException('Unknown key $key', json);
    }
    final name = json['name'] ?? '';
    if (name is! String) throw FormatException('name must be a string', json);
    List<T> list<T>(String key, T Function(Map<String, Object?>) from) {
      final items = json[key] ?? const [];
      if (items is! List || items.any((item) => item is! Map)) {
        throw FormatException('$key must be a list of objects', json);
      }
      return [for (final item in items) from(item as Map<String, Object?>)];
    }

    final document = AudGraphDocument(
      name: name,
      inputChannels:
          AudGraphDocumentNode._channels(
            json['inputChannels'],
            'inputChannels',
          ) ??
          const [],
      outputChannels:
          AudGraphDocumentNode._channels(
            json['outputChannels'],
            'outputChannels',
          ) ??
          const [2],
      nodes: list('nodes', AudGraphDocumentNode.fromJson),
      connections: list('connections', AudGraphConnection.fromJson),
      eventConnections: list(
        'eventConnections',
        AudGraphEventConnection.fromJson,
      ),
      transport: json['transport'] == null
          ? const AudGraphTransportSettings()
          : AudGraphTransportSettings.fromJson(
              json['transport']! as Map<String, Object?>,
            ),
    );
    final problems = document.validate();
    if (problems.isNotEmpty) throw FormatException(problems.join('; '), json);
    return document;
  }

  /// A document from a JSON [text].
  factory AudGraphDocument.parse(String text) =>
      AudGraphDocument.fromJson(jsonDecode(text) as Map<String, Object?>);

  // ...........................................................................
  /// The version of the document schema.
  static const int schemaVersion = 1;

  /// The id of the JSON schema.
  static const String schemaId =
      'https://audaudio.github.io/schemas/aud_graph_document.schema.json';

  /// The id of the graph itself in connections.
  static const String graphId = 'graph';

  /// The form of a node id: a word of letters, digits, `_` and `-`.
  static final RegExp idPattern = RegExp(r'^[A-Za-z][A-Za-z0-9_-]*$');

  static const Set<String> _keys = {
    'schema',
    'name',
    'inputChannels',
    'outputChannels',
    'nodes',
    'connections',
    'eventConnections',
    'transport',
  };

  /// The JSON schema of a document (draft 2020-12).
  static const Map<String, Object?> jsonSchema = {
    r'$schema': 'https://json-schema.org/draft/2020-12/schema',
    r'$id': schemaId,
    'title': 'Audanika Audio Engine graph document',
    'description':
        'The buses, nodes, connections and transport settings of one '
        'graph of the Audanika Audio Engine.',
    'type': 'object',
    'required': ['schema'],
    'additionalProperties': false,
    r'$defs': {
      'channels': {
        'description': 'The channel count of each bus.',
        'type': 'array',
        'items': {'type': 'integer', 'minimum': 1},
      },
      'nodeId': {
        'description': 'A node id, or "graph" for the graph itself.',
        'type': 'string',
        'pattern': r'^[A-Za-z][A-Za-z0-9_-]*$',
      },
      'index': {'type': 'integer', 'minimum': 0},
    },
    'properties': {
      'schema': {
        'description': 'The version of this schema.',
        'const': schemaVersion,
      },
      'name': {'description': 'The name of the graph.', 'type': 'string'},
      'inputChannels': {r'$ref': r'#/$defs/channels'},
      'outputChannels': {r'$ref': r'#/$defs/channels'},
      'nodes': {
        'type': 'array',
        'items': {
          'type': 'object',
          'required': ['id', 'type'],
          'additionalProperties': false,
          'properties': {
            'id': {r'$ref': r'#/$defs/nodeId'},
            'type': {
              'description': 'The type id, e.g. aud.graph.oscillator.',
              'type': 'string',
              'pattern': r'^[a-z][a-z0-9_]*(\.[a-z][a-z0-9_]*)+$',
            },
            'inputChannels': {r'$ref': r'#/$defs/channels'},
            'outputChannels': {r'$ref': r'#/$defs/channels'},
            'delayFrames': {
              'description': 'The delay of a feedback node in frames.',
              'type': 'integer',
              'minimum': 0,
            },
            'preset': {
              'description': 'The node preset (aud_node_preset.schema.json).',
              r'$ref': AudNodePreset.schemaId,
            },
          },
        },
      },
      'connections': {
        'type': 'array',
        'items': {
          'type': 'object',
          'required': ['from', 'to'],
          'additionalProperties': false,
          'properties': {
            'from': {r'$ref': r'#/$defs/nodeId'},
            'fromBus': {r'$ref': r'#/$defs/index'},
            'to': {r'$ref': r'#/$defs/nodeId'},
            'toBus': {r'$ref': r'#/$defs/index'},
            'lowLatency': {'type': 'boolean'},
          },
        },
      },
      'eventConnections': {
        'type': 'array',
        'items': {
          'type': 'object',
          'required': ['from', 'to'],
          'additionalProperties': false,
          'properties': {
            'from': {r'$ref': r'#/$defs/nodeId'},
            'fromPort': {r'$ref': r'#/$defs/index'},
            'to': {r'$ref': r'#/$defs/nodeId'},
            'toPort': {r'$ref': r'#/$defs/index'},
          },
        },
      },
      'transport': {
        'type': 'object',
        'additionalProperties': false,
        'properties': {
          'tempo': {'type': 'number', 'exclusiveMinimum': 0},
          'numerator': {'type': 'integer', 'minimum': 1},
          'denominator': {'type': 'integer', 'minimum': 1},
          'loopStart': {'type': 'number'},
          'loopEnd': {'type': 'number'},
        },
      },
    },
  };

  // ...........................................................................
  /// The name of the graph.
  final String name;

  /// The channels of each input bus of the graph.
  final List<int> inputChannels;

  /// The channels of each output bus of the graph.
  final List<int> outputChannels;

  /// The nodes.
  final List<AudGraphDocumentNode> nodes;

  /// The audio connections.
  final List<AudGraphConnection> connections;

  /// The event connections.
  final List<AudGraphEventConnection> eventConnections;

  /// The transport settings.
  final AudGraphTransportSettings transport;

  // ...........................................................................
  /// The problems of the document: duplicate ids, connections to unknown
  /// ids. Empty when the document is consistent.
  List<String> validate() {
    final problems = <String>[];
    final ids = <String>{};
    for (final node in nodes) {
      if (node.id == graphId) problems.add('A node must not be named $graphId');
      if (!ids.add(node.id)) problems.add('Duplicate node id ${node.id}');
    }
    ids.add(graphId);
    for (final c in connections) {
      if (!ids.contains(c.from)) problems.add('Unknown node ${c.from}');
      if (!ids.contains(c.to)) problems.add('Unknown node ${c.to}');
    }
    for (final c in eventConnections) {
      if (!ids.contains(c.from)) problems.add('Unknown node ${c.from}');
      if (!ids.contains(c.to)) problems.add('Unknown node ${c.to}');
    }
    return problems;
  }

  /// The document as JSON.
  Map<String, Object?> toJson() => {
    'schema': schemaVersion,
    if (name.isNotEmpty) 'name': name,
    'inputChannels': inputChannels,
    'outputChannels': outputChannels,
    'nodes': [for (final node in nodes) node.toJson()],
    'connections': [for (final c in connections) c.toJson()],
    'eventConnections': [for (final c in eventConnections) c.toJson()],
    'transport': transport.toJson(),
  };

  /// The document as a JSON text.
  String toJsonText() => jsonEncode(toJson());

  @override
  bool operator ==(Object other) =>
      other is AudGraphDocument && other.toJsonText() == toJsonText();

  @override
  int get hashCode => toJsonText().hashCode;

  @override
  String toString() => 'AudGraphDocument(${toJsonText()})';
}
