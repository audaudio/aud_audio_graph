// @license
// Copyright (c) Audanika. All Rights Reserved.
//
// Use of this source code is governed by terms that can be
// found in the LICENSE file in the root of this package.

import 'dart:io';

import 'package:test/test.dart';

void main() {
  group('scripts/test-native.js', () {
    test('builds and runs the native tests with the sanitizers', () {
      final result = Process.runSync('node', [
        'scripts/test-native.js',
        if (Platform.environment['AUD_NO_SANITIZE'] == '1') '--no-sanitize',
      ]);
      expect(result.exitCode, 0, reason: '${result.stdout}\n${result.stderr}');
      expect(result.stdout, contains('0 checks failed'));
    }, timeout: const Timeout(Duration(minutes: 5)));
  });
}
