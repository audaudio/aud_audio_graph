// @license
// Copyright (c) Audanika. All Rights Reserved.
//
// Use of this source code is governed by terms that can be
// found in the LICENSE file in the root of this package.

import 'dart:io';
import 'dart:typed_data';

import 'package:aud_audio_core/aud_audio_core_ffi.dart';
import 'package:aud_audio_graph/aud_audio_graph_ffi.dart';
import 'package:aud_midi_standard/aud_midi_standard.dart';
import 'package:test/test.dart';

void main() {
  group('AudOfflineRenderer', () {
    test('passes the input to the output in any block sizes', () {
      final graph = AudGraphFfi(
        listen: false,
        maxFrames: 256,
        inputChannels: const [1],
        outputChannels: const [1],
      );
      addTearDown(graph.dispose);
      graph.transaction((tx) => tx.connect(graph.io, graph.io));
      final renderer = AudOfflineRenderer(graph);
      expect(
        () => renderer.render(frames: 10),
        throwsA(
          isA<AudGraphException>().having(
            (e) => e.name,
            'name',
            'AUD_ERROR_STATE',
          ),
        ),
      );
      graph.start();
      final input = Float32List.fromList([
        for (var i = 0; i < 1000; i++) i / 1000,
      ]);
      final output = renderer.render(
        frames: 1000,
        blockSizes: const [100, 37, 256, 1],
        inputs: [
          [input],
        ],
      );
      expect(output.single.single, input);
      expect(
        renderer.render(frames: 100, blockFrames: 50).single.single,
        everyElement(0),
      );
      expect(renderer.render(frames: 0), [
        [[]],
      ]);
    });

    test('checks the inputs', () {
      final graph = AudGraphFfi(listen: false, inputChannels: const [2]);
      addTearDown(graph.dispose);
      graph.start();
      final renderer = AudOfflineRenderer(graph);
      expect(() => renderer.render(frames: 4, inputs: []), throwsArgumentError);
      expect(
        () => renderer.render(
          frames: 4,
          inputs: [
            [Float32List(4)],
          ],
        ),
        throwsArgumentError,
      );
      expect(
        () => renderer.render(
          frames: 4,
          inputs: [
            [Float32List(4), Float32List(3)],
          ],
        ),
        throwsArgumentError,
      );
    });

    test('renders WAV data of the first output bus', () {
      final graph = AudGraphFfi(listen: false, outputChannels: const [2, 1]);
      addTearDown(graph.dispose);
      final osc = graph.createNode(
        'aud.graph.oscillator',
        outputChannels: const [2],
      );
      graph.transaction((tx) => tx.connect(osc, graph.io));
      graph.start();
      final wav = AudOfflineRenderer(graph).renderWav(frames: 480);
      expect(wav.sampleRate, 48000);
      expect(wav.channels, hasLength(2));
      expect(wav.frames, 480);
      expect(wav.channels[0], wav.channels[1]);
      final silent = AudGraphFfi(listen: false, outputChannels: const []);
      addTearDown(silent.dispose);
      silent.start();
      expect(
        AudOfflineRenderer(silent).renderWav(frames: 16).channels,
        isEmpty,
      );
    });

    test('renders the golden oscillator through the filter', () {
      final graph = AudGraphFfi(
        listen: false,
        maxFrames: 256,
        outputChannels: const [1],
      );
      addTearDown(graph.dispose);
      final osc = graph.createNode('aud.graph.oscillator');
      final filter = graph.createNode(
        'aud.graph.filter',
        inputChannels: const [1],
        outputChannels: const [1],
      );
      graph.setParam(filter, 'cutoff', 800);
      graph.setParam(filter, 'resonance', 0.5);
      graph.setParam(osc, 'waveform', 1);
      graph.transaction((tx) {
        tx.connect(osc, filter);
        tx.connect(filter, graph.io);
      });
      graph.start();
      graph.sendEvent(
        osc,
        AudUmpEvent.fromMessage(
          const MidiNoteOn(channel: 0, note: 57, velocity: 100),
        ).single,
        at: const AudTimestamp.sample(1000),
      );
      graph.setParam(
        osc,
        'amplitude',
        0.2,
        rampFrames: 480,
        at: const AudTimestamp.sample(2400),
      );
      final wav = AudOfflineRenderer(
        graph,
      ).renderWav(frames: 4800, blockSizes: const [256, 100, 37]);
      final file = File('test/goldens/oscillator_filter.wav');
      if (Platform.environment['UPDATE_GOLDENS'] == '1') {
        file.parent.createSync(recursive: true);
        AudWavFile.write(file.path, wav);
      }
      expect(
        file.existsSync(),
        isTrue,
        reason: 'run with UPDATE_GOLDENS=1 first',
      );
      final golden = AudWavFile.read(file.path);
      expect(golden.frames, wav.frames);
      for (var i = 0; i < wav.frames; i++) {
        expect(
          wav.channels[0][i],
          closeTo(golden.channels[0][i], 1e-5),
          reason: 'frame $i',
        );
      }
      expect(wav.channels[0].sublist(3000).any((v) => v != 0), isTrue);
    });
  });
}
