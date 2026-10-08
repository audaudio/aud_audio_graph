// @license
// Copyright (c) Audanika. All Rights Reserved.
//
// Use of this source code is governed by terms that can be
// found in the LICENSE file in the root of this package.

import 'package:aud_audio_graph/aud_audio_graph.dart';
import 'package:test/test.dart';

void main() {
  group('AudGraphStats', () {
    test('counts the blocks of an offline render', () {
      final graph = AudGraph(listen: false, maxFrames: 256);
      addTearDown(graph.dispose);
      final osc = graph.createNode('aud.graph.oscillator');
      graph.transaction((tx) => tx.connect(osc, graph.io));
      graph.start();
      AudOfflineRenderer(graph).render(frames: 1000, blockFrames: 256);
      final stats = graph.stats;
      expect(stats.state, AudGraphState.running);
      expect(stats.revision, 1);
      expect(stats.scheduled, 0);
      expect(stats.blocksRendered, 4);
      expect(stats.framesRendered, 1000);
      expect(stats.renderTimeMaxNs, greaterThan(0));
      expect(
        stats.renderTimeSumNs,
        greaterThanOrEqualTo(stats.renderTimeMaxNs),
      );
      expect(stats.renderTimeMeanNs, stats.renderTimeSumNs / 4);
      expect(stats.eventsDelivered, 0);
      expect(stats.eventsLate, 0);
      expect(stats.eventsDropped, 0);
      expect(stats.paramsApplied, 0);
      expect(stats.rejected, 0);
      expect(stats.notificationsDropped, 0);
      expect(stats.overloads, 0);
      expect(stats.timeFilterResets, 0);
      expect(stats.outputPeak, closeTo(0.5, 0.01));
      expect(stats.toJson()['blocksRendered'], 4);
      expect(stats.toString(), contains('framesRendered: 1000'));
      graph.resetStats();
      final reset = graph.stats;
      expect(reset.blocksRendered, 0);
      expect(reset.renderTimeMeanNs, 0);
      expect(reset.outputPeak, 0);
    });
  });
}
