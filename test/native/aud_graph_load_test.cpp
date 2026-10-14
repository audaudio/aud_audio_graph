// @license
// Copyright (c) Audanika. All Rights Reserved.
//
// Use of this source code is governed by terms that can be
// found in the LICENSE file in the root of this package.

// The render load of ticket 29: the gauge on its own - the shares of a
// known sequence, the weighting by frames, the window by frames at the
// sample rate, the ring that wraps, the stop at a slot the writer
// overwrote, the frames of a huge block - and in the graph: live renders
// only, untouched by a reset of the counters, the overloads of a slow
// node, a reader while the audio thread renders.

#include <atomic>
#include <thread>
#include <vector>

#include "aud_audio_graph.h"
#include "aud_clock.h"
#include "aud_graph_fixture.hpp"
#include "aud_graph_load.hpp"
#include "aud_test.hpp"

using namespace aud_test_fixture;

namespace {

AudGraphLoad readGauge(const aud::LoadGauge& gauge, int64_t windowNs,
                       uint32_t bufferFrames = 0) {
  AudGraphLoad load{};
  load.struct_size = sizeof(AudGraphLoad);
  gauge.read(windowNs, bufferFrames, &load);
  return load;
}

AudGraphLoad renderLoadOf(Fixture& f, int64_t windowNs,
                          int32_t* result = nullptr) {
  AudGraphLoad load{};
  load.struct_size = sizeof(AudGraphLoad);
  const int32_t r = aud_graph_render_load(f.graph, windowNs, 0, &load);
  if (result != nullptr) *result = r;
  return load;
}

// ............................................................................
// aud.test.burner: spins on the audio thread for g_burnNs per block, then
// renders silence - a render that takes at least that long.

std::atomic<int64_t> g_burnNs{0};

void burnerProcess(void*, const AudProcessContext* context) {
  const int64_t burn = g_burnNs.load(std::memory_order_relaxed);
  const int64_t until = aud_clock_now_ns() + burn;
  while (burn > 0 && aud_clock_now_ns() < until) {
  }
  silentProcess(nullptr, context);
}

const AudNodeVTable kBurnerVTable = {
    sizeof(AudNodeVTable), fixedCreate,   fixedDestroy,  delayPrepare,
    delayReset,            delaySetParam, burnerProcess, nullptr,
    nullptr,               nullptr,       nullptr,       nullptr,
};

const AudNodeDescriptor kBurnerDescriptor = {
    sizeof(AudNodeDescriptor),
    AUD_ABI_VERSION_MAJOR,
    AUD_ABI_VERSION_MINOR,
    1,
    "aud.test.burner",
    "Burner",
    "Test",
    AUD_NODE_CAP_VARIABLE_BLOCK,
    0,
    0,
    1,
    nullptr,
    kMonoOut,
    0,
    0,
    nullptr,
    nullptr,
    0,
    0,
    nullptr,
    nullptr,
    &kBurnerVTable,
};

}  // namespace

// ............................................................................
// The gauge

AUD_TEST(load_gauge_reads_zeros_without_renders) {
  aud::LoadGauge gauge;
  for (const int64_t window : {int64_t{0}, int64_t{1000000000}}) {
    const AudGraphLoad load = readGauge(gauge, window);
    AUD_CHECK(load.renders == 0);
    AUD_CHECK(load.frames == 0);
    AUD_CHECK(load.window_ns == 0);
    AUD_CHECK(load.mean == 0);
    AUD_CHECK(load.p99 == 0);
    AUD_CHECK(load.max == 0);
    AUD_CHECK(load.renders_total == 0);
    AUD_CHECK(load.overloads_total == 0);
  }
}

