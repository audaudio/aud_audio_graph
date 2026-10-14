// @license
// Copyright (c) Audanika. All Rights Reserved.
//
// Use of this source code is governed by terms that can be
// found in the LICENSE file in the root of this package.

import 'aud_graph_constants.dart' as bindings;

// #############################################################################
/// The load of the engine over a window of its last live renders (ticket
/// 29): `AudGraphLoad` of the engine. Each render took a share of its
/// block - its render time over the time its frames last, 1 when it took
/// exactly as long as its block -; the gauge keeps the last [maxRenders] of
/// them, never counts offline renders and is not touched by
/// `AudGraph.resetStats`. A load governor reads it, e.g. every 100 ms over
/// the last second.
class AudGraphLoad {
  /// Creates a reading of the gauge; the defaults are an empty window.
  const AudGraphLoad({
    this.window = Duration.zero,
    this.renders = 0,
    this.frames = 0,
    this.mean = 0,
    this.p99 = 0,
    this.max = 0,
    this.rendersTotal = 0,
    this.overloadsTotal = 0,
    this.bufferFrames = 0,
    this.bufferOverloads = 0,
    this.bufferP99 = 0,
    this.bufferMax = 0,
  });

  // ...........................................................................
  /// The time the renders of the window cover: their [frames] at the
  /// sample rate. Shorter than the window asked for while fewer renders
  /// have run, or when [maxRenders] renders do not cover it.
  final Duration window;

  /// The renders in the window.
  final int renders;

  /// The frames of the renders in the window.
  final int frames;

  /// The share of its block a render of the window took, weighted by its
  /// frames: the render time of the window over its duration.
  final double mean;

  /// The 99th percentile of the shares by nearest rank: 99 % of the renders
  /// of the window took at most this share of their block.
  final double p99;

  /// The largest share of the window.
  final double max;

  /// The live renders since the graph was created.
  final int rendersTotal;

  /// The live renders since the graph was created that took longer than
  /// their block.
  final int overloadsTotal;

  /// The device buffer the runs fill, in frames, as asked; at most one
  /// block makes every render a run.
  ///
  /// A stream whose buffer holds several renders plays on when one render
  /// takes longer than its block and the next ones catch up; it drops out
  /// when the renders that fill a buffer take longer than it lasts. A run
  /// is a render of the window and the older ones before it whose frames
  /// reach [bufferFrames]; its share is their render time over their
  /// duration. Runs that reach past the window do not count.
  final int bufferFrames;

  /// The runs of the window that took longer than they last.
  final int bufferOverloads;

  /// The 99th percentile of the shares of the runs by nearest rank.
  final double bufferP99;

  /// The largest share of a run.
  final double bufferMax;

  // ...........................................................................
  /// The reading as JSON; the window in microseconds.
  Map<String, Object?> toJson() => {
    'windowUs': window.inMicroseconds,
    'renders': renders,
    'frames': frames,
    'mean': mean,
    'p99': p99,
    'max': max,
    'rendersTotal': rendersTotal,
    'overloadsTotal': overloadsTotal,
    'bufferFrames': bufferFrames,
    'bufferOverloads': bufferOverloads,
    'bufferP99': bufferP99,
    'bufferMax': bufferMax,
  };

  @override
  bool operator ==(Object other) =>
      other is AudGraphLoad &&
      other.window == window &&
      other.renders == renders &&
      other.frames == frames &&
      other.mean == mean &&
      other.p99 == p99 &&
      other.max == max &&
      other.rendersTotal == rendersTotal &&
      other.overloadsTotal == overloadsTotal &&
      other.bufferFrames == bufferFrames &&
      other.bufferOverloads == bufferOverloads &&
      other.bufferP99 == bufferP99 &&
      other.bufferMax == bufferMax;

  @override
  int get hashCode => Object.hash(
    window,
    renders,
    frames,
    mean,
    p99,
    max,
    rendersTotal,
    overloadsTotal,
    bufferFrames,
    bufferOverloads,
    bufferP99,
    bufferMax,
  );

  @override
  String toString() => 'AudGraphLoad(${toJson()})';

  // ...........................................................................
  /// The live renders the gauge keeps: 1024.
  static const int maxRenders = bindings.AUD_GRAPH_LOAD_RENDERS;
}
