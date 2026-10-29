<!--
@license
Copyright (c) Audanika. All Rights Reserved.

Use of this source code is governed by terms that can be
found in the LICENSE file in the root of this package.
-->

# Follow the rename of aud_audio_with_midi

## Motivation

- The package `aud_audio_midi` is renamed to `aud_audio_with_midi`.
- This repo names it in its index and docs; the old name would point
  readers to a package that no longer exists under it.

## Strategy

- Replace the name wherever this repo mentions the package.
- Change no code and no API.

## Implementation

- `index.jsonc`: `interfaces.usedBy` names `aud_audio_with_midi`.
- Doc comments, and the READMEs where they link the package, use the new
  name and the new GitHub URL.

## Result

- Docs and index match the renamed package; nothing else changed.
