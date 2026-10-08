// @license
// Copyright (c) Audanika. All Rights Reserved.
//
// Use of this source code is governed by terms that can be
// found in the LICENSE file in the root of this package.

import 'package:ffi/ffi.dart';

import 'aud_audio_graph_bindings_generated.dart' as bindings;

// #############################################################################
/// An asset of the document an [AudHost] loaded: its id, the path the
/// document names and the path the nodes receive (plugin-002).
class AudHostAsset {
  /// Creates the description of an asset.
  const AudHostAsset({
    required this.id,
    required this.path,
    required this.resolved,
    required this.exists,
  });

  /// The asset from its native struct.
  factory AudHostAsset.fromNative(bindings.AudHostAsset native) => AudHostAsset(
    id: native.id.cast<Utf8>().toDartString(),
    path: native.path.cast<Utf8>().toDartString(),
    resolved: native.resolved.cast<Utf8>().toDartString(),
    exists: native.exists != 0,
  );

  // ...........................................................................
  /// The id the string settings name as `asset:<id>`.
  final String id;

  /// The path as the document names it.
  final String path;

  /// The path the nodes receive: [path] resolved against the host's base
  /// directory.
  final String resolved;

  /// Whether the file existed when the host looked last.
  final bool exists;

  /// The asset as JSON.
  Map<String, Object?> toJson() => {
    'id': id,
    'path': path,
    'resolved': resolved,
    'exists': exists,
  };

  @override
  bool operator ==(Object other) =>
      other is AudHostAsset &&
      other.id == id &&
      other.path == path &&
      other.resolved == resolved &&
      other.exists == exists;

  @override
  int get hashCode => Object.hash(id, path, resolved, exists);

  @override
  String toString() => 'AudHostAsset(${toJson()})';
}
