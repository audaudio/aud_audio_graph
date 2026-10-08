// @license
// Copyright (c) Audanika. All Rights Reserved.
//
// Use of this source code is governed by terms that can be
// found in the LICENSE file in the root of this package.

import 'dart:ffi';

import 'package:aud_audio_graph/aud_audio_graph.dart';
import 'package:aud_audio_graph/src/aud_audio_graph_bindings_generated.dart'
    as bindings;
import 'package:ffi/ffi.dart';
import 'package:test/test.dart';

void main() {
  group('aud_audio_graph_bindings_generated.dart', () {
    test('aud_engine_io_render renders through the engine handle', () {
      final engine = AudEngine(maxFrames: 64);
      addTearDown(engine.dispose);
      final sine = engine.createNode('aud.ref.sine');
      engine.setChain([sine]);
      final output = calloc<Float>(128);
      try {
        bindings.aud_engine_io_render(engine.handle, output, 64, 3);
        expect(engine.stats.blocksRendered, 0, reason: 'wrong channel count');
        bindings.aud_engine_io_render(engine.handle, output, 64, 2);
        expect(engine.stats.blocksRendered, 1);
        expect(output.asTypedList(128).any((s) => s != 0), isTrue);
        bindings.aud_engine_io_render(nullptr, output, 64, 2);
      } finally {
        calloc.free(output);
      }
    });

    test('null engines are tolerated by the C API', () {
      final stats = calloc<bindings.AudEngineStats>();
      try {
        expect(bindings.aud_engine_create(nullptr), nullptr);
        expect(bindings.aud_engine_host_api(nullptr), nullptr);
        expect(bindings.aud_engine_num_node_types(nullptr), 0);
        expect(bindings.aud_engine_node_type_id(nullptr, 0), nullptr);
        expect(bindings.aud_engine_node_type_name(nullptr, 0), nullptr);
        expect(bindings.aud_engine_node_type_capabilities(nullptr, 0), 0);
        expect(
          bindings.aud_engine_node_type_num_params(nullptr, 0),
          lessThan(0),
        );
        expect(bindings.aud_engine_create_node(nullptr, nullptr), lessThan(0));
        expect(bindings.aud_engine_set_chain(nullptr, nullptr, 1), lessThan(0));
        bindings.aud_engine_render(nullptr, nullptr, 0);
        bindings.aud_engine_get_stats(nullptr, stats);
        bindings.aud_engine_reset_stats(nullptr);
        bindings.aud_engine_destroy(nullptr);
      } finally {
        calloc.free(stats);
      }
    });
  });
}
