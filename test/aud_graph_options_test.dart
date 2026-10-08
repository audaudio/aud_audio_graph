// @license
// Copyright (c) Audanika. All Rights Reserved.
//
// Use of this source code is governed by terms that can be
// found in the LICENSE file in the root of this package.

import 'dart:ffi';

import 'package:aud_audio_graph/aud_audio_graph.dart';
import 'package:aud_audio_graph/src/aud_audio_graph_bindings_generated.dart'
    as bindings;
import 'package:ffi/ffi.dart';
import 'package:test/test.dart';

void main() {
  group('AudGraphOptions', () {
    const options = AudGraphOptions(
      maxNodes: 4,
      maxConnections: 5,
      paramQueueCapacity: 6,
      eventQueueCapacity: 7,
      schedulerCapacity: 8,
      notificationCapacity: 9,
      maxEventsPerBlock: 10,
      fadeFrames: 11,
      maxTailFrames: 12,
      lookahead: Duration(seconds: 2),
      dropLateEvents: true,
    );

    test('round trip through JSON', () {
      expect(AudGraphOptions.fromJson(options.toJson()), options);
      expect(AudGraphOptions.fromJson(const {}), const AudGraphOptions());
      expect(
        options.hashCode,
        AudGraphOptions.fromJson(options.toJson()).hashCode,
      );
      expect(options.toString(), contains('maxNodes: 4'));
    });

    test('writeTo(config) fills the native configuration', () {
      final config = calloc<bindings.AudGraphConfig>();
      try {
        options.writeTo(config.ref);
        expect(config.ref.flags, bindings.AUD_GRAPH_DROP_LATE_EVENTS);
        expect(config.ref.max_nodes, 4);
        expect(config.ref.max_connections, 5);
        expect(config.ref.param_queue_capacity, 6);
        expect(config.ref.event_queue_capacity, 7);
        expect(config.ref.scheduler_capacity, 8);
        expect(config.ref.notification_capacity, 9);
        expect(config.ref.max_events_per_block, 10);
        expect(config.ref.fade_frames, 11);
        expect(config.ref.max_tail_frames, 12);
        expect(config.ref.lookahead_ns, 2000000000);
        const AudGraphOptions().writeTo(config.ref);
        expect(config.ref.flags, 0);
        expect(config.ref.lookahead_ns, 0);
      } finally {
        calloc.free(config);
      }
    });

    test('shapes a graph', () {
      final graph = AudGraph(
        listen: false,
        options: const AudGraphOptions(maxNodes: 1, dropLateEvents: true),
      );
      addTearDown(graph.dispose);
      graph.createNode('aud.graph.oscillator');
      expect(
        () => graph.createNode('aud.graph.oscillator'),
        throwsA(
          isA<AudGraphException>().having(
            (e) => e.name,
            'name',
            'AUD_ERROR_CAPACITY',
          ),
        ),
      );
    });
  });
}
