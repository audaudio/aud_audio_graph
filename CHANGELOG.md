# Changelog

## Unreleased

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
