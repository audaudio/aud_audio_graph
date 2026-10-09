// @license
// Copyright (c) Audanika. All Rights Reserved.
//
// Use of this source code is governed by terms that can be
// found in the LICENSE file in the root of this package.

import 'dart:ffi';

import 'package:aud_audio_core/aud_audio_core_ffi.dart';
import 'package:aud_audio_graph/aud_audio_graph_ffi.dart';
import 'package:aud_audio_graph/src/aud_audio_graph_bindings_generated.dart'
    as bindings;
import 'package:ffi/ffi.dart';
import 'package:test/test.dart';

void main() {
  late Pointer<bindings.AudGraphNotification> native;

  setUp(() {
    native = calloc<bindings.AudGraphNotification>();
    native.ref
      ..struct_size = sizeOf<bindings.AudGraphNotification>()
      ..sample_position = 512;
  });
  tearDown(() => calloc.free(native));

  AudGraphNotification parse(int type) {
    native.ref.type = type;
    return native.ref.toDart();
  }

  group('native.toDart()', () {
    test('a revision', () {
      native.ref.revision = 3;
      final n =
          parse(bindings.AUD_NOTIFY_REVISION) as AudRevisionAdoptedNotification;
      expect(n.revision, 3);
      expect(n.samplePosition, 512);
      expect(n.toJson(), {
        'type': 'revision',
        'revision': 3,
        'samplePosition': 512,
      });
      expect(n.toOsc(1), const AudRevisionAdopted(graph: 1, revision: 3));
      expect(
        n.toString(),
        'AudRevisionAdoptedNotification({type: revision, revision: 3, samplePosition: 512})',
      );
    });

    test('a state', () {
      native.ref.code = bindings.AUD_GRAPH_RUNNING;
      final n = parse(bindings.AUD_NOTIFY_STATE) as AudStateNotification;
      expect(n.state, AudGraphState.running);
      expect(n.toJson()['state'], 'running');
      expect(n.toOsc(1), isNull);
    });

    test('a finished node', () {
      native.ref.node = 4;
      final n = parse(bindings.AUD_NOTIFY_NODE_DONE) as AudNodeDoneNotification;
      expect(n.node, 4);
      expect(n.toJson()['type'], 'nodeDone');
      expect(n.toOsc(2), const AudNodeRetired(graph: 2, node: 4));
    });

    test('a diagnostic with its message', () {
      final messages = {
        AUD_ERROR_LATE: 'events arrived late',
        AUD_ERROR_RETIRED: 'events of a retired node were dropped',
        AUD_ERROR_QUEUE_FULL: 'entries were dropped: a capacity was exceeded',
        AUD_ERROR_CAPACITY: 'a note tracker was full: note ons were dropped',
        AUD_ERROR_STATE: 'a node is parked for its state: its events wait',
        AUD_ERROR_OVERLOAD: 'the block took 99 ns, longer than it lasts',
        AUD_ERROR_UNSUPPORTED: 'the transport refused a request',
        AUD_ERROR_FAILED: 'AUD_ERROR_FAILED',
      };
      for (final entry in messages.entries) {
        native.ref
          ..code = entry.key
          ..count = 2
          ..node = 0
          ..value = 99;
        final n =
            parse(bindings.AUD_NOTIFY_DIAGNOSTIC) as AudDiagnosticNotification;
        expect(n.message, entry.value);
        expect(n.name, AudAbi.resultName(entry.key));
        expect(n.toJson()['count'], 2);
        expect(
          n.toOsc(1),
          AudDiagnostic(code: entry.key, message: '${entry.value} (2)'),
        );
      }
      native.ref.node = 7;
      final n =
          parse(bindings.AUD_NOTIFY_DIAGNOSTIC) as AudDiagnosticNotification;
      expect(
        n.toOsc(1),
        const AudDiagnostic(
          code: AUD_ERROR_FAILED,
          message: 'AUD_ERROR_FAILED (2)',
          address: '/graph/1/node/7',
        ),
      );
    });

    test('a realtime violation the watchdog caught', () {
      native.ref
        ..code = bindings.AUD_GRAPH_ERROR_REALTIME_VIOLATION
        ..count = 5
        ..value = bindings.AUD_GRAPH_VIOLATION_NEW;
      final n =
          parse(bindings.AUD_NOTIFY_DIAGNOSTIC) as AudDiagnosticNotification;
      expect(n.name, 'AUD_GRAPH_ERROR_REALTIME_VIOLATION');
      expect(n.message, 'the realtime thread allocated, freed or logged');
      expect(n.toJson()['name'], 'AUD_GRAPH_ERROR_REALTIME_VIOLATION');
    });

    test('an event', () {
      native.ref.node = 5;
      const AudParamEvent(
        paramIndex: 1,
        value: 0.5,
      ).writeToRef(native.ref.event);
      final n = parse(bindings.AUD_NOTIFY_EVENT) as AudEventNotification;
      expect(n.node, 5);
      expect(n.event, const AudParamEvent(paramIndex: 1, value: 0.5));
      expect(
        n.toJson()['event'],
        const AudParamEvent(paramIndex: 1, value: 0.5).toJson(),
      );
    });

    test('the transport', () {
      native.ref
        ..code = AUD_TRANSPORT_REQUEST_SET_TEMPO
        ..value = AudBeats.ticks(2.5)
        ..number = 90
        ..count = 1;
      final n =
          parse(bindings.AUD_NOTIFY_TRANSPORT) as AudTransportNotification;
      expect(n.type, AudTransportRequestType.setTempo);
      expect(n.beat, 2.5);
      expect(n.tempo, 90);
      expect(n.playing, isTrue);
      expect(n.toJson()['request'], 'setTempo');
    });

    test('a time reset', () {
      native.ref.count = 1;
      final n =
          parse(bindings.AUD_NOTIFY_TIME_RESET) as AudTimeResetNotification;
      expect(n.count, 1);
      expect(n.toJson(), {
        'type': 'timeReset',
        'count': 1,
        'samplePosition': 512,
      });
    });

    test('refuses an unknown type', () {
      expect(() => parse(99), throwsArgumentError);
    });
  });

  group('AudGraphOptions', () {
    const options = AudGraphOptions(
      maxNodes: 4,
      maxConnections: 5,
      paramQueueCapacity: 6,
      eventQueueCapacity: 7,
      schedulerCapacity: 8,
      notificationCapacity: 9,
      maxEventsPerBlock: 10,
      fadeFrames: 11,
      maxTailFrames: 12,
      lookahead: Duration(seconds: 2),
      dropLateEvents: true,
    );
    test('writeTo(config) fills the native configuration', () {
      final config = calloc<bindings.AudGraphConfig>();
      try {
        options.writeTo(config.ref);
        expect(config.ref.flags, bindings.AUD_GRAPH_DROP_LATE_EVENTS);
        expect(config.ref.max_nodes, 4);
        expect(config.ref.max_connections, 5);
        expect(config.ref.param_queue_capacity, 6);
        expect(config.ref.event_queue_capacity, 7);
        expect(config.ref.scheduler_capacity, 8);
        expect(config.ref.notification_capacity, 9);
        expect(config.ref.max_events_per_block, 10);
        expect(config.ref.fade_frames, 11);
        expect(config.ref.max_tail_frames, 12);
        expect(config.ref.lookahead_ns, 2000000000);
        const AudGraphOptions().writeTo(config.ref);
        expect(config.ref.flags, 0);
        expect(config.ref.lookahead_ns, 0);
      } finally {
        calloc.free(config);
      }
    });
  });

  group('AudGraphStats', () {
    test('toDart() reads the counters', () {
      final stats = calloc<bindings.AudGraphStats>();
      stats.ref
        ..state = AUD_GRAPH_RUNNING
        ..revision = 2
        ..blocks_rendered = 3
        ..render_time_max_ns = 4
        ..output_peak = 0.5
        ..realtime_violations = 1;
      final read = stats.ref.toDart();
      calloc.free(stats);
      expect(read.state, AudGraphState.running);
      expect(read.revision, 2);
      expect(read.blocksRendered, 3);
      expect(read.renderTimeMaxNs, 4);
      expect(read.outputPeak, 0.5);
      expect(read.realtimeViolations, 1);
    });
  });

  group('AudTransportState', () {
    test('toDart() reads the transport', () {
      final state = calloc<bindings.AudGraphTransportState>();
      state.ref
        ..playing = 1
        ..beat = AudBeats.factor
        ..tempo = 90
        ..numerator = 3
        ..denominator = 8
        ..looping = 1
        ..loop_start = 0
        ..loop_end = 4 * AudBeats.factor;
      final read = state.ref.toDart();
      calloc.free(state);
      expect(read.playing, isTrue);
      expect(read.beatTicks, AudBeats.factor);
      expect(read.tempo, 90);
      expect(read.numerator, 3);
      expect(read.denominator, 8);
      expect(read.looping, isTrue);
      expect(read.loopEndTicks, 4 * AudBeats.factor);
    });
  });
}
