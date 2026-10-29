<!--
@license
Copyright (c) Audanika. All Rights Reserved.

Use of this source code is governed by terms that can be
found in the LICENSE file in the root of this package.
-->

# Der Test des Timing-Histogramms stellt seine eigene Uhr

## Motivation

- Der native Test `graph_timing_histogram_ignores_a_single_gap_at_start_up`
  prüft, dass eine einzelne Start-up-Lücke von 177 ms p99.9 des
  Perioden-Histogramms nicht verschiebt: Median und p99.9 bleiben der
  Callback von 5,33 ms.
- Er maß echte Wartezeiten: `sleep_for` für die Lücke, dann 1000 Renders im
  Takt von `sleep_until`, und er ließ p99.9 zwischen 5,4 ms und 40 ms zu.
- Bei 1001 Lücken ist p99.9 die zweitlängste. Unter dem Thread-Sanitizer
  auf einer ausgelasteten Maschine reichte ein einziges spätes Aufwachen
  über 40 ms, und der Test schlug fehl. Mit 16 Busy-Loops auf 8 Kernen war
  es 1 Lauf von 15.
- Ein Test, der ohne Fehler rot wird, verdeckt die Läufe, in denen er
  wegen eines Fehlers rot wird. Er soll das Histogramm prüfen, nicht wie
  der Host einen Thread einplant.

## Strategie

- Die Engine liest den Start eines Blocks aus `aud_clock_now_ns`. Diese
  Zeit kann ein Test nicht stellen, also bekommt die Engine eine
  Render-Uhr, die ein nativer Test ersetzen kann.
- Der Ersatz wird nur mit `AUD_GRAPH_TEST_CLOCK` einkompiliert, wie der
  Debug-Watchdog mit `AUD_GRAPH_WATCHDOG`: Ein Release-Build liest
  `aud_clock_now_ns` direkt und trägt keine Indirektion.
- Die öffentliche C-API bleibt, wie sie ist; die Tests erreichen die Uhr
  über einen internen Header, da sie die Engine in ein Programm linken.
- Der Test stellt die Uhr von Hand vor und prüft exakte Bins statt weiter
  Grenzen.

## Die Render-Uhr

- Neuer Header `src/aud_graph_test_clock.hpp` mit `aud::renderNowNs()`.
- Mit `AUD_GRAPH_TEST_CLOCK` ruft sie die Uhr, die `aud::setTestClock()`
  gespeichert hat, oder `aud_clock_now_ns`, solange keine gesetzt ist. Der
  Zeiger ist ein Atomic, so bleiben Tests, die auf eigenem Thread rendern,
  unter dem Thread-Sanitizer sauber.
- `aud_graph_render.cpp` liest die Zeit des Render-Threads an drei Stellen
  über sie: den Start eines Blocks, den das Perioden-Histogramm misst, die
  Dauer eines Blocks und den Ursprung einer synthetisierten Stream-Zeit.
  Alle drei kommen von derselben Uhr, so mischt die Dauer eines Blocks nie
  eine Test-Uhr mit der echten.
- `scripts/test-native.js` baut jede Variante mit
  `-DAUD_GRAPH_TEST_CLOCK=1`, neben dem Watchdog.

## Der Test

- Eine `ManualClock` im Test setzt die Uhr für ihren Scope und stellt am
  Ende die echte wieder her.
- Der Test rendert einen Block, stellt die Uhr um 177 ms vor, rendert
  erneut und stellt dann vor jedem von 1000 Renders um 5333333 ns vor.
  Kein Thread schläft.
- Er prüft exakte Werte: Die längste Lücke ist 177 ms, Bin 53 (5,3 ms)
  hält 1000 Lücken, der Überlauf-Bin hält die Lücke, und Median und p99.9
  sind beide 5,4 ms, die obere Kante von Bin 53.
- Der Test läuft jetzt in Millisekunden statt in etwa fünfeinhalb
  Sekunden.

## Ergebnis

- `node scripts/test-native.js` bestand 20-mal hintereinander: 137 von 137
  Tests im Build mit Address- und Undefined-Behaviour-Sanitizer und im
  Build mit Thread-Sanitizer.
- Der Test bestand 15 von 15 Läufen unter der Last, die ihn vorher
  scheitern ließ.
- Ein Build ohne das Define kompiliert den Render-Pfad unverändert.

## Offene Punkte

- Der RealtimeSanitizer-Build lief nicht: Kein Compiler auf der
  Testmaschine hat ihn. `renderNowNs` ist ein atomares Load und ein
  Aufruf, beides im Audio-Thread erlaubt.
- Andere Timing-Tests schlafen weiter und behalten weite Grenzen, etwa
  `graph_timing_histogram_takes_the_gap_of_split_callbacks` und
  `graph_timing_histogram_drops_the_gap_that_reset_the_filter`. Sie können
  auf die Test-Uhr wechseln, wenn sie flaky werden.
