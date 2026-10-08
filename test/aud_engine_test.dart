// @license
// Copyright (c) Audanika. All Rights Reserved.
//
// Use of this source code is governed by terms that can be
// found in the LICENSE file in the root of this package.

import 'dart:ffi';
import 'dart:math';
import 'dart:typed_data';

import 'package:aud_audio_core/aud_audio_core.dart';
import 'package:aud_audio_graph/aud_audio_graph.dart';
import 'package:ffi/ffi.dart';
import 'package:test/test.dart';

void main() {
  late AudEngine engine;

  setUp(() => engine = AudEngine());
  tearDown(() => engine.dispose());

  // Counts the rising zero crossings of the first channel.
  int risingZeroCrossings(Float32List interleaved, int channels) {
    var count = 0;
    for (var i = channels; i < interleaved.length; i += channels) {
      if (interleaved[i - channels] < 0 && interleaved[i] >= 0) count++;
    }
    return count;
  }

  // Renders one second in blocks and returns the samples.
  Float32List renderSecond() {
    final result = Float32List(engine.sampleRate.toInt() * engine.channels);
    var offset = 0;
    while (offset < result.length) {
      final remaining = (result.length - offset) ~/ engine.channels;
      final block = engine.render(min(engine.maxFrames, remaining));
      result.setRange(offset, offset + block.length, block);
      offset += block.length;
    }
    return result;
  }

  group('AudEngine', () {
    test('refuses an invalid configuration', () {
      expect(
        () => AudEngine(sampleRate: 0),
        throwsA(
          isA<AudEngineException>()
              .having((e) => e.code, 'code', AUD_ERROR_INVALID_ARGUMENT)
              .having((e) => e.toString(), 'toString', contains('created')),
        ),
      );
    });

    test('registers the reference node types with their parameters', () {
      final types = engine.nodeTypes;
      expect(types.map((t) => t.id), ['aud.ref.sine', 'aud.ref.gain']);
      final sine = types.first;
      expect(sine.name, 'Reference sine oscillator');
      expect(sine.takesEvents, isFalse);
      expect(sine.params.map((p) => p.id), ['frequency', 'gain']);
      final frequency = sine.params.first;
      expect(frequency.unit, 'Hz');
      expect(frequency.min, 20);
      expect(frequency.max, 20000);
      expect(frequency.defaultValue, 440);
      expect(types.last.params.single.max, 4);
    });

    test('exposes the handle, the host api and the render callback', () {
      expect(engine.handle, isNot(nullptr));
      expect(engine.hostApi, isNot(nullptr));
      expect(AudEngine.renderCallback, isNot(nullptr));
      final host = engine.hostApi.cast<AudHostApi>().ref;
      expect(host.abi_major, AudAbi.major);
      expect(host.abi_minor, AudAbi.minor);
      expect(host.host, engine.handle);
    });

    group('createNode(typeId)', () {
      test('creates instances with ids in slot order', () {
        expect(engine.createNode('aud.ref.sine'), 0);
        expect(engine.createNode('aud.ref.gain'), 1);
      });

      test('refuses an unknown type', () {
        expect(
          () => engine.createNode('aud.ref.unknown'),
          throwsA(
            isA<AudEngineException>()
                .having((e) => e.code, 'code', AUD_ERROR_UNKNOWN_TYPE)
                .having((e) => e.message, 'message', contains('UNKNOWN_TYPE')),
          ),
        );
      });

      test('refuses more instances than slots', () {
        final small = AudEngine(maxNodes: 1);
        addTearDown(small.dispose);
        small.createNode('aud.ref.sine');
        expect(
          () => small.createNode('aud.ref.sine'),
          throwsA(
            isA<AudEngineException>().having(
              (e) => e.code,
              'code',
              AUD_ERROR_OUT_OF_MEMORY,
            ),
          ),
        );
      });
    });

    group('setChain(nodes) and render(frames)', () {
      test('renders silence without a chain', () {
        final output = engine.render(256);
        expect(output, hasLength(512));
        expect(output.every((s) => s == 0), isTrue);
        expect(engine.stats.blocksRendered, 1);
        expect(engine.stats.framesRendered, 256);
      });

      test('renders the sine through the gain at 440 Hz', () {
        final sine = engine.createNode('aud.ref.sine');
        final gain = engine.createNode('aud.ref.gain');
        expect(engine.setChain([sine, gain]), 1);
        final output = renderSecond();
        expect(risingZeroCrossings(output, engine.channels), closeTo(440, 2));
        expect(output.reduce((a, b) => a > b ? a : b), closeTo(0.2, 0.01));
        expect(output[0], output[1], reason: 'both channels carry the sine');
        expect(engine.stats.programRevision, 1);
        expect(engine.stats.outputPeak, closeTo(0.2, 0.01));
        engine.resetStats();
        final reset = engine.stats;
        expect(reset.blocksRendered, 0);
        expect(reset.outputPeak, 0);
        expect(reset.commandLatencyMinNs, 0);
        expect(reset.renderTimeMeanNs, 0);
      });

      test('refuses a chain with an unknown node', () {
        expect(
          () => engine.setChain([7]),
          throwsA(
            isA<AudEngineException>().having(
              (e) => e.code,
              'code',
              AUD_ERROR_INVALID_ARGUMENT,
            ),
          ),
        );
      });

      test('refuses more frames than prepared', () {
        expect(() => engine.render(engine.maxFrames + 1), throwsArgumentError);
      });

      test('adopts a new chain at the next block and bumps the revision', () {
        final sine = engine.createNode('aud.ref.sine');
        engine.setChain([sine]);
        engine.render(64);
        expect(engine.setChain([]), 2);
        expect(engine.setChain([sine]), 3);
        engine.render(64);
        expect(engine.stats.programRevision, 3);
        expect(engine.setChain([]), 4);
        engine.render(64);
        expect(engine.render(64).every((s) => s == 0), isTrue);
      });
    });

    group('setParam(node, param, value)', () {
      test('is applied at the next block start and measured', () {
        final sine = engine.createNode('aud.ref.sine');
        engine.setChain([sine]);
        engine.setParam(sine, 0, 880);
        final output = renderSecond();
        expect(risingZeroCrossings(output, engine.channels), closeTo(880, 2));
        final stats = engine.stats;
        expect(stats.commandsApplied, 1);
        expect(stats.commandsRejected, 0);
        expect(stats.commandLatencyCount, 1);
        expect(stats.commandLatencyMinNs, greaterThanOrEqualTo(0));
        expect(stats.commandLatencyMaxNs, lessThan(1000000000));
        expect(stats.commandLatencyMeanNs, stats.commandLatencySumNs);
        expect(stats.renderTimeMaxNs, greaterThan(0));
        expect(stats.renderTimeMeanNs, greaterThan(0));
      });

      test('ramps the gain to zero without a click', () {
        final sine = engine.createNode('aud.ref.sine');
        final gain = engine.createNode('aud.ref.gain');
        engine.setChain([sine, gain]);
        engine.render(engine.maxFrames);
        engine.setParam(gain, 0, 0);
        final ramp = engine.render(engine.maxFrames);
        expect(
          ramp.any((s) => s != 0),
          isTrue,
          reason: 'ramps inside the block',
        );
        final silent = engine.render(engine.maxFrames);
        expect(silent.every((s) => s == 0), isTrue);
      });

      test('refuses an unknown node or parameter', () {
        final sine = engine.createNode('aud.ref.sine');
        for (final call in [
          () => engine.setParam(sine, 2, 1),
          () => engine.setParam(5, 0, 1),
        ]) {
          expect(
            call,
            throwsA(
              isA<AudEngineException>().having(
                (e) => e.code,
                'code',
                AUD_ERROR_INVALID_ARGUMENT,
              ),
            ),
          );
        }
      });

      test('refuses when the queue is full and counts the rejection', () {
        final small = AudEngine(commandQueueCapacity: 4);
        addTearDown(small.dispose);
        final sine = small.createNode('aud.ref.sine');
        for (var i = 0; i < 4; i++) {
          small.setParam(sine, 1, 0.1);
        }
        expect(
          () => small.setParam(sine, 1, 0.1),
          throwsA(
            isA<AudEngineException>().having(
              (e) => e.code,
              'code',
              AUD_ERROR_QUEUE_FULL,
            ),
          ),
        );
        expect(small.stats.commandsRejected, 1);
        small.render(16);
        expect(small.stats.commandsApplied, 4);
      });
    });

    group('noteOn, noteOff and setString', () {
      test('refuse nodes that take no events or strings', () {
        final sine = engine.createNode('aud.ref.sine');
        expect(
          () => engine.noteOn(sine, number: 60),
          throwsA(
            isA<AudEngineException>().having(
              (e) => e.code,
              'code',
              AUD_ERROR_INVALID_ARGUMENT,
            ),
          ),
        );
        expect(
          () => engine.noteOff(sine, number: 60),
          throwsA(isA<AudEngineException>()),
        );
        expect(
          () => engine.setString(sine, 0, 'value'),
          throwsA(
            isA<AudEngineException>().having(
              (e) => e.code,
              'code',
              AUD_ERROR_STATE,
            ),
          ),
        );
      });
    });

    group('destroyNode(node)', () {
      test('destroys a node outside the chain', () {
        final sine = engine.createNode('aud.ref.sine');
        engine.setChain([sine]);
        expect(
          () => engine.destroyNode(sine),
          throwsA(
            isA<AudEngineException>().having(
              (e) => e.code,
              'code',
              AUD_ERROR_STATE,
            ),
          ),
        );
        engine.setChain([]);
        expect(
          () => engine.destroyNode(sine),
          throwsA(
            isA<AudEngineException>().having(
              (e) => e.code,
              'code',
              AUD_ERROR_STATE,
            ),
          ),
          reason: 'the realtime thread has not adopted the new chain',
        );
        engine.render(16);
        engine.destroyNode(sine);
        expect(
          () => engine.destroyNode(sine),
          throwsA(
            isA<AudEngineException>().having(
              (e) => e.code,
              'code',
              AUD_ERROR_INVALID_ARGUMENT,
            ),
          ),
        );
        expect(engine.createNode('aud.ref.gain'), sine, reason: 'slot reused');
      });
    });

    group('hostApi.register_node_type', () {
      late Pointer<AudNodeDescriptor> descriptor;
      late Pointer<AudNodeVTable> vtable;
      late Pointer<Utf8> typeId;
      late int Function(Pointer<Void>, Pointer<AudNodeDescriptor>) register;

      setUp(() {
        vtable = calloc<AudNodeVTable>();
        vtable.ref
          ..struct_size = sizeOf<AudNodeVTable>()
          ..create = Pointer.fromAddress(1)
          ..destroy = Pointer.fromAddress(1)
          ..prepare = Pointer.fromAddress(1)
          ..process = Pointer.fromAddress(1);
        typeId = 'aud.test.node'.toNativeUtf8();
        descriptor = calloc<AudNodeDescriptor>();
        descriptor.ref
          ..struct_size = sizeOf<AudNodeDescriptor>()
          ..abi_major = AudAbi.major
          ..abi_minor = AudAbi.minor
          ..type_id = typeId.cast()
          ..name = typeId.cast()
          ..num_outputs = 1
          ..vtable = vtable;
        register = engine.hostApi
            .cast<AudHostApi>()
            .ref
            .register_node_type
            .asFunction();
      });

      tearDown(() {
        calloc.free(descriptor);
        calloc.free(vtable);
        calloc.free(typeId);
      });

      test('accepts a matching descriptor once', () {
        expect(register(engine.handle, descriptor), AUD_OK);
        expect(engine.nodeTypes.last.id, 'aud.test.node');
        expect(register(engine.handle, descriptor), AUD_ERROR_DUPLICATE_TYPE);
      });

      test('refuses another major and a newer minor', () {
        descriptor.ref.abi_major = AudAbi.major + 1;
        expect(register(engine.handle, descriptor), AUD_ERROR_ABI_MAJOR);
        descriptor.ref.abi_major = AudAbi.major;
        descriptor.ref.abi_minor = AudAbi.minor + 1;
        expect(register(engine.handle, descriptor), AUD_ERROR_ABI_MINOR);
      });

      test('refuses a descriptor without a vtable or with a bad size', () {
        vtable.ref.process = nullptr;
        expect(register(engine.handle, descriptor), AUD_ERROR_INVALID_ARGUMENT);
        descriptor.ref.struct_size = 4;
        expect(register(engine.handle, descriptor), AUD_ERROR_INVALID_ARGUMENT);
        expect(register(engine.handle, nullptr), AUD_ERROR_INVALID_ARGUMENT);
      });
    });

    test('dispose() can be called twice', () {
      final other = AudEngine();
      other.dispose();
      other.dispose();
    });
  });
}
