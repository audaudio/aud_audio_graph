// @license
// Copyright (c) Audanika. All Rights Reserved.
//
// Use of this source code is governed by terms that can be
// found in the LICENSE file in the root of this package.
import 'aud_graph.dart';
import 'aud_graph_document.dart';
import 'aud_graph_options.dart';

Never _unsupported() =>
    throw UnsupportedError('The graph needs the native engine (web: S5)');

/// The graph where the native engine is not available.
AudGraph createGraph({
  required double sampleRate,
  required int maxFrames,
  required List<int> inputChannels,
  required List<int> outputChannels,
  required AudGraphOptions options,
  required int id,
  required bool registerCoreNodes,
  required bool listen,
}) => _unsupported();

/// The graph from a document where the native engine is not available.
AudGraph createGraphFromDocument(
  AudGraphDocument document, {
  required double sampleRate,
  required int maxFrames,
  required AudGraphOptions options,
  required int id,
  required bool listen,
  required String? baseDirectory,
}) => _unsupported();
