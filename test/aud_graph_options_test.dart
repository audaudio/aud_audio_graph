// @license
// Copyright (c) Audanika. All Rights Reserved.
//
// Use of this source code is governed by terms that can be
// found in the LICENSE file in the root of this package.

import 'package:aud_audio_graph/aud_audio_graph_ffi.dart';
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

    test('shapes a graph', () {
      final graph = AudGraphFfi(
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
