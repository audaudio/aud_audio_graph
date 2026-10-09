# aud_audio_graph

Audio graph engine of the Audanika Audio Engine: C++ render programs with persistent node instances, transactions, realtime queues, scheduler, transport, offline renderer, headless host; Dart graph API and graph document.

Teil der Audanika Audio Engine; geplant in [aud_audio_pm](https://github.com/audaudio/aud_audio_pm).

## Was das Paket enthält (ABI 0.3, Ticket 19)

Die Engine hinter `src/aud_audio_graph.h` auf den Verträgen von
`aud_audio_core` (Schritt S2 des Plans): persistente Node-Instanzen mit
stabilen Handles und unveränderliche Render-Programme (graph-001,
graph-003), die der Control-Thread in Transaktionen bearbeitet und der
Realtime-Thread am Blockanfang übernimmt; Fades für entfernte und neue
Verbindungen, Tails für entfernte Nodes; Parameter- und Event-Queues mit
festen Kapazitäten, ein zeitgeordneter Scheduler, die Vorab-Zustellung um
die Pfadlatenz, der Note-Tracker und der Benachrichtigungs-Thread
(interop-002); der Lebenszyklus (lifecycle-001); der Zeitfilter und der
interne Transport mit Segmenten pro Block (time-001); die Render-Funktion
von plugin-002 und der Offline-Renderer; die Referenzknoten Oszillator,
Mixer, Filter, Feedback und Tap.

Die Dart-API: `AudGraph`, `AudNode`, `AudGraphTransaction`,
`AudGraphNotification`, `AudGraphDocument` mit JSON-Schema,
`AudOfflineRenderer` und `AudWavFile`. Die nativen Tests laufen mit
`scripts/test-native.js` unter den Sanitizern.

## Der Headless Host (Ticket 20)

Die C-API `aud_host_*` betreibt einen Graphen ohne Dart, so wie es die
Plugin-Shells tun (plugin-002): `aud_host_load` prüft ein ganzes
Graph-Dokument - Typen, IDs, Busse, Parameter, String-Schlüssel,
State-Versionen, Assets auf der Platte, Parameter-IDs - und legt erst dann
die Nodes in einer Transaktion an; ein abgelehntes Dokument ändert nichts.
Presets wirken in der Reihenfolge Strings, State-Blob, Parameter;
`aud_host_save` schreibt den ganzen Zustand als Dokument zurück. Assets
stehen in einer Tabelle des Dokuments, ein String `asset:<id>` erreicht
den Node als aufgelöster Pfad. Stabile Parameter-IDs sind FNV-1a über
`<Node-ID>/<Parameter-ID>` mit gelöschtem obersten Bit. Latenz, Tail, die
Events des Graphen für den Host und ein Freewheel-Flag kommen dazu; für
die State-Aufrufe wird ein Node eines laufenden Graphen geparkt. In Dart:
`AudHost`, `AudGraph.saveState`, `loadState`, `addAsset` und
`outputTail`.

## Der Realtime-Vertrag unter Test

Die nativen Tests laufen unter Address- und Undefined-Behaviour-Sanitizer,
unter Clangs RealtimeSanitizer, wenn ein Compiler auf der Maschine ihn hat
(LLVM aus Homebrew auf macOS; eine Probe beweist, dass der Lauf eine
Allokation im Audio-Thread fängt), und unter dem Thread-Sanitizer.
Stresstests prüfen Queue-Überlauf, späte Events, eine Transaktion vor
jedem Block mit Klick-Detektor, Routenwechsel bei klingenden Noten, vollen
Scheduler und Note-Tracker und einen Stream auf eigenem Thread. Der
Debug-Watchdog zählt Allokationen, Frees und Log-Aufrufe im Audio-Thread;
eine App schaltet ihn mit dem User-Define `watchdog: true` von
`aud_audio_graph` ein.
