// @license
// Copyright (c) Audanika. All Rights Reserved.
//
// Use of this source code is governed by terms that can be
// found in the LICENSE file in the root of this package.

import 'package:aud_audio_graph/aud_audio_graph_ffi.dart';
// ignore: implementation_imports
import 'package:aud_audio_graph/src/aud_graph_factory_native.dart';
import 'package:test/test.dart';

void main() {
  group('the native graph factory', () {
    test('creates an AudGraphFfi', () {
      final graph = createGraph(
        sampleRate: 44100,
        maxFrames: 128,
        inputChannels: const [],
        outputChannels: const [1],
        options: const AudGraphOptions(),
        id: 2,
        registerCoreNodes: false,
        listen: false,
      );
      addTearDown(graph.dispose);
      expect(graph, isA<AudGraphFfi>());
      expect(graph.sampleRate, 44100);
      expect(graph.id, 2);
      final copy = createGraphFromDocument(
        graph.toDocument(),
        sampleRate: 48000,
        maxFrames: 64,
        options: const AudGraphOptions(),
        id: 3,
        listen: false,
        baseDirectory: null,
      );
      addTearDown(copy.dispose);
      expect(copy.outputChannels, [1]);
      expect(copy.maxFrames, 64);
    });
  });
}
