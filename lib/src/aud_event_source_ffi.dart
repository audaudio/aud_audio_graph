// @license
// Copyright (c) Audanika. All Rights Reserved.
//
// Use of this source code is governed by terms that can be
// found in the LICENSE file in the root of this package.

import 'dart:ffi';

import 'package:aud_audio_core/aud_audio_core.dart';
import 'package:ffi/ffi.dart';

import 'aud_audio_graph_bindings_generated.dart' as bindings;
import 'aud_event_source.dart';
import 'aud_graph_exception.dart';
import 'aud_graph_native_conversions.dart';
import 'aud_graph_node.dart';

// #############################################################################
/// An [AudEventSource] of the native engine: its [address] and the
/// addresses of the push, release and size functions let a native
/// producer - the tap adapters of `aud_audio_midi` - feed it without Dart.
final class AudEventSourceFfi implements AudEventSource {
  /// Wraps the native source [pointer] of [graph]; `AudGraphFfi`'s
  /// `createEventSource` creates one.
  AudEventSourceFfi({
    required this._graph,
    required this._pointer,
    required this.node,
    required this.port,
    required this.timing,
    required this._latencyNs,
    required this._onDispose,
  });

  // ...........................................................................
  @override
  final AudNode node;

  @override
  final int port;

  @override
  final AudSourceTiming timing;

  /// The native source; `nullptr` once disposed.
  Pointer<bindings.AudEventSource> get pointer => _pointer;

  /// The address of the native source for a native producer.
  int get address => _pointer.address;

  @override
  int get capacity => bindings.aud_event_source_capacity(_pointer);

  @override
  int get size => bindings.aud_event_source_size(_pointer);

  @override
  bool get isDisposed => _pointer == nullptr;

  @override
  int get latencyNs => _latencyNs;

  @override
  set latencyNs(int latencyNs) {
    _checkLive();
    AudGraphException.check(
      bindings.aud_event_source_set_latency(_pointer, latencyNs),
      'set the latency of the source of ${node.name}',
    );
    _latencyNs = latencyNs;
  }

  @override
  void attach(int token) {
    _checkLive();
    AudGraphException.check(
      bindings.aud_event_source_attach(_pointer, token),
      'attach producer $token to the source of ${node.name}',
    );
  }

  @override
  void detach(int token) {
    _checkLive();
    AudGraphException.check(
      bindings.aud_event_source_detach(_pointer, token),
      'detach producer $token from the source of ${node.name}',
    );
  }

  @override
  bool push(List<int> words, {int hostTimeNs = 0}) {
    _checkLive();
    if (words.isEmpty || words.length > 4) {
      throw ArgumentError.value(words, 'words', 'One to four UMP words');
    }
    for (var i = 0; i < words.length; i++) {
      _words[i] = words[i];
    }
    final result = bindings.aud_event_source_push(
      _pointer,
      _words,
      words.length,
      hostTimeNs,
    );
    if (result == AUD_ERROR_QUEUE_FULL) return false;
    AudGraphException.check(result, 'push a packet into the source');
    return true;
  }

  @override
  void release() {
    if (isDisposed) return;
    bindings.aud_event_source_release(_pointer);
  }

  @override
  AudEventSourceStats get stats {
    _checkLive();
    final native = calloc<bindings.AudEventSourceStats>();
    try {
      native.ref.struct_size = sizeOf<bindings.AudEventSourceStats>();
      AudGraphException.check(
        bindings.aud_event_source_stats(_pointer, native),
        'read the counters of the source',
      );
      return native.ref.toDart();
    } finally {
      calloc.free(native);
    }
  }

  @override
  void dispose() {
    if (isDisposed) return;
    AudGraphException.check(
      bindings.aud_graph_destroy_event_source(_graph, _pointer),
      'destroy the source of ${node.name}',
    );
    _onDispose(this);
    markDisposed();
  }

  /// Marks the source disposed without a call: the graph went with it.
  void markDisposed() {
    if (isDisposed) return;
    _pointer = nullptr;
    calloc.free(_words);
  }

  @override
  String toString() =>
      'AudEventSource(${node.name}:$port, ${timing.name}, $latencyNs ns)';

  // ...........................................................................
  /// The address of `aud_event_source_push`, an
  /// `AudEventSourcePushFunction`.
  static int get pushFunctionAddress =>
      Native.addressOf<
            NativeFunction<
              Int32 Function(
                Pointer<bindings.AudEventSource>,
                Pointer<Uint32>,
                Uint32,
                Int64,
              )
            >
          >(bindings.aud_event_source_push)
          .address;

  /// The address of `aud_event_source_release`, an
  /// `AudEventSourceReleaseFunction`.
  static int get releaseFunctionAddress =>
      Native.addressOf<
            NativeFunction<Void Function(Pointer<bindings.AudEventSource>)>
          >(bindings.aud_event_source_release)
          .address;

  /// The address of `aud_event_source_size`, an
  /// `AudEventSourceSizeFunction`.
  static int get sizeFunctionAddress =>
      Native.addressOf<
            NativeFunction<Uint32 Function(Pointer<bindings.AudEventSource>)>
          >(bindings.aud_event_source_size)
          .address;

  // ...........................................................................
  final Pointer<bindings.AudGraph> _graph;
  Pointer<bindings.AudEventSource> _pointer;
  int _latencyNs;
  final void Function(AudEventSourceFfi source) _onDispose;
  final Pointer<Uint32> _words = calloc<Uint32>(4);

  void _checkLive() {
    if (isDisposed) throw StateError('The event source is disposed');
  }
}
