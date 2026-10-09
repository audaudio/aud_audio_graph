// @license
// Copyright (c) Audanika. All Rights Reserved.
//
// Use of this source code is governed by terms that can be
// found in the LICENSE file in the root of this package.

import 'dart:ffi';

import 'package:aud_audio_core/aud_audio_core_ffi.dart';
import 'package:ffi/ffi.dart';

import 'aud_audio_graph_bindings_generated.dart' as bindings;
import 'aud_graph_ffi.dart';
import 'aud_graph_document.dart';
import 'aud_graph_exception.dart';
import 'aud_host_asset.dart';
import 'aud_host_document_info.dart';
import 'aud_host_param.dart';

// #############################################################################
/// The headless host of the engine (plugin-002, ticket 20) over a graph: it
/// loads a graph document with its presets, state blobs and assets, saves
/// the whole state back as a document, and gives what a plugin shell needs
/// - stable parameter ids, latency and tail.
///
/// The plugin shells call the C API `aud_host_*` of `src/aud_audio_graph.h`
/// without Dart; this class wraps it for tools and tests. The nodes the host
/// creates live in the native graph: the node lists of [AudGraph] do not
/// show them.
class AudHost {
  /// Creates a host over [graph]; relative asset paths resolve against
  /// [baseDirectory], or against the working directory without one.
  AudHost(this.graph, {this.baseDirectory}) {
    final options = calloc<bindings.AudHostOptions>();
    final base = baseDirectory?.toNativeUtf8();
    try {
      options.ref
        ..struct_size = sizeOf<bindings.AudHostOptions>()
        ..base_directory = base?.cast() ?? nullptr;
      _pointer = bindings.aud_host_create(graph.pointer, options);
    } finally {
      if (base != null) calloc.free(base);
      calloc.free(options);
    }
  }

  // ...........................................................................
  /// Reads the buses, the counts and the name of the document [json]
  /// without a graph, so that a shell can create the graph that fits.
  /// Throws an [AudGraphException] for text that is no document.
  static AudHostDocumentInfo inspect(String json) {
    final text = json.toNativeUtf8();
    final info = calloc<bindings.AudHostDocumentInfo>();
    try {
      info.ref.struct_size = sizeOf<bindings.AudHostDocumentInfo>();
      AudGraphException.check(
        bindings.aud_host_inspect(text.cast(), text.length, info),
        'inspect the document',
      );
      return AudHostDocumentInfo.fromNative(info.ref);
    } finally {
      calloc.free(info);
      calloc.free(text);
    }
  }

  /// The stable id of the parameter [paramId] of the node [nodeId]: FNV-1a
  /// over `<node id>/<parameter id>` with the top bit cleared.
  static int paramIdOf(String nodeId, String paramId) {
    final node = nodeId.toNativeUtf8();
    final param = paramId.toNativeUtf8();
    try {
      return bindings.aud_host_param_id(node.cast(), param.cast());
    } finally {
      calloc.free(node);
      calloc.free(param);
    }
  }

  // ...........................................................................
  /// The graph the host loads documents into.
  final AudGraphFfi graph;

  /// The directory relative asset paths resolve against.
  final String? baseDirectory;

  /// The native host.
  Pointer<bindings.AudHost> get pointer => _pointer;

  /// Whether [dispose] ran.
  bool get isDisposed => _pointer == nullptr;

  /// What the last failed load, save, preset or asset call complained
  /// about; empty after a call that succeeded.
  String get lastError =>
      bindings.aud_host_last_error(_pointer).cast<Utf8>().toDartString();

  // ...........................................................................
  /// Loads the document [json]: validates all of it, then creates the
  /// nodes, applies their presets and connects them in one transaction
  /// while the nodes of the previous document retire. Throws an
  /// [AudGraphException] with the host's message for a document that does
  /// not fit; nothing of it is applied then.
  void load(String json) {
    final text = json.toNativeUtf8();
    try {
      _check(
        bindings.aud_host_load(_pointer, text.cast(), text.length),
        'load the document',
      );
    } finally {
      calloc.free(text);
    }
  }

  /// Loads [document]; see [load].
  void loadDocument(AudGraphDocument document) => load(document.toJsonText());

  /// The loaded document with the current state - parameters, strings,
  /// state blobs, assets - as JSON text.
  String save() => _text(
    (buffer, capacity, size) =>
        bindings.aud_host_save(_pointer, buffer, capacity, size),
    'save the document',
  );

  /// The loaded document with the current state; see [save].
  AudGraphDocument saveDocument() => AudGraphDocument.parse(save());

  /// The ids of the nodes of the loaded document, in document order.
  List<String> get nodeIds => [
    for (var i = 0; i < bindings.aud_host_num_nodes(_pointer); i++)
      nodeIdOf(bindings.aud_host_node_at(_pointer, i))!,
  ];

  /// The handle of the node [id] of the loaded document, or null.
  int? nodeHandle(String id) {
    final native = id.toNativeUtf8();
    try {
      final handle = bindings.aud_host_node(_pointer, native.cast());
      return handle > 0 ? handle : null;
    } finally {
      calloc.free(native);
    }
  }

  /// The document id of the node [handle], or null.
  String? nodeIdOf(int handle) {
    final id = bindings.aud_host_node_id(_pointer, handle);
    return id == nullptr ? null : id.cast<Utf8>().toDartString();
  }

