# aud_audio_graph

Audio-Graph-Engine der Audanika Audio Engine: C++-Render-Programme mit persistenten Node-Instanzen, Transaktionen, Realtime-Queues, Scheduler, Transport, Offline-Renderer und Headless Host; Dart-Graph-API und Graph-Dokument.

Teil der Audanika Audio Engine; geplant in [aud_audio_pm](https://github.com/audaudio/aud_audio_pm).

## Was das Paket enthält (ABI 0.4, Tickets 19 und 25)

Die Engine hinter `src/aud_audio_graph.h`, gebaut auf den Verträgen von
`aud_audio_core` (Schritt S2 des Plans):

- Persistente Node-Instanzen mit stabilen Handles und unveränderliche
  Render-Programme (graph-001, graph-003): Der Control-Thread bearbeitet
  die Topologie in Transaktionen, ein Commit kompiliert ein Programm —
  topologische Ordnung, Feedback-Nodes geteilt in Leser und Schreiber,
  Latenzausgleich mit Low-Latency-Ausnahme, Puffer nach Lebensdauer
  wiederverwendet — und der Realtime-Thread übernimmt es am nächsten
  Blockanfang und bestätigt die Revision. Entfernte Verbindungen blenden
  über 5 ms aus, neue blenden ein, entfernte Nodes rendern ihren Tail
  (höchstens 10 s), bevor sie freigegeben werden.
- Der Realtime-Vertrag (interop-002): eine Parameter- und eine Event-Queue
  mit festen Kapazitäten, die ablehnen statt zu verwerfen, ein Budget pro
  Block, ein zeitgeordneter Scheduler mit 10 s Vorausschau für Events in
  Sample-, Host- oder Beat-Zeit, die Vorab-Zustellung um die Pfadlatenz,
  der Abbruch nach ID oder Node, späte Events am Blockanfang gespielt oder
  verworfen, klingende Noten beim Ausscheiden, beim Stopp und beim Reset
  geschlossen, verlustbehaftete Taps, ein Benachrichtigungs-Thread, der
  den Control-Thread weckt.
- Der Lebenszyklus (lifecycle-001): created, prepared, running,
  suspended, stopped, disposed; ein erneutes Prepare für eine neue
  Abtastrate oder Blockgröße.
- Zeit (time-001): Die Host-Zeiten des Streams speisen den Filter des
  Cores, ohne sie eine synthetisierte Host-Zeit, der interne Transport mit
  Segmenten pro Block (Start, Stopp, Seek, Tempo, Taktart, Loop) und
  registrierte Transport-Provider; der Snapshot eines Plugin-Hosts hat
  Vorrang.
- Die Render-Funktion von plugin-002, `aud_graph_render`, mit Host-Bussen
  und Events, und der Offline-Renderer über einer virtuellen Zeitachse.
- Die Referenzknoten `aud.graph.oscillator`, `aud.graph.mixer`,
  `aud.graph.filter`, `aud.graph.feedback` und `aud.graph.tap`;
  `aud.core.gain` des Cores registriert die Dart-Seite.

## Die Ressourcen des Prozesses (Ticket 25)

Die Graphen eines Prozesses teilen sich, was die Pakete für ihre Assets
brauchen (lifecycle-002), über die Host-API von ABI 0.4:

- Zwei Worker-Threads mit einer lock-freien Task-Queue, in die der
  Control-Thread, die Worker und der Audio-Thread Tasks stellen dürfen.
  Ein Graph zählt die Tasks seiner Nodes: Wird er zerstört, wartet er auf
  sie, nicht auf die anderer Graphen. `aud_tasks_wait_idle` wartet auf
  jeden Task des Prozesses; ein Offline-Render wartet so vor seinem ersten
  Block und nach jedem Block, damit er sich genau wiederholt.
- Loads mit IDs: Ein Node öffnet einen Load in `set_string`, das seine ID
  zurückgibt, meldet den Fortschritt und beendet ihn mit einem Ergebnis -
  fertig, fehlgeschlagen mit einer Meldung oder abgebrochen. Ein Graph
  behält die laufenden Loads und die letzten 64 (`aud_graph_loads`,
  `aud_graph_load_info`, `aud_graph_cancel_load`, `aud_graph_wait_loads`)
  und meldet sie als `AUD_NOTIFY_LOAD`; ein Fortschritt, der noch wartet,
  weicht dem nächsten. Das Entfernen eines Nodes bricht seine Loads ab,
  und ein Load, den sein Node laufen lässt, endet als abgebrochen.
- Ein Sample-Cache-Budget für den Prozess, standardmäßig 256 MB
  (`aud_cache_set_budget`, `aud_cache_budget`): Pakete registrieren ihre
  Caches und melden ihre Bytes; hält der Prozess mehr, bittet ein Worker
  den größten Cache, freizugeben, was niemand nutzt, das am längsten
  Ungenutzte zuerst. Ein kleineres Budget kürzt sofort. `aud_cache_usage`
  füllt ein `AudCacheUsage` mit den Caches, dem Budget, den Bytes, den
  Kürzungen und den Workern.
- Meldungen eines Nodes aus dem Audio-Thread, pro Block summiert in
  `AUD_NOTIFY_DIAGNOSTIC`, z. B. ein Stream, der nicht nachkam. Ein
  Control-Aufruf der Host-API im Audio-Thread ist eine Verletzung, die der
  Watchdog als `AUD_GRAPH_VIOLATION_CONTROL` zählt.
- Nachrichten an Nodes (osc-002): `aud_graph_send_message` kopiert eine
  OSC-Nachricht mit höchstens 16 Argumenten und 1000 Bytes
  (`AUD_GRAPH_MAX_MESSAGE_ARGS`, `AUD_GRAPH_MESSAGE_BYTES`) in einen der
  `message_capacity` Slots des Graphen (`AudGraphConfig`) und stellt sie
  als `AUD_EVENT_MESSAGE`-Event zu, geordnet mit den anderen Events. Die
  Antworten des Nodes, in denselben Grenzen, warten in einer Queue von
  `reply_capacity` auf `aud_graph_take_messages`, angekündigt durch
  `AUD_NOTIFY_MESSAGE`; eine Antwort jenseits der Grenzen oder der Queue
  zählt in `messages_dropped` von `AudGraphStats`.
- Die Engine liest den Tail eines Nodes bei jedem Commit, damit ein Tail
  dem folgen kann, was der Node geladen hat.

## Der Headless Host (Ticket 20)

Die C-API `aud_host_*` betreibt einen Graphen ohne Dart, so wie es die
Plugin-Shells tun (plugin-002):

- `aud_host_load` prüft ein ganzes Graph-Dokument - Typen, IDs, Busse,
  Parameter, String-Schlüssel, State-Versionen, Assets auf der Platte,
  Parameter-IDs - und legt erst dann die Nodes an, wendet ihre Presets an
  und verbindet sie in einer Transaktion; ein abgelehntes Dokument ändert
  nichts.
- Presets wirken in der Reihenfolge Strings, State-Blob, Parameter - die
  Strings in der Reihenfolge der String-Schlüssel des Nodes -; der Host
  behält jeden Parameterwert. Ein String, der einen Load startet, ist in
  Ordnung: Der Host wartet mit `aud_host_wait_loads` auf die Loads eines
  Dokuments. `aud_host_save` schreibt das Dokument mit den aktuellen
  Parametern, Strings und State-Blobs zurück.
- Assets: Die Tabelle `assets` des Dokuments ordnet IDs Pfaden zu, ein
  String `asset:<id>` erreicht den Node als aufgelöster Pfad;
  `aud_host_set_asset_path` verknüpft neu.
- Stabile Parameter-IDs: FNV-1a über `<Node-ID>/<Parameter-ID>` mit
  gelöschtem obersten Bit; kollidierende Dokumente werden abgelehnt.
- `aud_host_latency` und `aud_host_tail` (`aud_graph_output_tail`);
  `aud_host_render` und `aud_graph_render_host` übergeben dem Host die
  Events, die der Graph an seinen Event-Eingang sendet, und nehmen ein
  Freewheel-Flag.
- `aud_graph_node_save_state` und `aud_graph_node_load_state` parken einen
  Node eines laufenden Graphen für den Aufruf.

Dart kapselt ihn als `AudHost`; `AudGraph` hat `saveState`, `loadState`,
`addAsset`, `assetPath`, `outputTail` und schreibt die State-Blobs in
seine Dokumente:

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

## Der Realtime-Vertrag unter Test

Die nativen Tests in `test/native` laufen unter dem Address- und dem
Undefined-Behaviour-Sanitizer, unter Clangs RealtimeSanitizer, wenn ein
Compiler auf der Maschine ihn hat (LLVM aus Homebrew auf macOS; eine Probe
beweist, dass der Lauf eine Allokation im Audio-Thread fängt), und unter
dem Thread-Sanitizer. Stresstests prüfen Queue-Überlauf, späte Events,
eine Transaktion vor jedem Block mit Klick-Detektor, Routenwechsel bei
klingenden Noten, vollen Scheduler und Note-Tracker und einen Stream, der
auf eigenem Thread rendert. Der Debug-Watchdog zählt Allokationen, Frees
und Log-Aufrufe im Audio-Thread - in einer App die der eigenen Bibliothek
des Graphen und jeden Aufruf der Host-API; `AudGraphFfi.watchdogEnabled`
und `watchdogViolations` lesen ihn. Er ist in den nativen Tests
eingeschaltet und in einer App, die Folgendes setzt:

```yaml
hooks:
  user_defines:
    aud_audio_graph:
      watchdog: true
```

## Dart-API

`aud_audio_graph.dart` ist plattformneutral und kompiliert für das Web
(web-001): `AudGraph` ist ein Interface, dessen Factory auf nativen
Plattformen ein `AudGraphFfi` erzeugt. `aud_audio_graph_ffi.dart` ergänzt
`AudGraphFfi` mit `pointer` und `hostApi`, den Render-Einstieg
`aud_graph_render`, den Headless Host, den Offline-Renderer und
WAV-Dateien.

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
```

Die Benachrichtigungen tragen `AudLoadNotification` für die Loads und
`AudMessageNotification` für die Antworten eines Blocks; `AudGraphOptions`
setzt `messageCapacity` und `replyCapacity`.

`graph.send(command)` nimmt die typisierten Kommandos des Cores und
`graph.handleOsc(message)` die OSC-Nachrichten von osc-001; DSP-Pakete
registrieren ihre Node-Typen mit `graph.hostApi`; ein Stream von
`aud_audio_io` ruft `aud_graph_render` mit `graph.pointer` als
User-Pointer auf, so wie es `AudEngine` von `aud_audio` tut.

## Tests

`dart test` deckt die Dart-Seite ab und startet `scripts/test-native.js`,
das die nativen Tests von `test/native` unter jedem Sanitizer baut und
ausführt, den die Maschine hat (`--no-sanitize`, `--no-rtsan`, `--no-tsan`
und `--rtsan` wählen aus). Das Golden-Rendering in `test/goldens` wird mit
`UPDATE_GOLDENS=1` aktualisiert.

Die C-API steht in `src/aud_audio_graph.h`; die Bindings erzeugt
`dart run ffigen --config ffigen.yaml` neu.
