// @license
// Copyright (c) Audanika. All Rights Reserved.
//
// Use of this source code is governed by terms that can be
// found in the LICENSE file in the root of this package.

import 'dart:ffi';

import 'package:aud_audio_core/aud_audio_core_ffi.dart';
import 'package:aud_audio_graph/aud_audio_graph_ffi.dart';
import 'package:aud_audio_graph/src/aud_audio_graph_bindings_generated.dart'
    as bindings;
import 'package:ffi/ffi.dart';
import 'package:test/test.dart';

void main() {
  group('aud_audio_graph_bindings_generated.dart', () {
    test('null graphs are tolerated by the C API', () {
      final stats = calloc<bindings.AudGraphStats>();
      final notification = calloc<bindings.AudGraphNotification>();
      try {
        expect(bindings.aud_graph_create(nullptr), nullptr);
        expect(bindings.aud_graph_host_api(nullptr), nullptr);
        expect(bindings.aud_graph_num_node_types(nullptr), 0);
        expect(bindings.aud_graph_node_type(nullptr, 0), nullptr);
        expect(bindings.aud_graph_node_type_by_id(nullptr, nullptr), nullptr);
        expect(bindings.aud_graph_node_descriptor(nullptr, 1), nullptr);
        expect(
          bindings.aud_graph_create_node(nullptr, nullptr, nullptr),
          lessThan(0),
        );
        expect(bindings.aud_graph_nodes(nullptr, nullptr, 0), lessThan(0));
        expect(bindings.aud_graph_node_channels(nullptr, 1, 0, 0), lessThan(0));
        expect(bindings.aud_graph_node_latency(nullptr, 1), lessThan(0));
        expect(bindings.aud_graph_node_lead(nullptr, 1), lessThan(0));
        expect(bindings.aud_graph_output_latency(nullptr), lessThan(0));
        expect(bindings.aud_graph_begin(nullptr), lessThan(0));
        expect(bindings.aud_graph_connect(nullptr, 0, 0, 0, 0, 0), lessThan(0));
        expect(bindings.aud_graph_disconnect(nullptr, 0, 0, 0, 0), lessThan(0));
        expect(
          bindings.aud_graph_connect_events(nullptr, 0, 0, 0, 0),
          lessThan(0),
        );
        expect(
          bindings.aud_graph_disconnect_events(nullptr, 0, 0, 0, 0),
          lessThan(0),
        );
        expect(bindings.aud_graph_remove_node(nullptr, 1), lessThan(0));
        expect(bindings.aud_graph_commit(nullptr), lessThan(0));
        expect(bindings.aud_graph_rollback(nullptr), lessThan(0));
        expect(bindings.aud_graph_revision(nullptr), 0);
        expect(bindings.aud_graph_set_param(nullptr, 1, 0, 0, 0), lessThan(0));
        expect(
          bindings.aud_graph_send_event(nullptr, 1, nullptr, nullptr, 0),
          lessThan(0),
        );
        expect(bindings.aud_graph_cancel(nullptr, 0, 0), lessThan(0));
        expect(
          bindings.aud_graph_set_string(nullptr, 1, 0, nullptr),
          lessThan(0),
        );
        expect(bindings.aud_graph_transport(nullptr, nullptr), lessThan(0));
        expect(
          bindings.aud_graph_transport_state(nullptr, nullptr),
          lessThan(0),
        );
        expect(
          bindings.aud_graph_set_transport_provider(nullptr, nullptr),
          lessThan(0),
        );
        expect(bindings.aud_graph_sample_position(nullptr), 0);
        expect(bindings.aud_graph_prepare(nullptr, 0, 0), lessThan(0));
        expect(bindings.aud_graph_start(nullptr), lessThan(0));
        expect(bindings.aud_graph_suspend(nullptr), lessThan(0));
        expect(bindings.aud_graph_resume(nullptr), lessThan(0));
        expect(bindings.aud_graph_stop(nullptr), lessThan(0));
        expect(bindings.aud_graph_state(nullptr), lessThan(0));
        expect(bindings.aud_graph_sample_rate(nullptr), 0);
        expect(bindings.aud_graph_max_frames(nullptr), 0);
        expect(bindings.aud_graph_render(nullptr, nullptr), lessThan(0));
        expect(
          bindings.aud_graph_render_offline(nullptr, nullptr),
          lessThan(0),
        );
        expect(
          bindings.aud_graph_set_listener(nullptr, nullptr, nullptr),
          lessThan(0),
        );
        expect(
          bindings.aud_graph_take_notifications(nullptr, notification, 1),
          lessThan(0),
        );
        expect(
          bindings.aud_graph_tap_read(nullptr, 1, 0, nullptr, 0),
          lessThan(0),
        );
        expect(
          bindings.aud_graph_tap_meter(nullptr, 1, 0, nullptr, nullptr),
          lessThan(0),
        );
        expect(bindings.aud_graph_get_stats(nullptr, stats), lessThan(0));
        bindings.aud_graph_reset_stats(nullptr);
        bindings.aud_graph_destroy(nullptr);
        // The calls of ticket 20.
        expect(bindings.aud_graph_output_tail(nullptr), 0);
        expect(
          bindings.aud_graph_node_save_state(nullptr, 1, nullptr, 0, nullptr),
          lessThan(0),
        );
        expect(
          bindings.aud_graph_node_load_state(nullptr, 1, nullptr, 0, 0),
          lessThan(0),
        );
        expect(bindings.aud_graph_render_host(nullptr, nullptr), lessThan(0));
        expect(bindings.aud_host_create(nullptr, nullptr), nullptr);
        expect(bindings.aud_host_graph(nullptr), nullptr);
        expect(bindings.aud_host_load(nullptr, nullptr, 0), lessThan(0));
        expect(
          bindings.aud_host_save(nullptr, nullptr, 0, nullptr),
          lessThan(0),
        );
        expect(bindings.aud_host_num_nodes(nullptr), lessThan(0));
        expect(bindings.aud_host_num_params(nullptr), lessThan(0));
        expect(bindings.aud_host_num_assets(nullptr), lessThan(0));
        expect(bindings.aud_host_latency(nullptr), lessThan(0));
        expect(bindings.aud_host_tail(nullptr), 0);
        expect(bindings.aud_host_render(nullptr, nullptr), lessThan(0));
        expect(
          bindings.aud_host_last_error(nullptr).cast<Utf8>().toDartString(),
          isEmpty,
        );
        bindings.aud_host_destroy(nullptr);
      } finally {
        calloc.free(stats);
        calloc.free(notification);
      }
    });

    test('the C API refuses short structs and bad arguments', () {
      final graph = AudGraphFfi(listen: false);
      addTearDown(graph.dispose);
      final stats = calloc<bindings.AudGraphStats>();
      final state = calloc<bindings.AudGraphTransportState>();
      final request = calloc<bindings.AudOfflineRequest>();
      try {
        expect(
          bindings.aud_graph_get_stats(graph.pointer, stats),
          AUD_ERROR_INVALID_ARGUMENT,
        );
        expect(
          bindings.aud_graph_transport_state(graph.pointer, state),
          AUD_ERROR_INVALID_ARGUMENT,
        );
        expect(
          bindings.aud_graph_render_offline(graph.pointer, request),
          AUD_ERROR_INVALID_ARGUMENT,
        );
        expect(
          bindings.aud_graph_take_notifications(graph.pointer, nullptr, 1),
          lessThan(0),
        );
        expect(
          bindings.aud_graph_nodes(graph.pointer, nullptr, 1),
          lessThan(0),
        );
        expect(
          bindings.aud_graph_prepare(graph.pointer, -1, 0),
          AUD_ERROR_INVALID_ARGUMENT,
        );
        expect(
          bindings.aud_graph_set_transport_provider(graph.pointer, nullptr),
          AUD_OK,
        );
      } finally {
        calloc.free(stats);
        calloc.free(state);
        calloc.free(request);
      }
    });
  });
}
