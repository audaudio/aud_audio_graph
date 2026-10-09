// @license
// Copyright (c) Audanika. All Rights Reserved.
//
// Use of this source code is governed by terms that can be
// found in the LICENSE file in the root of this package.

import 'dart:ffi';
import 'dart:io';
import 'dart:typed_data';

import 'package:aud_audio_core/aud_audio_core.dart';
import 'package:aud_audio_core/aud_audio_core_bindings.dart' as core;
import 'package:aud_audio_graph/aud_audio_graph.dart';
import 'package:aud_midi_standard/aud_midi_standard.dart';
import 'package:ffi/ffi.dart';
import 'package:test/test.dart';

void main() {
  stringNodeTests();
  late AudGraph graph;

  setUp(() {
    graph = AudGraph(listen: false, maxFrames: 256, outputChannels: const [1]);
  });
  tearDown(() => graph.dispose());

  AudNode oscillator({String name = 'osc'}) =>
      graph.createNode('aud.graph.oscillator', name: name);

  Float32List render(int frames) =>
      AudOfflineRenderer(graph).render(frames: frames).single.single;

  int risingCrossings(Float32List samples) {
    var count = 0;
    for (var i = 1; i < samples.length; i++) {
      if (samples[i - 1] < 0 && samples[i] >= 0) count++;
    }
    return count;
  }

  AudEvent noteOn(int note) => AudUmpEvent.fromMessage(
    MidiNoteOn(channel: 0, note: note, velocity: 100),
  ).single;

  group('AudGraph', () {
    test('is created with its format and its node types', () {
      expect(graph.sampleRate, 48000);
      expect(graph.maxFrames, 256);
      expect(graph.inputChannels, isEmpty);
      expect(graph.outputChannels, [1]);
      expect(graph.id, 1);
      expect(graph.isDisposed, isFalse);
      expect(graph.state, AudGraphState.created);
      expect(graph.hostApi, isNot(nullptr));
      expect(graph.nodeTypes.map((t) => t.typeId), [
        'aud.graph.feedback',
        'aud.graph.tap',
        'aud.graph.oscillator',
        'aud.graph.mixer',
        'aud.graph.filter',
        'aud.core.gain',
      ]);
      expect(graph.nodeType('aud.graph.filter')?.params.length, 3);
      expect(graph.nodeType('aud.nothing'), isNull);
      expect(graph.nodes, isEmpty);
      expect(graph.revision, 0);
      expect(graph.outputLatency, 0);
      expect(graph.samplePosition, 0);
      final bare = AudGraph(listen: false, registerCoreNodes: false, id: 2);
      addTearDown(bare.dispose);
      expect(bare.nodeType('aud.core.gain'), isNull);
      expect(bare.id, 2);
      final withInput = AudGraph(listen: false, inputChannels: const [2]);
      addTearDown(withInput.dispose);
      expect(withInput.io.outputChannels, [2]);
      expect(withInput.io.descriptor.outputBuses.single.id, 'in0');
    });

    test('refuses an invalid configuration', () {
      expect(
        () => AudGraph(listen: false, maxFrames: 0),
        throwsA(
          isA<AudGraphException>().having(
            (e) => e.message,
            'message',
            contains('not created'),
          ),
        ),
      );
    });

    group('createNode(typeId, ...)', () {
      test('creates and names instances', () {
        final first = graph.createNode('aud.graph.filter');
        expect(first.handle, 1);
        expect(first.name, 'n1');
        expect(first.inputChannels, [2]);
        expect(first.outputChannels, [2]);
        final mono = graph.createNode(
          'aud.graph.filter',
          name: 'mono',
          inputChannels: const [1],
          outputChannels: const [1],
        );
        expect(mono.inputChannels, [1]);
        final feedback = graph.createNode(
          'aud.graph.feedback',
          delayFrames: 512,
        );
        expect(feedback.descriptor.typeId, 'aud.graph.feedback');
        expect(graph.nodes, [first, mono, feedback]);
        expect(graph.node(2), mono);
        expect(graph.node(9), isNull);
        expect(graph.nodeNamed('mono'), mono);
        expect(graph.nodeNamed('graph'), graph.io);
        expect(graph.nodeNamed('nothing'), isNull);
        expect(graph.router.lookup('/graph/1/node/2'), isNotNull);
        expect(graph.paramsOf(mono), isEmpty);
        expect(graph.stringsOf(mono), isEmpty);
        expect(graph.latencyOf(mono), 0);
        expect(graph.leadOf(mono), 0);
      });

      test('refuses bad names and types', () {
        graph.createNode('aud.graph.filter', name: 'taken');
        expect(
          () => graph.createNode('aud.graph.filter', name: 'taken'),
          throwsArgumentError,
        );
        expect(
          () => graph.createNode('aud.graph.filter', name: '1 bad'),
          throwsArgumentError,
        );
        expect(
          () => graph.createNode('aud.nothing'),
          throwsA(
            isA<AudGraphException>().having(
              (e) => e.name,
              'name',
              'AUD_ERROR_UNKNOWN_TYPE',
            ),
          ),
        );
        expect(
          () => graph.createNode('aud.graph.filter', inputChannels: const [99]),
          throwsA(
            isA<AudGraphException>().having(
              (e) => e.name,
              'name',
              'AUD_ERROR_FORMAT',
            ),
          ),
        );
      });

      test('applies a preset', () {
        final osc = graph.createNode(
          'aud.graph.oscillator',
          preset: const AudNodePreset(
            typeId: 'aud.graph.oscillator',
            params: {'frequency': 880, 'waveform': 1},
          ),
        );
        expect(graph.paramsOf(osc), {'frequency': 880, 'waveform': 1});
        expect(
          () => graph.applyPreset(
            osc,
            const AudNodePreset(typeId: 'aud.graph.filter'),
          ),
          throwsArgumentError,
        );
        expect(
          () => graph.applyPreset(
            osc,
            const AudNodePreset(
              typeId: 'aud.graph.oscillator',
              strings: {'file': 'x'},
            ),
          ),
          throwsArgumentError,
        );
      });
    });

    group('transaction(edits)', () {
      test('commits revisions and records the topology', () {
        final osc = oscillator();
        expect(graph.transaction((tx) => tx.connect(osc, graph.io)), 1);
        expect(graph.connections, [
          (from: osc.handle, fromBus: 0, to: 0, toBus: 0, lowLatency: false),
        ]);
        expect(graph.transaction((tx) => tx.connectEvents(graph.io, osc)), 2);
        expect(graph.eventConnections, [
          (from: 0, fromPort: 0, to: osc.handle, toPort: 0),
        ]);
        graph.transaction((tx) => tx.connectEvents(graph.io, osc));
        expect(graph.eventConnections, hasLength(1));
        graph.transaction((tx) => tx.disconnectEvents(graph.io, osc));
        expect(graph.eventConnections, isEmpty);
        graph.transaction((tx) => tx.disconnect(osc, graph.io));
        expect(graph.connections, isEmpty);
        graph.transaction((tx) => tx.connectEvents(graph.io, osc));
        graph.transaction((tx) => tx.remove(osc));
        expect(graph.nodes, isEmpty);
        expect(graph.eventConnections, isEmpty);
        expect(graph.router.lookup('/graph/1/node/${osc.handle}'), isNull);
      });

      test('rolls back a failing edit or commit', () {
        final a = graph.createNode('aud.graph.filter', name: 'a');
        final b = graph.createNode('aud.graph.filter', name: 'b');
        const ghost = AudNode(
          handle: 77,
          name: 'ghost',
          descriptor: AudNodeDescriptor(typeId: 'x.y'),
          inputChannels: [],
          outputChannels: [],
        );
        expect(
          () => graph.transaction((tx) {
            tx.connect(a, b);
            tx.connect(a, ghost);
          }),
          throwsA(
            isA<AudGraphException>().having(
              (e) => e.name,
              'name',
              'AUD_ERROR_NOT_FOUND',
            ),
          ),
        );
        expect(graph.connections, isEmpty);
        expect(
          () => graph.transaction((tx) {
            tx.connect(a, b);
            tx.connect(b, a);
          }),
          throwsA(
            isA<AudGraphException>().having(
              (e) => e.name,
              'name',
              'AUD_ERROR_CYCLE',
            ),
          ),
        );
        expect(graph.connections, isEmpty);
        expect(graph.transaction((tx) => tx.connect(a, b)), 1);
      });

      test(
        'adopted(revision) completes once the realtime thread has it',
        () async {
          final osc = oscillator();
          expect(graph.adopted(0), completes);
          final revision = graph.transaction((tx) => tx.connect(osc, graph.io));
          final future = graph.adopted(revision);
          graph.start();
          render(256);
          expect(graph.revision, 1);
          expect(graph.pump(), greaterThan(0));
          await future;
          expect(graph.adopted(1), completes);
          final never = graph.adopted(9);
          graph.dispose();
          await never;
        },
      );
    });

    test('renders a sine and counts', () {
      final osc = oscillator();
      graph.transaction((tx) => tx.connect(osc, graph.io));
      graph.start();
      final second = render(48000);
      expect(risingCrossings(second), inInclusiveRange(439, 441));
      expect(graph.stats.framesRendered, 48000);
      expect(graph.samplePosition, 48000);
      expect(graph.stats.outputPeak, closeTo(0.5, 0.01));
      graph.resetStats();
      expect(graph.stats.framesRendered, 0);
    });

    group('setParam(node, param, value, ...)', () {
      test('sets by id or index, at once or ramped', () {
        final osc = oscillator();
        graph.setParam(osc, 'frequency', 880);
        graph.setParam(osc, AudOscillatorParams.amplitude, 1.0);
        expect(graph.paramsOf(osc), {'frequency': 880, 'amplitude': 1});
        graph.transaction((tx) => tx.connect(osc, graph.io));
        graph.start();
        expect(risingCrossings(render(48000)), inInclusiveRange(879, 881));
        graph.setParam(osc, 'frequency', 440, rampFrames: 480);
        render(4800);
        expect(risingCrossings(render(48000)), inInclusiveRange(439, 441));
        graph.setParam(
          osc,
          'amplitude',
          0,
          rampFrames: 100,
          at: AudTimestamp.sample(graph.samplePosition + 256),
        );
        render(256);
        expect(render(512).sublist(200), everyElement(0));
        expect(() => graph.setParam(osc, 9, 1), throwsArgumentError);
        expect(() => graph.setParam(osc, 'nothing', 1), throwsArgumentError);
      });
    });

    group('sendEvent(node, event, ...)', () {
      test('plays notes, schedules and cancels', () {
        final osc = oscillator();
        expect(
          () => graph.sendEvent(osc, noteOn(69)),
          throwsA(
            isA<AudGraphException>().having(
              (e) => e.name,
              'name',
              'AUD_ERROR_STATE',
            ),
          ),
        );
        graph.transaction((tx) => tx.connect(osc, graph.io));
        graph.start();
        graph.sendEvent(osc, noteOn(81));
        render(256);
        expect(risingCrossings(render(48000)), inInclusiveRange(879, 881));
        graph.sendEvent(
          osc,
          noteOn(69),
          at: AudTimestamp.sample(graph.samplePosition + 10000),
          id: 5,
        );
        graph.cancel(node: osc, id: 5);
        graph.cancel(node: osc);
        graph.cancel();
        render(20000);
        expect(risingCrossings(render(48000)), inInclusiveRange(879, 881));
        expect(graph.stats.eventsDelivered, 1);
      });

      test('routes events of the graph to the control thread', () {
        final osc = oscillator();
        graph.transaction((tx) {
          tx.connect(osc, graph.io);
          tx.connectEvents(graph.io, graph.io);
        });
        graph.start();
        final received = <AudGraphNotification>[];
        graph.notifications.listen(received.add);
        graph.sendEvent(graph.io, noteOn(60));
        render(256);
        graph.pump();
        final events = received.whereType<AudEventNotification>().toList();
        expect(events.single.node, 0);
        expect(events.single.event, noteOn(60));
        expect(
          received.whereType<AudRevisionAdoptedNotification>(),
          hasLength(1),
        );
        expect(
          received.whereType<AudStateNotification>().single.state,
          AudGraphState.running,
        );
      });

      test('refuses events to a removed node', () {
        final osc = oscillator();
        graph.transaction((tx) => tx.connect(osc, graph.io));
        graph.start();
        render(256);
        graph.transaction((tx) => tx.remove(osc));
        expect(
          () => graph.sendEvent(osc, noteOn(60)),
          throwsA(
            isA<AudGraphException>().having(
              (e) => e.name,
              'name',
              'AUD_ERROR_RETIRED',
            ),
          ),
        );
        render(256);
        graph.pump();
        render(256);
        expect(graph.revision, 3);
      });
    });

    test('setString(node, key, value) reaches nodes with string keys', () {
      final osc = oscillator();
      expect(() => graph.setString(osc, 'file', 'x'), throwsArgumentError);
      expect(
        () => graph.setString(osc, 0, 'x'),
        throwsA(
          isA<AudGraphException>().having(
            (e) => e.name,
            'name',
            'AUD_ERROR_UNSUPPORTED',
          ),
        ),
      );
    });

    test('transport(request) drives the internal transport', () {
      graph.start();
      graph.transport(const AudTransportRequest.start());
      graph.transport(const AudTransportRequest.setTempo(60));
      render(48000);
      expect(graph.transportState.beat, closeTo(1, 1e-6));
      expect(graph.transportState.playing, isTrue);
      graph.transport(const AudTransportRequest.stop());
      render(256);
      expect(graph.transportState.playing, isFalse);
      final notifications = <AudGraphNotification>[];
      graph.notifications.listen(notifications.add);
      graph.pump();
      expect(notifications.whereType<AudTransportNotification>(), hasLength(3));
    });

    group('send(command)', () {
      test('dispatches every command', () {
        final osc = oscillator();
        graph.transaction((tx) => tx.connect(osc, graph.io));
        graph.start();
        graph.send(
          AudSetParamCommand(node: osc.handle, paramIndex: 0, value: 220),
        );
        expect(graph.paramsOf(osc)['frequency'], 220);
        graph.send(AudEventCommand(node: osc.handle, event: noteOn(60), id: 3));
        graph.send(const AudCancelCommand(id: 3));
        graph.send(AudCancelCommand(node: osc.handle));
        graph.send(
          const AudTransportCommand(
            graph: 1,
            request: AudTransportRequest.start(),
          ),
        );
        render(256);
        expect(graph.transportState.playing, isTrue);
        expect(
          () => graph.send(
            AudSetStringCommand(node: osc.handle, key: 0, value: 'x'),
          ),
          throwsA(isA<AudGraphException>()),
        );
        expect(
          () => graph.send(
            const AudSetParamCommand(node: 99, paramIndex: 0, value: 1),
          ),
          throwsArgumentError,
        );
        expect(
          () => graph.send(
            AudSetParamCommand(node: graph.io.handle, paramIndex: 0, value: 1),
          ),
          throwsArgumentError,
        );
      });
    });

    group('handleOsc(message)', () {
      test('converts and sends OSC', () {
        final osc = oscillator();
        final commands = graph.handleOsc(
          AudOscMessage('/graph/1/node/${osc.handle}/param/frequency', [330.0]),
        );
        expect(commands, [
          AudSetParamCommand(node: osc.handle, paramIndex: 0, value: 330),
        ]);
        expect(graph.paramsOf(osc)['frequency'], 330);
        final bundled = graph.handleOscBundle(
          AudOscBundle(
            elements: [
              AudOscMessage('/graph/1/node/${osc.handle}/param/amplitude', [
                0.25,
              ]),
            ],
          ),
        );
        expect(bundled, hasLength(1));
        expect(graph.paramsOf(osc)['amplitude'], 0.25);
        expect(
          () =>
              graph.handleOsc(AudOscMessage('/graph/1/node/99/param/x', [1.0])),
          throwsA(isA<AudOscException>()),
        );
      });
    });

    group('lifecycle', () {
      test('walks the states', () {
        final osc = oscillator();
        graph.transaction((tx) => tx.connect(osc, graph.io));
        expect(() => graph.suspend(), throwsA(isA<AudGraphException>()));
        graph.prepare(sampleRate: 44100, maxFrames: 128);
        expect(graph.state, AudGraphState.prepared);
        expect(graph.sampleRate, 44100);
        expect(graph.maxFrames, 128);
        graph.start();
        expect(graph.state, AudGraphState.running);
        expect(() => graph.prepare(), throwsA(isA<AudGraphException>()));
        expect(render(128), isNot(everyElement(0)));
        graph.suspend();
        expect(graph.state, AudGraphState.suspended);
        expect(() => render(128), throwsA(isA<AudGraphException>()));
        graph.resume();
        graph.stop();
        expect(graph.state, AudGraphState.stopped);
        graph.start();
        expect(render(128), isNot(everyElement(0)));
        graph.dispose();
        expect(graph.isDisposed, isTrue);
        expect(graph.state, AudGraphState.disposed);
        expect(graph.pump(), 0);
        graph.dispose();
      });

      test('the listener wakes pump() after a render', () async {
        final listening = AudGraph(maxFrames: 256, outputChannels: const [1]);
        addTearDown(listening.dispose);
        final osc = listening.createNode('aud.graph.oscillator');
        listening.transaction((tx) => tx.connect(osc, listening.io));
        listening.start();
        final first = listening.notifications.first;
        AudOfflineRenderer(listening).render(frames: 256);
        final notification = await first.timeout(const Duration(seconds: 5));
        expect(notification, isA<AudRevisionAdoptedNotification>());
        expect(listening.revision, 1);
      });
    });

    test('reports diagnostics', () {
      final osc = oscillator();
      graph.transaction((tx) => tx.connect(osc, graph.io));
      graph.start();
      render(256);
      graph.sendEvent(osc, noteOn(60), at: const AudTimestamp.sample(10));
      render(256);
      final notifications = <AudGraphNotification>[];
      graph.notifications.listen(notifications.add);
      graph.pump();
      final diagnostic = notifications
          .whereType<AudDiagnosticNotification>()
          .single;
      expect(diagnostic.code, AUD_ERROR_LATE);
      expect(diagnostic.count, 1);
      expect(graph.stats.eventsLate, 1);
    });

    test('taps the signal', () {
      final osc = oscillator();
      final tap = graph.createNode(
        'aud.graph.tap',
        inputChannels: const [1],
        outputChannels: const [1],
      );
      graph.transaction((tx) {
        tx.connect(osc, tap);
        tx.connect(tap, graph.io);
      });
      graph.start();
      final output = render(512);
      final recent = graph.readTap(tap, frames: 64);
      expect(recent, output.sublist(448));
      final meter = graph.tapMeter(tap);
      expect(meter.peak, closeTo(0.5, 0.01));
      expect(meter.rms, closeTo(0.35, 0.02));
      expect(
        () => graph.readTap(osc, frames: 4),
        throwsA(isA<AudGraphException>()),
      );
      expect(() => graph.tapMeter(osc), throwsA(isA<AudGraphException>()));
    });

    group('state, tail and assets', () {
      Uint8List gainState(double gain) =>
          Uint8List(4)..buffer.asByteData().setFloat32(0, gain, Endian.host);

      double gainOf(Uint8List state) =>
          ByteData.sublistView(state).getFloat32(0, Endian.host);

      AudNode gain() => graph.createNode(
        'aud.core.gain',
        name: 'gain',
        inputChannels: const [1],
        outputChannels: const [1],
      );

      test('saveState(node) and loadState(node, data) move a state blob', () {
        final g = gain();
        graph.setParam(g, 'gain', 0.25);
        final state = graph.saveState(g);
        expect(state, hasLength(4));
        expect(gainOf(state), 0.25);
        graph.loadState(g, gainState(0.5));
        expect(graph.saveState(g), gainState(0.5));
        expect(
          () => graph.loadState(g, gainState(0.5), version: 9),
          throwsA(
            isA<AudGraphException>().having(
              (e) => e.code,
              'code',
              AUD_ERROR_STATE_VERSION,
            ),
          ),
        );
        final osc = oscillator();
        expect(
          () => graph.saveState(osc),
          throwsA(
            isA<AudGraphException>().having(
              (e) => e.code,
              'code',
              AUD_ERROR_UNSUPPORTED,
            ),
          ),
        );
        // Running, the node is parked for the call.
        graph.transaction((tx) {
          tx.connect(osc, g);
          tx.connect(g, graph.io);
        });
        graph.start();
        render(256);
        graph.loadState(g, state);
        expect(graph.saveState(g), state);
      });

      test('presets apply strings, then the state, then parameters', () {
        final g = gain();
        graph.applyPreset(
          g,
          AudNodePreset(
            typeId: 'aud.core.gain',
            params: const {'gain': 0.75},
            state: gainState(0.5),
            stateVersion: 1,
          ),
        );
        // The parameter comes last: it wins over the gain in the state.
        expect(gainOf(graph.saveState(g)), 0.75);
        graph.applyPreset(
          g,
          AudNodePreset(
            typeId: 'aud.core.gain',
            state: gainState(0.5),
            stateVersion: 1,
          ),
        );
        expect(gainOf(graph.saveState(g)), 0.5);
      });

      test('documents carry the state blobs of the nodes', () {
        graph.createNode(
          'aud.core.gain',
          name: 'gain',
          inputChannels: const [1],
          outputChannels: const [1],
          preset: const AudNodePreset(
            typeId: 'aud.core.gain',
            params: {'gain': 0.5},
          ),
        );
        final document = graph.toDocument();
        final preset = document.nodes.single.preset!;
        expect(preset.stateVersion, 1);
        expect(gainOf(preset.state!), 0.5);
        final bare = graph.toDocument(includeState: false);
        expect(bare.nodes.single.preset!.state, isNull);
        expect(bare.nodes.single.preset!.stateVersion, isNull);
        final copy = AudGraph.fromDocument(
          document,
          maxFrames: 256,
          listen: false,
        );
        addTearDown(copy.dispose);
        expect(copy.toDocument(), document);
      });

      test('outputTail reports the tail of the program', () {
        expect(graph.outputTail, 0);
        final feedback = graph.createNode(
          'aud.graph.feedback',
          inputChannels: const [1],
          outputChannels: const [1],
          delayFrames: 512,
        );
        final osc = oscillator();
        graph.transaction((tx) {
          tx.connect(osc, feedback);
          tx.connect(feedback, graph.io);
        });
        expect(graph.outputTail, 512);
        expect(AudGraph.infiniteTail, 0xFFFFFFFF);
      });

      test('assets resolve against a base directory', () {
        final dir = Directory.systemTemp.createTempSync('aud_graph_assets');
        addTearDown(() => dir.deleteSync(recursive: true));
        File('${dir.path}/a.sfz').writeAsStringSync('x');
        const asset = AudGraphAsset(id: 'a', path: 'a.sfz');
        graph.addAsset(asset, baseDirectory: dir.path);
        expect(graph.assets, [asset]);
        expect(
          graph.assetPath('a'),
          '${dir.path}${Platform.pathSeparator}a.sfz',
        );
        expect(graph.assetPath('b'), isNull);
        // An absolute path stays as it is.
        graph.addAsset(
          AudGraphAsset(id: 'abs', path: '${dir.path}/a.sfz'),
          baseDirectory: '/elsewhere',
        );
        expect(graph.assetPath('abs'), '${dir.path}/a.sfz');
        expect(
          () =>
              graph.addAsset(const AudGraphAsset(id: 'm', path: 'missing.sfz')),
          throwsA(
            isA<ArgumentError>().having(
              (e) => e.message,
              'message',
              contains('Asset m not found'),
            ),
          ),
        );
        // A document brings its assets along.
        final copy = AudGraph.fromDocument(
          const AudGraphDocument(outputChannels: [1], assets: [asset]),
          maxFrames: 256,
          listen: false,
          baseDirectory: dir.path,
        );
        addTearDown(copy.dispose);
        expect(copy.assets, [asset]);
        expect(copy.toDocument().assets, [asset]);
      });

      test('the watchdog is off in a build without the user define', () {
        expect(AudGraph.watchdogEnabled, isFalse);
        AudGraph.resetWatchdog();
        expect(AudGraph.watchdogViolations, 0);
      });
    });

    group('documents', () {
      test('round trip through a document', () {
        final osc = graph.createNode(
          'aud.graph.oscillator',
          name: 'osc',
          preset: const AudNodePreset(
            typeId: 'aud.graph.oscillator',
            params: {'frequency': 220},
          ),
        );
        final feedback = graph.createNode(
          'aud.graph.feedback',
          name: 'fb',
          inputChannels: const [1],
          outputChannels: const [1],
          delayFrames: 512,
        );
        graph.transaction((tx) {
          tx.connect(osc, feedback);
          tx.connect(feedback, graph.io, lowLatency: true);
          tx.connectEvents(graph.io, osc);
        });
        graph.transport(AudTransportRequest.setLoop(start: 1, end: 3));
        graph.start();
        render(256);
        final document = graph.toDocument(name: 'Demo');
        expect(document.name, 'Demo');
        expect(document.nodes.map((n) => n.id), ['osc', 'fb']);
        expect(document.nodes.first.preset?.params, {'frequency': 220});
        expect(document.nodes.last.delayFrames, 512);
        expect(document.connections, hasLength(2));
        expect(document.connections.last.lowLatency, isTrue);
        expect(document.eventConnections.single.to, 'osc');
        expect(document.transport.loopEnd, 3);
        final copy = AudGraph.fromDocument(
          document,
          maxFrames: 256,
          listen: false,
        );
        addTearDown(copy.dispose);
        expect(copy.nodeNamed('osc'), isNotNull);
        expect(copy.paramsOf(copy.nodeNamed('osc')!)['frequency'], 220);
        expect(copy.connections, hasLength(2));
        expect(copy.eventConnections, hasLength(1));
        copy.start();
        AudOfflineRenderer(copy).render(frames: 256);
        expect(copy.transportState.looping, isTrue);
        expect(copy.toDocument(name: 'Demo'), document);
      });

      test('refuses documents that do not fit', () {
        expect(
          () => graph.load(const AudGraphDocument(outputChannels: [2])),
          throwsArgumentError,
        );
        expect(
          () => graph.load(
            const AudGraphDocument(
              outputChannels: [1],
              connections: [AudGraphConnection(from: 'x', to: 'graph')],
            ),
          ),
          throwsArgumentError,
        );
        expect(
          () => AudGraph.fromDocument(
            const AudGraphDocument(
              outputChannels: [1],
              nodes: [AudGraphDocumentNode(id: 'a', typeId: 'aud.nothing')],
            ),
            listen: false,
          ),
          throwsA(isA<AudGraphException>()),
        );
      });
    });
  });
}

