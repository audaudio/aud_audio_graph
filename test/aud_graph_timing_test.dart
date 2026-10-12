// @license
// Copyright (c) Audanika. All Rights Reserved.
//
// Use of this source code is governed by terms that can be
// found in the LICENSE file in the root of this package.

import 'package:aud_audio_graph/aud_audio_graph.dart';
import 'package:test/test.dart';

void main() {
  group('AudGraphTiming', () {
    test('is invalid by default and converts nothing', () {
      const timing = AudGraphTiming();
      expect(timing.valid, isFalse);
      expect(timing.outputLatencyNs, 0);
      expect(timing.hostTimeAt(100), 0);
      expect(timing.sampleAt(100), 0);
      expect(timing.toJson()['valid'], false);
      expect(timing.toString(), startsWith('AudGraphTiming('));
    });

    test('converts between samples and host time on the last block', () {
      const timing = AudGraphTiming(
        valid: true,
        offline: true,
        sampleRate: 48000,
        samplePosition: 4800,
        hostTimeNs: 1000000000,
        frames: 256,
        outputLatencyFrames: 480,
        maxPeriodNs: 5333333,
        nominalPeriodNs: 5333333,
        timeFilterResets: 1,
      );
      expect(timing.outputLatencyNs, 10000000);
      expect(timing.hostTimeAt(4800 + 48000), 2000000000);
      expect(timing.hostTimeAt(4800 - 48), 999000000);
      expect(timing.sampleAt(1500000000), 4800 + 24000);
      expect(timing.toJson(), {
        'valid': true,
        'offline': true,
        'sampleRate': 48000.0,
        'samplePosition': 4800,
        'hostTimeNs': 1000000000,
        'frames': 256,
        'outputLatencyFrames': 480,
        'maxPeriodNs': 5333333,
        'nominalPeriodNs': 5333333,
        'timeFilterResets': 1,
      });
    });
  });
}
