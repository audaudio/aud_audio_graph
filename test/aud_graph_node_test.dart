// @license
// Copyright (c) Audanika. All Rights Reserved.
//
// Use of this source code is governed by terms that can be
// found in the LICENSE file in the root of this package.

import 'package:aud_audio_core/aud_audio_core_ffi.dart';
import 'package:aud_audio_graph/aud_audio_graph_ffi.dart';
import 'package:test/test.dart';

void main() {
  group('AudNode', () {
    const node = AudNode(
      handle: 3,
      name: 'filter1',
      descriptor: AudNodeDescriptor(
        typeId: 'aud.graph.filter',
        params: [
          AudParamDescriptor(id: 'cutoff'),
          AudParamDescriptor(id: 'mode'),
        ],
        stringKeys: [AudStringKeyDescriptor(key: 4, id: 'file')],
      ),
      inputChannels: [2],
      outputChannels: [2],
    );

    test('describes the instance', () {
      expect(node.typeId, 'aud.graph.filter');
      expect(node.isGraph, isFalse);
      expect(node.paramIndex('mode'), 1);
      expect(() => node.paramIndex('nothing'), throwsArgumentError);
      expect(node.stringKey('file'), 4);
      expect(() => node.stringKey('nothing'), throwsArgumentError);
      expect(node.toJson(), {
        'handle': 3,
        'name': 'filter1',
        'type': 'aud.graph.filter',
        'inputChannels': [2],
        'outputChannels': [2],
      });
      expect(node.toString(), 'AudNode(filter1, aud.graph.filter, #3)');
    });

    test('compares by handle', () {
      const same = AudNode(
        handle: 3,
        name: 'other',
        descriptor: AudNodeDescriptor(typeId: 'x.y'),
        inputChannels: [],
        outputChannels: [],
      );
      expect(node, same);
      expect(node.hashCode, same.hashCode);
      expect(
        node,
        isNot(
          const AudNode(
            handle: 4,
            name: 'filter1',
            descriptor: AudNodeDescriptor(typeId: 'x.y'),
            inputChannels: [],
            outputChannels: [],
          ),
        ),
      );
    });

    test('the graph is the node 0', () {
      final graph = AudGraphFfi(
        listen: false,
        inputChannels: const [1],
        outputChannels: const [2],
      );
      addTearDown(graph.dispose);
      expect(graph.io.isGraph, isTrue);
      expect(graph.io.name, 'graph');
      expect(graph.io.inputChannels, [2]);
      expect(graph.io.outputChannels, [1]);
      expect(graph.io.descriptor.inputBuses.single.id, 'out0');
      expect(graph.io.descriptor.outputBuses.single.id, 'in0');
      expect(graph.io.descriptor.eventInputs.single.midi, isTrue);
    });
  });
}