/// The parameter indices of the oscillator, as a test of index access.
abstract final class AudOscillatorParams {
  static const int amplitude = 1;
}

/// A node type written in Dart and registered through the host api, the
/// way a DSP package registers its types: no buses, one string key whose
/// values it records. Its callbacks run on the control thread, so it only
/// renders offline.
class DartStringNode {
  DartStringNode(Pointer<core.AudHostApi> host) {
    typeId = 'aud.test.strings'.toNativeUtf8();
    keyId = 'file'.toNativeUtf8();
    name = 'Strings'.toNativeUtf8();
    key = calloc<core.AudStringKeyDescriptor>();
    key.ref
      ..struct_size = sizeOf<core.AudStringKeyDescriptor>()
      ..key = 7
      ..id = keyId.cast()
      ..name = name.cast();
    vtable = calloc<core.AudNodeVTable>();
    vtable.ref
      ..struct_size = sizeOf<core.AudNodeVTable>()
      ..create = _create.nativeFunction
      ..destroy = _destroy.nativeFunction
      ..prepare = _prepare.nativeFunction
      ..process = _process.nativeFunction
      ..set_string = _setString.nativeFunction;
    descriptor = calloc<core.AudNodeDescriptor>();
    descriptor.ref
      ..struct_size = sizeOf<core.AudNodeDescriptor>()
      ..abi_major = AudAbi.major
      ..abi_minor = AudAbi.minor
      ..version = 1
      ..type_id = typeId.cast()
      ..name = name.cast()
      ..vendor = name.cast()
      ..capabilities = AUD_NODE_CAP_VARIABLE_BLOCK | AUD_NODE_CAP_STRINGS
      ..num_string_keys = 1
      ..string_keys = key
      ..vtable = vtable;
    final register = host.ref.register_node_type
        .asFunction<
          int Function(Pointer<Void>, Pointer<core.AudNodeDescriptor>)
        >();
    result = register(host.ref.host, descriptor);
  }

