// @license
// Copyright (c) Audanika. All Rights Reserved.
//
// Use of this source code is governed by terms that can be
// found in the LICENSE file in the root of this package.

import 'dart:ffi';

import 'package:aud_audio_core/aud_audio_core_bindings.dart' as core;
import 'package:aud_audio_core/aud_audio_core_ffi.dart';
import 'package:aud_audio_graph/aud_audio_graph_ffi.dart';
import 'package:ffi/ffi.dart';
import 'package:test/test.dart';

/// What the recorder node saw of an event.
typedef Recorded = ({int position, int offset, int word0, int flags});

/// A node type defined in Dart that records the events it receives; the
/// offline renderer calls it on the test's own thread.
class DartRecorderNode {
  DartRecorderNode(Pointer<core.AudHostApi> host) {
    typeId = 'aud.test.recorder'.toNativeUtf8();
    name = 'Recorder'.toNativeUtf8();
    port = calloc<core.AudEventPortDescriptor>();
    port.ref
      ..struct_size = sizeOf<core.AudEventPortDescriptor>()
      ..id = name.cast()
      ..name = name.cast()
      ..flags = AUD_EVENT_PORT_MIDI;
    vtable = calloc<core.AudNodeVTable>();
    vtable.ref
      ..struct_size = sizeOf<core.AudNodeVTable>()
      ..create = _create.nativeFunction
      ..destroy = _destroy.nativeFunction
      ..prepare = _prepare.nativeFunction
      ..process = _process.nativeFunction;
    descriptor = calloc<core.AudNodeDescriptor>();
    descriptor.ref
      ..struct_size = sizeOf<core.AudNodeDescriptor>()
      ..abi_major = AudAbi.major
      ..abi_minor = AudAbi.minor
      ..version = 1
      ..type_id = typeId.cast()
      ..name = name.cast()
      ..vendor = name.cast()
      ..capabilities = AUD_NODE_CAP_VARIABLE_BLOCK | AUD_NODE_CAP_EVENTS
      ..num_event_inputs = 1
      ..event_inputs = port
      ..vtable = vtable;
    final register = host.ref.register_node_type
        .asFunction<
          int Function(Pointer<Void>, Pointer<core.AudNodeDescriptor>)
        >();
    result = register(host.ref.host, descriptor);
  }

  late final Pointer<Utf8> typeId;
  late final Pointer<Utf8> name;
  late final Pointer<core.AudEventPortDescriptor> port;
  late final Pointer<core.AudNodeVTable> vtable;
  late final Pointer<core.AudNodeDescriptor> descriptor;
  late final int result;

  /// The events the instances received.
  static final List<Recorded> records = [];

  static final _create =
      NativeCallable<
        Pointer<Void> Function(
          Pointer<core.AudNodeDescriptor>,
          Pointer<core.AudHostApi>,
        )
      >.isolateLocal(_onCreate);
  static final _destroy =
      NativeCallable<Void Function(Pointer<Void>)>.isolateLocal(_onDestroy);
  static final _prepare =
      NativeCallable<
        Int32 Function(Pointer<Void>, Pointer<core.AudPrepareInfo>)
      >.isolateLocal(_onPrepare, exceptionalReturn: AUD_ERROR_FAILED);
  static final _process =
      NativeCallable<
        Void Function(Pointer<Void>, Pointer<core.AudProcessContext>)
      >.isolateLocal(_onProcess);

  static Pointer<Void> _onCreate(
    Pointer<core.AudNodeDescriptor> descriptor,
    Pointer<core.AudHostApi> host,
  ) => calloc<Uint8>(1).cast();
  static void _onDestroy(Pointer<Void> instance) => calloc.free(instance);
  static int _onPrepare(
    Pointer<Void> instance,
    Pointer<core.AudPrepareInfo> info,
  ) => AUD_OK;
  static void _onProcess(
    Pointer<Void> instance,
    Pointer<core.AudProcessContext> context,
  ) {
    for (var i = 0; i < context.ref.num_events; i++) {
      final event = context.ref.events[i];
      records.add((
        position: context.ref.sample_position,
        offset: event.sample_offset,
        word0: event.words[0],
        flags: event.flags,
      ));
    }
  }

