// @license
// Copyright (c) Audanika. All Rights Reserved.
//
// Use of this source code is governed by terms that can be
// found in the LICENSE file in the root of this package.

import 'dart:io';
import 'dart:typed_data';

// #############################################################################
/// The samples of a WAV file: planar channels at a sample rate.
class AudWavData {
  /// Creates the data.
  const AudWavData({required this.channels, required this.sampleRate});

  /// The channels, one list of samples each, all of the same length.
  final List<Float32List> channels;

  /// The sample rate in Hz.
  final double sampleRate;

  /// The frames per channel.
  int get frames => channels.isEmpty ? 0 : channels.first.length;
}

// #############################################################################
/// Reads and writes WAV files with 32-bit float samples (format 3), the
/// format of the golden renders; reading also takes 16-bit PCM (format 1).
abstract final class AudWavFile {
  /// The number of the IEEE float format.
  static const int floatFormat = 3;

  /// The number of the PCM format.
  static const int pcmFormat = 1;

  // ...........................................................................
  /// Encodes [data] as a float WAV file.
  static Uint8List encode(AudWavData data) {
    final channels = data.channels.length;
    final frames = data.frames;
    final dataBytes = frames * channels * 4;
    final bytes = ByteData(44 + dataBytes);
    void ascii(int offset, String text) {
      for (var i = 0; i < text.length; i++) {
        bytes.setUint8(offset + i, text.codeUnitAt(i));
      }
    }

    ascii(0, 'RIFF');
    bytes.setUint32(4, 36 + dataBytes, Endian.little);
    ascii(8, 'WAVE');
    ascii(12, 'fmt ');
    bytes.setUint32(16, 16, Endian.little);
    bytes.setUint16(20, floatFormat, Endian.little);
    bytes.setUint16(22, channels, Endian.little);
    bytes.setUint32(24, data.sampleRate.round(), Endian.little);
    bytes.setUint32(28, data.sampleRate.round() * channels * 4, Endian.little);
    bytes.setUint16(32, channels * 4, Endian.little);
    bytes.setUint16(34, 32, Endian.little);
    ascii(36, 'data');
    bytes.setUint32(40, dataBytes, Endian.little);
    var offset = 44;
    for (var i = 0; i < frames; i++) {
      for (var c = 0; c < channels; c++) {
        bytes.setFloat32(offset, data.channels[c][i], Endian.little);
        offset += 4;
      }
    }
    return bytes.buffer.asUint8List();
  }

  /// Decodes a WAV file in the float or the 16-bit PCM format; throws a
  /// [FormatException] for anything else.
  static AudWavData decode(Uint8List bytes) {
    final data = ByteData.sublistView(bytes);
    String ascii(int offset) =>
        String.fromCharCodes(bytes.sublist(offset, offset + 4));
    if (bytes.length < 12 || ascii(0) != 'RIFF' || ascii(8) != 'WAVE') {
      throw const FormatException('Not a WAV file');
    }
    var format = 0;
    var channels = 0;
    var sampleRate = 0;
    var bits = 0;
    var offset = 12;
    while (offset + 8 <= bytes.length) {
      final id = ascii(offset);
      final size = data.getUint32(offset + 4, Endian.little);
      final body = offset + 8;
      if (id == 'fmt ') {
        format = data.getUint16(body, Endian.little);
        channels = data.getUint16(body + 2, Endian.little);
        sampleRate = data.getUint32(body + 4, Endian.little);
        bits = data.getUint16(body + 14, Endian.little);
      } else if (id == 'data') {
        return _samples(data, body, size, format, channels, sampleRate, bits);
      }
      offset = body + size + (size & 1);
    }
    throw const FormatException('No data chunk');
  }

  static AudWavData _samples(
    ByteData data,
    int body,
    int size,
    int format,
    int channels,
    int sampleRate,
    int bits,
  ) {
    if (channels == 0 ||
        !((format == floatFormat && bits == 32) ||
            (format == pcmFormat && bits == 16))) {
      throw FormatException('Unsupported WAV format $format with $bits bits');
    }
    final bytesPerSample = bits ~/ 8;
    final frames = size ~/ (channels * bytesPerSample);
    final result = [for (var c = 0; c < channels; c++) Float32List(frames)];
    var offset = body;
    for (var i = 0; i < frames; i++) {
      for (var c = 0; c < channels; c++) {
        result[c][i] = format == floatFormat
            ? data.getFloat32(offset, Endian.little)
            : data.getInt16(offset, Endian.little) / 32768;
        offset += bytesPerSample;
      }
    }
    return AudWavData(channels: result, sampleRate: sampleRate.toDouble());
  }

  // ...........................................................................
  /// Writes [data] to the file at [path].
  static void write(String path, AudWavData data) =>
      File(path).writeAsBytesSync(encode(data));

  /// Reads the file at [path].
  static AudWavData read(String path) => decode(File(path).readAsBytesSync());
}
