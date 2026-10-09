// @license
// Copyright (c) Audanika. All Rights Reserved.
//
// Use of this source code is governed by terms that can be
// found in the LICENSE file in the root of this package.

import 'dart:io';

import 'package:test/test.dart';

void main() {
  group('aud_audio_graph.dart', () {
    test('compiles for the web without dart:ffi (web-001)', () async {
      final out = await Directory.systemTemp.createTemp('aud_web_');
      try {
        final result = await Process.run('dart', [
          'compile',
          'js',
          '-o',
          '${out.path}/main.js',
          'test/fixtures/web/main.dart',
        ]);
        expect(result.exitCode, 0, reason: '${result.stdout}${result.stderr}');
      } finally {
        await out.delete(recursive: true);
      }
    });
  });
}
