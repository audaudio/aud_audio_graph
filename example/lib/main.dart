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

/// Lists the node types of a spike engine and renders one block offline.
class AudGraphExampleApp extends StatefulWidget {
  /// Creates the example app.
  const AudGraphExampleApp({super.key});

  @override
  State<AudGraphExampleApp> createState() => _AudGraphExampleAppState();
}

class _AudGraphExampleAppState extends State<AudGraphExampleApp> {
  final AudEngine _engine = AudEngine();
  late final List<AudNodeTypeInfo> _types = _engine.nodeTypes;
  late final double _peak = _renderPeak();

  double _renderPeak() {
    final sine = _engine.createNode('aud.ref.sine');
    _engine.setChain([sine]);
    return _engine.render(_engine.maxFrames).reduce((a, b) => a > b ? a : b);
  }

  @override
  void dispose() {
    _engine.dispose();
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
                title: Text(type.id),
                subtitle: Text(type.params.map((p) => p.id).join(', ')),
              ),
            Text('Peak of one rendered block: ${_peak.toStringAsFixed(3)}'),
          ],
        ),
      ),
    );
  }
}
