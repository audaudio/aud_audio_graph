# aud_audio_graph

Audio graph engine of the Audanika Audio Engine: C++ render programs, scheduler, transport, headless host; Dart graph API.

Part of the Audanika Audio Engine; planned in [aud_audio_pm](https://github.com/audaudio/aud_audio_pm).

## The spike engine (ticket 5)

`AudEngine` hosts node types registered through the C ABI of
`aud_audio_core`, renders a serial chain of node instances block by block
on the realtime thread and takes parameter changes and events from a
lock-free command queue. It ships the reference nodes `aud.ref.sine`
(parameters `frequency`, `gain`) and `aud.ref.gain` (`gain`).

```dart
import 'package:aud_audio_graph/aud_audio_graph.dart';

final engine = AudEngine(sampleRate: 48000, maxFrames: 1024, channels: 2);
engine.nodeTypes;                         // [AudNodeTypeInfo(aud.ref.sine), ...]
final sine = engine.createNode('aud.ref.sine');
final gain = engine.createNode('aud.ref.gain');
engine.setChain([sine, gain]);            // adopted at the next block start
engine.setParam(sine, 0, 880);            // enqueued; throws when the queue is full
engine.noteOn(node, number: 60);          // for nodes that take events
engine.setString(node, key, value);       // control-thread settings, e.g. loading
final block = engine.render(256);         // offline, for tests
engine.stats;                             // commands, latencies, render time
engine.dispose();
```

A DSP package registers with `engine.hostApi`; an `aud_audio_io` stream
pulls blocks with `AudEngine.renderCallback` and `engine.handle` as the
user pointer. `AudEngineException` carries the ABI result code of a
refused call.

The C API is in `src/aud_audio_graph.h`; regenerate the bindings with
`dart run ffigen --config ffigen.yaml`.
