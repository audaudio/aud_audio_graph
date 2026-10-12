// @license
// Copyright (c) Audanika. All Rights Reserved.
//
// Use of this source code is governed by terms that can be
// found in the LICENSE file in the root of this package.

import 'aud_graph_constants.dart' as bindings;
import 'aud_graph_node.dart';
import 'aud_histogram.dart';

// #############################################################################
/// The timing policy of an event source (midi-003, ticket 26).
enum AudSourceTiming {
  /// An event sounds at its timestamp plus the source's latency C at the
  /// output; one that misses its target plays at the start of the block
  /// and counts as late. The events carry no live flag, so the lead of
  /// the path places them.
  fixed(bindings.AUD_SOURCE_TIMING_FIXED),

  /// An event plays at the start of the block that drains it; the events
  /// carry the live flag.
  immediate(bindings.AUD_SOURCE_TIMING_IMMEDIATE);

  const AudSourceTiming(this.code);

  /// The `AUD_SOURCE_TIMING_*` code.
  final int code;

  /// The policy of a code.
  static AudSourceTiming fromCode(int code) => values.firstWhere(
    (t) => t.code == code,
    orElse: () => throw ArgumentError.value(code, 'code', 'Unknown timing'),
  );
}

// #############################################################################
/// The counters of an event source: `AudEventSourceStats` of the engine,
/// with the percentiles of its histograms.
class AudEventSourceStats {
  /// Creates a snapshot of the counters.
  const AudEventSourceStats({
    this.attached = false,
    this.pushed = 0,
    this.rejected = 0,
    this.delivered = 0,
    this.late = 0,
    this.maxLateNs = 0,
    this.dropped = 0,
    this.reordered = 0,
    this.releases = 0,
    this.maxDelayNs = 0,
    this.lastWord0 = 0,
    this.lastTimestampNs = 0,
    this.delayHistogram = const [],
    this.latencyHistogram = const [],
  });

  // ...........................................................................
  /// Whether a producer holds the token.
  final bool attached;

  /// The packets the queue took.
  final int pushed;

  /// The packets refused: the queue was full, or the packet malformed.
  final int rejected;

  /// The packets delivered to the target.
  final int delivered;

  /// The packets played at the block start after their time.
  final int late;

  /// The largest lateness in nanoseconds.
  final int maxLateNs;

  /// The packets dropped: note ons of a release, packets for a target
  /// that takes no events.
  final int dropped;

  /// The timestamps raised to their predecessor's.
  final int reordered;

  /// The releases the realtime thread carried out.
  final int releases;

  /// The largest delivery delay - push time minus timestamp, taken on the
  /// producer thread - in nanoseconds.
  final int maxDelayNs;

  /// The first word of the last delivered packet: the activity.
  final int lastWord0;

  /// The timestamp of the last delivered packet in nanoseconds.
  final int lastTimestampNs;

  /// The delivery delays in bins of [delayBinNs], the last bin for what
  /// lies beyond [delayBins] bins.
  final List<int> delayHistogram;

  /// The input to output times - the output time of the event's frame
  /// minus its timestamp - in bins of [latencyBinNs], the last bin for what
  /// lies beyond [latencyBins] bins.
  final List<int> latencyHistogram;

  // ...........................................................................
  /// The delivery delay in nanoseconds below which [fraction] of the
  /// packets stayed - the upper edge of the bin -, null without packets;
  /// at least `delayBins * delayBinNs` when the fraction lies in the
  /// overflow bin.
  int? delayPercentileNs(double fraction) => AudHistogram(
    counts: delayHistogram,
    binNs: delayBinNs,
  ).percentileNs(fraction);

  /// The input to output time in nanoseconds below which [fraction] of
  /// the packets stayed, like [delayPercentileNs].
  int? latencyPercentileNs(double fraction) => AudHistogram(
    counts: latencyHistogram,
    binNs: latencyBinNs,
  ).percentileNs(fraction);

  /// The median delivery delay in nanoseconds, null without packets.
  int? get delayMedianNs => delayPercentileNs(0.5);

