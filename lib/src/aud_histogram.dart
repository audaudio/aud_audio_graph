// @license
// Copyright (c) Audanika. All Rights Reserved.
//
// Use of this source code is governed by terms that can be
// found in the LICENSE file in the root of this package.

// #############################################################################
/// A histogram of times in bins of a fixed width, the last bin for what
/// lies beyond: the shape of the engine's histograms - the delivery delays
/// and input to output times of an event source, the render periods of
/// the timing - and the percentiles the bridges read from them.
class AudHistogram {
  /// Creates a histogram of [counts] in bins of [binNs] nanoseconds.
  const AudHistogram({required this.counts, required this.binNs});

  // ...........................................................................
  /// The time in nanoseconds below which [fraction] of the counts stayed -
  /// the upper edge of the bin -, null without counts; at least the edge
  /// of the last bin when the fraction lies in the overflow bin.
  ///
  /// - [fraction] between 0 and 1
  int? percentileNs(double fraction) {
    if (fraction < 0 || fraction > 1) {
      throw ArgumentError.value(fraction, 'fraction', 'Not between 0 and 1');
    }
    var total = 0;
    for (final count in counts) {
      total += count;
    }
    if (total == 0) return null;
    final wanted = fraction * total;
    var seen = 0;
    for (var bin = 0; bin < counts.length; bin++) {
      seen += counts[bin];
      if (seen > 0 && seen >= wanted) return (bin + 1) * binNs;
    }
    return counts.length * binNs; // coverage:ignore-line
  }

  // ...........................................................................
  /// The counts per bin.
  final List<int> counts;

  /// The width of a bin in nanoseconds.
  final int binNs;
}
