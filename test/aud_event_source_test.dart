// @license
// Copyright (c) Audanika. All Rights Reserved.
//
// Use of this source code is governed by terms that can be
// found in the LICENSE file in the root of this package.

import 'package:aud_audio_graph/aud_audio_graph.dart';
import 'package:test/test.dart';

void main() {
  group('AudSourceTiming', () {
    test('carries the codes of the engine', () {
      expect(AudSourceTiming.fixed.code, 0);
      expect(AudSourceTiming.immediate.code, 1);
      expect(AudSourceTiming.fromCode(1), AudSourceTiming.immediate);
      expect(() => AudSourceTiming.fromCode(7), throwsStateError);
    });
  });

  group('AudEventSourceStats', () {
    test('has defaults, JSON and a string', () {
      const stats = AudEventSourceStats();
      expect(stats.attached, isFalse);
      expect(stats.pushed, 0);
      expect(stats.delayMedianNs, isNull);
      expect(stats.delayP99Ns, isNull);
      expect(stats.latencyMedianNs, isNull);
      expect(stats.latencyP99Ns, isNull);
      expect(stats.toJson(), {
        'attached': false,
        'pushed': 0,
        'rejected': 0,
        'delivered': 0,
        'late': 0,
        'maxLateNs': 0,
        'dropped': 0,
        'reordered': 0,
        'releases': 0,
        'maxDelayNs': 0,
        'lastWord0': 0,
        'lastTimestampNs': 0,
        'delayMedianNs': null,
        'delayP99Ns': null,
        'latencyMedianNs': null,
        'latencyP99Ns': null,
      });
      expect(stats.toString(), startsWith('AudEventSourceStats('));
      expect(AudEventSourceStats.delayBinNs, 50000);
      expect(AudEventSourceStats.delayBins, 500);
      expect(AudEventSourceStats.latencyBinNs, 250000);
      expect(AudEventSourceStats.latencyBins, 1024);
    });

    test('takes percentiles from the histograms', () {
      // 10 packets in bin 0, 80 in bin 2, 10 in the overflow bin.
      final delay = List.filled(AudEventSourceStats.delayBins + 1, 0)
        ..[0] = 10
        ..[2] = 80
        ..[AudEventSourceStats.delayBins] = 10;
      final latency = List.filled(AudEventSourceStats.latencyBins + 1, 0)
        ..[4] = 1;
      final stats = AudEventSourceStats(
        pushed: 100,
        delayHistogram: delay,
        latencyHistogram: latency,
      );
      expect(stats.delayPercentileNs(0), 50000);
      expect(stats.delayPercentileNs(0.1), 50000);
      expect(stats.delayMedianNs, 150000);
      expect(stats.delayPercentileNs(0.9), 150000);
      expect(stats.delayP99Ns, 501 * 50000);
      expect(stats.delayPercentileNs(1), 501 * 50000);
      expect(stats.latencyMedianNs, 5 * 250000);
      expect(stats.latencyP99Ns, 5 * 250000);
      expect(stats.toJson()['delayMedianNs'], 150000);
      expect(() => stats.delayPercentileNs(1.5), throwsArgumentError);
      expect(() => stats.latencyPercentileNs(-0.1), throwsArgumentError);
    });
  });
}
