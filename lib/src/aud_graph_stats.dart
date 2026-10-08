// @license
// Copyright (c) Audanika. All Rights Reserved.
//
// Use of this source code is governed by terms that can be
// found in the LICENSE file in the root of this package.

import 'aud_audio_graph_bindings_generated.dart' as bindings;
import 'aud_graph_state.dart';

// #############################################################################
/// The counters the realtime thread keeps: `AudGraphStats` of the engine.
class AudGraphStats {
  /// Creates a snapshot of the counters.
  const AudGraphStats({
    required this.state,
    required this.revision,
    required this.scheduled,
    required this.blocksRendered,
    required this.framesRendered,
    required this.renderTimeMaxNs,
    required this.renderTimeSumNs,
    required this.eventsDelivered,
    required this.eventsLate,
    required this.eventsDropped,
    required this.paramsApplied,
    required this.rejected,
    required this.notificationsDropped,
    required this.overloads,
    required this.timeFilterResets,
    required this.outputPeak,
    this.realtimeViolations = 0,
  });

  /// The counters from their native struct.
  factory AudGraphStats.fromNative(bindings.AudGraphStats native) =>
      AudGraphStats(
        state: AudGraphState.fromCode(native.state),
        revision: native.revision,
        scheduled: native.scheduled,
        blocksRendered: native.blocks_rendered,
        framesRendered: native.frames_rendered,
        renderTimeMaxNs: native.render_time_max_ns,
        renderTimeSumNs: native.render_time_sum_ns,
        eventsDelivered: native.events_delivered,
        eventsLate: native.events_late,
        eventsDropped: native.events_dropped,
        paramsApplied: native.params_applied,
        rejected: native.rejected,
        notificationsDropped: native.notifications_dropped,
        overloads: native.overloads,
        timeFilterResets: native.time_filter_resets,
        outputPeak: native.output_peak,
        realtimeViolations: native.realtime_violations,
      );

  // ...........................................................................
  /// The lifecycle state.
  final AudGraphState state;

  /// The revision the realtime thread adopted last.
  final int revision;

  /// The events waiting in the scheduler.
  final int scheduled;

  /// The blocks rendered.
  final int blocksRendered;

  /// The frames rendered.
  final int framesRendered;

  /// The longest block render in nanoseconds.
  final int renderTimeMaxNs;

  /// The sum of all block renders in nanoseconds.
  final int renderTimeSumNs;

  /// The events delivered to nodes.
  final int eventsDelivered;

  /// The events that arrived late and played at a block start.
  final int eventsLate;

  /// The events dropped: late with the drop policy, of retired nodes, or
  /// beyond a capacity.
  final int eventsDropped;

  /// The parameter changes applied.
  final int paramsApplied;

  /// The enqueues the control thread refused.
  final int rejected;

  /// The notifications lost because their queue was full.
  final int notificationsDropped;

  /// The blocks whose render took longer than the block lasts.
  final int overloads;

  /// The resets of the time filter.
  final int timeFilterResets;

  /// The largest absolute output sample since the last reset.
  final double outputPeak;

  /// What the debug watchdog caught on the realtime thread: allocations,
  /// frees and log calls (ticket 20); 0 without the watchdog.
  final int realtimeViolations;

  /// The mean block render in nanoseconds.
  double get renderTimeMeanNs =>
      blocksRendered == 0 ? 0 : renderTimeSumNs / blocksRendered;

  // ...........................................................................
  /// The counters as JSON.
  Map<String, Object?> toJson() => {
    'state': state.name,
    'revision': revision,
    'scheduled': scheduled,
    'blocksRendered': blocksRendered,
    'framesRendered': framesRendered,
    'renderTimeMaxNs': renderTimeMaxNs,
    'renderTimeSumNs': renderTimeSumNs,
    'eventsDelivered': eventsDelivered,
    'eventsLate': eventsLate,
    'eventsDropped': eventsDropped,
    'paramsApplied': paramsApplied,
    'rejected': rejected,
    'notificationsDropped': notificationsDropped,
    'overloads': overloads,
    'timeFilterResets': timeFilterResets,
    'outputPeak': outputPeak,
    'realtimeViolations': realtimeViolations,
  };

  @override
  String toString() => 'AudGraphStats(${toJson()})';
}
