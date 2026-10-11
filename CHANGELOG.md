# Changelog

## Unreleased

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