  late final Pointer<Utf8> typeId;
  late final Pointer<Utf8> keyId;
  late final Pointer<Utf8> name;
  late final Pointer<core.AudStringKeyDescriptor> key;
  late final Pointer<core.AudNodeVTable> vtable;
  late final Pointer<core.AudNodeDescriptor> descriptor;
  late final int result;

  /// The strings the instances received, as `key=value`.
  static final List<String> received = [];

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
  static final _setString =
      NativeCallable<
        Int32 Function(Pointer<Void>, Uint32, Pointer<Char>)
      >.isolateLocal(_onSetString, exceptionalReturn: AUD_ERROR_FAILED);

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
  ) {}
  static int _onSetString(
    Pointer<Void> instance,
    int key,
    Pointer<Char> value,
  ) {
    received.add('$key=${value.cast<Utf8>().toDartString()}');
    return AUD_OK;
  }

  /// Frees the descriptor; the graph that uses it has to be disposed.
  void dispose() {
    calloc.free(descriptor);
    calloc.free(vtable);
    calloc.free(key);
    calloc.free(name);
    calloc.free(keyId);
    calloc.free(typeId);
  }
}

void stringNodeTests() {
  group('AudGraph with a Dart node type', () {
    test('resolves asset references and keeps them in documents', () {
      final graph = AudGraph(listen: false, maxFrames: 256);
      final type = DartStringNode(graph.hostApi);
      final dir = Directory.systemTemp.createTempSync('aud_graph_strings');
      addTearDown(() {
        graph.dispose();
        type.dispose();
        dir.deleteSync(recursive: true);
      });
      File('${dir.path}/b.sfz').writeAsStringSync('x');
      DartStringNode.received.clear();
      const document = AudGraphDocument(
        assets: [AudGraphAsset(id: 'b', path: 'b.sfz')],
        nodes: [
          AudGraphDocumentNode(
            id: 's',
            typeId: 'aud.test.strings',
            preset: AudNodePreset(
              typeId: 'aud.test.strings',
              strings: {'file': 'asset:b'},
            ),
          ),
        ],
      );
      final nodes = graph.load(document, baseDirectory: dir.path);
      final node = nodes['s']!;
      expect(DartStringNode.received, [
        '7=${dir.path}${Platform.pathSeparator}b.sfz',
      ]);
      expect(graph.stringsOf(node), {'file': 'asset:b'});
      final saved = graph.toDocument();
      expect(saved.assets, document.assets);
      expect(saved.nodes.single.preset?.strings, {'file': 'asset:b'});
      expect(
        () => graph.setString(node, 'file', 'asset:nope'),
        throwsArgumentError,
      );
      expect(
        () => graph.applyPreset(
          node,
          const AudNodePreset(
            typeId: 'aud.test.strings',
            strings: {'file': 'asset:nope'},
          ),
        ),
        throwsA(
          isA<ArgumentError>().having(
            (e) => e.message,
            'message',
            contains('unknown asset nope'),
          ),
        ),
      );
      expect(
        () => graph.load(
          const AudGraphDocument(
            assets: [AudGraphAsset(id: 'x', path: 'nowhere.wav')],
          ),
          baseDirectory: dir.path,
        ),
        throwsArgumentError,
      );
    });

    test('applies string settings from presets, calls and commands', () {
      final graph = AudGraph(listen: false, maxFrames: 256);
      final type = DartStringNode(graph.hostApi);
      addTearDown(() {
        graph.dispose();
        type.dispose();
      });
      expect(type.result, AUD_OK);
      DartStringNode.received.clear();
      final node = graph.createNode(
        'aud.test.strings',
        preset: const AudNodePreset(
          typeId: 'aud.test.strings',
          strings: {'file': 'a.sfz'},
        ),
      );
      expect(graph.stringsOf(node), {'file': 'a.sfz'});
      graph.setString(node, 'file', 'b.sfz');
      graph.send(
        AudSetStringCommand(node: node.handle, key: 7, value: 'c.sfz'),
      );
      expect(DartStringNode.received, ['7=a.sfz', '7=b.sfz', '7=c.sfz']);
      expect(graph.stringsOf(node), {'file': 'c.sfz'});
      expect(graph.toDocument().nodes.single.preset?.strings, {
        'file': 'c.sfz',
      });
      graph.transaction((_) {});
      graph.start();
      AudOfflineRenderer(graph).render(frames: 256);
      expect(graph.revision, 1);
    });
  });
}
