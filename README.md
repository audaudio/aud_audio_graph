# aud_audio_graph

Audio graph engine of the Audanika Audio Engine: C++ render programs with persistent node instances, transactions, realtime queues, scheduler, transport, offline renderer, headless host; Dart graph API and graph document.

Part of the Audanika Audio Engine; planned in [aud_audio_pm](https://github.com/audaudio/aud_audio_pm).

## What the package holds (ABI 0.4, tickets 19 and 25)

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

## The resources of the process (ticket 25)

The graphs of a process share what the packages need for their assets
(lifecycle-002), through the host api of ABI 0.4:

- Two worker threads with a lock-free task queue that the control thread,
  the workers and the audio thread may post to. An offline render waits
  for the workers after every block, so it repeats.
- Loads with ids: a node opens a load in `set_string`, which returns its
  id, reports progress and ends it with a result - done, failed with a
  message, or cancelled. A graph keeps the running loads and the last 64
  (`aud_graph_loads`, `aud_graph_load_info`, `aud_graph_cancel_load`,
  `aud_graph_wait_loads`, `AUD_NOTIFY_LOAD`).
- One sample cache budget for the process, 256 MB by default
  (`aud_cache_set_budget`): packages register their caches and report
  their bytes; when the process holds more, a worker asks the largest
  cache to free what nobody uses, the least recently used first. A
  smaller budget trims at once.
- Reports of a node from the audio thread, summed per block into
  `AUD_NOTIFY_DIAGNOSTIC`, e.g. a stream that fell behind.
- Messages to nodes (osc-002): `aud_graph_send_message` copies an OSC
  message into a slot of the graph and delivers it as an
  `AUD_EVENT_MESSAGE` event, ordered with the other events; the node's
  answers come back through `aud_graph_take_messages`.
- The engine reads the tail of a node at every commit, so that a tail may
  follow what the node loaded.

## The headless host (ticket 20)

The C API `aud_host_*` runs a graph without Dart, as the plugin shells do
(plugin-002):

- `aud_host_load` validates a whole graph document - types, ids, buses,
  parameters, string keys, state versions, assets on disk, parameter ids -
  and only then creates the nodes, applies their presets and connects
  them in one transaction; a refused document changes nothing.
- Presets apply in the order strings, state blob, parameters - the strings
  in the order of the node's string keys -; the host keeps every
  parameter value. A string that starts a load is fine: the host waits
  for the loads of a document with `aud_host_wait_loads`. `aud_host_save` writes the document back
  with the current parameters, strings and state blobs.
- Assets: the document's `assets` table maps ids to paths, a string
  `asset:<id>` reaches the node as the resolved path;
  `aud_host_set_asset_path` relinks.
- Stable parameter ids: FNV-1a over `<node id>/<parameter id>` with the
  top bit cleared; colliding documents are refused.
- `aud_host_latency` and `aud_host_tail` (`aud_graph_output_tail`);
  `aud_host_render` and `aud_graph_render_host` hand the events the graph
  sends to its event input to the host and take a freewheel flag.
- `aud_graph_node_save_state` and `aud_graph_node_load_state` park a node
  of a running graph for the call.

Dart wraps it as `AudHost`; `AudGraph` has `saveState`, `loadState`,
`addAsset`, `assetPath`, `outputTail` and writes the state blobs into
its documents:

```dart
final host = AudHost(graph, baseDirectory: presetFolder);
host.loadDocument(document);                            // validated, then one transaction
final cutoff = AudHost.paramIdOf('filter', 'cutoff');   // stable across versions
host.setParam(cutoff, 1200);
final state = host.save();                              // the whole state as JSON
final info = AudHost.inspect(state);                    // buses and counts, no graph

graph.addAsset(const AudGraphAsset(id: 'piano', path: 'piano.sfz'),
    baseDirectory: presetFolder);                       // strings name it asset:piano
final blob = graph.saveState(gainNode);                 // parks a running node
graph.loadState(gainNode, blob);
print(graph.outputTail);                                // AudGraph.infiniteTail if endless
```

## The realtime contract under test

The native tests in `test/native` run under the address and undefined
behaviour sanitizers, under Clang's RealtimeSanitizer when a compiler on
the machine has it (LLVM from Homebrew on macOS; a probe proves the run
catches an allocation on the audio thread) and under the thread
sanitizer. Stress tests cover queue overflow, late events, a transaction
in front of every block with a click detector, route changes while notes
play, a full scheduler and note tracker, and a stream rendering on its own
thread. The debug watchdog counts allocations, frees and log calls on the
audio thread - in an app those of the graph's own library and every call of
the host api; `AudGraph.watchdogEnabled` and `watchdogViolations` read
it. It is on in the native tests and in an app that sets

```yaml
hooks:
  user_defines:
    aud_audio_graph:
      watchdog: true
```

## Dart API

`aud_audio_graph.dart` is platform-neutral and compiles for the web
(web-001): `AudGraph` is an interface whose factory creates an
`AudGraphFfi` on native platforms. `aud_audio_graph_ffi.dart` adds
`AudGraphFfi` with `pointer` and `hostApi`, the render entry
`aud_graph_render`, the headless host, the offline renderer and WAV files.

```dart
import 'package:aud_audio_graph/aud_audio_graph_ffi.dart';

final graph = AudGraphFfi(sampleRate: 48000, maxFrames: 256, outputChannels: [2]);
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
final copy = AudGraph.fromDocument(document);       // an AudGraphFfi

final id = graph.setString(sampler, 'sfz_file', path);   // a load on a worker
final load = await graph.loadEnded(id);                  // AudLoad: result, progress, time
graph.sendMessage(sampler, AudOscMessage('/num_regions'));
graph.messages.listen((m) => print(m.arguments));        // the node's answers
graph.cacheBudget = 128 << 20;                           // of the process
print(graph.cacheUsage.bytes);
```

`graph.send(command)` takes the typed commands of the core and
`graph.handleOsc(message)` the OSC messages of osc-001; DSP packages
register their node types with `graph.hostApi`; an `aud_audio_io` stream
calls `aud_graph_render` with `graph.pointer` as the user pointer, as
`AudEngine` of `aud_audio` does.

## Tests

`dart test` covers the Dart side and runs `scripts/test-native.js`, which
builds and runs the native tests of `test/native` under every sanitizer
the machine has (`--no-sanitize`, `--no-rtsan`, `--no-tsan` and
`--rtsan` select). The golden render in `test/goldens` is updated with
`UPDATE_GOLDENS=1`.

The C API is in `src/aud_audio_graph.h`; regenerate the bindings with
`dart run ffigen --config ffigen.yaml`.
