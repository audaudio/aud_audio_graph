// @license
// Copyright (c) Audanika. All Rights Reserved.
//
// Use of this source code is governed by terms that can be
// found in the LICENSE file in the root of this package.
import 'aud_graph.dart';
import 'aud_graph_document.dart';
import 'aud_graph_ffi.dart';
import 'aud_graph_options.dart';

/// A native [AudGraph]; see its constructor.
AudGraph createGraph({
  required double sampleRate,
  required int maxFrames,
  required List<int> inputChannels,
  required List<int> outputChannels,
  required AudGraphOptions options,
  required int id,
  required bool registerCoreNodes,
  required bool listen,
}) => AudGraphFfi(
  sampleRate: sampleRate,
  maxFrames: maxFrames,
  inputChannels: inputChannels,
  outputChannels: outputChannels,
  options: options,
  id: id,
  registerCoreNodes: registerCoreNodes,
  listen: listen,
);

/// A native [AudGraph] from [document]; see `AudGraph.fromDocument`.
AudGraph createGraphFromDocument(
  AudGraphDocument document, {
  required double sampleRate,
  required int maxFrames,
  required AudGraphOptions options,
  required int id,
  required bool listen,
  required String? baseDirectory,
}) => AudGraphFfi.fromDocument(
  document,
  sampleRate: sampleRate,
  maxFrames: maxFrames,
  options: options,
  id: id,
  listen: listen,
  baseDirectory: baseDirectory,
);
