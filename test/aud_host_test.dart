// @license
// Copyright (c) Audanika. All Rights Reserved.
//
// Use of this source code is governed by terms that can be
// found in the LICENSE file in the root of this package.

import 'dart:ffi';
import 'dart:io';
import 'dart:typed_data';

import 'package:aud_audio_core/aud_audio_core_ffi.dart';
import 'package:aud_audio_graph/aud_audio_graph_ffi.dart';
import 'package:test/test.dart';

void main() {
  late AudGraphFfi graph;
  late AudHost host;
  late Directory dir;

  setUp(() {
    dir = Directory.systemTemp.createTempSync('aud_host_test');
    File('${dir.path}/ir.wav').writeAsStringSync('RIFF');
    File('${dir.path}/other.wav').writeAsStringSync('RIFF');
    graph = AudGraphFfi(listen: false, maxFrames: 256);
    host = AudHost(graph, baseDirectory: dir.path);
  });
  tearDown(() {
    host.dispose();
    graph.dispose();
    dir.deleteSync(recursive: true);
  });

  Uint8List gainState(double gain) =>
      Uint8List(4)..buffer.asByteData().setFloat32(0, gain, Endian.host);

  // The reference chain of the docs site with a state blob and an asset.
  AudGraphDocument chain() => AudGraphDocument(
    name: 'Reference chain',
    assets: const [AudGraphAsset(id: 'ir', path: 'ir.wav')],
    nodes: [
      const AudGraphDocumentNode(
        id: 'osc',
        typeId: 'aud.graph.oscillator',
        outputChannels: [2],
        preset: AudNodePreset(
          typeId: 'aud.graph.oscillator',
          params: {'frequency': 220, 'waveform': 1},
        ),
      ),
      const AudGraphDocumentNode(
        id: 'filter',
        typeId: 'aud.graph.filter',
        preset: AudNodePreset(
          typeId: 'aud.graph.filter',
          params: {'cutoff': 800, 'resonance': 0.3},
        ),
      ),
      AudGraphDocumentNode(
        id: 'gain',
        typeId: 'aud.core.gain',
        preset: AudNodePreset(
          typeId: 'aud.core.gain',
          params: const {'gain': 0.8},
          state: gainState(0.8),
          stateVersion: 1,
        ),
      ),
      const AudGraphDocumentNode(id: 'mixer', typeId: 'aud.graph.mixer'),
    ],
    connections: const [
      AudGraphConnection(from: 'osc', to: 'filter'),
      AudGraphConnection(from: 'filter', to: 'gain'),
      AudGraphConnection(from: 'gain', to: 'mixer'),
      AudGraphConnection(from: 'mixer', to: 'graph'),
    ],
    eventConnections: const [AudGraphEventConnection(from: 'graph', to: 'osc')],
  );

  group('AudHost', () {
    test('loads a document and renders it', () {
      expect(host.pointer, isNot(nullptr));
      expect(host.graph, graph);
      expect(host.baseDirectory, dir.path);
      host.loadDocument(chain());
      expect(host.lastError, isEmpty);
      expect(host.nodeIds, ['osc', 'filter', 'gain', 'mixer']);
      final osc = host.nodeHandle('osc')!;
      expect(osc, greaterThan(0));
      expect(host.nodeIdOf(osc), 'osc');
      expect(host.nodeHandle('nothing'), isNull);
      expect(host.nodeIdOf(999), isNull);
      expect(host.latency, 0);
      expect(host.tail, 0);
      graph.start();
      final output = AudOfflineRenderer(graph).render(frames: 4800);
      final peak = output.first.first.fold<double>(
        0,
        (peak, v) => v.abs() > peak ? v.abs() : peak,
      );
      expect(peak, greaterThan(0.01));
    });

    test('works without a base directory', () {
      final bare = AudHost(graph);
      addTearDown(bare.dispose);
      expect(bare.baseDirectory, isNull);
      expect(
        () => bare.loadDocument(chain()),
        throwsA(
          isA<AudGraphException>().having(
            (e) => e.message,
            'message',
            contains('asset ir not found: ir.wav'),
          ),
        ),
      );
    });

    test('sets and reads parameters by their stable ids', () {
      host.loadDocument(chain());
      final params = host.params;
      expect(params, hasLength(3 + 3 + 1 + 9));
      final ids = [for (final p in params) p.id];
      expect(ids, [...ids]..sort());
      final cutoff = AudHost.paramIdOf('filter', 'cutoff');
      final param = host.param(cutoff)!;
      expect(param.nodeId, 'filter');
      expect(param.paramId, 'cutoff');
      expect(param.value, 800);
      expect(param.node, host.nodeHandle('filter'));
      expect(host.param(12345), isNull);
      host.setParam(cutoff, 1200, rampFrames: 64);
      expect(host.getParam(cutoff), 1200);
      expect(
        () => host.setParam(cutoff, 1),
        throwsA(
          isA<AudGraphException>().having(
            (e) => e.code,
            'code',
            AUD_ERROR_INVALID_ARGUMENT,
          ),
        ),
      );
      expect(
        () => host.setParam(12345, 1),
        throwsA(
          isA<AudGraphException>().having(
            (e) => e.code,
            'code',
            AUD_ERROR_NOT_FOUND,
          ),
        ),
      );
      expect(() => host.getParam(12345), throwsA(isA<AudGraphException>()));
      // Pinned: a plugin state of a host depends on it.
      expect(AudHost.paramIdOf('osc', 'frequency'), 0x3697a565);
    });

    test('saves the state and restores it on another graph', () {
      host.loadDocument(chain());
      host.setParam(AudHost.paramIdOf('gain', 'gain'), 0.5);
      graph.start();
      AudOfflineRenderer(graph).render(frames: 256);
      final saved = host.saveDocument();
      expect(saved.name, 'Reference chain');
      expect(saved.assets, chain().assets);
      final gain = saved.nodes.firstWhere((n) => n.id == 'gain').preset!;
      expect(gain.params, {'gain': 0.5});
      expect(gain.state, gainState(0.5));
      final other = AudGraphFfi(listen: false, maxFrames: 256);
      final second = AudHost(other, baseDirectory: dir.path);
      addTearDown(() {
        second.dispose();
        other.dispose();
      });
      second.load(host.save());
      other.start();
      AudOfflineRenderer(other).render(frames: 256);
      expect(second.save(), host.save());
    });

    test('applies presets and reads them back', () {
      host.loadDocument(chain());
      host.applyPreset(
        'gain',
        AudNodePreset(
          typeId: 'aud.core.gain',
          params: const {'gain': 0.25},
          state: gainState(0.25),
          stateVersion: 1,
        ),
      );
      final preset = host.nodePreset('gain');
      expect(preset.params, {'gain': 0.25});
      expect(preset.state, gainState(0.25));
      expect(
        () => host.applyPreset(
          'gain',
          const AudNodePreset(typeId: 'aud.core.gain', params: {'nope': 1}),
        ),
        throwsA(
          isA<AudGraphException>().having(
            (e) => e.message,
            'message',
            contains('unknown param nope of node gain'),
          ),
        ),
      );
      expect(host.lastError, contains('unknown param nope'));
      expect(
        () => host.nodePreset('nothing'),
        throwsA(
          isA<AudGraphException>().having(
            (e) => e.code,
            'code',
            AUD_ERROR_NOT_FOUND,
          ),
        ),
      );
    });

    test('resolves and relinks assets', () {
      host.loadDocument(chain());
      expect(host.assets, [
        AudHostAsset(
          id: 'ir',
          path: 'ir.wav',
          resolved: '${dir.path}/ir.wav',
          exists: true,
        ),
      ]);
      host.setAssetPath('ir', 'other.wav');
      expect(host.assets.single.path, 'other.wav');
      expect(
        () => host.setAssetPath('ir', 'missing.wav'),
        throwsA(
          isA<AudGraphException>().having(
            (e) => e.message,
            'message',
            contains('asset ir not found'),
          ),
        ),
      );
      expect(host.saveDocument().assets.single.path, 'other.wav');
    });

    test('refuses documents that do not fit and keeps the loaded one', () {
      host.loadDocument(chain());
      expect(
        () => host.load('{"schema": 2}'),
        throwsA(
          isA<AudGraphException>().having(
            (e) => e.message,
            'message',
            'Could not load the document: schema must be 1',
          ),
        ),
      );
      expect(host.nodeIds, hasLength(4));
      expect(
        () => host.loadDocument(const AudGraphDocument(outputChannels: [1])),
        throwsA(
          isA<AudGraphException>().having(
            (e) => e.code,
            'code',
            AUD_ERROR_FORMAT,
          ),
        ),
      );
    });

    test('inspect(json) reads a document without a graph', () {
      expect(
        AudHost.inspect(chain().toJsonText()),
        const AudHostDocumentInfo(
          schema: 1,
          name: 'Reference chain',
          inputChannels: [],
          outputChannels: [2],
          numNodes: 4,
          numAssets: 1,
        ),
      );
      expect(() => AudHost.inspect('nope'), throwsA(isA<AudGraphException>()));
    });

    test('dispose() leaves the graph to its owner', () {
      host.loadDocument(chain());
      host.dispose();
      expect(host.isDisposed, isTrue);
      host.dispose();
      expect(graph.isDisposed, isFalse);
    });
  });
}
