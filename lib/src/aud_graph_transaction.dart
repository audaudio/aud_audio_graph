// @license
// Copyright (c) Audanika. All Rights Reserved.
//
// Use of this source code is governed by terms that can be
// found in the LICENSE file in the root of this package.

import 'aud_audio_graph_bindings_generated.dart' as bindings;
import 'aud_graph.dart';
import 'aud_graph_exception.dart';
import 'aud_graph_node.dart';

// #############################################################################
/// An audio connection between two buses, as the graph records it.
typedef AudConnection = ({
  int from,
  int fromBus,
  int to,
  int toBus,
  bool lowLatency,
});

/// An event connection between two ports, as the graph records it.
typedef AudEventConnection = ({int from, int fromPort, int to, int toPort});

/// The kinds of edits of a transaction.
enum AudEditKind {
  /// An audio connection was added.
  connect,

  /// An audio connection was removed.
  disconnect,

  /// An event connection was added.
  connectEvents,

  /// An event connection was removed.
  disconnectEvents,

  /// A node was removed.
  remove,
}

/// One edit of a transaction in the order it was made: [what] is an
/// [AudConnection], an [AudEventConnection] or an [AudNode].
typedef AudEdit = ({AudEditKind kind, Object what});

// #############################################################################
/// The edits of one transaction (graph-003): `AudGraph.transaction` opens
/// it, hands it to the edits and commits it; every edit goes to the engine
/// at once and is checked there, the commit compiles them into one
/// program.
class AudGraphTransaction {
  /// Creates the transaction of [graph]; `AudGraph.transaction` does this.
  AudGraphTransaction(this.graph);

  /// The graph.
  final AudGraph graph;

  /// The audio connections added.
  final List<AudConnection> connected = [];

  /// The audio connections removed.
  final List<AudConnection> disconnected = [];

  /// The event connections added.
  final List<AudEventConnection> eventsConnected = [];

  /// The event connections removed.
  final List<AudEventConnection> eventsDisconnected = [];

  /// The nodes removed.
  final List<AudNode> removed = [];

  /// Every edit in the order it was made.
  final List<AudEdit> edits = [];

  // ...........................................................................
  /// Connects an output bus of [from] to an input bus of [to]; the graph
  /// itself is `graph.io`. With [lowLatency] the connection opts out of
  /// the latency alignment at its destination (graph-001).
  void connect(
    AudNode from,
    AudNode to, {
    int fromBus = 0,
    int toBus = 0,
    bool lowLatency = false,
  }) {
    AudGraphException.check(
      bindings.aud_graph_connect(
        graph.pointer,
        from.handle,
        fromBus,
        to.handle,
        toBus,
        lowLatency ? bindings.AUD_CONNECTION_LOW_LATENCY : 0,
      ),
      'connect ${from.name}:$fromBus to ${to.name}:$toBus',
    );
    final connection = (
      from: from.handle,
      fromBus: fromBus,
      to: to.handle,
      toBus: toBus,
      lowLatency: lowLatency,
    );
    connected.add(connection);
    edits.add((kind: AudEditKind.connect, what: connection));
  }

  /// Removes an audio connection.
  void disconnect(AudNode from, AudNode to, {int fromBus = 0, int toBus = 0}) {
    AudGraphException.check(
      bindings.aud_graph_disconnect(
        graph.pointer,
        from.handle,
        fromBus,
        to.handle,
        toBus,
      ),
      'disconnect ${from.name}:$fromBus from ${to.name}:$toBus',
    );
    final connection = (
      from: from.handle,
      fromBus: fromBus,
      to: to.handle,
      toBus: toBus,
      lowLatency: false,
    );
    disconnected.add(connection);
    edits.add((kind: AudEditKind.disconnect, what: connection));
  }

  /// Connects an event output of [from] to an event input of [to].
  void connectEvents(
    AudNode from,
    AudNode to, {
    int fromPort = 0,
    int toPort = 0,
  }) {
    AudGraphException.check(
      bindings.aud_graph_connect_events(
        graph.pointer,
        from.handle,
        fromPort,
        to.handle,
        toPort,
      ),
      'connect events of ${from.name}:$fromPort to ${to.name}:$toPort',
    );
    final connection = (
      from: from.handle,
      fromPort: fromPort,
      to: to.handle,
      toPort: toPort,
    );
    eventsConnected.add(connection);
    edits.add((kind: AudEditKind.connectEvents, what: connection));
  }

  /// Removes an event connection.
  void disconnectEvents(
    AudNode from,
    AudNode to, {
    int fromPort = 0,
    int toPort = 0,
  }) {
    AudGraphException.check(
      bindings.aud_graph_disconnect_events(
        graph.pointer,
        from.handle,
        fromPort,
        to.handle,
        toPort,
      ),
      'disconnect events of ${from.name}:$fromPort from ${to.name}:$toPort',
    );
    final connection = (
      from: from.handle,
      fromPort: fromPort,
      to: to.handle,
      toPort: toPort,
    );
    eventsDisconnected.add(connection);
    edits.add((kind: AudEditKind.disconnectEvents, what: connection));
  }

  /// Removes [node] with its connections; the instance retires at the
  /// commit and is freed once its fade or tail has passed.
  void remove(AudNode node) {
    AudGraphException.check(
      bindings.aud_graph_remove_node(graph.pointer, node.handle),
      'remove ${node.name}',
    );
    removed.add(node);
    edits.add((kind: AudEditKind.remove, what: node));
  }
}
