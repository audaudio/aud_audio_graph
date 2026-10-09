// @license
// Copyright (c) Audanika. All Rights Reserved.
//
// Use of this source code is governed by terms that can be
// found in the LICENSE file in the root of this package.

import 'package:aud_audio_core/aud_audio_core_ffi.dart';
import 'package:aud_audio_graph/aud_audio_graph_ffi.dart';
import 'package:test/test.dart';

void main() {
  group('AudTransportState', () {
    test('converts ticks to beats and compares', () {
      final state = AudTransportState(
        playing: true,
        beatTicks: AudBeats.ticks(1.5),
        tempo: 100,
        numerator: 3,
        denominator: 8,
        looping: true,
        loopStartTicks: AudBeats.ticks(1),
        loopEndTicks: AudBeats.ticks(5),
      );
      expect(state.beat, 1.5);
      expect(state.loopStart, 1);
      expect(state.loopEnd, 5);
      expect(state.toJson(), {
        'playing': true,
        'beat': 1.5,
        'tempo': 100.0,
        'numerator': 3,
        'denominator': 8,
        'looping': true,
        'loopStart': 1.0,
        'loopEnd': 5.0,
      });
      expect(state, isNot(const AudTransportState()));
      expect(
        state,
        AudTransportState(
          playing: true,
          beatTicks: AudBeats.ticks(1.5),
          tempo: 100,
          numerator: 3,
          denominator: 8,
          looping: true,
          loopStartTicks: AudBeats.ticks(1),
          loopEndTicks: AudBeats.ticks(5),
        ),
      );
      expect(state.hashCode, isNot(const AudTransportState().hashCode));
      expect(state.toString(), contains('tempo: 100'));
    });

    test('reads the transport of a graph', () {
      final graph = AudGraphFfi(listen: false, maxFrames: 256);
      addTearDown(graph.dispose);
      expect(graph.transportState, const AudTransportState());
      graph.start();
      graph.transport(const AudTransportRequest.start());
      graph.transport(
        const AudTransportRequest.setTimeSignature(
          numerator: 3,
          denominator: 4,
        ),
      );
      graph.transport(AudTransportRequest.setLoop(start: 0, end: 8));
      AudOfflineRenderer(graph).render(frames: 48000);
      final state = graph.transportState;
      expect(state.playing, isTrue);
      expect(state.beat, closeTo(2, 1e-6));
      expect(state.numerator, 3);
      expect(state.denominator, 4);
      expect(state.looping, isTrue);
      expect(state.loopEnd, 8);
    });
  });
}
