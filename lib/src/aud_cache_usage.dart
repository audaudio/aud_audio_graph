// @license
// Copyright (c) Audanika. All Rights Reserved.
//
// Use of this source code is governed by terms that can be
// found in the LICENSE file in the root of this package.

// #############################################################################
/// The resources of the process (lifecycle-001, ticket 25): the caches the
/// packages registered, the sample cache budget they share, the trims and
/// the worker threads.
class AudCacheUsage {
  /// Creates the usage.
  const AudCacheUsage({
    this.caches = 0,
    this.budget = 0,
    this.bytes = 0,
    this.trims = 0,
    this.trimmed = 0,
    this.workers = 0,
    this.tasks = 0,
  });

  /// The registered caches.
  final int caches;

  /// The sample cache budget of the process in bytes.
  final int budget;

  /// The bytes the caches hold together.
  final int bytes;

  /// The trims so far.
  final int trims;

  /// The bytes the trims freed so far.
  final int trimmed;

  /// The worker threads that run.
  final int workers;

  /// The tasks queued or running.
  final int tasks;

  /// The share of the budget the caches hold, 0 to 1 and beyond.
  double get load => budget == 0 ? 0 : bytes / budget;

  /// The usage as JSON.
  Map<String, Object?> toJson() => {
    'caches': caches,
    'budget': budget,
    'bytes': bytes,
    'trims': trims,
    'trimmed': trimmed,
    'workers': workers,
    'tasks': tasks,
  };

  @override
  bool operator ==(Object other) =>
      other is AudCacheUsage &&
      other.caches == caches &&
      other.budget == budget &&
      other.bytes == bytes &&
      other.trims == trims &&
      other.trimmed == trimmed &&
      other.workers == workers &&
      other.tasks == tasks;

  @override
  int get hashCode =>
      Object.hash(caches, budget, bytes, trims, trimmed, workers, tasks);

  @override
  String toString() => 'AudCacheUsage(${toJson()})';
}