  /// Applies [preset] to the node [nodeId] in the order strings, state,
  /// parameters. A preset the validation refuses changes nothing; a part
  /// the node or a full queue refuses stops it there and the parts before
  /// stay.
  void applyPreset(String nodeId, AudNodePreset preset) {
    final id = nodeId.toNativeUtf8();
    final text = preset.toJsonText().toNativeUtf8();
    try {
      _check(
        bindings.aud_host_apply_preset(
          _pointer,
          id.cast(),
          text.cast(),
          text.length,
        ),
        'apply a preset to $nodeId',
      );
    } finally {
      calloc.free(text);
      calloc.free(id);
    }
  }

  /// The preset of the node [nodeId] with its current parameters, strings
  /// and state.
  AudNodePreset nodePreset(String nodeId) {
    final id = nodeId.toNativeUtf8();
    try {
      return AudNodePreset.parse(
        _text(
          (buffer, capacity, size) => bindings.aud_host_node_preset(
            _pointer,
            id.cast(),
            buffer,
            capacity,
            size,
          ),
          'read the preset of $nodeId',
        ),
      );
    } finally {
      calloc.free(id);
    }
  }

  // ...........................................................................
  /// The parameters of the loaded document, ordered by their stable ids.
  List<AudHostParam> get params {
    final native = calloc<bindings.AudHostParam>();
    try {
      native.ref.struct_size = sizeOf<bindings.AudHostParam>();
      return [
        for (var i = 0; i < bindings.aud_host_num_params(_pointer); i++)
          if (bindings.aud_host_param(_pointer, i, native) == AUD_OK)
            AudHostParam.fromNative(native.ref),
      ];
    } finally {
      calloc.free(native);
    }
  }

  /// The parameter with the stable [id], or null.
  AudHostParam? param(int id) {
    final index = bindings.aud_host_param_index(_pointer, id);
    if (index < 0) return null;
    final native = calloc<bindings.AudHostParam>();
    try {
      native.ref.struct_size = sizeOf<bindings.AudHostParam>();
      AudGraphException.check(
        bindings.aud_host_param(_pointer, index, native),
        'read parameter $id',
      );
      return AudHostParam.fromNative(native.ref);
    } finally {
      calloc.free(native);
    }
  }

  /// Sets the parameter with the stable [id]; throws an
  /// [AudGraphException] for an unknown id or a value outside the
  /// parameter's range.
  void setParam(int id, double value, {int rampFrames = 0}) =>
      AudGraphException.check(
        bindings.aud_host_set_param(_pointer, id, value, rampFrames),
        'set parameter $id',
      );

  /// The value the host set last for the parameter with the stable [id].
  double getParam(int id) {
    final value = calloc<Float>();
    try {
      AudGraphException.check(
        bindings.aud_host_get_param(_pointer, id, value),
        'read parameter $id',
      );
      return value.value;
    } finally {
      calloc.free(value);
    }
  }

  /// The latency of the graph from its inputs to its outputs in frames.
  int get latency => bindings.aud_host_latency(_pointer);

  /// The tail of the graph in frames; [AudGraph.infiniteTail] when a node
  /// never ends.
  int get tail => bindings.aud_host_tail(_pointer);

  // ...........................................................................
  /// The assets of the loaded document.
  List<AudHostAsset> get assets {
    final native = calloc<bindings.AudHostAsset>();
    try {
      native.ref.struct_size = sizeOf<bindings.AudHostAsset>();
      return [
        for (var i = 0; i < bindings.aud_host_num_assets(_pointer); i++)
          if (bindings.aud_host_asset(_pointer, i, native) == AUD_OK)
            AudHostAsset.fromNative(native.ref),
      ];
    } finally {
      calloc.free(native);
    }
  }

  /// Relinks the asset [id] to [path]: the strings that name it reach their
  /// nodes again with the new path, and [save] writes it.
  void setAssetPath(String id, String path) {
    final nativeId = id.toNativeUtf8();
    final nativePath = path.toNativeUtf8();
    try {
      _check(
        bindings.aud_host_set_asset_path(
          _pointer,
          nativeId.cast(),
          nativePath.cast(),
        ),
        'relink asset $id',
      );
    } finally {
      calloc.free(nativePath);
      calloc.free(nativeId);
    }
  }

  /// Destroys the native host; the graph and its nodes stay.
  void dispose() {
    if (isDisposed) return;
    bindings.aud_host_destroy(_pointer);
    _pointer = nullptr;
  }

  // ...........................................................................
  late Pointer<bindings.AudHost> _pointer;

  // Throws for an error code with the message the host left.
  void _check(int result, String what) {
    if (result < 0) {
      throw AudGraphException(result, 'Could not $what: $lastError');
    }
  }

  // Reads a text the host writes into a buffer of the caller.
  String _text(
    int Function(Pointer<Char> buffer, int capacity, Pointer<Size> size) call,
    String what,
  ) {
    final size = calloc<Size>();
    Pointer<Char> buffer = nullptr;
    try {
      final probe = call(nullptr, 0, size);
      if (probe != AUD_ERROR_BUFFER_TOO_SMALL) _check(probe, what);
      buffer = calloc<Char>(size.value + 1);
      _check(call(buffer, size.value + 1, size), what);
      return buffer.cast<Utf8>().toDartString(length: size.value);
    } finally {
      calloc.free(size);
      if (buffer != nullptr) calloc.free(buffer);
    }
  }
}
