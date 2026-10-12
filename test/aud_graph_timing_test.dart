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
      expect(timing.periodHistogram, isEmpty);
      expect(timing.periodPercentileNs(0.5), isNull);
      expect(timing.toJson()['valid'], false);
      expect(timing.toJson()['periodP999Ns'], isNull);
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
        periodHistogram: [0, 0, 0, 3],
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
        'periodMedianNs': 400000,
        'periodP99Ns': 400000,
        'periodP999Ns': 400000,
      });
    });

    group('periodPercentileNs(fraction)', () {
      test('has bins of 100 us up to 51.2 ms', () {
        expect(AudGraphTiming.periodBinNs, 100000);
        expect(AudGraphTiming.periodBins, 512);
      });

      test('is null without renders', () {
        const timing = AudGraphTiming(periodHistogram: [0, 0, 0]);
        expect(timing.periodPercentileNs(0), isNull);
        expect(timing.periodPercentileNs(0.999), isNull);
        expect(timing.periodMedianNs, isNull);
        expect(timing.periodP99Ns, isNull);
        expect(timing.periodP999Ns, isNull);
      });

      test('is the upper edge of the bin that reaches the fraction', () {
        // 1000 renders of 5.33 ms (bin 53) and one gap of 177 ms in the
        // overflow bin: p99.9 is the callback, the fraction 1 the overflow.
        final histogram = List<int>.filled(513, 0)
          ..[53] = 1000
          ..[512] = 1;
        final timing = AudGraphTiming(periodHistogram: histogram);
        expect(timing.periodPercentileNs(0), 5400000);
        expect(timing.periodMedianNs, 5400000);
        expect(timing.periodP99Ns, 5400000);
        expect(timing.periodP999Ns, 5400000);
        expect(timing.periodPercentileNs(1), 513 * 100000);
      });

      test('takes the longest gap of split callbacks', () {
        // Two renders back to back, then 10.7 ms: half the gaps are near
        // zero, the high percentiles are the callback.
        final histogram = List<int>.filled(513, 0)
          ..[0] = 20
          ..[107] = 19;
        final timing = AudGraphTiming(periodHistogram: histogram);
        expect(timing.periodMedianNs, 100000);
        expect(timing.periodP99Ns, 10800000);
        expect(timing.periodP999Ns, 10800000);
      });

      test('reads a single bin', () {
        const timing = AudGraphTiming(periodHistogram: [0, 7]);
        expect(timing.periodPercentileNs(0.5), 200000);
        expect(timing.periodPercentileNs(1), 200000);
      });

      test('refuses a fraction outside 0 to 1', () {
        const timing = AudGraphTiming(periodHistogram: [1]);
        expect(() => timing.periodPercentileNs(-0.1), throwsArgumentError);
        expect(() => timing.periodPercentileNs(1.1), throwsArgumentError);
      });
    });
  });
}
