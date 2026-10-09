// @license
// Copyright (c) Audanika. All Rights Reserved.
//
// Use of this source code is governed by terms that can be
// found in the LICENSE file in the root of this package.

import 'package:aud_audio_core/aud_audio_core.dart';
import 'package:aud_audio_graph/aud_audio_graph.dart';
import 'package:test/test.dart';

void main() {
  group('AudGraph', () {
    test('creates the graph of the platform', () {
      final graph = AudGraph(maxFrames: 256, listen: false);
      addTearDown(graph.dispose);
      expect(graph.maxFrames, 256);
      expect(graph.outputChannels, [2]);
      final osc = graph.createNode('aud.graph.oscillator');
      graph.transaction((tx) => tx.connect(osc, graph.io));
      final copy = AudGraph.fromDocument(graph.toDocument(), listen: false);
      addTearDown(copy.dispose);
      expect(copy.nodes.map((n) => n.descriptor.typeId), [
        'aud.graph.oscillator',
      ]);
    });

    test('infiniteTail is the tail of a node that never ends', () {
      expect(AudGraph.infiniteTail, AUD_TAIL_INFINITE);
    });
  });
}
