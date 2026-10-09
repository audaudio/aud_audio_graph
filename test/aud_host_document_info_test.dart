// @license
// Copyright (c) Audanika. All Rights Reserved.
//
// Use of this source code is governed by terms that can be
// found in the LICENSE file in the root of this package.

import 'package:aud_audio_graph/aud_audio_graph_ffi.dart';
import 'package:test/test.dart';

void main() {
  group('AudHostDocumentInfo', () {
    test('describes the buses, the counts and the name of a document', () {
      final info = AudHost.inspect(
        '{"schema": 1, "name": "Grüße", "inputChannels": [2, 1], '
        '"outputChannels": [1], "nodes": [], '
        '"assets": [{"id": "a", "path": "a.wav"}]}',
      );
      expect(info.schema, 1);
      expect(info.name, 'Grüße');
      expect(info.inputChannels, [2, 1]);
      expect(info.outputChannels, [1]);
      expect(info.numNodes, 0);
      expect(info.numAssets, 1);
      expect(info.toJson(), {
        'schema': 1,
        'name': 'Grüße',
        'inputChannels': [2, 1],
        'outputChannels': [1],
        'numNodes': 0,
        'numAssets': 1,
      });
      expect(info.toString(), startsWith('AudHostDocumentInfo({schema: 1'));
      const same = AudHostDocumentInfo(
        schema: 1,
        name: 'Grüße',
        inputChannels: [2, 1],
        outputChannels: [1],
        numNodes: 0,
        numAssets: 1,
      );
      expect(same, info);
      expect(same.hashCode, info.hashCode);
      // A document without buses has one stereo output, as the schema says.
      final bare = AudHost.inspect('{"schema": 1}');
      expect(bare.outputChannels, [2]);
      expect(bare.name, isEmpty);
      expect(bare == info, isFalse);
    });
  });
}
