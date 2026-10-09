// @license
// Copyright (c) Audanika. All Rights Reserved.
//
// Use of this source code is governed by terms that can be
// found in the LICENSE file in the root of this package.

import 'package:aud_audio_core/aud_audio_core.dart';

// #############################################################################
/// The transport as the realtime thread published it last (time-001).
class AudTransportState {
  /// Creates a transport state.
  const AudTransportState({
    this.playing = false,
    this.beatTicks = 0,
    this.tempo = 120,
    this.numerator = 4,
    this.denominator = 4,
    this.looping = false,
    this.loopStartTicks = 0,
    this.loopEndTicks = 0,
  });

  // ...........................................................................
  /// Whether the transport plays.
  final bool playing;

  /// The musical position in ticks.
  final int beatTicks;

  /// The tempo in beats per minute.
  final double tempo;

  /// The numerator of the time signature.
  final int numerator;

  /// The denominator of the time signature.
  final int denominator;

  /// Whether the transport loops.
  final bool looping;

  /// The loop start in ticks.
  final int loopStartTicks;

  /// The loop end in ticks.
  final int loopEndTicks;

  /// The musical position in beats.
  double get beat => AudBeats.beats(beatTicks);

  /// The loop start in beats.
  double get loopStart => AudBeats.beats(loopStartTicks);

  /// The loop end in beats.
  double get loopEnd => AudBeats.beats(loopEndTicks);

  // ...........................................................................
  /// The state as JSON.
  Map<String, Object?> toJson() => {
    'playing': playing,
    'beat': beat,
    'tempo': tempo,
    'numerator': numerator,
    'denominator': denominator,
    'looping': looping,
    'loopStart': loopStart,
    'loopEnd': loopEnd,
  };

  @override
  bool operator ==(Object other) =>
      other is AudTransportState &&
      other.playing == playing &&
      other.beatTicks == beatTicks &&
      other.tempo == tempo &&
      other.numerator == numerator &&
      other.denominator == denominator &&
      other.looping == looping &&
      other.loopStartTicks == loopStartTicks &&
      other.loopEndTicks == loopEndTicks;

  @override
  int get hashCode => Object.hash(
    playing,
    beatTicks,
    tempo,
    numerator,
    denominator,
    looping,
    loopStartTicks,
    loopEndTicks,
  );

  @override
  String toString() => 'AudTransportState(${toJson()})';
}
