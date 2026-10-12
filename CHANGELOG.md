# Changelog

## Unreleased

### Added

- Add event sources for native producers and the engine's timing

- Event sources (interop-003): one lock-free queue per producer, drained after the scheduled events with peek and pop, the per-block budget and the note tracking

- Timing policies fixed and immediate (midi-003); release flushes a source and closes its target's notes with sustain and soft pedal off

- aud_graph_timing: the fitted output time, the reported output latency and the longest period for the control side

- Retiring nodes keep their event routes until they are done, so their note offs arrive

- Dart: AudEventSource, AudGraphTiming, AudSourceDoneNotification; native, stress and Dart tests; the example resolves core through its tag

- Add aud_midi_standard from pub.dev

- aud_midi_standard ^0.1.0 hosted for the tests, as the aud_midi family resolves it (ticket 26)
- The example runs against the ticket's aud_audio_core again

## 0.5.0 - 2026-10-12

### Added

- Add worker tasks, loads, the cache budget and node messages

- The resources of the process: two worker threads with a lock-free task queue that the audio thread may post to, the sample cache budget (256 MB) with trims of the registered caches, the largest first; a trim requested while one runs is not lost, a smaller budget trims at once on the calling thread

- The host api of ABI 0.4: post_task, begin_load, load_progress, end_load, load_cancelled, register_cache, unregister_cache, cache_changed, cache_budget, report and emit_message

- Loads with ids, progress, cancellation and the last 64 that ended: aud_graph_loads, aud_graph_load_info, aud_graph_cancel_load, aud_graph_wait_loads, AUD_NOTIFY_LOAD

- Messages of nodes: aud_graph_send_message delivers an engine-owned OSC message as AUD_EVENT_MESSAGE, the slots come back on every path; answers through aud_graph_take_messages and AUD_NOTIFY_MESSAGE

- Diagnostics nodes report per block, the watchdog kind AUD_GRAPH_VIOLATION_CONTROL, offline renders that wait for the workers after every block

- The tails of the nodes are read again at every commit, so a tail may follow what a node loaded

- aud_graph_wait_loads reads the tails again; message slots come back when a block is skipped; host events cannot be messages; a reply blob without bytes is empty

- Presets apply their strings in the order of the node's string keys, in the headless host and in Dart

- The headless host takes load ids from set_string and waits for the loads: aud_host_wait_loads

- Dart: AudLoad, AudNodeMessage, AudCacheUsage, AudLoadNotification, AudMessageNotification; AudGraph.setString returns the load id, loads, loadOf, cancelLoad, loadEnded, waitLoads, sendMessage, messages, cacheBudget, cacheUsage, waitTasks; AudHost.waitLoads

- Native tests of the tasks, loads, caches, reports and messages under ASan/UBSan, RealtimeSanitizer and ThreadSanitizer

- Add the native fixes of the ticket 25 review

- Message slots come back when a node with a fixed block is prepared again, ends its fade before its block is full or is destroyed while its wrapper holds a message

- A cache budget above INT64_MAX is INT64_MAX, no limit: UINT64_MAX no longer trims every cache

- The byte checks of messages and replies take no sum that wraps on a 32-bit target

- An offline render waits for the workers before its first block too

- The commit that removes a node cancels its loads; a load that a destroyed node, or one whose create or prepare failed, leaves running ends as AUD_ERROR_CANCELLED; a load begun in create or the first prepare carries the handle of the new node, which a failed node spends

- A load keeps one waiting progress notification; a progress beyond notification_capacity is dropped and counted, a begin or an end never

- A graph counts the tasks its nodes post and waits only for them when it is destroyed

- Messages to and from nodes: at most AUD_GRAPH_MAX_MESSAGE_ARGS (16) arguments and AUD_GRAPH_MESSAGE_BYTES (1000) in the layout of a reply, tags and values counted; AUD_ERROR_CAPACITY beyond them, a reply beyond them is dropped and counted

- The header explains the reports of nodes, the dropped replies and the merged reports of AUD_NOTIFY_DIAGNOSTIC and cites lifecycle-002 and osc-002

- Helpers for the loads, constants for the progress throttle and one wakeListener; a load notification is queued under the lock of its load

- Native tests of every fix under ASan/UBSan, RealtimeSanitizer and ThreadSanitizer

- Add the Dart and doc fixes of the ticket 25 review

- README.md names the new C API and Dart members: aud_tasks_wait_idle, aud_cache_budget, aud_cache_usage, AUD_NOTIFY_MESSAGE, the message limits and capacities, messages_dropped, AUD_GRAPH_VIOLATION_CONTROL; loads, loadOf, cancelLoad, waitLoads, waitTasks, AudHost.waitLoads, AudNodeMessage, AudLoadNotification, AudMessageNotification

- README.de.md says in German what README.md says: ABI 0.4, the resources of the process, the headless host with its loads, the Dart API and the tests

- A diagnostic AUD_ERROR_UNSUPPORTED of a node reads as a refusal of the node, not of the transport

- waitLoads, waitTasks and AudHost.waitLoads round a timeout up to milliseconds: one under a millisecond waits one, not as long as it takes; a negative one throws an ArgumentError

- sendMessage documents the limits of a message and what it throws

- The docs cite lifecycle-002 and osc-002 and name loadOf, the message of a load that ended well, the notifications of the loads and the control calls the watchdog counts

- The error tests check the type and the message; AudCacheUsage and AudNodeMessage get their landmarks


## 0.4.0 - 2026-10-09

### Changed

- Split the Dart API into neutral and ffi parts
- Describe the neutral and ffi APIs in the README

## 0.3.0 - 2026-10-09

### Added

- Add the headless host, stress tests and watchdog (ticket 20)

- Add the headless host aud_host_* (plugin-002): graph documents with presets, state blobs and assets, stable parameter ids, latency and tail, the graph's events for the host

- Park a node for its state calls: no realtime call overlaps them; its events, parameter changes and resets wait for its next block

- Add the output tail and aud_graph_render_host with an event output and a freewheel flag

- Add the debug watchdog behind AUD_GRAPH_WATCHDOG and the user define watchdog

- Add stress tests; run the native tests under ASan/UBSan, RealtimeSanitizer and ThreadSanitizer

- Reserve scheduler places at the enqueue and budget scheduled events per block

- Keep the transport position across prepare and stop; close tracked notes after a restart

- Keep a note on and its own note off in order; only a sounding note retriggers

- Fix two data races the thread sanitizer found: the tap ring and the pending program

- Refuse descriptors without parameter or string ids

- Add AudHost and the asset table of the graph document on the Dart side


## 0.2.0 - 2026-10-08

### Changed

- Build the audio graph engine on ABI 0.3 (ticket 19, step S2)
- Replace the spike chain by compiled render programs with persistent
node instances, transactions, fades and retirement
- Add the realtime queues, the scheduler, the running-note tracker and
the notification thread
- Add the internal transport, the time filter feed and the offline
renderer
- Add the reference nodes oscillator, mixer, filter, feedback and tap
- Add the Dart graph API, the graph document with its JSON schema and
the native tests under the sanitizers
- Apply the review-light fixes: shared helpers, named constants, set and
binary-search lookups, the transport capture split in two
- Describe the graph engine in the changelog

## 0.1.0 - 2026-10-08

### Added

- Add the spike engine with reference nodes

## 0.0.2 - 2026-10-08

### Added

- Initial boilerplate

### Changed

- Record the gg commit state
- Set up GitHub repo settings and branch rules
- Update dev dependencies
