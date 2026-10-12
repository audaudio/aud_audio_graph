// @license
// Copyright (c) Audanika. All Rights Reserved.
//
// Use of this source code is governed by terms that can be
// found in the LICENSE file in the root of this package.

import 'package:aud_audio_graph/aud_audio_graph.dart';
import 'package:test/test.dart';

void main() {
  group('AudCacheUsage', () {
    const usage = AudCacheUsage(
      caches: 1,
      budget: 1000,
      bytes: 250,
      trims: 2,
      trimmed: 600,
      workers: 2,
      tasks: 1,
    );

    test('relates the bytes to the budget', () {
      expect(usage.load, 0.25);
      expect(const AudCacheUsage().load, 0);
    });

    test('json and equality', () {
      expect(usage.toJson(), {
        'caches': 1,
        'budget': 1000,
        'bytes': 250,
        'trims': 2,
        'trimmed': 600,
        'workers': 2,
        'tasks': 1,
      });
      const same = AudCacheUsage(
        caches: 1,
        budget: 1000,
        bytes: 250,
        trims: 2,
        trimmed: 600,
        workers: 2,
        tasks: 1,
      );
      expect(same, usage);
      expect(same.hashCode, usage.hashCode);
      expect(usage, isNot(const AudCacheUsage()));
      expect(usage.toString(), contains('trimmed'));
    });
  });
}
