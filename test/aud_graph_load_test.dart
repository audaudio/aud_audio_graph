// @license
// Copyright (c) Audanika. All Rights Reserved.
//
// Use of this source code is governed by terms that can be
// found in the LICENSE file in the root of this package.

import 'package:aud_audio_graph/aud_audio_graph.dart';
import 'package:test/test.dart';

void main() {
  group('AudGraphLoad', () {
    const load = AudGraphLoad(
      window: Duration(milliseconds: 1000),
      renders: 188,
      frames: 48128,
      mean: 0.25,
      p99: 0.5,
      max: 1.25,
      rendersTotal: 5000,
      overloadsTotal: 2,
      bufferFrames: 384,
      bufferOverloads: 1,
      bufferP99: 0.75,
      bufferMax: 1.05,
    );

    test('is an empty window by default', () {
      const empty = AudGraphLoad();
      expect(empty.window, Duration.zero);
      expect(empty.renders, 0);
      expect(empty.frames, 0);
      expect(empty.mean, 0);
      expect(empty.p99, 0);
      expect(empty.max, 0);
      expect(empty.rendersTotal, 0);
      expect(empty.overloadsTotal, 0);
      expect(empty.bufferFrames, 0);
      expect(empty.bufferOverloads, 0);
      expect(empty.bufferP99, 0);
      expect(empty.bufferMax, 0);
    });

    test('keeps the last 1024 renders', () {
      expect(AudGraphLoad.maxRenders, 1024);
      expect(AudGraphLoad.maxRenders, AUD_GRAPH_LOAD_RENDERS);
    });

    test('json and equality', () {
      expect(load.toJson(), {
        'windowUs': 1000000,
        'renders': 188,
        'frames': 48128,
        'mean': 0.25,
        'p99': 0.5,
        'max': 1.25,
        'rendersTotal': 5000,
        'overloadsTotal': 2,
        'bufferFrames': 384,
        'bufferOverloads': 1,
        'bufferP99': 0.75,
        'bufferMax': 1.05,
      });
      const same = AudGraphLoad(
        window: Duration(seconds: 1),
        renders: 188,
        frames: 48128,
        mean: 0.25,
        p99: 0.5,
        max: 1.25,
        rendersTotal: 5000,
        overloadsTotal: 2,
        bufferFrames: 384,
        bufferOverloads: 1,
        bufferP99: 0.75,
        bufferMax: 1.05,
      );
      expect(same, load);
      expect(same.hashCode, load.hashCode);
      expect(load, isNot(const AudGraphLoad()));
      expect(load, isNot(const AudGraphLoad(bufferMax: 1.05)));
      expect(load.toString(), startsWith('AudGraphLoad('));
      expect(load.toString(), contains('overloadsTotal'));
    });
  });
}
