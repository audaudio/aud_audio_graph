// @license
// Copyright (c) Audanika. All Rights Reserved.
//
// Use of this source code is governed by terms that can be
// found in the LICENSE file in the root of this package.

import 'dart:convert';
import 'dart:ffi';
import 'dart:typed_data';

import 'package:aud_audio_core/aud_audio_core_bindings.dart' as core;
import 'package:aud_audio_core/aud_audio_core_ffi.dart';
import 'package:ffi/ffi.dart';

import 'aud_audio_graph_bindings_generated.dart' as bindings;
import 'aud_cache_usage.dart';
import 'aud_event_source.dart';
import 'aud_graph_notification.dart';
import 'aud_graph_options.dart';
import 'aud_graph_state.dart';
import 'aud_graph_stats.dart';
import 'aud_graph_timing.dart';
import 'aud_graph_transport_state.dart';
import 'aud_load.dart';
import 'aud_node_message.dart';

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
      bindings.AUD_NOTIFY_LOAD => AudLoadNotification(
        node: node,
        load: value,
        result: code,
        progress: number,
        samplePosition: position,
      ),
      bindings.AUD_NOTIFY_MESSAGE => AudMessageNotification(
        count: count,
        samplePosition: position,
      ),
      bindings.AUD_NOTIFY_SOURCE_DONE => AudSourceDoneNotification(
        node: node,
        source: value,
        samplePosition: position,
      ),
      _ => throw ArgumentError.value(type, 'type', 'Unknown notification'),
    };
  }
}

/// Reads an [AudEventSourceStats] from its native struct (ticket 26).
extension AudNativeEventSourceStatsToDart on bindings.AudEventSourceStats {
  /// The counters from their native struct.
  AudEventSourceStats toDart() => AudEventSourceStats(
    attached: attached != 0,
    pushed: pushed,
    rejected: rejected,
    delivered: delivered,
    late: late,
    maxLateNs: max_late_ns,
    dropped: dropped,
    reordered: reordered,
    releases: releases,
    maxDelayNs: max_delay_ns,
    lastWord0: last_word0,
    lastTimestampNs: last_timestamp_ns,
    delayHistogram: List.unmodifiable([
      for (var i = 0; i <= bindings.AUD_SOURCE_DELAY_BINS; i++)
        delay_histogram[i],
    ]),
    latencyHistogram: List.unmodifiable([
      for (var i = 0; i <= bindings.AUD_SOURCE_LATENCY_BINS; i++)
        latency_histogram[i],
    ]),
  );
}

/// Reads an [AudGraphTiming] from its native struct (ticket 26).
extension AudNativeGraphTimingToDart on bindings.AudGraphTiming {
  /// The timing from its native struct.
  AudGraphTiming toDart() => AudGraphTiming(
    valid: flags & bindings.AUD_TIMING_VALID != 0,
    offline: flags & bindings.AUD_TIMING_OFFLINE != 0,
    sampleRate: sample_rate,
    samplePosition: sample_position,
    hostTimeNs: host_time_ns,
    frames: frames,
    outputLatencyFrames: output_latency_frames,
    maxPeriodNs: max_period_ns,
    nominalPeriodNs: nominal_period_ns,
    timeFilterResets: time_filter_resets,
  );
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
    messagesDropped: messages_dropped,
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
      ..lookahead_ns = lookahead.inMicroseconds * 1000
      ..message_capacity = messageCapacity
      ..reply_capacity = replyCapacity;
  }
}

// #############################################################################
/// Reads an [AudLoad] from its native struct (ticket 25).
extension AudNativeLoadInfoToDart on bindings.AudLoadInfo {
  /// The load of the struct.
  AudLoad toDart() => AudLoad(
    id: id,
    node: node,
    name: _text(name, bindings.AUD_GRAPH_MAX_TEXT),
    result: result,
    progress: progress,
    message: _text(message, bindings.AUD_GRAPH_MAX_TEXT),
    cancelled: cancelled != 0,
    startNs: start_ns,
    endNs: end_ns,
  );
}

/// Reads an [AudCacheUsage] from its native struct (ticket 25).
extension AudNativeCacheUsageToDart on bindings.AudCacheUsage {
  /// The usage of the struct.
  AudCacheUsage toDart() => AudCacheUsage(
    caches: caches,
    budget: budget,
    bytes: bytes,
    trims: trims,
    trimmed: trimmed,
    workers: workers,
    tasks: tasks,
  );
}

