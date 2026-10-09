// @license
// Copyright (c) Audanika. All Rights Reserved.
//
// Use of this source code is governed by terms that can be
// found in the LICENSE file in the root of this package.

import 'package:aud_audio_core/aud_audio_core.dart';
import 'package:aud_audio_graph/aud_audio_graph.dart';
import 'package:test/test.dart';

void main() {
  group('AudGraphNotification', () {
    test('every type has JSON, a string and its OSC notification', () {
      const revision = AudRevisionAdoptedNotification(
        revision: 3,
        samplePosition: 512,
      );
      expect(revision.toJson()['revision'], 3);
      expect(revision.toString(), contains('AudRevisionAdoptedNotification'));
      expect(revision.toOsc(1), isA<AudRevisionAdopted>());

      const state = AudStateNotification(
        state: AudGraphState.running,
        samplePosition: 0,
      );
      expect(state.toJson()['state'], 'running');
      expect(state.toOsc(1), isNull);

      const done = AudNodeDoneNotification(node: 4, samplePosition: 0);
      expect(done.toJson()['node'], 4);
      expect(done.toOsc(1), isA<AudNodeRetired>());

      const event = AudEventNotification(
        node: 2,
        event: AudParamEvent(paramIndex: 1, value: 0.5),
        samplePosition: 0,
      );
      expect(event.toJson()['node'], 2);

      final transport = AudTransportNotification(
        type: AudTransportRequestType.start,
        beatTicks: AudBeats.ticks(2),
        tempo: 120,
        playing: true,
        samplePosition: 0,
      );
      expect(transport.beat, 2);
      expect(transport.toJson()['request'], 'start');

      const reset = AudTimeResetNotification(count: 1, samplePosition: 0);
      expect(reset.toJson()['count'], 1);
    });

    test('diagnostics name their code and explain it', () {
      const messages = {
        AUD_ERROR_LATE: 'late',
        AUD_ERROR_RETIRED: 'retired',
        AUD_ERROR_QUEUE_FULL: 'capacity',
        AUD_ERROR_CAPACITY: 'note tracker',
        AUD_ERROR_STATE: 'parked',
        AUD_ERROR_OVERLOAD: 'longer than it lasts',
        AUD_ERROR_UNSUPPORTED: 'transport',
        AUD_GRAPH_ERROR_REALTIME_VIOLATION: 'allocated',
        AUD_ERROR_FAILED: 'AUD_ERROR_FAILED',
      };
      for (final MapEntry(key: code, value: text) in messages.entries) {
        final diagnostic = AudDiagnosticNotification(
          code: code,
          count: 2,
          samplePosition: 0,
        );
        expect(diagnostic.message, contains(text));
        expect(diagnostic.toJson()['code'], code);
      }
      expect(
        const AudDiagnosticNotification(
          code: AUD_GRAPH_ERROR_REALTIME_VIOLATION,
          count: 1,
          samplePosition: 0,
        ).name,
        'AUD_GRAPH_ERROR_REALTIME_VIOLATION',
      );
      final osc = const AudDiagnosticNotification(
        code: AUD_ERROR_LATE,
        count: 3,
        node: 5,
        samplePosition: 0,
      ).toOsc(1);
      expect((osc as AudDiagnostic).address, isNotNull);
      expect(
        (const AudDiagnosticNotification(
                  code: AUD_ERROR_LATE,
                  count: 3,
                  samplePosition: 0,
                ).toOsc(1)
                as AudDiagnostic)
            .address,
        isNull,
      );
    });
  });
}
