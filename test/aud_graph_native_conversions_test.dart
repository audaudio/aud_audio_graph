// @license
// Copyright (c) Audanika. All Rights Reserved.
//
// Use of this source code is governed by terms that can be
// found in the LICENSE file in the root of this package.

import 'dart:ffi';
import 'dart:typed_data';

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

    test('a load', () {
      native.ref
        ..node = 3
        ..value = 7
        ..code = AUD_PENDING
        ..number = 0.5;
      final n = parse(bindings.AUD_NOTIFY_LOAD) as AudLoadNotification;
      expect((n.node, n.load, n.result, n.progress), (3, 7, AUD_PENDING, 0.5));
      expect(n.isRunning, isTrue);
      expect(n.toJson(), {
        'type': 'load',
        'node': 3,
        'load': 7,
        'result': 'AUD_PENDING',
        'progress': 0.5,
        'samplePosition': 512,
      });
    });

    test('messages of nodes', () {
      native.ref.count = 2;
      final n = parse(bindings.AUD_NOTIFY_MESSAGE) as AudMessageNotification;
      expect(n.count, 2);
      expect(n.toJson(), {
        'type': 'message',
        'count': 2,
        'samplePosition': 512,
      });
    });

    test('a retired event source', () {
      native.ref
        ..node = 4
        ..value = 0xABCD;
      final n =
          parse(bindings.AUD_NOTIFY_SOURCE_DONE) as AudSourceDoneNotification;
      expect(n.node, 4);
      expect(n.source, 0xABCD);
      expect(n.toJson()['type'], 'sourceDone');
    });

    test('refuses an unknown type', () {
      expect(() => parse(99), throwsArgumentError);
    });
  });

  group('event sources and timing (ticket 26)', () {
    test('AudEventSourceStats.toDart() reads the counters', () {
      final stats = calloc<bindings.AudEventSourceStats>();
      stats.ref
        ..attached = 1
        ..pushed = 10
        ..rejected = 1
        ..delivered = 9
        ..late = 2
        ..max_late_ns = 3000
        ..dropped = 1
        ..reordered = 2
        ..releases = 3
        ..max_delay_ns = 4000
        ..last_word0 = 0x20903C64
        ..last_timestamp_ns = 5000;
      stats.ref.delay_histogram[1] = 9;
      stats.ref.latency_histogram[bindings.AUD_SOURCE_LATENCY_BINS] = 9;
      final read = stats.ref.toDart();
      calloc.free(stats);
      expect(read.attached, isTrue);
      expect(read.pushed, 10);
      expect(read.rejected, 1);
      expect(read.delivered, 9);
      expect(read.late, 2);
      expect(read.maxLateNs, 3000);
      expect(read.dropped, 1);
      expect(read.reordered, 2);
      expect(read.releases, 3);
      expect(read.maxDelayNs, 4000);
      expect(read.lastWord0, 0x20903C64);
      expect(read.lastTimestampNs, 5000);
      expect(read.delayHistogram, hasLength(501));
      expect(read.delayHistogram[1], 9);
      expect(read.latencyHistogram, hasLength(1025));
      expect(read.latencyHistogram.last, 9);
      expect(read.delayMedianNs, 100000);
      expect(read.latencyMedianNs, 1025 * 250000);
    });

    test('AudGraphTiming.toDart() reads the timing', () {
      final timing = calloc<bindings.AudGraphTiming>();
      timing.ref
        ..flags = bindings.AUD_TIMING_VALID | bindings.AUD_TIMING_OFFLINE
        ..sample_rate = 44100
        ..sample_position = 1024
        ..host_time_ns = 7
        ..frames = 128
        ..output_latency_frames = 441
        ..max_period_ns = 9
        ..nominal_period_ns = 8
        ..time_filter_resets = 2;
      final read = timing.ref.toDart();
      calloc.free(timing);
      expect(read.valid, isTrue);
      expect(read.offline, isTrue);
      expect(read.sampleRate, 44100);
      expect(read.samplePosition, 1024);
      expect(read.hostTimeNs, 7);
      expect(read.frames, 128);
      expect(read.outputLatencyFrames, 441);
      expect(read.maxPeriodNs, 9);
      expect(read.nominalPeriodNs, 8);
      expect(read.timeFilterResets, 2);
    });
  });

  group('loads, usage and messages (ticket 25)', () {
    test('AudLoadInfo.toDart() reads a load with its texts', () {
      final info = calloc<bindings.AudLoadInfo>();
      info.ref
        ..id = 4
        ..node = 2
        ..result = AUD_ERROR_FAILED
        ..progress = 0.25
        ..cancelled = 1
        ..start_ns = 10
        ..end_ns = 20;
      const name = 'piano.sfz';
      for (var i = 0; i < name.length; i++) {
        info.ref.name[i] = name.codeUnitAt(i);
      }
      info.ref.message[0] = 'x'.codeUnitAt(0);
      final load = info.ref.toDart();
      calloc.free(info);
      expect(
        load,
        const AudLoad(
          id: 4,
          node: 2,
          name: 'piano.sfz',
          result: AUD_ERROR_FAILED,
          progress: 0.25,
          message: 'x',
          cancelled: true,
          startNs: 10,
          endNs: 20,
        ),
      );
    });

    test('AudCacheUsage.toDart() reads the resources', () {
      final usage = calloc<bindings.AudCacheUsage>();
      usage.ref
        ..caches = 1
        ..budget = 100
        ..bytes = 50
        ..trims = 2
        ..trimmed = 30
        ..workers = 2
        ..tasks = 3;
      final read = usage.ref.toDart();
      calloc.free(usage);
      expect(
        read,
        const AudCacheUsage(
          caches: 1,
          budget: 100,
          bytes: 50,
          trims: 2,
          trimmed: 30,
          workers: 2,
          tasks: 3,
        ),
      );
    });

    test('a message is written natively and read back from a record', () {
      final message = AudOscMessage('/region0/volume', [
        7,
        1 << 40,
        0.5,
        'x',
        Uint8List.fromList([1, 2]),
        Uint8List(0),
        true,
        false,
        null,
        AudImpulse.instance,
      ]);
      using((arena) {
        final written = message.writeNative(arena);
        expect(written.ref.num_args, 10);
        expect(written.ref.path.cast<Utf8>().toDartString(), '/region0/volume');
        final args = written.ref.args;
        expect(
          [for (var i = 0; i < 10; i++) String.fromCharCode(args[i].type)],
          [
            'i', 'h', 'f', 's', 'b', 'b', 'T', 'F', 'N', 'I', //
          ],
        );
        expect(args[0].value.i, 7);
        expect(args[1].value.h, 1 << 40);
        expect(args[2].value.f, 0.5);
        expect(args[3].value.s.cast<Utf8>().toDartString(), 'x');
        expect(args[4].size, 2);
        expect(args[4].value.b.cast<Uint8>()[1], 2);

        // The packed record of the C API, as a node's answer.
        final record = arena<bindings.AudGraphMessage>();
        final bytes = BytesBuilder()
          ..add('/r'.codeUnits)
          ..addByte(0)
          ..addByte('i'.codeUnitAt(0))
          ..add(
            (ByteData(4)..setInt32(0, -3, Endian.little)).buffer.asUint8List(),
          )
          ..addByte('h'.codeUnitAt(0))
          ..add(
            (ByteData(
              8,
            )..setInt64(0, 1 << 40, Endian.little)).buffer.asUint8List(),
          )
          ..addByte('f'.codeUnitAt(0))
          ..add(
            (ByteData(
              4,
            )..setFloat32(0, 0.25, Endian.little)).buffer.asUint8List(),
          )
          ..addByte('d'.codeUnitAt(0))
          ..add(
            (ByteData(
              8,
            )..setFloat64(0, 0.125, Endian.little)).buffer.asUint8List(),
          )
          ..addByte('s'.codeUnitAt(0))
          ..add('label'.codeUnits)
          ..addByte(0)
          ..addByte('b'.codeUnitAt(0))
          ..add(
            (ByteData(4)..setUint32(0, 2, Endian.little)).buffer.asUint8List(),
          )
          ..add([9, 8])
          ..addByte('T'.codeUnitAt(0))
          ..addByte('F'.codeUnitAt(0))
          ..addByte('N'.codeUnitAt(0))
          ..addByte('I'.codeUnitAt(0));
        final packed = bytes.toBytes();
        for (var i = 0; i < packed.length; i++) {
          record.ref.data[i] = packed[i];
        }
        record.ref
          ..node = 5
          ..sample_position = 256
          ..num_args = 10
          ..size = packed.length;
        final read = record.ref.toDart();
        expect(read.node, 5);
        expect(read.samplePosition, 256);
        expect(read.path, '/r');
        expect(read.arguments.sublist(0, 6), [
          -3,
          1 << 40,
          0.25,
          0.125,
          'label',
          Uint8List.fromList([9, 8]),
        ]);
        expect(read.arguments.sublist(6), [
          true,
          false,
          null,
          AudImpulse.instance,
        ]);
      });
    });

    test('a timetag is no argument of a node message', () {
      using((arena) {
        expect(
          () =>
              AudOscMessage('/x', [AudOscTimetag.immediate]).writeNative(arena),
          throwsA(
            isA<ArgumentError>().having(
              (e) => e.message,
              'message',
              'Not an argument of a node message',
            ),
          ),
        );
      });
    });
  });

  group('Duration.timeoutMs', () {
    const cases = [
      (Duration.zero, 0),
      (Duration(microseconds: 100), 1),
      (Duration(milliseconds: 1), 1),
      (Duration(microseconds: 1001), 2),
      (Duration(seconds: 2), 2000),
      (Duration(days: 50), 0xFFFFFFFF),
    ];
    for (final (timeout, ms) in cases) {
      test('$timeout waits $ms ms', () {
        expect(timeout.timeoutMs, ms);
      });
    }

    test('a negative timeout throws', () {
      expect(
        () => const Duration(milliseconds: -1).timeoutMs,
        throwsA(
          isA<ArgumentError>().having((e) => e.message, 'message', 'Negative'),
        ),
      );
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
      messageCapacity: 13,
      replyCapacity: 14,
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
        expect(config.ref.message_capacity, 13);
        expect(config.ref.reply_capacity, 14);
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
        ..realtime_violations = 1
        ..messages_dropped = 6;
      final read = stats.ref.toDart();
      calloc.free(stats);
      expect(read.state, AudGraphState.running);
      expect(read.revision, 2);
      expect(read.blocksRendered, 3);
      expect(read.renderTimeMaxNs, 4);
      expect(read.outputPeak, 0.5);
      expect(read.realtimeViolations, 1);
      expect(read.messagesDropped, 6);
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
