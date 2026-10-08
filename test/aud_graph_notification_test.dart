// @license
// Copyright (c) Audanika. All Rights Reserved.
//
// Use of this source code is governed by terms that can be
// found in the LICENSE file in the root of this package.

import 'dart:ffi';

import 'package:aud_audio_core/aud_audio_core.dart';
import 'package:aud_audio_graph/aud_audio_graph.dart';
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
    return AudGraphNotification.fromNative(native.ref);
  }

  group('AudGraphNotification.fromNative(native)', () {
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
        AUD_ERROR_CAPACITY: 'the scheduler or a note tracker was full',
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
}
