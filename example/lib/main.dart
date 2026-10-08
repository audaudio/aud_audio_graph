// @license
// Copyright (c) Audanika. All Rights Reserved.
//
// Use of this source code is governed by terms that can be
// found in the LICENSE file in the root of this package.

import 'package:aud_audio_graph/aud_audio_graph.dart';
import 'package:flutter/material.dart';

void main() {
  runApp(const AudGraphExampleApp());
}

/// Lists the node types of a graph and renders the reference chain
/// oscillator → filter → gain → output offline.
class AudGraphExampleApp extends StatefulWidget {
  /// Creates the example app.
  const AudGraphExampleApp({super.key});

  @override
  State<AudGraphExampleApp> createState() => _AudGraphExampleAppState();
}

class _AudGraphExampleAppState extends State<AudGraphExampleApp> {
  final AudGraph _graph = AudGraph(maxFrames: 256, outputChannels: const [1]);
  late final _types = _graph.nodeTypes;
  late final double _peak = _renderPeak();

  double _renderPeak() {
    final osc = _graph.createNode('aud.graph.oscillator', name: 'osc');
    final filter = _graph.createNode(
      'aud.graph.filter',
      name: 'filter',
      inputChannels: const [1],
      outputChannels: const [1],
    );
    final gain = _graph.createNode(
      'aud.core.gain',
      name: 'gain',
      inputChannels: const [1],
      outputChannels: const [1],
    );
    _graph.setParam(filter, 'cutoff', 500);
    _graph.setParam(gain, 'gain', 0.8);
    _graph.transaction((tx) {
      tx.connect(osc, filter);
      tx.connect(filter, gain);
      tx.connect(gain, _graph.io);
    });
    _graph.start();
    final output = AudOfflineRenderer(_graph).render(frames: 4800);
    _graph.stop();
    return output.single.single
        .map((v) => v.abs())
        .reduce((a, b) => a > b ? a : b);
  }

  @override
  void dispose() {
    _graph.dispose();
    super.dispose();
  }

  @override
  Widget build(BuildContext context) {
    return MaterialApp(
      home: Scaffold(
        appBar: AppBar(title: const Text('aud_audio_graph')),
        body: ListView(
          padding: const EdgeInsets.all(16),
          children: [
            for (final type in _types)
              ListTile(
                title: Text(type.typeId),
                subtitle: Text(type.params.map((p) => p.id).join(', ')),
              ),
            Text('Peak of the rendered chain: ${_peak.toStringAsFixed(3)}'),
          ],
        ),
      ),
    );
  }
}
