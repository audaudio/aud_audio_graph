// @license
// Copyright (c) Audanika. All Rights Reserved.
//
// Use of this source code is governed by terms that can be
// found in the LICENSE file in the root of this package.

import 'package:aud_audio_core/aud_audio_core.dart';
import 'package:aud_audio_graph/aud_audio_graph.dart';
import 'package:test/test.dart';

void main() {
  group('AudGraphException', () {
    test('names its code', () {
      const exception = AudGraphException(AUD_ERROR_CYCLE, 'A loop');
      expect(exception.name, 'AUD_ERROR_CYCLE');
      expect(
        exception.toString(),
        'AudGraphException(AUD_ERROR_CYCLE): A loop',
      );
    });

    group('check(result, what)', () {
      test('returns a result that is no error', () {
        expect(AudGraphException.check(0, 'x'), 0);
        expect(AudGraphException.check(7, 'x'), 7);
      });

      test('throws for an error code', () {
        expect(
          () => AudGraphException.check(AUD_ERROR_QUEUE_FULL, 'send'),
          throwsA(
            isA<AudGraphException>()
                .having((e) => e.code, 'code', AUD_ERROR_QUEUE_FULL)
                .having(
                  (e) => e.message,
                  'message',
                  'Could not send: AUD_ERROR_QUEUE_FULL',
                ),
          ),
        );
      });
    });
  });
}
