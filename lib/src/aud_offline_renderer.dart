// @license
// Copyright (c) Audanika. All Rights Reserved.
//
// Use of this source code is governed by terms that can be
// found in the LICENSE file in the root of this package.

import 'dart:ffi';
import 'dart:typed_data';

import 'package:aud_audio_core/aud_audio_core_bindings.dart' as core;
import 'package:ffi/ffi.dart';

import 'aud_audio_graph_bindings_generated.dart' as bindings;
import 'aud_graph.dart';
import 'aud_graph_exception.dart';
import 'aud_wav_file.dart';

// #############################################################################
/// Renders a running graph offline on the calling thread (plan of ticket
/// 17, S2): a virtual timeline, any sequence of block sizes, optional input
/// signals; the result is deterministic, so the golden tests render with
/// it. Events are scheduled before the render through the graph.
class AudOfflineRenderer {
  /// Creates a renderer for [graph].
  const AudOfflineRenderer(this.graph);

  /// The graph; it has to be running.
  final AudGraph graph;

  // ...........................................................................
  /// Renders [frames] frames and returns the output buses, one list of
  /// channels each.
  ///
  /// - [blockFrames] the block size; 0 takes the largest block
  /// - [blockSizes] a cycle of block sizes that overrides [blockFrames]
  /// - [inputs] the input buses, one list of channels each, [frames] long;
  ///   null renders silence into every input
  /// - [startSamplePosition] the sample position of the first frame; by
  ///   default the timeline continues where the graph stands
  /// - [startHostTimeNs] the host time of the first frame; by default the
  ///   sample position converted at the sample rate
  List<List<Float32List>> render({
    required int frames,
    int blockFrames = 0,
    List<int> blockSizes = const [],
    List<List<Float32List>>? inputs,
    int? startSamplePosition,
    int? startHostTimeNs,
  }) {
    final inputChannels = graph.inputChannels;
    final outputChannels = graph.outputChannels;
    final position = startSamplePosition ?? graph.samplePosition;
    final hostTime =
        startHostTimeNs ?? (position * 1e9 / graph.sampleRate).round();
    if (inputs != null) _checkInputs(inputs, inputChannels, frames);
    final allocations = <Pointer<NativeType>>[];
    Pointer<Float> channel() {
      final pointer = calloc<Float>(frames == 0 ? 1 : frames);
      allocations.add(pointer);
      return pointer;
    }

    Pointer<core.AudAudioBus> buses(List<int> channels, bool input) {
      final pointer = calloc<core.AudAudioBus>(
        channels.isEmpty ? 1 : channels.length,
      );
      allocations.add(pointer);
      for (var b = 0; b < channels.length; b++) {
        final pointers = calloc<Pointer<Float>>(channels[b]);
        allocations.add(pointers);
        for (var c = 0; c < channels[b]; c++) {
          pointers[c] = channel();
          if (input && inputs != null) {
            pointers[c].asTypedList(frames).setAll(0, inputs[b][c]);
          }
        }
        pointer[b]
          ..struct_size = sizeOf<core.AudAudioBus>()
          ..num_channels = channels[b]
          ..channels = pointers;
      }
      return pointer;
    }

    try {
      final inputBuses = buses(inputChannels, true);
      final outputBuses = buses(outputChannels, false);
      final sizes = calloc<Uint32>(blockSizes.isEmpty ? 1 : blockSizes.length);
      allocations.add(sizes);
      sizes
          .asTypedList(blockSizes.isEmpty ? 1 : blockSizes.length)
          .setAll(0, blockSizes.isEmpty ? const [0] : blockSizes);
      final request = calloc<bindings.AudOfflineRequest>();
      allocations.add(request);
      request.ref
        ..struct_size = sizeOf<bindings.AudOfflineRequest>()
        ..frames = frames
        ..block_frames = blockFrames
        ..num_block_sizes = blockSizes.length
        ..block_sizes = sizes
        ..inputs = inputBuses
        ..outputs = outputBuses
        ..start_sample_position = position
        ..start_host_time_ns = hostTime;
      AudGraphException.check(
        bindings.aud_graph_render_offline(graph.pointer, request),
        'render $frames frames offline',
      );
      return [
        for (var b = 0; b < outputChannels.length; b++)
          [
            for (var c = 0; c < outputChannels[b]; c++)
              Float32List.fromList(
                outputBuses[b].channels[c].asTypedList(frames),
              ),
          ],
      ];
    } finally {
      for (final pointer in allocations) {
        calloc.free(pointer);
      }
    }
  }

  /// Renders [frames] frames and returns the first output bus as WAV data
  /// at the graph's sample rate; see [render] for the parameters.
  AudWavData renderWav({
    required int frames,
    int blockFrames = 0,
    List<int> blockSizes = const [],
    List<List<Float32List>>? inputs,
  }) {
    final output = render(
      frames: frames,
      blockFrames: blockFrames,
      blockSizes: blockSizes,
      inputs: inputs,
    );
    return AudWavData(
      channels: output.isEmpty ? const [] : output.first,
      sampleRate: graph.sampleRate,
    );
  }

  static void _checkInputs(
    List<List<Float32List>> inputs,
    List<int> inputChannels,
    int frames,
  ) {
    if (inputs.length != inputChannels.length) {
      throw ArgumentError.value(
        inputs.length,
        'inputs',
        'The graph has ${inputChannels.length} input buses',
      );
    }
    for (var b = 0; b < inputs.length; b++) {
      if (inputs[b].length != inputChannels[b] ||
          inputs[b].any((c) => c.length != frames)) {
        throw ArgumentError.value(
          inputs[b],
          'inputs',
          'Bus $b needs ${inputChannels[b]} channels of $frames frames',
        );
      }
    }
  }
}
