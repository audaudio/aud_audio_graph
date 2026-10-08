// @license
// Copyright (c) Audanika. All Rights Reserved.
//
// Use of this source code is governed by terms that can be
// found in the LICENSE file in the root of this package.

import 'dart:convert';
import 'dart:io';

import 'package:aud_audio_core/aud_audio_core.dart';
import 'package:aud_audio_graph/aud_audio_graph.dart';
import 'package:test/test.dart';

void main() {
  const document = AudGraphDocument(
    name: 'Demo',
    inputChannels: [1],
    outputChannels: [2],
    nodes: [
      AudGraphDocumentNode(
        id: 'osc',
        typeId: 'aud.graph.oscillator',
        outputChannels: [1],
        preset: AudNodePreset(
          typeId: 'aud.graph.oscillator',
          params: {'frequency': 220},
        ),
      ),
      AudGraphDocumentNode(
        id: 'fb',
        typeId: 'aud.graph.feedback',
        delayFrames: 512,
      ),
      AudGraphDocumentNode(id: 'filter', typeId: 'aud.graph.filter'),
    ],
    connections: [
      AudGraphConnection(from: 'osc', to: 'filter'),
      AudGraphConnection(
        from: 'filter',
        to: 'graph',
        toBus: 0,
        lowLatency: true,
      ),
      AudGraphConnection(from: 'graph', to: 'fb', fromBus: 0),
    ],
    eventConnections: [
      AudGraphEventConnection(from: 'graph', to: 'osc', fromPort: 0, toPort: 0),
    ],
    transport: AudGraphTransportSettings(
      tempo: 100,
      numerator: 3,
      denominator: 4,
      loopStart: 0,
      loopEnd: 4,
    ),
  );

  group('AudGraphDocument', () {
    test('round trips through JSON', () {
      final json = document.toJson();
      expect(json['schema'], 1);
      expect(json['name'], 'Demo');
      expect(AudGraphDocument.fromJson(json), document);
      expect(AudGraphDocument.parse(document.toJsonText()), document);
      expect(
        document.hashCode,
        AudGraphDocument.parse(document.toJsonText()).hashCode,
      );
      expect(document.toString(), startsWith('AudGraphDocument({'));
      expect(document.validate(), isEmpty);
      expect(
        AudGraphDocument.fromJson(const {'schema': 1}),
        const AudGraphDocument(),
      );
      expect(const AudGraphDocument().toJson().containsKey('name'), isFalse);
    });

    test('validate() names duplicate and unknown ids', () {
      const broken = AudGraphDocument(
        nodes: [
          AudGraphDocumentNode(id: 'a', typeId: 'x.y'),
          AudGraphDocumentNode(id: 'a', typeId: 'x.y'),
          AudGraphDocumentNode(id: 'graph', typeId: 'x.y'),
        ],
        connections: [AudGraphConnection(from: 'b', to: 'c')],
        eventConnections: [AudGraphEventConnection(from: 'd', to: 'e')],
      );
      expect(broken.validate(), [
        'Duplicate node id a',
        'A node must not be named graph',
        'Unknown node b',
        'Unknown node c',
        'Unknown node d',
        'Unknown node e',
      ]);
      expect(
        () => AudGraphDocument.fromJson(broken.toJson()),
        throwsA(
          isA<FormatException>().having(
            (e) => e.message,
            'message',
            contains('Duplicate'),
          ),
        ),
      );
    });

    test('fromJson(json) refuses what does not follow the schema', () {
      final cases = <String, Map<String, Object?>>{
        'schema': {'schema': 2},
        'key': {'schema': 1, 'extra': 1},
        'name': {'schema': 1, 'name': 3},
        'channels': {
          'schema': 1,
          'inputChannels': [0],
        },
        'list': {'schema': 1, 'nodes': 'x'},
        'node id': {
          'schema': 1,
          'nodes': [
            {'id': '1x', 'type': 'a.b'},
          ],
        },
        'node type': {
          'schema': 1,
          'nodes': [
            {'id': 'a', 'type': 'nodots'},
          ],
        },
        'node delay': {
          'schema': 1,
          'nodes': [
            {'id': 'a', 'type': 'a.b', 'delayFrames': -1},
          ],
        },
        'node channels': {
          'schema': 1,
          'nodes': [
            {'id': 'a', 'type': 'a.b', 'inputChannels': 'x'},
          ],
        },
        'connection ends': {
          'schema': 1,
          'connections': [
            {'from': 1, 'to': 'a'},
          ],
        },
        'connection bus': {
          'schema': 1,
          'connections': [
            {'from': 'graph', 'to': 'graph', 'toBus': -1},
          ],
        },
        'event ends': {
          'schema': 1,
          'eventConnections': [
            {'from': 'graph'},
          ],
        },
        'event port': {
          'schema': 1,
          'eventConnections': [
            {'from': 'graph', 'to': 'graph', 'fromPort': 'x'},
          ],
        },
        'transport tempo': {
          'schema': 1,
          'transport': {'tempo': 0},
        },
        'transport loop': {
          'schema': 1,
          'transport': {'loopStart': 1},
        },
      };
      for (final entry in cases.entries) {
        expect(
          () => AudGraphDocument.fromJson(entry.value),
          throwsFormatException,
          reason: entry.key,
        );
      }
    });

    test('the schema file matches jsonSchema', () {
      final file = File('doc/schemas/aud_graph_document.schema.json');
      expect(file.existsSync(), isTrue, reason: 'schema file missing');
      expect(jsonDecode(file.readAsStringSync()), AudGraphDocument.jsonSchema);
      expect(AudGraphDocument.jsonSchema[r'$id'], AudGraphDocument.schemaId);
    });
  });

  group(
    'AudGraphDocumentNode, AudGraphConnection, AudGraphEventConnection',
    () {
      test('compare by content and print', () {
        const node = AudGraphDocumentNode(id: 'a', typeId: 'x.y');
        expect(node, const AudGraphDocumentNode(id: 'a', typeId: 'x.y'));
        expect(
          node.hashCode,
          const AudGraphDocumentNode(id: 'a', typeId: 'x.y').hashCode,
        );
        expect(node, isNot(const AudGraphDocumentNode(id: 'b', typeId: 'x.y')));
        expect(node.toString(), 'AudGraphDocumentNode({id: a, type: x.y})');
        expect(node.toJson().containsKey('preset'), isFalse);
        const connection = AudGraphConnection(from: 'a', to: 'b');
        expect(connection, const AudGraphConnection(from: 'a', to: 'b'));
        expect(
          connection.hashCode,
          const AudGraphConnection(from: 'a', to: 'b').hashCode,
        );
        expect(connection.toString(), 'AudGraphConnection({from: a, to: b})');
        const events = AudGraphEventConnection(from: 'a', to: 'b', toPort: 1);
        expect(events, AudGraphEventConnection.fromJson(events.toJson()));
        expect(
          events.hashCode,
          AudGraphEventConnection.fromJson(events.toJson()).hashCode,
        );
        expect(
          events.toString(),
          'AudGraphEventConnection({from: a, to: b, toPort: 1})',
        );
      });
    },
  );

  group('AudGraphTransportSettings', () {
    test('needs a start and an end for a loop', () {
      const settings = AudGraphTransportSettings();
      expect(settings.looping, isFalse);
      expect(settings.toJson(), {
        'tempo': 120.0,
        'numerator': 4,
        'denominator': 4,
      });
      expect(settings, AudGraphTransportSettings.fromJson(const {}));
      expect(
        settings.hashCode,
        AudGraphTransportSettings.fromJson(const {}).hashCode,
      );
      expect(settings.toString(), contains('tempo: 120'));
      expect(document.transport.looping, isTrue);
      expect(
        () => AudGraphTransportSettings(loopStart: 1),
        throwsA(isA<AssertionError>()),
      );
    });
  });
}
