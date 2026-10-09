// @license
// Copyright (c) Audanika. All Rights Reserved.
//
// Use of this source code is governed by terms that can be
// found in the LICENSE file in the root of this package.

import 'dart:io';

import 'package:aud_audio_graph/aud_audio_graph_ffi.dart';
import 'package:test/test.dart';

void main() {
  group('AudHostAsset', () {
    test('describes an asset of a loaded document', () {
      final dir = Directory.systemTemp.createTempSync('aud_host_asset');
      File('${dir.path}/ir.wav').writeAsStringSync('RIFF');
      final graph = AudGraphFfi(listen: false, maxFrames: 256);
      final host = AudHost(graph, baseDirectory: dir.path);
      addTearDown(() {
        host.dispose();
        graph.dispose();
        dir.deleteSync(recursive: true);
      });
      host.loadDocument(
        const AudGraphDocument(
          assets: [AudGraphAsset(id: 'ir', path: 'ir.wav')],
        ),
      );
      final asset = host.assets.single;
      expect(asset.id, 'ir');
      expect(asset.path, 'ir.wav');
      expect(asset.resolved, '${dir.path}/ir.wav');
      expect(asset.exists, isTrue);
      expect(asset.toJson(), {
        'id': 'ir',
        'path': 'ir.wav',
        'resolved': '${dir.path}/ir.wav',
        'exists': true,
      });
      expect(asset.toString(), startsWith('AudHostAsset({id: ir'));
      final same = AudHostAsset(
        id: 'ir',
        path: 'ir.wav',
        resolved: '${dir.path}/ir.wav',
        exists: true,
      );
      expect(same, asset);
      expect(same.hashCode, asset.hashCode);
      expect(
        asset ==
            const AudHostAsset(
              id: 'ir',
              path: 'ir.wav',
              resolved: 'ir.wav',
              exists: true,
            ),
        isFalse,
      );
    });
  });
}
