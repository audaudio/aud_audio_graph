<!--
@license
Copyright (c) Audanika. All Rights Reserved.

Use of this source code is governed by terms that can be
found in the LICENSE file in the root of this package.
-->

# Jeder Timing-Test stellt seine eigene Uhr

## Motivation

- Die Render-Uhr der Tests (`AUD_GRAPH_TEST_CLOCK`) hat den Test der
  Start-up-Lücke im Perioden-Histogramm exakt gemacht. Drei weitere Tests
  des Histogramms maßen noch echte Wartezeiten:
  - `graph_timing_reports_the_last_block` schlief 20 ms zwischen zwei
    Renders
  - `graph_timing_histogram_drops_the_gap_that_reset_the_filter` taktete
    40 Renders mit `sleep_until` und schlief 60 ms vor dem Reset
  - `graph_timing_histogram_takes_the_gap_of_split_callbacks` schlief den
    Callback von 10,7 ms zwischen Paaren von Renders
- Sie ließen weite Grenzen zu, etwa p99.9 unter 50 ms oder zwischen
  10,7 ms und 40 ms, und hingen trotzdem davon ab, wie spät der Host einen
  schlafenden Thread weckt. Unter dem Thread-Sanitizer auf einer
  ausgelasteten Maschine ist das dasselbe Risiko, an dem der Test der
  Start-up-Lücke scheiterte.

## Strategie

- Jeder Test, der Perioden, das Histogramm oder die Dauer eines Renders
  prüft, läuft auf der `ManualClock` der Testdatei.
- Tests echter Nebenläufigkeit behalten die echte Zeit: die Übergabe
  zwischen Producer-Threads, die Stresstests, die Tasks und die
  Lastanzeige, die liest, während ein Thread rendert.
- Aus weiten Grenzen werden exakte Bins.

## Die Tests

- `graph_timing_reports_the_last_block` stellt die Uhr um 20 ms vor: Die
  längste Lücke ist 20 ms, Bin 200 hält sie und p99.9 ist 20,1 ms. Nach
  dem Reset des Zeitfilters liegt die nächste Lücke mit 5,33 ms in Bin 53.
- `graph_timing_histogram_drops_the_gap_that_reset_the_filter` stellt vor
  jedem Render 5333333 ns vor und 60 ms vor dem Reset. Vor dem Reset hält
  Bin 53 19 Lücken, danach 20, der Überlauf-Bin keine, die längste Lücke
  ist 5,33 ms und p99.9 ist 5,4 ms.
- `graph_timing_histogram_takes_the_gap_of_split_callbacks` lässt die Uhr
  innerhalb eines Paars stehen und stellt zwischen den Paaren 10666667 ns
  vor: Bin 0 hält 20 Lücken, Bin 106 hält 19, der Median ist 0,1 ms und
  p99.9 ist 10,7 ms.
- Der Helfer `renderPaced` und die Timer-Auflösung von Windows, die er
  brauchte, sind weg; kein Test der Datei schläft mehr eine Periode.

## Eine hängende Note unter Last

- Die Läufe unter Last fanden einen echten Fehler: In einem von 20 Läufen
  endete der Stresstest
  `stress_source_takes_10000_events_per_second_from_a_producer_thread` mit
  einer klingenden Note, obwohl jedes Event ankam.
- Events an einem Offset werden nach Rang sortiert. Ein Note-Off kam immer
  zuerst, wenn seine Tonhöhe klang, damit ein Note-On derselben Tonhöhe
  neu anschlägt.
- Fällt der Render-Thread zurück, landen späte Pakete alle auf Offset 0
  eines Blocks. Für eine klingende Tonhöhe kann ein Block dann
  `off(1) on(2) off(2)` enthalten. Beide Note-Offs kamen zuerst, der Block
  spielte `off off on`, und die zweite Note hing.
- Jetzt kommt nur das erste Note-Off einer klingenden Tonhöhe in einem
  Block zuerst: Nur ein Note-Off kann die frühere Note schließen. Jedes
  weitere gehört zu einem Note-On desselben Blocks und behält seinen Platz
  dahinter.
- Jeder Knoten merkt sich die Tonhöhen, die der Block geschlossen hat, in
  einem zweiten `NoteTracker`, den ein Blockzähler bei Bedarf leert. Ist
  seine Tabelle mit 256 Einträgen voll, behalten weitere Note-Offs ihre
  Reihenfolge.
- `keeps_a_short_note_in_order_and_retriggers_a_sounding_one` schickt für
  eine klingende Tonhöhe `off on off` in einem Block und erwartet diese
  Reihenfolge. Ohne den Fix schlägt er fehl.

## Ergebnis

- `node scripts/test-native.js` lief 20-mal hintereinander grün, während
  16 Busy-Loops die Maschine auslasteten: 137 von 137 Tests im Build mit
  Address- und Undefined-Behaviour-Sanitizer und im Build mit
  Thread-Sanitizer.
- `dart test` lief grün.
- Die drei Timing-Tests warten nicht mehr; vorher schliefen sie etwa eine
  halbe Sekunde.

## Offene Punkte

- Die Tests der Lastanzeige in `aud_graph_load_test.cpp` lesen weiter die
  echte Uhr, prüfen aber nur Untergrenzen oder Zähler, die Last nicht
  brechen kann.
