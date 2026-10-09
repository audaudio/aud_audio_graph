// @license
// Copyright (c) Audanika. All Rights Reserved.
//
// Use of this source code is governed by terms that can be
// found in the LICENSE file in the root of this package.

import 'package:aud_audio_graph/aud_audio_graph_ffi.dart';
import 'package:test/test.dart';

void main() {
  group('AudGraphState', () {
    test('fromCode(code) maps every code', () {
      for (final state in AudGraphState.values) {
        expect(AudGraphState.fromCode(state.code), state);
      }
      expect(AudGraphState.values.map((s) => s.code), [0, 1, 2, 3, 4, 5]);
      expect(() => AudGraphState.fromCode(9), throwsArgumentError);
    });
  });
}