AUD_TEST(load_gauge_reads_mean_p99_and_max_of_known_shares) {
  aud::LoadGauge gauge;
  // Shares 0.01 to 1.00 in blocks of 1 ms: a render as long as its block
  // is no overload.
  for (int64_t i = 1; i <= 100; ++i) {
    gauge.record(i * 10000, 1000000, 48, kRate);
  }
  AudGraphLoad load = readGauge(gauge, 0);
  AUD_CHECK(load.renders == 100);
  AUD_CHECK(load.frames == 4800);
  AUD_CHECK(load.window_ns == 100000000);
  AUD_CHECK_NEAR(load.mean, 0.505, 1e-6);
  AUD_CHECK_NEAR(load.p99, 0.99, 1e-6);  // the 99th of 100
  AUD_CHECK_NEAR(load.max, 1.0, 1e-6);
  AUD_CHECK(load.renders_total == 100);
  AUD_CHECK(load.overloads_total == 0);
  // One render of one and a half blocks.
  gauge.record(1500000, 1000000, 48, kRate);
  load = readGauge(gauge, 0);
  AUD_CHECK(load.renders == 101);
  AUD_CHECK_NEAR(load.p99, 1.0, 1e-6);  // the 100th of 101
  AUD_CHECK_NEAR(load.max, 1.5, 1e-6);
  AUD_CHECK(load.renders_total == 101);
  AUD_CHECK(load.overloads_total == 1);
}

AUD_TEST(load_gauge_takes_the_p99_by_nearest_rank_in_any_order) {
  aud::LoadGauge gauge;
  // 200 renders: 197 of 0.1 and the shares 3, 1 and 2 among them; the
  // 198th of 200 in ascending order is 1.
  for (int64_t i = 0; i < 200; ++i) {
    int64_t elapsed = 100;
    if (i == 20) elapsed = 3000;
    if (i == 90) elapsed = 1000;
    if (i == 150) elapsed = 2000;
    gauge.record(elapsed, 1000, 48, kRate);
  }
  const AudGraphLoad load = readGauge(gauge, 0);
  AUD_CHECK_NEAR(load.p99, 1.0, 1e-6);
  AUD_CHECK_NEAR(load.max, 3.0, 1e-6);
  AUD_CHECK(load.overloads_total == 2);
  // A single render is its own percentile.
  aud::LoadGauge single;
  single.record(250, 1000, 48, kRate);
  AUD_CHECK_NEAR(readGauge(single, 0).p99, 0.25, 1e-6);
}

AUD_TEST(load_gauge_weights_the_mean_by_the_frames) {
  aud::LoadGauge gauge;
  gauge.record(500, 1000, 100, kRate);   // half of a short block
  gauge.record(1000, 1000, 300, kRate);  // all of a long one
  const AudGraphLoad load = readGauge(gauge, 0);
  AUD_CHECK(load.frames == 400);
  AUD_CHECK_NEAR(load.mean, (0.5 * 100 + 1.0 * 300) / 400, 1e-9);
  AUD_CHECK_NEAR(load.max, 1.0, 1e-9);
}

AUD_TEST(load_gauge_window_covers_the_frames_at_the_rate) {
  aud::LoadGauge gauge;
  // 200 blocks of 10 ms.
  for (int i = 0; i < 200; ++i) gauge.record(1000000, 10000000, 480, kRate);
  AudGraphLoad load = readGauge(gauge, 100000000);
  AUD_CHECK(load.renders == 10);
  AUD_CHECK(load.frames == 4800);
  AUD_CHECK(load.window_ns == 100000000);
  AUD_CHECK_NEAR(load.mean, 0.1, 1e-6);
  AUD_CHECK(load.renders_total == 200);
  // The render that reaches into the window counts whole.
  load = readGauge(gauge, 105000000);
  AUD_CHECK(load.renders == 11);
  AUD_CHECK(load.window_ns == 110000000);
  AUD_CHECK(readGauge(gauge, 1).renders == 1);
  // No window, or one longer than the renders: all of them.
  for (const int64_t window :
       {int64_t{0}, int64_t{-5}, int64_t{10000000000}}) {
    load = readGauge(gauge, window);
    AUD_CHECK(load.renders == 200);
    AUD_CHECK(load.window_ns == 2000000000);
  }
  // The window counts at the rate of the newest render.
  gauge.record(1000000, 10000000, 480, 2 * kRate);
  AUD_CHECK(readGauge(gauge, 100000000).renders == 20);
}

