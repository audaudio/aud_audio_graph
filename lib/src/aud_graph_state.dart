// @license
// Copyright (c) Audanika. All Rights Reserved.
//
// Use of this source code is governed by terms that can be
// found in the LICENSE file in the root of this package.

import 'aud_graph_constants.dart' as bindings;

// #############################################################################
/// The lifecycle states of a graph (decision lifecycle-001).
enum AudGraphState {
  /// Created with its format; instances are prepared as they are created.
  created(bindings.AUD_GRAPH_CREATED),

  /// Prepared for a sample rate and a largest block.
  prepared(bindings.AUD_GRAPH_PREPARED),

  /// Rendering.
  running(bindings.AUD_GRAPH_RUNNING),

  /// Rendering silence; the transport and the pending events stay.
  suspended(bindings.AUD_GRAPH_SUSPENDED),

  /// Stopped: running notes closed, the transport stopped.
  stopped(bindings.AUD_GRAPH_STOPPED),

  /// Destroyed.
  disposed(bindings.AUD_GRAPH_DISPOSED);

  const AudGraphState(this.code);

  /// The `AUD_GRAPH_*` code.
  final int code;

  /// The state with [code].
  static AudGraphState fromCode(int code) => values.firstWhere(
    (state) => state.code == code,
    orElse: () => throw ArgumentError.value(code, 'code', 'Unknown state'),
  );
}
