# Changelog

## Unreleased

### Changed

- Build the audio graph engine (S2)

- Replace the spike chain by compiled render programs on ABI 0.3

- Add transactions with fades and retirement, queues and a scheduler

- Add the internal transport, the time filter feed and the offline renderer

- Add the Dart graph API, the graph document and the native tests


### Fixed

- Apply the review-light fixes (ticket 19)

- Extract the enqueue of commands, the binding factory and the stopped segment
- Name the tap ring minimum, the tap read attempts and the resonance range
- Look up connections of the topology in a set; find event routes by binary search
- Split the transport capture into earliestRequest and loopWrapOffset

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