AUD_TEST(load_gauge_keeps_the_last_1024_renders) {
  aud::LoadGauge gauge;
  // Render i takes i ns of a block of 1000 ns: the last 1024 are 76 to
  // 1099, the overloads 1001 to 1099.
  for (int64_t i = 0; i < 1100; ++i) gauge.record(i, 1000, 48, kRate);
  const AudGraphLoad load = readGauge(gauge, 0);
  AUD_CHECK(load.renders == AUD_GRAPH_LOAD_RENDERS);
  AUD_CHECK(load.frames == AUD_GRAPH_LOAD_RENDERS * 48);
  AUD_CHECK_NEAR(load.mean, (76 + 1099) / 2.0 / 1000, 1e-6);
  AUD_CHECK_NEAR(load.p99, (76 + 1013) / 1000.0, 1e-6);  // the 1014th
  AUD_CHECK_NEAR(load.max, 1.099, 1e-6);
  AUD_CHECK(load.renders_total == 1100);
  AUD_CHECK(load.overloads_total == 99);
}

AUD_TEST(load_gauge_stops_at_a_slot_the_writer_overwrote) {
  aud::LoadGauge gauge;
  for (int64_t i = 0; i < 1100; ++i) gauge.record(i, 1000, 48, kRate);
  // A reader that found 1050 renders while the writer went on to 1100:
  // the renders 26 to 75 are gone, 76 to 1049 are there.
  AudGraphLoad load{};
  load.struct_size = sizeof(AudGraphLoad);
  gauge.readFrom(1050, 49, 0, 0, &load);
  AUD_CHECK(load.renders == 1049 - 76 + 1);
  AUD_CHECK_NEAR(load.max, 1.049, 1e-6);
  AUD_CHECK(load.renders_total == 1050);
  AUD_CHECK(load.overloads_total == 49);
}

AUD_TEST(load_gauge_reads_the_runs_that_fill_the_device_buffer) {
  aud::LoadGauge gauge;
  // Blocks of 100 frames that take 0.5, 1.5, 0.5, 0.5, 0.5, 0.9, 0.9 of
  // their time, oldest first; a buffer of two blocks.
  for (const int64_t ns : {500, 1500, 500, 500, 500, 900, 900}) {
    gauge.record(ns, 1000, 100, kRate);
  }
  AudGraphLoad load = readGauge(gauge, 0, 200);
  AUD_CHECK(load.buffer_frames == 200);
  // One render alone is late twice, no run of two blocks is: the newest
  // runs are 0.9, 0.7, 0.5, 0.5, 1.0, 1.0.
  AUD_CHECK_NEAR(load.max, 1.5, 1e-6);
  AUD_CHECK(load.buffer_overloads == 0);
  AUD_CHECK_NEAR(load.buffer_max, 1.0, 1e-6);
  AUD_CHECK_NEAR(load.buffer_p99, 1.0, 1e-6);
  // A run that would reach past the window does not count: the window of
  // the newest three renders holds the runs 0.9 and 0.7.
  load = readGauge(gauge, static_cast<int64_t>(300 * 1e9 / kRate), 200);
  AUD_CHECK(load.renders == 3);
  AUD_CHECK_NEAR(load.buffer_max, 0.9, 1e-6);
  // A buffer of a block or less: every render is a run.
  for (const uint32_t frames : {0u, 50u, 100u}) {
    load = readGauge(gauge, 0, frames);
    AUD_CHECK_NEAR(load.buffer_max, 1.5, 1e-6);
    AUD_CHECK(load.buffer_overloads == 1);
    AUD_CHECK_NEAR(load.buffer_p99, load.p99, 1e-6);
  }
  // Two late blocks in a row overload a buffer of two.
  gauge.record(1200, 1000, 100, kRate);
  gauge.record(1200, 1000, 100, kRate);
  load = readGauge(gauge, 0, 200);
  AUD_CHECK(load.buffer_overloads == 2);
  AUD_CHECK_NEAR(load.buffer_max, 1.2, 1e-6);
  // A buffer longer than the window: no run.
  load = readGauge(gauge, 0, 100000);
  AUD_CHECK(load.buffer_overloads == 0);
  AUD_CHECK(load.buffer_p99 == 0);
  AUD_CHECK(load.buffer_max == 0);
}

