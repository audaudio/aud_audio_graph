// @license
// Copyright (c) Audanika. All Rights Reserved.
//
// Use of this source code is governed by terms that can be
// found in the LICENSE file in the root of this package.

import 'dart:io';
import 'dart:isolate';

import 'package:code_assets/code_assets.dart';
import 'package:hooks/hooks.dart';
import 'package:logging/logging.dart';
import 'package:native_toolchain_c/native_toolchain_c.dart';

// Builds the graph engine as C++17 against the header-only ABI of
// aud_audio_core, whose `src` directory is resolved through the package
// config - packages never link the core, they only include it.
//
// The debug watchdog of the realtime thread (ticket 20) compiles in when the
// app asks for it; the Dart SDK builds every hook in release mode, so there
// is no debug mode to key on:
//
//   hooks:
//     user_defines:
//       aud_audio_graph:
//         watchdog: true
void main(List<String> args) async {
  await build(args, (input, output) async {
    if (!input.config.buildCodeAssets) return;
    final packageName = input.packageName;
    final targetOS = input.config.code.targetOS;
    final watchdog = input.userDefines['watchdog'] == true;
    final cbuilder = CBuilder.library(
      name: packageName,
      assetName: 'src/${packageName}_bindings_generated.dart',
      sources: [
        'src/aud_graph.cpp',
        'src/aud_graph_compiler.cpp',
        'src/aud_graph_host.cpp',
        'src/aud_graph_nodes.cpp',
        'src/aud_graph_offline.cpp',
        'src/aud_graph_render.cpp',
        'src/aud_graph_watchdog.cpp',
        'src/aud_json.cpp',
      ],
      includes: ['src', await packageSrcDirectory('aud_audio_core')],
      defines: {if (watchdog) 'AUD_GRAPH_WATCHDOG': '1'},
      language: Language.cpp,
      std: 'c++17',
      cppLinkStdLib: targetOS == OS.android ? 'c++_static' : null,
      libraries: [
        if (targetOS == OS.android) ...['log', 'm'],
      ],
    );
    await cbuilder.run(
      input: input,
      output: output,
      logger: Logger('')
        ..level = Level.ALL
        ..onRecord.listen((record) => stdout.writeln(record.message)),
    );
  });
}

/// The `src` directory of [package], resolved through the package config.
Future<String> packageSrcDirectory(String package) async {
  final lib = await Isolate.resolvePackageUri(Uri.parse('package:$package/'));
  if (lib == null) throw StateError('Package $package is not resolvable');
  return lib.resolve('../src/').toFilePath();
}
