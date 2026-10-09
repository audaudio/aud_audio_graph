# Changelog

## Unreleased

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
