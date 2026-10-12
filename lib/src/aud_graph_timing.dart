// @license
// Copyright (c) Audanika. All Rights Reserved.
//
// Use of this source code is governed by terms that can be
// found in the LICENSE file in the root of this package.

// #############################################################################
/// The timing of the engine as the last block saw it (time-001,
/// midi-003): `AudGraphTiming` of the engine, what a bridge needs to
/// choose the latency C = P + L + m of its sources.
class AudGraphTiming {
  /// Creates a snapshot of the timing.
  const AudGraphTiming({
    this.valid = false,
    this.offline = false,
    this.sampleRate = 0,
    this.samplePosition = 0,
    this.hostTimeNs = 0,
    this.frames = 0,
    this.outputLatencyFrames = 0,
    this.maxPeriodNs = 0,
    this.nominalPeriodNs = 0,
    this.timeFilterResets = 0,
  });

  // ...........................................................................
  /// Whether a block has rendered; the other fields are 0 before.
  final bool valid;

  /// Whether the last block rendered offline.
  final bool offline;

  /// The sample rate in Hz.
  final double sampleRate;

  /// The sample position of the first frame of the last block.
  final int samplePosition;

  /// The fitted host time at which that frame reaches the output.
  final int hostTimeNs;

  /// The frames of the last block.
  final int frames;

  /// The output latency the stream reported with the last block, in
  /// frames: L of midi-003.
  final int outputLatencyFrames;

  /// P of midi-003: the longest time between the starts of two renders
  /// since the last reset of the time filter, in nanoseconds; offline the
  /// longest block.
  final int maxPeriodNs;

  /// The prepared largest block over the rate, in nanoseconds.
  final int nominalPeriodNs;

  /// The resets of the time filter.
  final int timeFilterResets;

  /// The output latency in nanoseconds.
  double get outputLatencyNs =>
      sampleRate == 0 ? 0 : outputLatencyFrames * 1e9 / sampleRate;

  /// The host time at which [sample] reaches the output, on the line of
  /// the last block.
  int hostTimeAt(int sample) => sampleRate == 0
      ? hostTimeNs
      : hostTimeNs + ((sample - samplePosition) * 1e9 / sampleRate).round();

  /// The sample that reaches the output at [hostTime], on the line of the
  /// last block.
  int sampleAt(int hostTime) => sampleRate == 0
      ? samplePosition
      : samplePosition + ((hostTime - hostTimeNs) * sampleRate / 1e9).round();

  // ...........................................................................
  /// The timing as JSON.
  Map<String, Object?> toJson() => {
    'valid': valid,
    'offline': offline,
    'sampleRate': sampleRate,
    'samplePosition': samplePosition,
    'hostTimeNs': hostTimeNs,
    'frames': frames,
    'outputLatencyFrames': outputLatencyFrames,
    'maxPeriodNs': maxPeriodNs,
    'nominalPeriodNs': nominalPeriodNs,
    'timeFilterResets': timeFilterResets,
  };

  @override
  String toString() => 'AudGraphTiming(${toJson()})';
}
