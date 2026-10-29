<!--
@license
Copyright (c) Audanika. All Rights Reserved.

Use of this source code is governed by terms that can be
found in the LICENSE file in the root of this package.
-->

# aud_audio_with_midi umbenannt nachziehen

## Motivation

- Das Paket `aud_audio_midi` heißt jetzt `aud_audio_with_midi`.
- Dieses Repo nennt es in Index und Doku; der alte Name würde auf ein
  Paket zeigen, das es unter diesem Namen nicht mehr gibt.

## Strategie

- Den Namen überall ersetzen, wo dieses Repo das Paket nennt.
- Weder Code noch API ändern.

## Umsetzung

- `index.jsonc`: `interfaces.usedBy` nennt `aud_audio_with_midi`.
- Der Doc-Kommentar der Event-Source-FFI nutzt den neuen Namen.

## Ergebnis

- Doku und Index passen zum umbenannten Paket; sonst ändert sich nichts.
