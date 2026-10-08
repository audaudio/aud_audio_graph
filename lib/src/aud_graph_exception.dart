// @license
// Copyright (c) Audanika. All Rights Reserved.
//
// Use of this source code is governed by terms that can be
// found in the LICENSE file in the root of this package.

import 'package:aud_audio_core/aud_audio_core.dart';

// #############################################################################
/// Thrown when a call of the graph engine returns an error code of the ABI.
class AudGraphException implements Exception {
  /// Creates the exception for a result [code] and a [message].
  const AudGraphException(this.code, this.message);

  /// The result code, one of the `AUD_ERROR_*` constants.
  final int code;

  /// What failed and why.
  final String message;

  /// The name of the result code, e.g. `AUD_ERROR_CYCLE`.
  String get name => AudAbi.resultName(code);

  /// Returns [result] when it is no error code, otherwise throws the
  /// exception for it: "Could not [what]".
  static int check(int result, String what) {
    if (result < 0) {
      throw AudGraphException(
        result,
        'Could not $what: ${AudAbi.resultName(result)}',
      );
    }
    return result;
  }

  @override
  String toString() => 'AudGraphException($name): $message';
}
