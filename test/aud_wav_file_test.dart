// @license
// Copyright (c) Audanika. All Rights Reserved.
//
// Use of this source code is governed by terms that can be
// found in the LICENSE file in the root of this package.

import 'dart:io';
import 'dart:typed_data';

import 'package:aud_audio_graph/aud_audio_graph.dart';
import 'package:test/test.dart';

void main() {
  group('AudWavFile', () {
    final data = AudWavData(
      channels: [
        Float32List.fromList([0, 0.5, -0.5, 1]),
        Float32List.fromList([1, 0.25, -0.25, 0]),
      ],
      sampleRate: 48000,
    );

    test('encodes and decodes float samples', () {
      final bytes = AudWavFile.encode(data);
      expect(bytes.length, 44 + 4 * 2 * 4);
      expect(String.fromCharCodes(bytes.sublist(0, 4)), 'RIFF');
      final decoded = AudWavFile.decode(bytes);
      expect(decoded.sampleRate, 48000);
      expect(decoded.frames, 4);
      expect(decoded.channels[0], data.channels[0]);
      expect(decoded.channels[1], data.channels[1]);
      expect(const AudWavData(channels: [], sampleRate: 1).frames, 0);
    });

    test('decodes 16-bit PCM and skips other chunks', () {
      final bytes = ByteData(12 + 8 + 4 + 1 + 8 + 16 + 8 + 4);
      void ascii(int offset, String text) {
        for (var i = 0; i < text.length; i++) {
          bytes.setUint8(offset + i, text.codeUnitAt(i));
        }
      }

      ascii(0, 'RIFF');
      ascii(8, 'WAVE');
      ascii(12, 'LIST');
      bytes.setUint32(16, 3, Endian.little); // odd size, padded
      ascii(24, 'fmt ');
      bytes.setUint32(28, 16, Endian.little);
      bytes.setUint16(32, AudWavFile.pcmFormat, Endian.little);
      bytes.setUint16(34, 1, Endian.little);
      bytes.setUint32(36, 44100, Endian.little);
      bytes.setUint16(46, 16, Endian.little);
      ascii(48, 'data');
      bytes.setUint32(52, 4, Endian.little);
      bytes.setInt16(56, 16384, Endian.little);
      bytes.setInt16(58, -32768, Endian.little);
      final decoded = AudWavFile.decode(bytes.buffer.asUint8List());
      expect(decoded.sampleRate, 44100);
      expect(decoded.channels.single, [0.5, -1]);
    });

    test('refuses what is no WAV file it can read', () {
      expect(() => AudWavFile.decode(Uint8List(4)), throwsFormatException);
      final bytes = AudWavFile.encode(data);
      bytes.setAll(8, 'WAVX'.codeUnits);
      expect(() => AudWavFile.decode(bytes), throwsFormatException);
      final noData = AudWavFile.encode(data).sublist(0, 36);
      expect(() => AudWavFile.decode(noData), throwsFormatException);
      final odd = AudWavFile.encode(data);
      odd.buffer.asByteData().setUint16(34, 24, Endian.little);
      expect(() => AudWavFile.decode(odd), throwsFormatException);
    });

    test('writes and reads files', () {
      final directory = Directory.systemTemp.createTempSync('aud_wav');
      addTearDown(() => directory.deleteSync(recursive: true));
      final path = '${directory.path}/test.wav';
      AudWavFile.write(path, data);
      final read = AudWavFile.read(path);
      expect(read.channels[0], data.channels[0]);
    });
  });
}
