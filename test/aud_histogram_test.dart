// @license
// Copyright (c) Audanika. All Rights Reserved.
//
// Use of this source code is governed by terms that can be
// found in the LICENSE file in the root of this package.

import 'package:aud_audio_graph/aud_audio_graph.dart';
import 'package:test/test.dart';

void main() {
  group('AudHistogram', () {
    group('percentileNs(fraction)', () {
      test('is null without counts', () {
        const histogram = AudHistogram(counts: [0, 0, 0], binNs: 100);
        expect(histogram.percentileNs(0), isNull);
        expect(histogram.percentileNs(0.5), isNull);
        expect(histogram.percentileNs(1), isNull);
        expect(
          const AudHistogram(counts: [], binNs: 100).percentileNs(0.5),
          isNull,
        );
      });

      test('is the upper edge of the bin that reaches the fraction', () {
        // 10 counts in bin 0, 80 in bin 2, 10 in the overflow bin.
        final counts = List<int>.filled(6, 0)
          ..[0] = 10
          ..[2] = 80
          ..[5] = 10;
        final histogram = AudHistogram(counts: counts, binNs: 1000);
        expect(histogram.percentileNs(0), 1000);
        expect(histogram.percentileNs(0.1), 1000);
        expect(histogram.percentileNs(0.5), 3000);
        expect(histogram.percentileNs(0.9), 3000);
        expect(histogram.percentileNs(0.99), 6000);
        expect(histogram.percentileNs(1), 6000);
      });

      test('reaches the fraction 0 in the first bin with a count', () {
        const histogram = AudHistogram(counts: [0, 7], binNs: 200);
        expect(histogram.percentileNs(0), 400);
        expect(histogram.percentileNs(1), 400);
      });

      test('refuses a fraction outside 0 to 1', () {
        const histogram = AudHistogram(counts: [1], binNs: 100);
        for (final fraction in [-0.1, 1.1]) {
          expect(
            () => histogram.percentileNs(fraction),
            throwsA(
              isA<ArgumentError>()
                  .having((e) => e.name, 'name', 'fraction')
                  .having((e) => e.message, 'message', 'Not between 0 and 1'),
            ),
          );
        }
      });
    });
  });
}
