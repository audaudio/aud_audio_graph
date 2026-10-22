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
  `aud.graph.filter`, `aud.graph.channel_split`, `aud.graph.feedback` and
  `aud.graph.tap`; `aud.core.gain` of the core is registered by the Dart
  side. `aud.graph.channel_split` hands every channel voice message of its
  event input to the event output of its MIDI channel, `ch1` to `ch16`,
  and every other event to `ch1` - e.g. to play an instrument of its own
  per MIDI channel of a plugin host. The nodes behind it track the notes
  it hands them, 256 per node, and close them on a stop or a prepare; the
  split drops no note on, closes the notes it handed out of the graph and,
  when it is removed, those it handed on, as far as its own tracker of 256
  notes holds them. The events the nodes emit in a block share an arena
  as large as the block's event table, 3 × `max_events_per_block` + 768
  entries: an event takes one entry per connection it leaves on, beyond
  the arena it is dropped and counted.

## The resources of the process (ticket 25)

The graphs of a process share what the packages need for their assets
(lifecycle-002), through the host api of ABI 0.4:

- Two worker threads with a lock-free task queue that the control thread,
  the workers and the audio thread may post to. A graph counts the tasks
  of its nodes: when it is destroyed it waits for them, not for those of
  other graphs. `aud_tasks_wait_idle` waits for every task of the process;
  an offline render waits like this before its first block and after every
  block, so it repeats.
- Loads with ids: a node opens a load in `set_string`, which returns its
  id, reports progress and ends it with a result - done, failed with a
  message, or cancelled. A graph keeps the running loads and the last 64
  (`aud_graph_loads`, `aud_graph_load_info`, `aud_graph_cancel_load`,
  `aud_graph_wait_loads`) and reports them as `AUD_NOTIFY_LOAD`; a
  progress that still waits gives way to the next one. Removing a node
  cancels its loads, and a load its node leaves running ends as cancelled.
- One sample cache budget for the process, 256 MB by default
  (`aud_cache_set_budget`, `aud_cache_budget`): packages register their
  caches and report their bytes; when the process holds more, a worker
  asks the largest cache to free what nobody uses, the least recently used
  first. A smaller budget trims at once. `aud_cache_usage` fills an
  `AudCacheUsage` with the caches, the budget, the bytes, the trims and
  the workers.
- Reports of a node from the audio thread, summed per block into
  `AUD_NOTIFY_DIAGNOSTIC`, e.g. a stream that fell behind. A control call
  of the host api on the audio thread is a watchdog violation of the kind
  `AUD_GRAPH_VIOLATION_CONTROL`.
- Messages to nodes (osc-002): `aud_graph_send_message` copies an OSC
  message of at most 16 arguments and 1000 bytes
  (`AUD_GRAPH_MAX_MESSAGE_ARGS`, `AUD_GRAPH_MESSAGE_BYTES`) into one of the
  `message_capacity` slots of the graph (`AudGraphConfig`) and delivers it
  as an `AUD_EVENT_MESSAGE` event, ordered with the other events. The
  node's answers, within the same limits, wait in a queue of
  `reply_capacity` for `aud_graph_take_messages`, announced by
  `AUD_NOTIFY_MESSAGE`; an answer beyond the limits or the queue counts in
  `messages_dropped` of `AudGraphStats`.
- The engine reads the tail of a node at every commit, so that a tail may
  follow what the node loaded.

## Event sources (ticket 26)

External MIDI - and later OSC and Link - reaches the audio thread through
event sources (interop-003, midi-003): a lock-free queue of UMP packets
with host timestamps per producer, created on the control thread for a
committed node and drained by the realtime thread every block, after the
scheduler and before the host's events.

- `aud_graph_create_event_source` takes an `AudEventSourceConfig` - the
  target node and event inlet, the capacity (rounded up to a power of
  two, at most 2^20, 4096 by default), the timing policy and the latency
  C -; at most `AUD_GRAPH_MAX_EVENT_SOURCES` (64) per graph.
- Timing: under `AUD_SOURCE_TIMING_FIXED` a packet sounds at its timestamp
  plus C at the output, pre-delivered by the lead of its path; under
  `AUD_SOURCE_TIMING_IMMEDIATE` it plays at the start of the block that
  drains it and carries `AUD_EVENT_FLAG_LIVE`. A packet whose time lies
  beyond the block stays in the queue (peek and pop), one that missed its
  time plays at the block start and counts as late with its lateness; the
  graph's policy of dropping late events does not apply. A timestamp
  earlier than its predecessor's is raised and counted; no lookahead is
  checked, so offline producers work. `max_events_per_block` bounds what
  all sources together deliver or drop per block, the rest follows in
  order.
- `aud_event_source_push` takes one packet of one to four words from one
  producer thread at a time; `aud_event_source_attach` and `_detach` hand
  the source from one producer to the next (CAS on a token,
  `AUD_GRAPH_ERROR_BUSY` while another is attached). A full queue refuses
  the push; a note off that does not fit raises release.
- `aud_event_source_release`, from any thread: before the next drain the
  queue empties - note offs and controllers at the block start, note ons
  dropped and counted -, then the target's notes close and CC64 = 0 and
  CC66 = 0 lift the pedals the source delivered. A removed target gets the
  pedal offs with its note offs; the routes of a retiring node to living
  nodes stay until it is done, so the note offs a pass-through node
  forwards arrive downstream.
- `aud_graph_destroy_event_source` retires the source; the realtime
  thread releases the notes and lets go at its next block
  (`AUD_NOTIFY_SOURCE_DONE`), `aud_graph_take_notifications` frees it. A
  node a live source targets is kept until the source is freed.