  /// Frees the descriptor; the graph that uses it has to be disposed.
  void dispose() {
    calloc.free(descriptor);
    calloc.free(vtable);
    calloc.free(port);
    calloc.free(name);
    calloc.free(typeId);
  }
}

void main() {
  late AudGraphFfi graph;
  late DartRecorderNode type;
  late AudNode recorder;

  setUp(() {
    graph = AudGraphFfi(listen: false, maxFrames: 256);
    type = DartRecorderNode(graph.hostApi);
    expect(type.result, AUD_OK);
    DartRecorderNode.records.clear();
    recorder = graph.createNode('aud.test.recorder');
  });
  tearDown(() {
    graph.dispose();
    type.dispose();
  });

  void commitAndStart() {
    graph.transaction((tx) {});
    graph.start();
  }

  int noteOn(int note) =>
      AudUmp.midi1Word(group: 0, statusByte: 0x90, data1: note, data2: 100);
  int noteOff(int note) =>
      AudUmp.midi1Word(group: 0, statusByte: 0x80, data1: note, data2: 64);
  int control(int controller, int value) => AudUmp.midi1Word(
    group: 0,
    statusByte: 0xB0,
    data1: controller,
    data2: value,
  );

  /// The host time of a frame on the virtual timeline from [origin].
  int atFrame(double frame, {int origin = 1000000000}) =>
      origin + (frame * 1e9 / 48000).round();

  /// Renders offline on the timeline of [atFrame], continuing where the
  /// graph stands.
  void render(int frames, {int blockFrames = 256}) =>
      AudOfflineRenderer(graph).render(
        frames: frames,
        blockFrames: blockFrames,
        startHostTimeNs: atFrame(graph.samplePosition.toDouble()),
      );

  group('AudGraph.createEventSource(node, ...)', () {
    test('needs a committed target and checks its arguments', () {
      expect(
        () => graph.createEventSource(recorder),
        throwsA(
          isA<AudGraphException>().having(
            (e) => e.name,
            'name',
            'AUD_ERROR_STATE',
          ),
        ),
      );
      commitAndStart();
      expect(
        () => graph.createEventSource(graph.io),
        throwsA(
          isA<AudGraphException>().having(
            (e) => e.name,
            'name',
            'AUD_ERROR_INVALID_ARGUMENT',
          ),
        ),
      );
      expect(
        () => graph.createEventSource(recorder, port: 3),
        throwsA(isA<AudGraphException>()),
      );
      final source = graph.createEventSource(
        recorder,
        timing: AudSourceTiming.immediate,
        latencyNs: 5,
        capacity: 100,
      );
      expect(source.node, recorder);
      expect(source.port, 0);
      expect(source.timing, AudSourceTiming.immediate);
      expect(source.latencyNs, 5);
      expect(source.capacity, 128);
      expect(source.size, 0);
      expect(source.isDisposed, isFalse);
      expect(source.address, isNot(0));
      expect(source.pointer, isNot(nullptr));
      expect(source.toString(), 'AudEventSource(n1:0, immediate, 5 ns)');
      expect(graph.eventSources, [source]);
      expect(AudEventSourceFfi.pushFunctionAddress, isNot(0));
      expect(AudEventSourceFfi.releaseFunctionAddress, isNot(0));
      expect(AudEventSourceFfi.sizeFunctionAddress, isNot(0));
      expect(graph.createEventSource(recorder).capacity, 4096);
      graph.transaction((tx) => tx.remove(recorder));
      expect(
        () => graph.createEventSource(recorder),
        throwsA(
          isA<AudGraphException>().having(
            (e) => e.name,
            'name',
            'AUD_ERROR_RETIRED',
          ),
        ),
      );
    });

    test('lands fixed packets at their timestamp plus C, to the sample', () {
      commitAndStart();
      // C = 10 ms = 480 frames: frame 1000 sounds at frame 1480, offset
      // 200 of the sixth block of 256; the input to output time is
      // 480.5 frames, bin 40 of 0.25 ms.
      final source = graph.createEventSource(recorder, latencyNs: 10000000);
      expect(source.push([noteOn(60)], hostTimeNs: atFrame(999.5)), isTrue);
      expect(source.push([noteOff(60)], hostTimeNs: atFrame(1999.5)), isTrue);
      expect(source.size, 2);
      render(1536);
      expect(source.size, 1);
      render(1280);
      final records = DartRecorderNode.records;
      expect(records, hasLength(2));
      expect(records[0].position + records[0].offset, 1480);
      expect(records[0].flags, 0);
      expect(records[1].position + records[1].offset, 2480);
      final stats = source.stats;
      expect(stats.pushed, 2);
      expect(stats.delivered, 2);
      expect(stats.late, 0);
      expect(stats.reordered, 0);
      expect(stats.lastWord0, noteOff(60));
      expect(stats.lastTimestampNs, atFrame(1999.5));
      expect(stats.latencyHistogram[40], 2);
      expect(stats.latencyMedianNs, 41 * 250000);
      expect(stats.delayP99Ns, isNotNull);
      expect(graph.stats.eventsDelivered, 2);
    });

    test('plays immediate packets at the block start with the live flag', () {
      commitAndStart();
      final source = graph.createEventSource(
        recorder,
        timing: AudSourceTiming.immediate,
      );
      expect(source.push([noteOn(60)], hostTimeNs: atFrame(999.5)), isTrue);
      AudOfflineRenderer(
        graph,
      ).render(frames: 512, blockFrames: 256, startHostTimeNs: 1000000000);
      final records = DartRecorderNode.records;
      expect(records, hasLength(1));
      expect(records[0].position, 0);
      expect(records[0].offset, 0);
      expect(records[0].flags, AUD_EVENT_FLAG_LIVE);
    });

    test('counts late and reordered packets', () {
      commitAndStart();
      final source = graph.createEventSource(recorder);
      render(256);
      source.push([noteOn(60)], hostTimeNs: atFrame(-100));
      source.push([noteOn(62)], hostTimeNs: atFrame(600.5));
      source.push([noteOn(64)], hostTimeNs: atFrame(500.5));
      render(512);
      final records = DartRecorderNode.records;
      expect(records, hasLength(3));
      expect(records[0].position, 256);
      expect(records[0].offset, 0);
      expect(records[1].position + records[1].offset, 601);
      expect(records[2].position + records[2].offset, 601);
      final stats = source.stats;
      expect(stats.late, 1);
      expect(stats.maxLateNs, closeTo(356 * 1e9 / 48000, 1000));
      expect(stats.reordered, 1);
    });

    test('push checks the packet and reports a full queue', () {
      commitAndStart();
      final source = graph.createEventSource(
        recorder,
        timing: AudSourceTiming.immediate,
        capacity: 4,
      );
      expect(() => source.push([]), throwsArgumentError);
      expect(() => source.push([1, 2, 3, 4, 5]), throwsArgumentError);
      // A MIDI 1.0 word with two words is malformed.
      expect(
        () => source.push([noteOn(60), 0]),
        throwsA(
          isA<AudGraphException>().having(
            (e) => e.name,
            'name',
            'AUD_ERROR_INVALID_ARGUMENT',
          ),
        ),
      );
      for (var i = 0; i < 4; i++) {
        expect(source.push([noteOn(60 + i)]), isTrue);
      }
      expect(source.push([noteOn(70)]), isFalse);
      expect(source.stats.rejected, 2);
      // A MIDI 2.0 note on takes two words.
      source.release();
      AudOfflineRenderer(graph).render(frames: 256);
      expect(
        source.push(
          AudUmp.midi2Note(
            group: 0,
            channel: 0,
            noteOn: true,
            note: 60,
            velocity16: 0xFFFF,
          ),
        ),
        isTrue,
      );
      AudOfflineRenderer(graph).render(frames: 256);
      expect(DartRecorderNode.records, hasLength(1));
    });

    test('release flushes the queue, closes the notes, lifts the pedal', () {
      commitAndStart();
      final source = graph.createEventSource(
        recorder,
        timing: AudSourceTiming.immediate,
      );
      source.push([noteOn(60)]);
      source.push([control(64, 127)]);
      AudOfflineRenderer(graph).render(frames: 256);
      expect(DartRecorderNode.records, hasLength(2));
      DartRecorderNode.records.clear();
      source.push([noteOn(62)]);
      source.push([control(1, 10)]);
      source.release();
      AudOfflineRenderer(graph).render(frames: 256);
      final words = [for (final r in DartRecorderNode.records) r.word0];
      expect(words, [noteOff(60), control(1, 10), control(64, 0)]);
      final stats = source.stats;
      expect(stats.dropped, 1);
      expect(stats.releases, 1);
    });

    test('hands over between producers', () {
      commitAndStart();
      final source = graph.createEventSource(recorder);
      source.attach(1);
      expect(source.stats.attached, isTrue);
      expect(
        () => source.attach(2),
        throwsA(
          isA<AudGraphException>().having(
            (e) => e.code,
            'code',
            AUD_GRAPH_ERROR_BUSY,
          ),
        ),
      );
      expect(() => source.detach(2), throwsA(isA<AudGraphException>()));
      expect(
        () => source.dispose(),
        throwsA(
          isA<AudGraphException>().having(
            (e) => e.code,
            'code',
            AUD_GRAPH_ERROR_BUSY,
          ),
        ),
      );
      source.detach(1);
      expect(source.stats.attached, isFalse);
      source.attach(2);
      source.detach(2);
      source.latencyNs = 7;
      expect(source.latencyNs, 7);
    });

    test('dispose retires the source and the engine announces it', () {
      commitAndStart();
      final source = graph.createEventSource(
        recorder,
        timing: AudSourceTiming.immediate,
      );
      source.push([noteOn(60)]);
      AudOfflineRenderer(graph).render(frames: 256);
      final address = source.address;
      final received = <AudGraphNotification>[];
      graph.notifications.listen(received.add);
      source.dispose();
      expect(source.isDisposed, isTrue);
      expect(graph.eventSources, isEmpty);
      source.dispose();
      source.release();
      expect(() => source.push([noteOn(60)]), throwsStateError);
      expect(() => source.stats, throwsStateError);
      expect(() => source.attach(1), throwsStateError);
      expect(() => source.detach(1), throwsStateError);
      expect(() => source.latencyNs = 1, throwsStateError);
      // The note of the target closes, then the source is let go of.
      AudOfflineRenderer(graph).render(frames: 256);
      graph.pump();
      expect(DartRecorderNode.records.last.word0, noteOff(60));
      final done = received.whereType<AudSourceDoneNotification>().single;
      expect(done.node, recorder.handle);
      expect(done.source, address);
      expect(done.toJson()['type'], 'sourceDone');
      // A graph that goes takes its sources with it.
      final left = graph.createEventSource(recorder);
      graph.dispose();
      expect(left.isDisposed, isTrue);
    });
  });

  group('AudGraph.timing', () {
    test('is invalid before a block and reports the last block after', () {
      expect(graph.timing.valid, isFalse);
      commitAndStart();
      AudOfflineRenderer(graph).render(
        frames: 600,
        blockSizes: const [256, 128],
        startHostTimeNs: 1000000000,
      );
      final timing = graph.timing;
      expect(timing.valid, isTrue);
      expect(timing.offline, isTrue);
      expect(timing.sampleRate, 48000);
      expect(timing.samplePosition, 384);
      expect(timing.frames, 216);
      expect(timing.hostTimeNs, atFrame(384));
      expect(timing.outputLatencyFrames, 0);
      expect(timing.maxPeriodNs, 5333333);
      expect(timing.nominalPeriodNs, 5333333);
      expect(timing.timeFilterResets, 0);
      expect(timing.hostTimeAt(600), atFrame(600));
      expect(timing.sampleAt(atFrame(600)), 600);
    });
  });
}