/// Reads an [AudNodeMessage] from the packed record of a node's message
/// (ticket 25): the path, then per argument its type tag and its value in
/// little endian.
extension AudNativeGraphMessageToDart on bindings.AudGraphMessage {
  /// The message of the record.
  AudNodeMessage toDart() {
    final bytes = Uint8List(size);
    for (var i = 0; i < size; i++) {
      bytes[i] = data[i];
    }
    final view = ByteData.sublistView(bytes);
    var at = 0;
    String text() {
      final end = bytes.indexOf(0, at);
      final value = utf8.decode(bytes.sublist(at, end), allowMalformed: true);
      at = end + 1;
      return value;
    }

    final path = text();
    final arguments = <Object?>[];
    for (var i = 0; i < num_args; i++) {
      final tag = String.fromCharCode(bytes[at++]);
      switch (tag) {
        case 'i':
          arguments.add(view.getInt32(at, Endian.little));
          at += 4;
        case 'h':
          arguments.add(view.getInt64(at, Endian.little));
          at += 8;
        case 'f':
          arguments.add(view.getFloat32(at, Endian.little));
          at += 4;
        case 'd':
          arguments.add(view.getFloat64(at, Endian.little));
          at += 8;
        case 's':
          arguments.add(text());
        case 'b':
          final length = view.getUint32(at, Endian.little);
          at += 4;
          arguments.add(Uint8List.fromList(bytes.sublist(at, at + length)));
          at += length;
        case 'T':
          arguments.add(true);
        case 'F':
          arguments.add(false);
        case 'I':
          arguments.add(AudImpulse.instance);
        default:
          arguments.add(null);
      }
    }
    return AudNodeMessage(
      node: node,
      message: AudOscMessage(path, arguments),
      samplePosition: sample_position,
    );
  }
}

/// Writes an [AudOscMessage] as the `AudMessage` of the ABI (ticket 25).
extension AudOscMessageToNative on AudOscMessage {
  /// The message in native memory of [allocator]: an int that fits 32 bits
  /// becomes `i`, a larger one `h`, a double `f`, a text `s`, bytes `b`.
  Pointer<core.AudMessage> writeNative(Allocator allocator) {
    final count = arguments.length;
    final args = allocator<core.AudMessageArg>(count == 0 ? 1 : count);
    for (var i = 0; i < count; i++) {
      final arg = args[i];
      arg.struct_size = sizeOf<core.AudMessageArg>();
      switch (arguments[i]) {
        case final int value when value >= -0x80000000 && value <= 0x7FFFFFFF:
          arg.type = AUD_ARG_INT32;
          arg.value.i = value;
        case final int value:
          arg.type = AUD_ARG_INT64;
          arg.value.h = value;
        case final double value:
          arg.type = AUD_ARG_FLOAT;
          arg.value.f = value;
        case final String value:
          arg.type = AUD_ARG_STRING;
          arg.value.s = value.toNativeUtf8(allocator: allocator).cast();
        case final Uint8List value:
          final bytes = allocator<Uint8>(value.isEmpty ? 1 : value.length);
          bytes.asTypedList(value.length).setAll(0, value);
          arg.type = AUD_ARG_BLOB;
          arg.size = value.length;
          arg.value.b = bytes.cast();
        case true:
          arg.type = AUD_ARG_TRUE;
        case false:
          arg.type = AUD_ARG_FALSE;
        case null:
          arg.type = AUD_ARG_NIL;
        case AudImpulse():
          arg.type = AUD_ARG_IMPULSE;
        default:
          throw ArgumentError.value(
            arguments[i],
            'arguments',
            'Not an argument of a node message',
          );
      }
    }
    final message = allocator<core.AudMessage>();
    message.ref
      ..struct_size = sizeOf<core.AudMessage>()
      ..num_args = count
      ..path = address.toNativeUtf8(allocator: allocator).cast()
      ..args = args;
    return message;
  }
}

// #############################################################################
/// Writes a timeout as the milliseconds of the waits of the C API
/// (ticket 25): `aud_graph_wait_loads`, `aud_host_wait_loads` and
/// `aud_tasks_wait_idle`, where 0 waits as long as it takes.
extension AudTimeoutToNative on Duration {
  /// The milliseconds of the timeout, rounded up: zero stays zero, a
  /// timeout shorter than a millisecond waits one, a longer one than the C
  /// API counts waits as long as it can (`0xFFFFFFFF` ms). Throws an
  /// [ArgumentError] for a negative timeout.
  int get timeoutMs {
    if (isNegative) throw ArgumentError.value(this, 'timeout', 'Negative');
    final rest = inMicroseconds % Duration.microsecondsPerMillisecond;
    return (inMilliseconds + (rest == 0 ? 0 : 1)).clamp(0, 0xFFFFFFFF);
  }
}

// The text of a fixed char array: up to its terminator, at most [length].
String _text(Array<Char> chars, int length) {
  final bytes = <int>[];
  for (var i = 0; i < length; i++) {
    final c = chars[i];
    if (c == 0) break;
    bytes.add(c & 0xFF);
  }
  return utf8.decode(bytes, allowMalformed: true);
}
