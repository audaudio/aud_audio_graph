// @license
// Copyright (c) Audanika. All Rights Reserved.
//
// Use of this source code is governed by terms that can be
// found in the LICENSE file in the root of this package.

import 'package:aud_audio_graph/aud_audio_graph_ffi.dart';
import 'package:test/test.dart';

void main() {
  late AudGraphFfi graph;
  late AudNode osc;
  late AudNode filter;

  setUp(() {
    graph = AudGraphFfi(listen: false, maxFrames: 256);
    osc = graph.createNode('aud.graph.oscillator', name: 'osc');
    filter = graph.createNode('aud.graph.filter', name: 'filter');
  });
  tearDown(() => graph.dispose());

  group('AudGraphTransaction', () {
    test('records the edits it made', () {
      late AudGraphTransaction recorded;
      final revision = graph.transaction((tx) {
        recorded = tx;
        tx.connect(osc, filter);
        tx.connect(filter, graph.io, lowLatency: true);
        tx.connectEvents(graph.io, osc);
        tx.disconnectEvents(graph.io, osc);
        tx.disconnect(osc, filter);
        tx.connect(osc, filter);
      });
      expect(revision, 1);
      expect(recorded.graph, graph);
      expect(recorded.connected, [
        (
          from: osc.handle,
          fromBus: 0,
          to: filter.handle,
          toBus: 0,
          lowLatency: false,
        ),
        (from: filter.handle, fromBus: 0, to: 0, toBus: 0, lowLatency: true),
        (
          from: osc.handle,
          fromBus: 0,
          to: filter.handle,
          toBus: 0,
          lowLatency: false,
        ),
      ]);
      expect(recorded.disconnected.single.to, filter.handle);
      expect(recorded.eventsConnected.single, (
        from: 0,
        fromPort: 0,
        to: osc.handle,
        toPort: 0,
      ));
      expect(recorded.eventsDisconnected, recorded.eventsConnected);
      expect(recorded.removed, isEmpty);
      expect(graph.connections, hasLength(2));
      expect(graph.eventConnections, isEmpty);
    });

    test('removes a node', () {
      graph.transaction((tx) => tx.connect(osc, filter));
      late AudGraphTransaction recorded;
      graph.transaction((tx) {
        recorded = tx;
        tx.remove(filter);
      });
      expect(recorded.removed, [filter]);
      expect(graph.nodes, [osc]);
      expect(graph.connections, isEmpty);
    });

    test('throws for what the engine refuses', () {
      expect(
        () => graph.transaction((tx) => tx.disconnect(osc, filter)),
        throwsA(
          isA<AudGraphException>()
              .having((e) => e.name, 'name', 'AUD_ERROR_NOT_FOUND')
              .having(
                (e) => e.message,
                'message',
                contains('disconnect osc:0 from filter:0'),
              ),
        ),
      );
      expect(
        () => graph.transaction((tx) => tx.disconnectEvents(osc, filter)),
        throwsA(
          isA<AudGraphException>().having(
            (e) => e.name,
            'name',
            'AUD_ERROR_NOT_FOUND',
          ),
        ),
      );
      expect(
        () => graph.transaction(
          (tx) => tx.connectEvents(osc, filter, fromPort: 3),
        ),
        throwsA(
          isA<AudGraphException>().having(
            (e) => e.name,
            'name',
            'AUD_ERROR_INVALID_ARGUMENT',
          ),
        ),
      );
      expect(
        () => graph.transaction((tx) => tx.remove(graph.io)),
        throwsA(
          isA<AudGraphException>().having(
            (e) => e.name,
            'name',
            'AUD_ERROR_INVALID_ARGUMENT',
          ),
        ),
      );
      expect(
        () => graph.transaction((tx) => tx.connect(osc, filter, toBus: 9)),
        throwsA(
          isA<AudGraphException>().having(
            (e) => e.message,
            'message',
            contains('connect osc:0 to filter:9'),
          ),
        ),
      );
    });
  });
}
