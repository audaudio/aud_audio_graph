// @license
// Copyright (c) Audanika. All Rights Reserved.
//
// Use of this source code is governed by terms that can be
// found in the LICENSE file in the root of this package.

import 'package:aud_audio_core/aud_audio_core.dart';
import 'package:aud_audio_graph/aud_audio_graph.dart';
import 'package:test/test.dart';

void main() {
  group('AudLoad', () {
    const running = AudLoad(id: 1, node: 2, name: 'piano.sfz', progress: 0.5);
    const done = AudLoad(
      id: 3,
      node: 2,
      name: 'piano.sfz',
      result: AUD_OK,
      progress: 1,
      startNs: 1000000,
      endNs: 3000000,
    );
    const failed = AudLoad(
      id: 4,
      node: 2,
      result: AUD_ERROR_FAILED,
      message: 'broken',
      cancelled: true,
    );

    test('says whether it runs, how it ended and how long it took', () {
      expect(running.isRunning, isTrue);
      expect(running.succeeded, isFalse);
      expect(running.resultName, 'AUD_PENDING');
      expect(running.duration, isNull);
      expect(done.isRunning, isFalse);
      expect(done.succeeded, isTrue);
      expect(done.duration, const Duration(milliseconds: 2));
      expect(failed.resultName, 'AUD_ERROR_FAILED');
    });

    test('json round trip and equality', () {
      for (final load in [running, done, failed]) {
        expect(AudLoad.fromJson(load.toJson()), load);
        expect(AudLoad.fromJson(load.toJson()).hashCode, load.hashCode);
      }
      expect(AudLoad.fromJson({'id': 1, 'node': 2}).result, AUD_PENDING);
      expect(running, isNot(done));
      expect(done.toString(), contains('AUD_OK'));
    });
  });
}
