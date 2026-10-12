// @license
// Copyright (c) Audanika. All Rights Reserved.
//
// Use of this source code is governed by terms that can be
// found in the LICENSE file in the root of this package.

import 'package:aud_audio_core/aud_audio_core.dart';
import 'package:aud_audio_graph/aud_audio_graph.dart';
import 'package:test/test.dart';

void main() {
  group('AudNodeMessage', () {
    final message = AudNodeMessage(
      node: 2,
      message: AudOscMessage('/num_regions', [3]),
      samplePosition: 512,
    );

    test('carries the path and the arguments below the node route', () {
      expect(message.path, '/num_regions');
      expect(message.arguments, [3]);
      expect(
        message.toOsc(1),
        AudOscMessage('/graph/1/node/2/msg/num_regions', [3]),
      );
    });

    test('json and equality', () {
      expect(message.toJson(), {
        'node': 2,
        'message': {
          'address': '/num_regions',
          'arguments': [
            {'i': 3},
          ],
        },
        'samplePosition': 512,
      });
      final same = AudNodeMessage(
        node: 2,
        message: AudOscMessage('/num_regions', [3]),
        samplePosition: 512,
      );
      expect(same, message);
      expect(same.hashCode, message.hashCode);
      expect(
        message,
        isNot(AudNodeMessage(node: 3, message: AudOscMessage('/x'))),
      );
      expect(message.toString(), contains('/num_regions'));
    });
  });
}