AUD_TEST(load_gauge_counts_a_huge_block_as_65535_frames) {
  aud::LoadGauge gauge;
  gauge.record(1000, 2000, 100000, kRate);
  const AudGraphLoad load = readGauge(gauge, 0);
  AUD_CHECK(load.frames == 65535);
  AUD_CHECK_NEAR(load.mean, 0.5, 1e-9);
}

// ............................................................................
// The gauge of the graph

AUD_TEST(graph_render_load_refuses_bad_arguments) {
  Fixture f;
  AudGraphLoad load{};
  load.struct_size = sizeof(AudGraphLoad);
  AUD_CHECK(aud_graph_render_load(nullptr, 0, 0, &load) ==
            AUD_ERROR_INVALID_ARGUMENT);
  AUD_CHECK(aud_graph_render_load(f.graph, 0, 0, nullptr) ==
            AUD_ERROR_INVALID_ARGUMENT);
  load.struct_size = sizeof(AudGraphLoad) - 1;
  AUD_CHECK(aud_graph_render_load(f.graph, 0, 0, &load) ==
            AUD_ERROR_INVALID_ARGUMENT);
  // Before a render: zeros.
  int32_t result = AUD_ERROR_STATE;
  load = renderLoadOf(f, 1000000000, &result);
  AUD_CHECK(result == AUD_OK);
  AUD_CHECK(load.renders == 0);
  AUD_CHECK(load.renders_total == 0);
  AUD_CHECK(load.max == 0);
}

AUD_TEST(graph_render_load_records_live_renders) {
  Fixture f(0, 1, 480);
  AUD_CHECK(aud_graph_start(f.graph) == AUD_OK);
  for (int i = 0; i < 200; ++i) f.render(480);
  AudGraphLoad load = renderLoadOf(f, 100000000);
  AUD_CHECK(load.renders == 10);
  AUD_CHECK(load.frames == 4800);
  AUD_CHECK(load.window_ns == 100000000);
  AUD_CHECK(load.max > 0);
  AUD_CHECK(load.p99 <= load.max);
  AUD_CHECK(load.mean <= load.max);
  AUD_CHECK(load.renders_total == 200);
  // The same renders and the same budget as the overload diagnostic.
  AUD_CHECK(load.overloads_total == f.stats().overloads);
  load = renderLoadOf(f, 0);
  AUD_CHECK(load.renders == 200);
  AUD_CHECK(load.window_ns == 2000000000);
}

AUD_TEST(graph_render_load_ignores_offline_renders) {
  Fixture f;
  AUD_CHECK(aud_graph_start(f.graph) == AUD_OK);
  for (int i = 0; i < 5; ++i) f.render(256);
  std::vector<float> out(1024);
  float* channels[] = {out.data()};
  AudAudioBus bus{sizeof(AudAudioBus), 1, channels};
  AudOfflineRequest request{};
  request.struct_size = sizeof(AudOfflineRequest);
  request.frames = 1024;
  request.block_frames = 128;
  request.outputs = &bus;
  AUD_CHECK(aud_graph_render_offline(f.graph, &request) == AUD_OK);
  AudHostRenderRequest host{};
  AUD_CHECK(f.renderHost(256, nullptr, 0, &host, nullptr, 0,
                         AUD_PROCESS_OFFLINE) == AUD_OK);
  AUD_CHECK(f.stats().blocks_rendered == 5 + 8 + 1);
  const AudGraphLoad load = renderLoadOf(f, 0);
  AUD_CHECK(load.renders == 5);
  AUD_CHECK(load.frames == 5 * 256);
  AUD_CHECK(load.renders_total == 5);
  // A live render through the host's render call counts.
  AUD_CHECK(f.renderHost(256, nullptr, 0, &host) == AUD_OK);
  AUD_CHECK(renderLoadOf(f, 0).renders_total == 6);
}

