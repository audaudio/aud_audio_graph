// @license
// Copyright (c) Audanika. All Rights Reserved.
//
// Use of this source code is governed by terms that can be
// found in the LICENSE file in the root of this package.

import 'package:aud_audio_core/aud_audio_core_ffi.dart';

import 'aud_audio_graph_bindings_generated.dart' as bindings;
import 'aud_graph_notification.dart';
import 'aud_graph_options.dart';
import 'aud_graph_state.dart';
import 'aud_graph_stats.dart';
import 'aud_graph_transport_state.dart';

// The conversions between the platform-neutral types of the graph and the
// structs of its C API, apart from the types so that no dart:ffi reaches a
// web build (web-001).

// #############################################################################
/// Reads an [AudGraphNotification] from its native struct.
extension AudNativeGraphNotificationToDart on bindings.AudGraphNotification {
  /// A notification of the struct.
  AudGraphNotification toDart() {
    final position = sample_position;
    return switch (type) {
      bindings.AUD_NOTIFY_REVISION => AudRevisionAdoptedNotification(
        revision: revision,
        samplePosition: position,
      ),
      bindings.AUD_NOTIFY_STATE => AudStateNotification(
        state: AudGraphState.fromCode(code),
        samplePosition: position,
      ),
      bindings.AUD_NOTIFY_NODE_DONE => AudNodeDoneNotification(
        node: node,
        samplePosition: position,
      ),
      bindings.AUD_NOTIFY_DIAGNOSTIC => AudDiagnosticNotification(
        code: code,
        count: count,
        node: node,
        value: value,
        samplePosition: position,
      ),
      bindings.AUD_NOTIFY_EVENT => AudEventNotification(
        node: node,
        event: event.toDart(),
        samplePosition: position,
      ),
      bindings.AUD_NOTIFY_TRANSPORT => AudTransportNotification(
        type: AudTransportRequestType.fromCode(code),
        beatTicks: value,
        tempo: number,
        playing: count != 0,
        samplePosition: position,
      ),
      bindings.AUD_NOTIFY_TIME_RESET => AudTimeResetNotification(
        count: count,
        samplePosition: position,
      ),
      _ => throw ArgumentError.value(type, 'type', 'Unknown notification'),
    };
  }
}

/// Reads an [AudGraphStats] from its native struct.
extension AudNativeGraphStatsToDart on bindings.AudGraphStats {
  /// The counters from their native struct.
  AudGraphStats toDart() => AudGraphStats(
    state: AudGraphState.fromCode(state),
    revision: revision,
    scheduled: scheduled,
    blocksRendered: blocks_rendered,
    framesRendered: frames_rendered,
    renderTimeMaxNs: render_time_max_ns,
    renderTimeSumNs: render_time_sum_ns,
    eventsDelivered: events_delivered,
    eventsLate: events_late,
    eventsDropped: events_dropped,
    paramsApplied: params_applied,
    rejected: rejected,
    notificationsDropped: notifications_dropped,
    overloads: overloads,
    timeFilterResets: time_filter_resets,
    outputPeak: output_peak,
    realtimeViolations: realtime_violations,
  );
}

/// Reads an [AudTransportState] from its native struct.
extension AudNativeTransportStateToDart on bindings.AudGraphTransportState {
  /// The state of the struct.
  AudTransportState toDart() => AudTransportState(
    playing: playing != 0,
    beatTicks: beat,
    tempo: tempo,
    numerator: numerator,
    denominator: denominator,
    looping: looping != 0,
    loopStartTicks: loop_start,
    loopEndTicks: loop_end,
  );
}

/// Writes [AudGraphOptions] into the configuration of a graph.
extension AudGraphOptionsToNative on AudGraphOptions {
  /// Writes the options into a native configuration.
  void writeTo(bindings.AudGraphConfig config) {
    config
      ..flags = dropLateEvents ? bindings.AUD_GRAPH_DROP_LATE_EVENTS : 0
      ..max_nodes = maxNodes
      ..max_connections = maxConnections
      ..param_queue_capacity = paramQueueCapacity
      ..event_queue_capacity = eventQueueCapacity
      ..scheduler_capacity = schedulerCapacity
      ..notification_capacity = notificationCapacity
      ..max_events_per_block = maxEventsPerBlock
      ..fade_frames = fadeFrames
      ..max_tail_frames = maxTailFrames
      ..lookahead_ns = lookahead.inMicroseconds * 1000;
  }
}
