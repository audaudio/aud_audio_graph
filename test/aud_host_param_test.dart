// @license
// Copyright (c) Audanika. All Rights Reserved.
//
// Use of this source code is governed by terms that can be
// found in the LICENSE file in the root of this package.

import 'package:aud_audio_core/aud_audio_core.dart';
import 'package:aud_audio_graph/aud_audio_graph.dart';
import 'package:test/test.dart';

void main() {
  group('AudHostParam', () {
    test('describes a parameter of a loaded document', () {
      final graph = AudGraph(listen: false, maxFrames: 256);
      final host = AudHost(graph);
      addTearDown(() {
        host.dispose();
        graph.dispose();
      });
      host.loadDocument(
        const AudGraphDocument(
          nodes: [
            AudGraphDocumentNode(
              id: 'filter',
              typeId: 'aud.graph.filter',
              preset: AudNodePreset(
                typeId: 'aud.graph.filter',
                params: {'cutoff': 500},
              ),
            ),
          ],
        ),
      );
      final cutoff = host.param(AudHost.paramIdOf('filter', 'cutoff'))!;
      expect(cutoff.nodeId, 'filter');
      expect(cutoff.index, 0);
      expect(cutoff.paramId, 'cutoff');
      expect(cutoff.descriptor.unit, 'Hz');
      expect(cutoff.value, 500);
      expect(cutoff.toJson(), {
        'id': AudHost.paramIdOf('filter', 'cutoff'),
        'node': host.nodeHandle('filter'),
        'nodeId': 'filter',
        'paramId': 'cutoff',
        'index': 0,
        'value': 500.0,
      });
      expect(cutoff.toString(), startsWith('AudHostParam({id: '));
      final same = AudHostParam(
        id: cutoff.id,
        node: cutoff.node,
        nodeId: cutoff.nodeId,
        index: cutoff.index,
        descriptor: cutoff.descriptor,
        value: cutoff.value,
      );
      expect(same, cutoff);
      expect(same.hashCode, cutoff.hashCode);
      final other = AudHostParam(
        id: cutoff.id,
        node: cutoff.node,
        nodeId: cutoff.nodeId,
        index: cutoff.index,
        descriptor: cutoff.descriptor,
        value: 600,
      );
      expect(other == cutoff, isFalse);
    });
  });
}
