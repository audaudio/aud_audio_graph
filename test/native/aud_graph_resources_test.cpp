// @license
// Copyright (c) Audanika. All Rights Reserved.
//
// Use of this source code is governed by terms that can be
// found in the LICENSE file in the root of this package.

// The native tests of what ticket 25 added to the engine: the worker
// threads and their tasks, the loads with their ids, progress and
// cancellation, the caches with the budget of the process, the diagnostics
// a node reports, and the messages of nodes with their answers.

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "aud_audio_graph.h"
#include "aud_graph_fixture.hpp"
#include "aud_test.hpp"
#include "aud_ump.h"

using namespace aud_test_fixture;

namespace {

// ............................................................................
// aud.test.loader: every string starts a load on a worker. The task reports
// half of the progress and waits for the gate or a cancellation; a value
// "fail" ends with an error.

std::atomic<bool> g_gate{true};

struct LoaderNode {
  const AudHostApi* host;
};

struct LoadJob {
  const AudHostApi* host;
  int32_t load;
  bool fail;
};

void loadTask(void* user) {
  auto* job = static_cast<LoadJob*>(user);
  const AudHostApi* host = job->host;
  host->load_progress(host->host, job->load, 0.5f);
  int32_t result = job->fail ? AUD_ERROR_FAILED : AUD_OK;
  while (!g_gate.load(std::memory_order_acquire)) {
    if (host->load_cancelled(host->host, job->load)) {
      result = AUD_ERROR_CANCELLED;
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  host->end_load(host->host, job->load, result,
                 job->fail ? "the file is broken" : nullptr);
  delete job;
}

void* loaderCreate(const AudNodeDescriptor*, const AudHostApi* host) {
  return new LoaderNode{host};
}
void loaderDestroy(void* instance) { delete static_cast<LoaderNode*>(instance); }
int32_t loaderPrepare(void*, const AudPrepareInfo*) { return AUD_OK; }
void loaderProcess(void*, const AudProcessContext* context) {
  for (uint32_t b = 0; b < context->num_output_buses; ++b) {
    for (uint32_t c = 0; c < context->outputs[b].num_channels; ++c) {
      std::memset(context->outputs[b].channels[c], 0,
                  sizeof(float) * context->frames);
    }
  }
}
int32_t loaderSetString(void* instance, uint32_t, const char* value) {
  const AudHostApi* host = static_cast<LoaderNode*>(instance)->host;
  const int32_t load = host->begin_load(host->host, instance, value);
  if (load <= 0) return load;
  auto* job = new LoadJob{host, load, std::strcmp(value, "fail") == 0};
  const AudTask task{sizeof(AudTask), 0, loadTask, job};
  const int32_t posted = host->post_task(host->host, &task);
  if (posted != AUD_OK) {
    host->end_load(host->host, load, posted, "no worker");
    delete job;
    return posted;
  }
  return load;
}

const AudBusDescriptor kOut[] = {
    {sizeof(AudBusDescriptor), "out", "Out", AUD_BUS_MAIN, 1, 2, 1},
};
const AudStringKeyDescriptor kLoaderKeys[] = {
    {sizeof(AudStringKeyDescriptor), 0, "file", "File"},
};
const AudNodeVTable kLoaderVTable = {
    sizeof(AudNodeVTable), loaderCreate,    loaderDestroy, loaderPrepare,
    nullptr,               nullptr,         loaderProcess, loaderSetString,
    nullptr,               nullptr,         nullptr,       nullptr,
};
const AudNodeDescriptor kLoaderDescriptor = {
    sizeof(AudNodeDescriptor),
    AUD_ABI_VERSION_MAJOR,
    AUD_ABI_VERSION_MINOR,
    1,
    "aud.test.loader",
    "Loader",
    "Test",
    AUD_NODE_CAP_VARIABLE_BLOCK | AUD_NODE_CAP_STRINGS,
    0,
    0,
    1,
    nullptr,
    kOut,
    0,
    0,
    nullptr,
    nullptr,
    0,
    1,
    nullptr,
    kLoaderKeys,
    &kLoaderVTable,
};

// ............................................................................
// aud.test.echo: answers every message with its path and its arguments
// plus the sample offset; reports an underflow for every note on; with
// g_echoPost set it posts a task from the audio thread; with
// g_echoControl it calls begin_load there, which the watchdog catches.

std::atomic<uint32_t> g_tasksRun{0};
std::atomic<bool> g_echoPost{false};
std::atomic<bool> g_echoControl{false};
std::vector<uint32_t> g_seenTasks;  // reserved, appended by the audio thread

struct EchoNode {
  const AudHostApi* host;
};

void countTask(void*) { g_tasksRun.fetch_add(1, std::memory_order_acq_rel); }

void* echoCreate(const AudNodeDescriptor*, const AudHostApi* host) {
  return new EchoNode{host};
}
void echoDestroy(void* instance) { delete static_cast<EchoNode*>(instance); }
void echoProcess(void* instance, const AudProcessContext* context) {
  const AudHostApi* host = static_cast<EchoNode*>(instance)->host;
  if (g_seenTasks.size() < g_seenTasks.capacity()) {
    g_seenTasks.push_back(g_tasksRun.load(std::memory_order_acquire));
  }
  if (g_echoPost.load(std::memory_order_acquire)) {
    const AudTask task{sizeof(AudTask), 0, countTask, nullptr};
    host->post_task(host->host, &task);
  }
  if (g_echoControl.load(std::memory_order_acquire)) {
    AUD_TEST_RTSAN_OFF();
    const int32_t load = host->begin_load(host->host, instance, "rt");
    host->end_load(host->host, load, AUD_OK, nullptr);
    AUD_TEST_RTSAN_ON();
  }
  for (uint32_t i = 0; i < context->num_events; ++i) {
    const AudEvent& event = context->events[i];
    if (event.type == AUD_EVENT_UMP &&
        aud_ump_is_note_on(event.words[0], event.words[1])) {
      host->report(host->host, instance, AUD_ERROR_UNDERFLOW, 1);
    }
    const AudMessage* message = aud_event_message(&event);
    if (message == nullptr) continue;
    AudMessageArg args[17];
    for (uint32_t a = 0; a < message->num_args && a < 16; ++a) {
      args[a] = message->args[a];
    }
    AudMessageArg& offset = args[message->num_args];
    offset = AudMessageArg{};
    offset.struct_size = sizeof(AudMessageArg);
    offset.type = AUD_ARG_INT32;
    offset.value.i = static_cast<int32_t>(event.sample_offset);
    const AudMessage reply{sizeof(AudMessage), message->num_args + 1,
                           message->path, args};
    host->emit_message(host->host, instance, &reply);
    // A message event cannot leave the node as an event.
    host->emit_event(host->host, instance, &event);
  }
  loaderProcess(instance, context);
}

const AudEventPortDescriptor kEchoInputs[] = {
    {sizeof(AudEventPortDescriptor), "midi", "MIDI", AUD_EVENT_PORT_MIDI, 0},
};
const AudNodeVTable kEchoVTable = {
    sizeof(AudNodeVTable), echoCreate, echoDestroy, loaderPrepare,
    nullptr,               nullptr,    echoProcess, nullptr,
    nullptr,               nullptr,    nullptr,     nullptr,
};
const AudNodeDescriptor kEchoDescriptor = {
    sizeof(AudNodeDescriptor),
    AUD_ABI_VERSION_MAJOR,
    AUD_ABI_VERSION_MINOR,
    1,
    "aud.test.echo",
    "Echo",
    "Test",
    AUD_NODE_CAP_VARIABLE_BLOCK | AUD_NODE_CAP_EVENTS | AUD_NODE_CAP_MESSAGES,
    0,
    0,
    1,
    nullptr,
    kOut,
    1,
    0,
    kEchoInputs,
    nullptr,
    0,
    0,
    nullptr,
    nullptr,
    &kEchoVTable,
};

// The same node without AUD_NODE_CAP_VARIABLE_BLOCK: the engine re-blocks
// it, so its messages travel through the fixed-block wrapper.
const AudNodeDescriptor kFixedEchoDescriptor = {
    sizeof(AudNodeDescriptor),
    AUD_ABI_VERSION_MAJOR,
    AUD_ABI_VERSION_MINOR,
    1,
    "aud.test.fixed_echo",
    "Fixed echo",
    "Test",
    AUD_NODE_CAP_EVENTS | AUD_NODE_CAP_MESSAGES,
    0,
    0,
    1,
    nullptr,
    kOut,
    1,
    0,
    kEchoInputs,
    nullptr,
    0,
    0,
    nullptr,
    nullptr,
    &kEchoVTable,
};

void registerTypes(Fixture& f) {
  const AudHostApi* host = aud_graph_host_api(f.graph);
  host->register_node_type(host->host, &kLoaderDescriptor);
  host->register_node_type(host->host, &kEchoDescriptor);
  host->register_node_type(host->host, &kFixedEchoDescriptor);
}

// A node in the graph, connected to the output.
int32_t addNode(Fixture& f, const char* type) {
  const int32_t node = f.node(type);
  f.commit([&] { f.connect(node, AUD_GRAPH_NODE); });
  return node;
}

// ............................................................................
// Messages

AudMessageArg intArg(int32_t value) {
  AudMessageArg arg{};
  arg.struct_size = sizeof(AudMessageArg);
  arg.type = AUD_ARG_INT32;
  arg.value.i = value;
  return arg;
}

AudMessageArg textArg(const char* value) {
  AudMessageArg arg{};
  arg.struct_size = sizeof(AudMessageArg);
  arg.type = AUD_ARG_STRING;
  arg.value.s = value;
  return arg;
}

int32_t send(Fixture& f, int32_t node, const char* path,
             std::vector<AudMessageArg> args = {},
             const AudTimestamp* at = nullptr, uint32_t id = 0) {
  const AudMessage message{sizeof(AudMessage),
                           static_cast<uint32_t>(args.size()), path,
                           args.data()};
  return aud_graph_send_message(f.graph, node, &message, at, id);
}

// A message a node sent, unpacked from the layout of AudGraphMessage.
struct Reply {
  int32_t node = 0;
  std::string path;
  std::string tags;
  std::vector<int64_t> ints;
  std::vector<double> numbers;
  std::vector<std::string> texts;
  std::vector<std::vector<uint8_t>> blobs;
};

Reply unpack(const AudGraphMessage& record) {
  Reply reply;
  reply.node = record.node;
  const uint8_t* data = record.data;
  size_t at = 0;
  reply.path = reinterpret_cast<const char*>(data);
  at += reply.path.size() + 1;
  for (uint32_t i = 0; i < record.num_args; ++i) {
    const char tag = static_cast<char>(data[at++]);
    reply.tags += tag;
    switch (tag) {
      case 'i': {
        int32_t v;
        std::memcpy(&v, data + at, 4);
        reply.ints.push_back(v);
        at += 4;
        break;
      }
      case 'h': {
        int64_t v;
        std::memcpy(&v, data + at, 8);
        reply.ints.push_back(v);
        at += 8;
        break;
      }
      case 'f': {
        float v;
        std::memcpy(&v, data + at, 4);
        reply.numbers.push_back(v);
        at += 4;
        break;
      }
      case 'd': {
        double v;
        std::memcpy(&v, data + at, 8);
        reply.numbers.push_back(v);
        at += 8;
        break;
      }
      case 's': {
        std::string v = reinterpret_cast<const char*>(data + at);
        at += v.size() + 1;
        reply.texts.push_back(v);
        break;
      }
      case 'b': {
        uint32_t size;
        std::memcpy(&size, data + at, 4);
        at += 4;
        reply.blobs.emplace_back(data + at, data + at + size);
        at += size;
        break;
      }
      default:
        break;
    }
  }
  return reply;
}

std::vector<Reply> replies(Fixture& f) {
  std::vector<AudGraphMessage> records(64);
  const int32_t count = aud_graph_take_messages(f.graph, records.data(), 64);
  std::vector<Reply> result;
  for (int32_t i = 0; i < count; ++i) result.push_back(unpack(records[i]));
  return result;
}

// ............................................................................
// A cache of the tests: entries of bytes, the oldest first.

struct TestCache {
  const AudHostApi* host = nullptr;
  std::vector<uint64_t> entries;
  std::vector<uint64_t> asked;  // what each trim asked for
};

uint64_t testTrim(void* cache, uint64_t bytes) {
  auto* c = static_cast<TestCache*>(cache);
  c->asked.push_back(bytes);
  uint64_t freed = 0;
  while (freed < bytes && !c->entries.empty()) {
    freed += c->entries.front();
    c->entries.erase(c->entries.begin());
  }
  // The engine counts the bytes off; the cache does not report them.
  return freed;
}

const AudCacheVTable kTestCacheVTable = {sizeof(AudCacheVTable), testTrim};

void hold(TestCache& cache, uint64_t bytes) {
  cache.entries.push_back(bytes);
  cache.host->cache_changed(cache.host->host, &cache,
                            static_cast<int64_t>(bytes));
}

AudCacheUsage usage() {
  AudCacheUsage u{};
  u.struct_size = sizeof(AudCacheUsage);
  aud_cache_usage(&u);
  return u;
}

AudLoadInfo info(Fixture& f, int32_t id) {
  AudLoadInfo i{};
  i.struct_size = sizeof(AudLoadInfo);
  aud_graph_load_info(f.graph, id, &i);
  return i;
}

}  // namespace

// ############################################################################
// Tasks

AUD_TEST(tasks_run_on_the_workers_of_the_process) {
  Fixture f;
  g_tasksRun.store(0);
  const AudHostApi* host = aud_graph_host_api(f.graph);
  const auto post = [host] {
    for (int i = 0; i < 100; ++i) {
      const AudTask task{sizeof(AudTask), 0, countTask, nullptr};
      while (host->post_task(host->host, &task) == AUD_ERROR_QUEUE_FULL) {
        std::this_thread::yield();
      }
    }
  };
  std::thread other(post);
  post();
  other.join();
  AUD_CHECK(aud_tasks_wait_idle(0) == AUD_OK);
  AUD_CHECK(g_tasksRun.load() == 200);
  AUD_CHECK(usage().workers == 2 && usage().tasks == 0);
  const AudTask empty{sizeof(AudTask), 0, nullptr, nullptr};
  AUD_CHECK(host->post_task(host->host, &empty) == AUD_ERROR_INVALID_ARGUMENT);
  AUD_CHECK(host->post_task(host->host, nullptr) == AUD_ERROR_INVALID_ARGUMENT);
}

AUD_TEST(the_workers_stop_with_the_last_graph) {
  {
    Fixture a;
    {
      Fixture b;
      AUD_CHECK(usage().workers == 2);
    }
    // One graph is left: the workers stay.
    g_tasksRun.store(0);
    const AudHostApi* host = aud_graph_host_api(a.graph);
    const AudTask task{sizeof(AudTask), 0, countTask, nullptr};
    AUD_CHECK(host->post_task(host->host, &task) == AUD_OK);
    AUD_CHECK(aud_tasks_wait_idle(1000) == AUD_OK);
    AUD_CHECK(g_tasksRun.load() == 1);
  }
  AUD_CHECK(usage().workers == 0);
  AUD_CHECK(aud_tasks_wait_idle(10) == AUD_OK);
}

AUD_TEST(a_node_posts_tasks_from_the_audio_thread) {
  Fixture f;
  registerTypes(f);
  const int32_t echo = addNode(f, "aud.test.echo");
  (void)echo;
  aud_graph_start(f.graph);
  g_tasksRun.store(0);
  g_seenTasks.clear();
  g_seenTasks.reserve(64);
  g_echoPost.store(true);
  for (int i = 0; i < 10; ++i) f.render(256);
  g_echoPost.store(false);
  AUD_CHECK(aud_tasks_wait_idle(1000) == AUD_OK);
  AUD_CHECK(g_tasksRun.load() == 10);
  AUD_CHECK(f.stats().realtime_violations == 0);
}

AUD_TEST(an_offline_render_waits_for_the_tasks_of_each_block) {
  Fixture f;
  registerTypes(f);
  addNode(f, "aud.test.echo");
  aud_graph_start(f.graph);
  g_tasksRun.store(0);
  g_seenTasks.clear();
  g_seenTasks.reserve(64);
  g_echoPost.store(true);
  std::vector<float> out(1024);
  float* channels[] = {out.data()};
  AudAudioBus bus{sizeof(AudAudioBus), 1, channels};
  AudOfflineRequest request{};
  request.struct_size = sizeof(AudOfflineRequest);
  request.frames = 1024;
  request.block_frames = 128;
  request.outputs = &bus;
  AUD_CHECK(aud_graph_render_offline(f.graph, &request) == AUD_OK);
  g_echoPost.store(false);
  // Every block saw the tasks of all blocks before it done.
  AUD_CHECK(g_seenTasks.size() == 8);
  for (uint32_t i = 0; i < g_seenTasks.size(); ++i) {
    AUD_CHECK(g_seenTasks[i] == i);
  }
}

// ############################################################################
// Loads

AUD_TEST(a_load_reports_its_progress_and_its_end) {
  Fixture f;
  registerTypes(f);
  const int32_t loader = addNode(f, "aud.test.loader");
  g_gate.store(false);
  const int32_t id = aud_graph_set_string(f.graph, loader, 0, "piano.sfz");
  AUD_CHECK(id > 0);
  AUD_CHECK(aud_graph_wait_loads(f.graph, loader, 20) == AUD_PENDING);
  AudLoadInfo running = info(f, id);
  AUD_CHECK(running.result == AUD_PENDING && running.node == loader);
  AUD_CHECK(std::string(running.name) == "piano.sfz");
  AUD_CHECK(running.start_ns > 0 && running.end_ns == 0);
  g_gate.store(true);
  AUD_CHECK(aud_graph_wait_loads(f.graph, loader, 0) == AUD_OK);
  const AudLoadInfo done = info(f, id);
  AUD_CHECK(done.result == AUD_OK && done.progress == 1.0f);
  AUD_CHECK(done.end_ns >= done.start_ns && done.cancelled == 0);
  // The control thread heard the begin, the progress and the end.
  std::vector<AudGraphNotification> loads;
  for (const AudGraphNotification& n : f.takeAll()) {
    if (n.type == AUD_NOTIFY_LOAD) loads.push_back(n);
  }
  AUD_CHECK(loads.size() == 3);
  AUD_CHECK(loads.front().code == AUD_PENDING && loads.front().value == id);
  AUD_CHECK(loads[1].code == AUD_PENDING && loads[1].number == 0.5);
  AUD_CHECK(loads.back().code == AUD_OK && loads.back().node == loader);
  int32_t ids[4];
  AUD_CHECK(aud_graph_loads(f.graph, ids, 4) == 1 && ids[0] == id);
  AUD_CHECK(aud_graph_load_info(f.graph, 9999999, &running) ==
            AUD_ERROR_NOT_FOUND);
}

AUD_TEST(a_load_fails_with_a_message_or_is_cancelled) {
  Fixture f;
  registerTypes(f);
  const int32_t loader = addNode(f, "aud.test.loader");
  g_gate.store(true);
  const int32_t failed = aud_graph_set_string(f.graph, loader, 0, "fail");
  AUD_CHECK(aud_graph_wait_loads(f.graph, 0, 0) == AUD_OK);
  AUD_CHECK(info(f, failed).result == AUD_ERROR_FAILED);
  AUD_CHECK(std::string(info(f, failed).message) == "the file is broken");
  g_gate.store(false);
  const int32_t cancelled = aud_graph_set_string(f.graph, loader, 0, "x");
  AUD_CHECK(aud_graph_cancel_load(f.graph, cancelled) == AUD_OK);
  AUD_CHECK(aud_graph_wait_loads(f.graph, loader, 0) == AUD_OK);
  AUD_CHECK(info(f, cancelled).result == AUD_ERROR_CANCELLED);
  AUD_CHECK(info(f, cancelled).cancelled == 1);
  AUD_CHECK(aud_graph_cancel_load(f.graph, cancelled) == AUD_ERROR_NOT_FOUND);
  g_gate.store(true);
}

AUD_TEST(the_graph_keeps_the_last_64_loads) {
  Fixture f;
  registerTypes(f);
  const int32_t loader = addNode(f, "aud.test.loader");
  g_gate.store(true);
  for (int i = 0; i < 70; ++i) aud_graph_set_string(f.graph, loader, 0, "x");
  AUD_CHECK(aud_graph_wait_loads(f.graph, 0, 0) == AUD_OK);
  AUD_CHECK(aud_graph_loads(f.graph, nullptr, 0) == 64);
}

AUD_TEST(the_host_takes_load_ids_and_waits_for_the_loads) {
  Fixture f(0, 1);
  registerTypes(f);
  g_gate.store(false);
  AudHostOptions options{sizeof(AudHostOptions), nullptr};
  AudHost* host = aud_host_create(f.graph, &options);
  const char* document =
      R"({"schema":1,"outputChannels":[1],"nodes":[{"id":"l","type":)"
      R"("aud.test.loader","outputChannels":[1],"preset":{"schema":1,)"
      R"("type":"aud.test.loader","strings":{"file":"sounds"}}}],)"
      R"("connections":[{"from":"l","to":"graph"}]})";
  AUD_CHECK(aud_host_load(host, document, std::strlen(document)) == AUD_OK);
  AUD_CHECK(aud_host_wait_loads(host, 20) == AUD_PENDING);
  g_gate.store(true);
  AUD_CHECK(aud_host_wait_loads(host, 0) == AUD_OK);
  AUD_CHECK(aud_host_wait_loads(nullptr, 0) == AUD_ERROR_INVALID_ARGUMENT);
  aud_host_destroy(host);
}

// ############################################################################
// Caches and the budget

AUD_TEST(caches_are_trimmed_to_the_budget_of_the_process) {
  Fixture f;
  const AudHostApi* host = aud_graph_host_api(f.graph);
  TestCache cache;
  cache.host = host;
  AUD_CHECK(host->register_cache(host->host, &kTestCacheVTable, &cache) ==
            AUD_OK);
  AUD_CHECK(host->register_cache(host->host, &kTestCacheVTable, &cache) ==
            AUD_OK);
  AUD_CHECK(usage().caches == 1);
  AUD_CHECK(aud_cache_budget() == 256ull << 20);
  AUD_CHECK(host->cache_budget(host->host) == 256ull << 20);
  aud_cache_set_budget(1000);
  AUD_CHECK(host->cache_budget(host->host) == 1000);
  hold(cache, 600);
  AUD_CHECK(aud_tasks_wait_idle(1000) == AUD_OK);
  AUD_CHECK(cache.asked.empty());
  hold(cache, 600);
  AUD_CHECK(aud_tasks_wait_idle(1000) == AUD_OK);
  // The oldest entry went; the cache fits again.
  AUD_CHECK(cache.asked.size() == 1 && cache.asked[0] == 200);
  AUD_CHECK(usage().bytes == 600 && usage().trimmed >= 600);
  // A smaller budget trims at once, on the calling thread.
  aud_cache_set_budget(100);
  AUD_CHECK(usage().bytes == 0 && cache.entries.empty());
  host->unregister_cache(host->host, &cache);
  AUD_CHECK(usage().caches == 0);
  aud_cache_set_budget(256ull << 20);
  AUD_CHECK(host->register_cache(host->host, nullptr, &cache) ==
            AUD_ERROR_INVALID_ARGUMENT);
  AudCacheUsage small{};
  AUD_CHECK(aud_cache_usage(&small) == AUD_ERROR_INVALID_ARGUMENT);
}

AUD_TEST(the_largest_cache_is_trimmed_first) {
  Fixture f;
  const AudHostApi* host = aud_graph_host_api(f.graph);
  TestCache small;
  TestCache large;
  small.host = host;
  large.host = host;
  host->register_cache(host->host, &kTestCacheVTable, &small);
  host->register_cache(host->host, &kTestCacheVTable, &large);
  aud_cache_set_budget(1000);
  hold(small, 400);
  hold(large, 700);
  AUD_CHECK(aud_tasks_wait_idle(1000) == AUD_OK);
  AUD_CHECK(large.asked.size() == 1 && small.asked.empty());
  AUD_CHECK(usage().bytes == 400);
  host->unregister_cache(host->host, &small);
  host->unregister_cache(host->host, &large);
  aud_cache_set_budget(256ull << 20);
}

// ############################################################################
// Diagnostics of nodes

AUD_TEST(nodes_report_diagnostics_once_per_block) {
  Fixture f;
  registerTypes(f);
  const int32_t echo = addNode(f, "aud.test.echo");
  aud_graph_start(f.graph);
  for (uint32_t note = 60; note < 63; ++note) {
    const AudEvent on = noteOn(note);
    AUD_CHECK(aud_graph_send_event(f.graph, echo, &on, nullptr, 0) == AUD_OK);
  }
  f.render(256);
  uint32_t reports = 0;
  for (const AudGraphNotification& n : f.takeAll()) {
    if (n.type != AUD_NOTIFY_DIAGNOSTIC || n.code != AUD_ERROR_UNDERFLOW) {
      continue;
    }
    reports += 1;
    AUD_CHECK(n.count == 3 && n.node == echo);
  }
  AUD_CHECK(reports == 1);
}

AUD_TEST(the_watchdog_catches_control_calls_on_the_audio_thread) {
  if (!aud_graph_watchdog_enabled()) return;
  Fixture f;
  registerTypes(f);
  addNode(f, "aud.test.echo");
  aud_graph_start(f.graph);
  aud_graph_watchdog_reset();
  g_echoControl.store(true);
  f.render(256);
  g_echoControl.store(false);
  AUD_CHECK(aud_graph_watchdog_violations() >= 2);
  bool control = false;
  for (const AudGraphNotification& n : f.takeAll()) {
    if (n.type == AUD_NOTIFY_DIAGNOSTIC &&
        n.code == AUD_GRAPH_ERROR_REALTIME_VIOLATION &&
        (n.value & AUD_GRAPH_VIOLATION_CONTROL)) {
      control = true;
    }
  }
  AUD_CHECK(control);
  aud_graph_watchdog_reset();
}

// ############################################################################
// Messages

AUD_TEST(a_message_reaches_its_node_and_the_answer_comes_back) {
  Fixture f;
  registerTypes(f);
  const int32_t echo = addNode(f, "aud.test.echo");
  aud_graph_start(f.graph);
  const uint8_t bytes[] = {1, 2, 3};
  std::vector<AudMessageArg> args(10);
  for (AudMessageArg& arg : args) arg.struct_size = sizeof(AudMessageArg);
  args[0] = intArg(-7);
  args[1].type = AUD_ARG_FLOAT;
  args[1].value.f = 0.25f;
  args[2].type = AUD_ARG_INT64;
  args[2].value.h = 1ll << 40;
  args[3].type = AUD_ARG_DOUBLE;
  args[3].value.d = 0.125;
  args[4] = textArg("label");
  args[5].type = AUD_ARG_BLOB;
  args[5].size = 3;
  args[5].value.b = bytes;
  args[6].type = AUD_ARG_TRUE;
  args[7].type = AUD_ARG_FALSE;
  args[8].type = AUD_ARG_NIL;
  args[9].type = AUD_ARG_IMPULSE;
  AUD_CHECK(send(f, echo, "/region0/volume", args) == AUD_OK);
  f.render(256);
  AUD_CHECK(f.countNotifications(AUD_NOTIFY_MESSAGE) == 1);
  const std::vector<Reply> all = replies(f);
  AUD_CHECK(all.size() == 1);
  const Reply& reply = all[0];
  AUD_CHECK(reply.node == echo && reply.path == "/region0/volume");
  AUD_CHECK(reply.tags == "ifhdsbTFNIi");
  AUD_CHECK(reply.ints.size() == 3 && reply.ints[0] == -7 &&
            reply.ints[1] == (1ll << 40) && reply.ints[2] == 0);
  AUD_CHECK(reply.numbers.size() == 2 && reply.numbers[0] == 0.25 &&
            reply.numbers[1] == 0.125);
  AUD_CHECK(reply.texts.size() == 1 && reply.texts[0] == "label");
  AUD_CHECK(reply.blobs.size() == 1 && reply.blobs[0].size() == 3 &&
            reply.blobs[0][2] == 3);
  // Nothing of the message left the node as an event.
  AUD_CHECK(f.countNotifications(AUD_NOTIFY_EVENT) == 0);
  AUD_CHECK(replies(f).empty());
}

AUD_TEST(a_scheduled_message_arrives_at_its_sample) {
  Fixture f;
  registerTypes(f);
  const int32_t echo = addNode(f, "aud.test.echo");
  aud_graph_start(f.graph);
  const AudTimestamp at = atSample(300);
  AUD_CHECK(send(f, echo, "/hello", {}, &at, 7) == AUD_OK);
  f.render(256);
  AUD_CHECK(replies(f).empty());
  f.render(256);
  const std::vector<Reply> all = replies(f);
  AUD_CHECK(all.size() == 1 && all[0].ints.size() == 1 && all[0].ints[0] == 44);
}

AUD_TEST(messages_free_their_slots_on_every_path) {
  Fixture f(0, 1, 256, [](AudGraphConfig& c) { c.message_capacity = 2; });
  registerTypes(f);
  const int32_t echo = addNode(f, "aud.test.echo");
  aud_graph_start(f.graph);
  // Two scheduled messages take both slots.
  const AudTimestamp later = atSample(48000);
  AUD_CHECK(send(f, echo, "/a", {}, &later, 1) == AUD_OK);
  AUD_CHECK(send(f, echo, "/b", {}, &later, 2) == AUD_OK);
  AUD_CHECK(send(f, echo, "/c") == AUD_ERROR_CAPACITY);
  // A cancellation gives one back.
  AUD_CHECK(aud_graph_cancel(f.graph, echo, 1) == AUD_OK);
  f.render(256);
  AUD_CHECK(send(f, echo, "/c") == AUD_OK);
  f.render(256);
  AUD_CHECK(replies(f).size() == 1);
  // The retirement of the node gives the last one back.
  f.commit([&] { aud_graph_remove_node(f.graph, echo); });
  f.render(256);
  const int32_t other = addNode(f, "aud.test.echo");
  f.render(256);
  AUD_CHECK(send(f, other, "/d") == AUD_OK);
  AUD_CHECK(send(f, other, "/e") == AUD_OK);
  f.render(256);
  AUD_CHECK(replies(f).size() == 2);
  // A host cannot hand a message event in: it would point anywhere and
  // free a slot it does not own.
  f.commit([&] {
    AUD_CHECK(aud_graph_connect_events(f.graph, AUD_GRAPH_NODE, 0, other, 0) == AUD_OK);
  });
  AUD_CHECK(send(f, other, "/f", {}, &later, 3) == AUD_OK);
  AudEvent forged{};
  forged.struct_size = sizeof(AudEvent);
  forged.type = AUD_EVENT_MESSAGE;
  forged.words[0] = 0xdeadbeef;
  forged.words[2] = 0;
  f.render(256, nullptr, &forged, 1);
  f.render(256, nullptr, &forged, 1);
  AUD_CHECK(replies(f).empty());
  AUD_CHECK(send(f, other, "/g") == AUD_OK);
  AUD_CHECK(send(f, other, "/h") == AUD_ERROR_CAPACITY);
}

AUD_TEST(messages_pass_the_fixed_block_wrapper) {
  Fixture f;
  registerTypes(f);
  const int32_t echo = addNode(f, "aud.test.fixed_echo");
  aud_graph_start(f.graph);
  // The wrapper renders 256 frames at a time, a block of 100 frames later.
  AUD_CHECK(send(f, echo, "/w") == AUD_OK);
  f.render(100);
  f.render(100);
  f.render(100);
  const std::vector<Reply> all = replies(f);
  AUD_CHECK(all.size() == 1 && all[0].path == "/w");
  // Its slot came back: the capacity holds again and again.
  for (int i = 0; i < 300; ++i) {
    AUD_CHECK(send(f, echo, "/again") == AUD_OK);
    f.render(256);
  }
  AUD_CHECK(replies(f).size() == 64);
}

AUD_TEST(a_full_reply_queue_drops_and_counts) {
  Fixture f(0, 1, 256, [](AudGraphConfig& c) { c.reply_capacity = 2; });
  registerTypes(f);
  const int32_t echo = addNode(f, "aud.test.echo");
  aud_graph_start(f.graph);
  for (int i = 0; i < 4; ++i) AUD_CHECK(send(f, echo, "/x") == AUD_OK);
  f.render(256);
  AUD_CHECK(f.stats().messages_dropped == 2);
  bool dropped = false;
  for (const AudGraphNotification& n : f.takeAll()) {
    if (n.type == AUD_NOTIFY_DIAGNOSTIC && n.code == AUD_ERROR_QUEUE_FULL &&
        n.node == echo && n.count == 2) {
      dropped = true;
    }
  }
  AUD_CHECK(dropped);
  AUD_CHECK(replies(f).size() == 2);
  aud_graph_reset_stats(f.graph);
  AUD_CHECK(f.stats().messages_dropped == 0);
}

AUD_TEST(messages_are_checked_before_they_queue) {
  Fixture f;
  registerTypes(f);
  const int32_t echo = addNode(f, "aud.test.echo");
  const int32_t loader = addNode(f, "aud.test.loader");
  const std::string large(2000, 'x');
  AUD_CHECK(send(f, echo, "/big", {textArg(large.c_str())}) ==
            AUD_ERROR_CAPACITY);
  AUD_CHECK(send(f, echo, "no-slash") == AUD_ERROR_INVALID_ARGUMENT);
  AUD_CHECK(send(f, loader, "/x") == AUD_ERROR_UNSUPPORTED);
  AUD_CHECK(send(f, 999, "/x") == AUD_ERROR_NOT_FOUND);
  std::vector<AudMessageArg> many(17, intArg(1));
  AUD_CHECK(send(f, echo, "/many", many) == AUD_ERROR_INVALID_ARGUMENT);
  AudMessageArg unknown = intArg(0);
  unknown.type = 'x';
  AUD_CHECK(send(f, echo, "/odd", {unknown}) == AUD_ERROR_INVALID_ARGUMENT);
  // A message event can only come from the engine.
  const AudMessage message{sizeof(AudMessage), 0, "/x", nullptr};
  const AudEvent event = aud_event_message_make(&message, 0, 0);
  AUD_CHECK(aud_graph_send_event(f.graph, echo, &event, nullptr, 0) ==
            AUD_ERROR_INVALID_ARGUMENT);
  AUD_CHECK(aud_event_message(&event) == &message);
  const AudEvent on = noteOn(60);
  AUD_CHECK(aud_event_message(&on) == nullptr);
  AUD_CHECK(aud_graph_take_messages(nullptr, nullptr, 0) ==
            AUD_ERROR_INVALID_ARGUMENT);
}