  /// The delivery delay of the 99th percentile in nanoseconds.
  int? get delayP99Ns => delayPercentileNs(0.99);

  /// The median input to output time in nanoseconds.
  int? get latencyMedianNs => latencyPercentileNs(0.5);

  /// The input to output time of the 99th percentile in nanoseconds.
  int? get latencyP99Ns => latencyPercentileNs(0.99);

  // ...........................................................................
  /// The counters as JSON; the histograms as their percentiles.
  Map<String, Object?> toJson() => {
    'attached': attached,
    'pushed': pushed,
    'rejected': rejected,
    'delivered': delivered,
    'late': late,
    'maxLateNs': maxLateNs,
    'dropped': dropped,
    'reordered': reordered,
    'releases': releases,
    'maxDelayNs': maxDelayNs,
    'lastWord0': lastWord0,
    'lastTimestampNs': lastTimestampNs,
    'delayMedianNs': delayMedianNs,
    'delayP99Ns': delayP99Ns,
    'latencyMedianNs': latencyMedianNs,
    'latencyP99Ns': latencyP99Ns,
  };

  @override
  String toString() => 'AudEventSourceStats(${toJson()})';

  // ...........................................................................
  /// The width of a bin of [delayHistogram] in nanoseconds: 50 us.
  static const int delayBinNs = bindings.AUD_SOURCE_DELAY_BIN_NS;

  /// The bins of [delayHistogram] below the overflow bin: up to 25 ms.
  static const int delayBins = bindings.AUD_SOURCE_DELAY_BINS;

  /// The width of a bin of [latencyHistogram] in nanoseconds: 0.25 ms.
  static const int latencyBinNs = bindings.AUD_SOURCE_LATENCY_BIN_NS;

  /// The bins of [latencyHistogram] below the overflow bin: up to 256 ms.
  static const int latencyBins = bindings.AUD_SOURCE_LATENCY_BINS;
}

// #############################################################################
/// A lock-free queue of UMP packets with host timestamps from one
/// producer at a time - a native MIDI thread, an isolate, an OSC server -
/// into one committed node of the graph (interop-003, midi-003). The
/// realtime thread drains it every block: a packet whose time lies beyond
/// the block waits in the queue, a late one plays at the block start.
/// Created by `AudGraph.createEventSource`.
abstract interface class AudEventSource {
  /// The target node.
  AudNode get node;

  /// The event input of the target.
  int get port;

  /// The timing policy.
  AudSourceTiming get timing;

  /// The capacity of the queue in packets, a power of two.
  int get capacity;

  /// The packets waiting; approximate from other threads.
  int get size;

  /// Whether [dispose] ran, or the graph was disposed.
  bool get isDisposed;

  /// C of [AudSourceTiming.fixed] in nanoseconds; a change takes effect at
  /// the next block.
  int get latencyNs;
  set latencyNs(int latencyNs);

  /// Attaches a producer with [token] (not 0) before its first push; throws
  /// an `AudGraphException` with `AUD_GRAPH_ERROR_BUSY` while another is
  /// attached. A new producer attaches only after the previous one
  /// detached; the token orders the pushes of the two.
  void attach(int token);

  /// Detaches the producer [token] after its last push; throws for a
  /// token that is not attached.
  void detach(int token);

  /// Pushes one UMP packet of one to four [words] with its timestamp
  /// [hostTimeNs] of the host clock (0: now). False when the queue is
  /// full - a note off that does not fit raises [release]; throws an
  /// `AudGraphException` for a malformed packet.
  bool push(List<int> words, {int hostTimeNs = 0});

  /// Raises release: before the next drain the queue empties - note offs
  /// and controllers at the block start, note ons dropped - and the
  /// target's notes close, then its sustain and soft pedals lift on every
  /// channel the source used them on. Idempotent; from any isolate.
  void release();

  /// A snapshot of the counters.
  AudEventSourceStats get stats;

  /// Retires the source; the graph frees it once the realtime thread let
  /// go of it (`AudSourceDoneNotification`). A producer must have
  /// detached.
  void dispose();
}
