# aud_audio_graph

Audio graph engine of the Audanika Audio Engine: C++ render programs with persistent node instances, transactions, realtime queues, scheduler, transport, offline renderer; Dart graph API and graph document.

Part of the Audanika Audio Engine; planned in [aud_audio_pm](https://github.com/audaudio/aud_audio_pm).

## What the package holds (ABI 0.3, ticket 19)

The engine behind `src/aud_audio_graph.h`, built on the contracts of
`aud_audio_core` (step S2 of the plan):

- Persistent node instances with stable handles and immutable render
  programs (graph-001, graph-003): the control thread edits the topology in
  transactions, a commit compiles a program — topological order, feedback
  nodes split into a reader and a writer, latency alignment with a
  low-latency opt-out, buffers reused by lifetime — and the realtime thread
  adopts it at the next block start and acknowledges the revision. Removed
  connections fade out over 5 ms, new ones fade in, removed nodes render
  their tail (at most 10 s) before they are freed.
- The realtime contract (interop-002): a parameter queue and an event queue
  with fixed capacities that refuse instead of dropping, a per-block budget,
  a time-ordered scheduler with a 10 s lookahead for events in sample, host
  or beat time, pre-delivery by the path latency, cancellation by id or
  node, late events played at the block start or dropped, running notes
  closed on retirement, stop and reset, lossy taps, a notification thread
  that wakes the control thread.
- The lifecycle (lifecycle-001): created, prepared, running, suspended,
  stopped, disposed; a re-prepare for a new sample rate or block size.
- Time (time-001): the stream's host times feed the filter of the core,
  synthesized host time without them, the internal transport with
  segments per block (start, stop, seek, tempo, time signature, loop) and
  registered transport providers; a plugin host's snapshot takes
  precedence.
- The render function of plugin-002, `aud_graph_render`, with host buses
  and events, and the offline renderer over a virtual timeline.
- The reference nodes `aud.graph.oscillator`, `aud.graph.mixer`,
  `aud.graph.filter`, `aud.graph.feedback` and `aud.graph.tap`;
  `aud.core.gain` of the core is registered by the Dart side.

The headless host, the stress tests and the debug watchdog follow in
ticket 20.

## Dart API

```dart
import 'package:aud_audio_graph/aud_audio_graph.dart';

final graph = AudGraph(sampleRate: 48000, maxFrames: 256, outputChannels: [2]);
final osc = graph.createNode('aud.graph.oscillator', name: 'osc');
final filter = graph.createNode('aud.graph.filter', name: 'filter');
graph.setParam(filter, 'cutoff', 800);                  // before the first commit: at once
final revision = graph.transaction((tx) {
  tx.connect(osc, filter);
  tx.connect(filter, graph.io);                          // graph.io is the graph itself
});
graph.start();
await graph.adopted(revision);                           // the realtime thread runs it
graph.sendEvent(osc, noteOn, at: AudTimestamp.beat(4));  // scheduled, pre-delivered by its lead
graph.transport(const AudTransportRequest.start());
graph.notifications.listen((n) => ...);                  // revisions, diagnostics, events
graph.readTap(tap, frames: 512);                         // the recent signal of a tap node

final wav = AudOfflineRenderer(graph).renderWav(frames: 48000);
AudWavFile.write('out.wav', wav);

final document = graph.toDocument(name: 'Demo');         // JSON, schema in doc/schemas
final copy = AudGraph.fromDocument(document);
```

`graph.send(command)` takes the typed commands of the core and
`graph.handleOsc(message)` the OSC messages of osc-001; DSP packages
register their node types with `graph.hostApi`; an `aud_audio_io` stream
calls `aud_graph_render` with `graph.pointer` as the user pointer.

## Tests

`dart test` covers the Dart side and runs `scripts/test-native.js`, which
builds the native tests of `test/native` with the address and undefined
behaviour sanitizers (`--no-sanitize` on a toolchain without them). The
golden render in `test/goldens` is updated with `UPDATE_GOLDENS=1`.

The C API is in `src/aud_audio_graph.h`; regenerate the bindings with
`dart run ffigen --config ffigen.yaml`.
