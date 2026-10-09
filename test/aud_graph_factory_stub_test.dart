// @license
// Copyright (c) Audanika. All Rights Reserved.
//
// Use of this source code is governed by terms that can be
// found in the LICENSE file in the root of this package.

import 'package:aud_audio_graph/aud_audio_graph.dart';
// ignore: implementation_imports
import 'package:aud_audio_graph/src/aud_graph_factory_stub.dart';
import 'package:test/test.dart';

void main() {
  group('the graph factory without the native engine', () {
    test('is unsupported', () {
      expect(
        () => createGraph(
          sampleRate: 48000,
          maxFrames: 128,
          inputChannels: const [],
          outputChannels: const [2],
          options: const AudGraphOptions(),
          id: 1,
          registerCoreNodes: true,
          listen: false,
        ),
        throwsUnsupportedError,
      );
      expect(
        () => createGraphFromDocument(
          const AudGraphDocument(),
          sampleRate: 48000,
          maxFrames: 128,
          options: const AudGraphOptions(),
          id: 1,
          listen: false,
          baseDirectory: null,
        ),
        throwsUnsupportedError,
      );
    });
  });
}