AUD_TEST(graph_render_load_survives_reset_stats_and_restarts) {
  Fixture f;
  AUD_CHECK(aud_graph_start(f.graph) == AUD_OK);
  for (int i = 0; i < 10; ++i) f.render(256);
  aud_graph_reset_stats(f.graph);
  AUD_CHECK(f.stats().blocks_rendered == 0);
  AudGraphLoad load = renderLoadOf(f, 0);
  AUD_CHECK(load.renders == 10);
  AUD_CHECK(load.renders_total == 10);
  AUD_CHECK(aud_graph_stop(f.graph) == AUD_OK);
  AUD_CHECK(aud_graph_prepare(f.graph, 0, 0) == AUD_OK);
  AUD_CHECK(aud_graph_start(f.graph) == AUD_OK);
  f.render(256);
  load = renderLoadOf(f, 0);
  AUD_CHECK(load.renders == 11);
  AUD_CHECK(load.renders_total == 11);
}

AUD_TEST(graph_render_load_measures_a_slow_node) {
  Fixture f(0, 1, 48);  // blocks of 1 ms
  const AudHostApi* host = aud_graph_host_api(f.graph);
  host->register_node_type(host->host, &kBurnerDescriptor);
  const int32_t burner = f.node("aud.test.burner");
  AUD_CHECK(burner > 0);
  AUD_CHECK(f.commit([&] { f.connect(burner, AUD_GRAPH_NODE); }) > 0);
  AUD_CHECK(aud_graph_start(f.graph) == AUD_OK);
  g_burnNs.store(0);
  for (int i = 0; i < 98; ++i) f.render(48);
  // Two renders of at least three blocks: the 99th of 100 is one of them.
  g_burnNs.store(3000000);
  f.render(48);
  f.render(48);
  g_burnNs.store(0);
  AudGraphLoad load = renderLoadOf(f, 0);
  AUD_CHECK(load.renders == 100);
  AUD_CHECK(load.max >= 3.0);
  AUD_CHECK(load.p99 >= 3.0);
  AUD_CHECK(load.mean >= 2 * 3.0 / 100);
  AUD_CHECK(load.overloads_total >= 2);
  AUD_CHECK(load.overloads_total == f.stats().overloads);
  // The newest render alone.
  load = renderLoadOf(f, 1);
  AUD_CHECK(load.renders == 1);
  AUD_CHECK(load.max >= 3.0);
  // The counters go, the gauge stays.
  const uint64_t overloads = load.overloads_total;
  aud_graph_reset_stats(f.graph);
  AUD_CHECK(f.stats().overloads == 0);
  AUD_CHECK(renderLoadOf(f, 0).overloads_total == overloads);
}

AUD_TEST(graph_render_load_reads_while_the_audio_thread_renders) {
  Fixture f;
  AUD_CHECK(aud_graph_start(f.graph) == AUD_OK);
  constexpr uint32_t kBlocks = 3000;
  std::atomic<bool> done{false};
  std::thread stream([&] {
    for (uint32_t i = 0; i < kBlocks; ++i) f.render(64);
    done.store(true, std::memory_order_release);
  });
  uint64_t last = 0;
  uint32_t reads = 0;
  bool sane = true;
  do {
    int32_t result = AUD_ERROR_STATE;
    // The whole ring, and 5 ms: four blocks of 64 frames at 48 kHz.
    const AudGraphLoad load =
        renderLoadOf(f, reads % 2 == 0 ? 0 : 5000000, &result);
    sane = sane && result == AUD_OK &&
           load.renders <= AUD_GRAPH_LOAD_RENDERS &&
           load.renders <= load.renders_total &&
           (reads % 2 == 0 || load.renders <= 4) &&
           load.frames == uint64_t{load.renders} * 64 &&
           load.overloads_total <= load.renders_total &&
           load.renders_total >= last && load.p99 <= load.max &&
           load.mean <= load.max * (1 + 1e-9) && load.max >= 0;
    last = load.renders_total;
    reads += 1;
  } while (!done.load(std::memory_order_acquire));
  stream.join();
  AUD_CHECK(sane);
  const AudGraphLoad load = renderLoadOf(f, 0);
  AUD_CHECK(load.renders == AUD_GRAPH_LOAD_RENDERS);
  AUD_CHECK(load.renders_total == kBlocks);
}