- `aud_event_source_stats`: pushed, rejected, delivered, late and the
  maximum lateness, dropped, reordered, releases, the maximum delivery
  delay (push time minus timestamp, taken on the producer thread), the
  last delivered packet and its timestamp, a histogram of the delivery
  delay (50 us bins up to 25 ms) and one of the input to output time
  (0.25 ms bins up to 256 ms), each with an overflow bin.
- `aud_graph_timing` is the timing of the last block for the control
  side: the sample position and its fitted output time, the frames, the
  output latency the stream reported, the longest period between two
  renders since the last reset of the time filter (offline: the longest
  block), the nominal period, the resets and a histogram of the periods
  (100 us bins up to 51.2 ms with an overflow bin, one count per render,
  reset with the time filter) - what a bridge needs for C = P + L + m.
  The longest period is a diagnostic, a single gap at start-up ruins it;
  P is a high percentile (p99.9) of the histogram. The realtime thread
  counts a bin with a relaxed add, the histogram is read after the
  snapshot of the other fields.

## The render load (ticket 29)

How much of its block the audio thread spends, for a load governor that
keeps the voices below what the device renders in time:

- Every live render records its share of the block - its render time
  over the time its frames last at the sample rate, the two numbers the
  overload diagnostic compares - in a lock-free ring of the last
  `AUD_GRAPH_LOAD_RENDERS` (1024) renders. One 64-bit atomic per slot
  holds the share, the frames and a tag of the render's index, so a
  reader needs no seqlock and stops at a slot the audio thread overwrote
  meanwhile. Offline renders are not recorded.
- `aud_graph_render_load` reads an `AudGraphLoad` over the newest renders
  whose frames cover a window at the sample rate (`window_ns` <= 0: the
  whole ring): the renders, their frames and duration, the mean share
  weighted by the frames, the 99th percentile by nearest rank, the
  largest share, and the live renders and overloads since the graph was
  created. From any thread but the audio thread; without allocation.
- The same call reads the load of the device buffer (`buffer_frames`): a
  stream whose buffer holds several renders plays on when one render
  takes longer than its block and the next ones catch up, and drops out
  when the renders that fill a buffer take longer than it lasts. A run is
  a render and the older ones before it whose frames reach the buffer;
  `buffer_p99`, `buffer_max` and `buffer_overloads` are taken over the
  runs of the window. The Galaxy Tab S4 renders 47 voices without an xrun
  at a p99 of 1.29 per render (ticket 29).
- `aud_graph_reset_stats` leaves the gauge alone, so tests and tools can
  reset the counters while a governor reads it.

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
  for the loads of a document with `aud_host_wait_loads`. `aud_host_save`
  writes the document back with the current parameters, strings and state
  blobs.
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
the host api; `AudGraphFfi.watchdogEnabled` and `watchdogViolations`
read it. It is on in the native tests and in an app that sets

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
graph.notifications.listen((n) => ...);                  // revisions, diagnostics, events, loads
graph.readTap(tap, frames: 512);                         // the recent signal of a tap node

final wav = AudOfflineRenderer(graph).renderWav(frames: 48000);
AudWavFile.write('out.wav', wav);

final document = graph.toDocument(name: 'Demo');         // JSON, schema in doc/schemas
final copy = AudGraph.fromDocument(document);       // an AudGraphFfi

final id = graph.setString(sampler, 'sfz_file', path);   // a load on a worker
final load = await graph.loadEnded(id);                  // AudLoad: result, progress, time
print(graph.loads);                                      // the running loads, the last 64 ended
print(graph.loadOf(id)?.message);                        // why it failed, what it could not do
graph.cancelLoad(id);                                    // false once it ended
graph.waitLoads(timeout: const Duration(seconds: 5));    // blocks: tests and offline renders
graph.sendMessage(sampler, AudOscMessage('/num_regions'));
graph.messages.listen((m) => print(m.arguments));        // AudNodeMessage: the node's answers
print(graph.stats.messagesDropped);                      // answers beyond the limits or the queue
graph.cacheBudget = 128 << 20;                           // of the process
print(graph.cacheUsage.bytes);                           // AudCacheUsage
graph.waitTasks();                                       // every task of the process
host.waitLoads();                                        // the loads of the host's document

final source = graph.createEventSource(sampler,         // ticket 26: one producer at a time
    timing: AudSourceTiming.fixed, latencyNs: 27000000);
source.attach(1);                                        // the producer's token, before its first push
source.push(packet.words, hostTimeNs: AudClock.nowNs()); // false when the queue is full
source.latencyNs = 30000000;                             // C, from the next block on
source.release();                                        // notes off, pedals up
source.detach(1);                                        // after the last push; the next producer attaches
print(source.stats.latencyP99Ns);                        // percentiles of the histograms
print(graph.timing.periodP999Ns);                        // P of C = P + L + m
final gauge = graph.renderLoad();                        // ticket 29: the last second
print('${gauge.mean} ${gauge.p99} ${gauge.max}');        // shares of the block, 1 = all of it
print(gauge.overloadsTotal);                             // never reset, unlike graph.stats
print(graph.renderLoad(bufferFrames: 384).bufferP99);    // over a device buffer
print(graph.eventSources);                               // the live sources of the graph
source.address;                                          // for a native producer, with
AudEventSourceFfi.pushFunctionAddress;                   // the push, release and size functions
source.dispose();                                        // AudSourceDoneNotification follows
```

The notifications carry `AudLoadNotification` for the loads and
`AudMessageNotification` for the answers of a block; `AudGraphOptions`
sets `messageCapacity` and `replyCapacity`.

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
