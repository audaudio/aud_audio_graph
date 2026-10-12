// @license
// Copyright (c) Audanika. All Rights Reserved.
//
// Use of this source code is governed by terms that can be
// found in the LICENSE file in the root of this package.

import 'aud_graph_constants.dart' as bindings;
import 'aud_histogram.dart';

// #############################################################################
/// The timing of the engine as the last block saw it (time-001,
/// midi-003): `AudGraphTiming` of the engine, what a bridge needs to
/// choose the latency C = P + L + m of its sources - P as a high
/// percentile of [periodHistogram].
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
    this.periodHistogram = const [],
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

  /// The longest time between the starts of two renders since the last
  /// reset of the time filter, in nanoseconds; offline the longest block.
  /// A diagnostic: one gap at start-up ruins it, [periodPercentileNs]
  /// gives P of midi-003.
  final int maxPeriodNs;

  /// The prepared largest block over the rate, in nanoseconds.
  final int nominalPeriodNs;

  /// The resets of the time filter.
  final int timeFilterResets;

  /// The times between the starts of two renders since the last reset of
  /// the time filter - offline the block durations - in bins of
  /// [periodBinNs], the last bin for what lies beyond [periodBins] bins;
  /// empty before a block rendered.
  final List<int> periodHistogram;

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
  /// The period in nanoseconds below which [fraction] of the renders
  /// stayed - the upper edge of the bin -, null without renders; at least
  /// `periodBins * periodBinNs` when the fraction lies in the overflow
  /// bin. P of midi-003 is [periodP999Ns].
  int? periodPercentileNs(double fraction) => AudHistogram(
    counts: periodHistogram,
    binNs: periodBinNs,
  ).percentileNs(fraction);

  /// The median period in nanoseconds, null without renders.
  int? get periodMedianNs => periodPercentileNs(0.5);

  /// The period of the 99th percentile in nanoseconds.
  int? get periodP99Ns => periodPercentileNs(0.99);

  /// The period of the 99.9th percentile in nanoseconds: P of midi-003.
  int? get periodP999Ns => periodPercentileNs(0.999);

  // ...........................................................................
  /// The timing as JSON; the histogram as its percentiles.
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
    'periodMedianNs': periodMedianNs,
    'periodP99Ns': periodP99Ns,
    'periodP999Ns': periodP999Ns,
  };

  @override
  String toString() => 'AudGraphTiming(${toJson()})';

  // ...........................................................................
  /// The width of a bin of [periodHistogram] in nanoseconds: 100 us.
  static const int periodBinNs = bindings.AUD_GRAPH_PERIOD_BIN_NS;

  /// The bins of [periodHistogram] below the overflow bin: up to 51.2 ms.
  static const int periodBins = bindings.AUD_GRAPH_PERIOD_BINS;
}
