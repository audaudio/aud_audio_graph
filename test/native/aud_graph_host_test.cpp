// @license
// Copyright (c) Audanika. All Rights Reserved.
//
// Use of this source code is governed by terms that can be
// found in the LICENSE file in the root of this package.

// The native tests of the headless host (plugin-002, ticket 20): loading
// documents with presets and assets, saving and restoring the state, the
// stable parameter ids, latency and tail, the events for the host, and
// every refusal that leaves the graph untouched.

#include <unistd.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "aud_audio_graph.h"
#include "aud_graph_fixture.hpp"
#include "aud_json.hpp"
#include "aud_test.hpp"
#include "aud_transport.h"

using namespace aud_test_fixture;

namespace {

// A directory with the asset of the reference document.
struct TempDir {
  std::string path;
  std::vector<std::string> files;

  TempDir() {
    char buffer[] = "/tmp/aud_host_XXXXXX";
    path = mkdtemp(buffer);
    write("song.txt");
  }

  ~TempDir() {
    for (const std::string& file : files) std::remove(file.c_str());
    rmdir(path.c_str());
  }

  void write(const std::string& name) {
    const std::string file = path + "/" + name;
    FILE* handle = std::fopen(file.c_str(), "w");
    if (handle != nullptr) {
      std::fputs("la\n", handle);
      std::fclose(handle);
    }
    files.push_back(file);
  }
};

// The state blob of aud.test.preset: its internal offset.
std::string presetState(float offset) {
  uint8_t bytes[sizeof(float)];
  std::memcpy(bytes, &offset, sizeof(float));
  return aud::base64Encode(bytes, sizeof(bytes));
}

// The reference chain with a preset node and an asset.
std::string referenceDocument(const std::string& state = presetState(0.125f)) {
  return std::string(R"({
  "schema": 1,
  "name": "Reference chain",
  "outputChannels": [1],
  "assets": [{ "id": "song", "path": "song.txt" }],
  "nodes": [
    { "id": "osc", "type": "aud.graph.oscillator",
      "preset": { "schema": 1, "type": "aud.graph.oscillator",
                  "params": { "frequency": 20, "waveform": 2, "amplitude": 0.5 } } },
    { "id": "preset", "type": "aud.test.preset",
      "preset": { "schema": 1, "type": "aud.test.preset", "nodeVersion": 2,
                  "params": { "a": 0.25 },
                  "strings": { "file": "asset:song", "name": "Hello" },
                  "state": ")") +
         state + R"(", "stateVersion": 3 } },
    { "id": "mixer", "type": "aud.graph.mixer",
      "inputChannels": [1, 1, 1, 1, 1, 1, 1, 1], "outputChannels": [1] }
  ],
  "connections": [
    { "from": "osc", "to": "mixer" },
    { "from": "preset", "to": "mixer", "toBus": 1 },
    { "from": "mixer", "to": "graph" }
  ],
  "eventConnections": [{ "from": "graph", "to": "osc" }],
  "transport": { "tempo": 100, "numerator": 3, "denominator": 4,
                 "loopStart": 0, "loopEnd": 4 }
})";
}

AudHost* hostOf(Fixture& f, const TempDir& dir) {
  AudHostOptions options{sizeof(AudHostOptions), dir.path.c_str()};
  return aud_host_create(f.graph, &options);
}

int32_t load(AudHost* host, const std::string& text) {
  return aud_host_load(host, text.data(), text.size());
}

std::string saved(AudHost* host) {
  size_t size = 0;
  const int32_t probe = aud_host_save(host, nullptr, 0, &size);
  AUD_CHECK(probe == AUD_ERROR_BUFFER_TOO_SMALL);
  std::string text(size + 1, '\0');
  AUD_CHECK(aud_host_save(host, &text[0], text.size(), &size) == AUD_OK);
  text.resize(size);
  return text;
}

bool contains(const char* text, const char* part) {
  return std::strstr(text, part) != nullptr;
}

const aud::Json& at(const aud::Json& list, size_t index) {
  return list.array[index];
}

}  // namespace

// ############################################################################

AUD_TEST(host_loads_a_document_and_applies_presets) {
  TempDir dir;
  Fixture f;
  AudHost* host = hostOf(f, dir);
  AUD_CHECK(host != nullptr && aud_host_graph(host) == f.graph);
  AUD_CHECK(aud_host_create(nullptr, nullptr) == nullptr);
  AudHostOptions bad{4, nullptr};
  AUD_CHECK(aud_host_create(f.graph, &bad) == nullptr);
  AUD_CHECK(aud_host_graph(nullptr) == nullptr);
  AUD_CHECK(std::string(aud_host_last_error(nullptr)).empty());
  const std::string text = referenceDocument();
  AUD_CHECK(load(host, text) == AUD_OK);
  AUD_CHECK(std::string(aud_host_last_error(host)).empty());
  AUD_CHECK(aud_host_num_nodes(host) == 3);
  const int32_t osc = aud_host_node(host, "osc");
  const int32_t preset = aud_host_node(host, "preset");
  AUD_CHECK(osc > 0 && preset > osc);
  AUD_CHECK(aud_host_node(host, "nothing") == AUD_ERROR_NOT_FOUND);
  AUD_CHECK(aud_host_node(host, nullptr) == AUD_ERROR_INVALID_ARGUMENT);
  AUD_CHECK(std::string(aud_host_node_id(host, preset)) == "preset");
  AUD_CHECK(aud_host_node_id(host, 999) == nullptr);
  AUD_CHECK(aud_host_node_id(nullptr, preset) == nullptr);
  // The preset: the strings, the state blob, the parameters.
  Preset* p = g_lastPreset;
  AUD_CHECK(p->a == 0.25f && p->b == 10.0f && p->offset == 0.125f && p->loads == 1);
  AUD_CHECK(p->file == dir.path + "/song.txt" && p->name == "Hello");
  // The parameters, by stable id.
  AUD_CHECK(aud_host_num_params(host) == 14);
  // FNV-1a with the top bit cleared: pinned, a plugin state depends on it.
  AUD_CHECK(aud_host_param_id("osc", "frequency") == 0x3697a565u);
  AUD_CHECK(aud_host_param_id("mixer", "master") == 0x50c7d2b5u);
  AUD_CHECK(aud_host_param_id("preset", "a") == 0x6ea46fe6u);
  AUD_CHECK(aud_host_param_id(nullptr, "x") == 0 && aud_host_param_id("x", nullptr) == 0);
  const uint32_t frequency = aud_host_param_id("osc", "frequency");
  const int32_t index = aud_host_param_index(host, frequency);
  AUD_CHECK(index >= 0);
  AudHostParam param{};
  param.struct_size = sizeof(AudHostParam);
  AUD_CHECK(aud_host_param(host, static_cast<uint32_t>(index), &param) == AUD_OK);
  AUD_CHECK(param.id == frequency && param.node == osc && param.index == 0);
  AUD_CHECK(std::string(param.node_id) == "osc" && std::string(param.param_id) == "frequency");
  AUD_CHECK(param.descriptor != nullptr && param.descriptor->min_value == 20.0f);
  AUD_CHECK(param.value == 20.0f);
  AUD_CHECK(aud_host_param(host, 99, &param) == AUD_ERROR_NOT_FOUND);
  AUD_CHECK(aud_host_param(host, 0, nullptr) == AUD_ERROR_INVALID_ARGUMENT);
  AUD_CHECK(aud_host_param_index(host, 12345) == AUD_ERROR_NOT_FOUND);
  uint32_t last = 0;
  for (uint32_t i = 0; i < 14; ++i) {
    AUD_CHECK(aud_host_param(host, i, &param) == AUD_OK);
    AUD_CHECK(i == 0 || param.id > last);
    last = param.id;
  }
  float value = 0;
  AUD_CHECK(aud_host_get_param(host, frequency, &value) == AUD_OK && value == 20.0f);
  AUD_CHECK(aud_host_get_param(host, 12345, &value) == AUD_ERROR_NOT_FOUND);
  AUD_CHECK(aud_host_get_param(host, frequency, nullptr) == AUD_ERROR_INVALID_ARGUMENT);
  AUD_CHECK(aud_host_set_param(host, 12345, 1, 0) == AUD_ERROR_NOT_FOUND);
  AUD_CHECK(aud_host_set_param(nullptr, frequency, 30, 0) == AUD_ERROR_INVALID_ARGUMENT);
  AUD_CHECK(aud_host_set_param(host, frequency, 2.0f, 0) == AUD_ERROR_INVALID_ARGUMENT);
  AUD_CHECK(aud_host_set_param(host, frequency, std::nanf(""), 0) == AUD_ERROR_INVALID_ARGUMENT);
  AUD_CHECK(aud_host_set_param(host, frequency, 30.0f, 0) == AUD_OK);
  AUD_CHECK(aud_host_get_param(host, frequency, &value) == AUD_OK && value == 30.0f);
  AUD_CHECK(aud_host_get_param(nullptr, frequency, &value) == AUD_ERROR_INVALID_ARGUMENT);
  AUD_CHECK(aud_host_param_index(nullptr, frequency) == AUD_ERROR_INVALID_ARGUMENT);
  AUD_CHECK(aud_host_param(nullptr, 0, &param) == AUD_ERROR_INVALID_ARGUMENT);
  const uint32_t a = aud_host_param_id("preset", "a");
  AUD_CHECK(aud_host_set_param(host, a, 0.3f, 0) == AUD_OK);
  AUD_CHECK(aud_host_latency(host) == 0 && aud_host_tail(host) == 0);
  AUD_CHECK(aud_host_latency(nullptr) == AUD_ERROR_INVALID_ARGUMENT);
  AUD_CHECK(aud_host_tail(nullptr) == 0);
  // The chain renders: the square wave plus the preset node's a + offset.
  aud_graph_start(f.graph);
  f.render(256);
  AUD_CHECK_NEAR(f.out[0][100], 0.5 + 0.3 + 0.125, 1e-6);
  // The nodes in document order.
  AUD_CHECK(aud_host_node_at(host, 0) == osc && aud_host_node_at(host, 1) == preset);
  AUD_CHECK(aud_host_node_at(host, 3) == AUD_ERROR_NOT_FOUND);
  AUD_CHECK(aud_host_node_at(nullptr, 0) == AUD_ERROR_INVALID_ARGUMENT);
  const AudGraphTransportState t = f.transport();
  AUD_CHECK(t.tempo == 100 && t.numerator == 3 && t.denominator == 4 && t.looping == 1);
  AUD_CHECK(aud_beats_from_ticks(t.loop_end) == 4);
  // The assets.
  AUD_CHECK(aud_host_num_assets(host) == 1);
  AudHostAsset asset{};
  asset.struct_size = sizeof(AudHostAsset);
  AUD_CHECK(aud_host_asset(host, 0, &asset) == AUD_OK);
  AUD_CHECK(std::string(asset.id) == "song" && std::string(asset.path) == "song.txt");
  AUD_CHECK(std::string(asset.resolved) == dir.path + "/song.txt" && asset.exists == 1);
  AUD_CHECK(aud_host_asset(host, 1, &asset) == AUD_ERROR_NOT_FOUND);
  AUD_CHECK(aud_host_asset(host, 0, nullptr) == AUD_ERROR_INVALID_ARGUMENT);
  AUD_CHECK(aud_host_num_nodes(nullptr) == AUD_ERROR_INVALID_ARGUMENT);
  AUD_CHECK(aud_host_num_params(nullptr) == AUD_ERROR_INVALID_ARGUMENT);
  AUD_CHECK(aud_host_num_assets(nullptr) == AUD_ERROR_INVALID_ARGUMENT);
  aud_host_destroy(host);
  aud_host_destroy(nullptr);
  // The nodes stay with the graph.
  AUD_CHECK(aud_graph_nodes(f.graph, nullptr, 0) == 3);
}

AUD_TEST(host_saves_the_state_and_restores_it) {
  TempDir dir;
  Fixture f;
  AudHost* host = hostOf(f, dir);
  AUD_CHECK(load(host, referenceDocument()) == AUD_OK);
  AUD_CHECK(aud_host_set_param(host, aud_host_param_id("preset", "a"), 0.3f, 0) == AUD_OK);
  aud_graph_start(f.graph);
  f.render(256);  // the queued parameter reaches the node
  size_t size = 0;
  AUD_CHECK(aud_host_save(host, nullptr, 0, &size) == AUD_ERROR_BUFFER_TOO_SMALL);
  AUD_CHECK(size > 100);
  AUD_CHECK(aud_host_save(host, nullptr, 0, nullptr) == AUD_ERROR_INVALID_ARGUMENT);
  AUD_CHECK(aud_host_save(nullptr, nullptr, 0, &size) == AUD_ERROR_INVALID_ARGUMENT);
  std::vector<char> short_(size);
  AUD_CHECK(aud_host_save(host, short_.data(), size, &size) == AUD_ERROR_BUFFER_TOO_SMALL);
  const std::string text = saved(host);
  AUD_CHECK(text.back() == '\n');
  aud::Json doc;
  std::string error;
  AUD_CHECK(aud::parseJson(text.data(), text.size(), &doc, &error));
  AUD_CHECK(doc.find("schema")->number == 1);
  AUD_CHECK(doc.find("name")->string == "Reference chain");
  AUD_CHECK(doc.find("inputChannels")->array.empty());
  AUD_CHECK(at(*doc.find("outputChannels"), 0).number == 1);
  const aud::Json& assets = *doc.find("assets");
  AUD_CHECK(assets.array.size() == 1 && at(assets, 0).find("id")->string == "song" &&
            at(assets, 0).find("path")->string == "song.txt");
  const aud::Json& nodes = *doc.find("nodes");
  AUD_CHECK(nodes.array.size() == 3);
  const aud::Json& preset = *at(nodes, 1).find("preset");
  AUD_CHECK(at(nodes, 1).find("id")->string == "preset");
  AUD_CHECK(at(nodes, 1).find("inputChannels")->array.empty());
  AUD_CHECK(at(*at(nodes, 1).find("outputChannels"), 0).number == 1);
  AUD_CHECK(preset.find("schema")->number == 1 && preset.find("nodeVersion")->number == 2);
  AUD_CHECK(preset.find("type")->string == "aud.test.preset");
  AUD_CHECK(preset.find("params")->find("a")->number == static_cast<double>(0.3f));
  AUD_CHECK(preset.find("params")->find("b")->number == 10);
  AUD_CHECK(preset.find("strings")->find("file")->string == "asset:song");
  AUD_CHECK(preset.find("strings")->find("name")->string == "Hello");
  AUD_CHECK(preset.find("stateVersion")->number == 3);
  AUD_CHECK(preset.find("state")->string == presetState(0.125f));
  const aud::Json& mixer = *at(nodes, 2).find("preset");
  AUD_CHECK(mixer.find("strings") == nullptr && mixer.find("state") == nullptr);
  AUD_CHECK(mixer.find("params")->object.size() == 9);
  const aud::Json& connections = *doc.find("connections");
  AUD_CHECK(connections.array.size() == 3);
  AUD_CHECK(at(connections, 1).find("toBus")->number == 1 && at(connections, 0).find("toBus") == nullptr);
  AUD_CHECK(at(*doc.find("eventConnections"), 0).find("from")->string == "graph");
  const aud::Json& transport = *doc.find("transport");
  AUD_CHECK(transport.find("tempo")->number == 100 && transport.find("numerator")->number == 3);
  AUD_CHECK(transport.find("loopStart")->number == 0 && transport.find("loopEnd")->number == 4);
  // A second host restores the text to the same state: it saves the same.
  Fixture g;
  AudHost* second = hostOf(g, dir);
  AUD_CHECK(load(second, text) == AUD_OK);
  AUD_CHECK(g_lastPreset->a == 0.3f && g_lastPreset->b == 10.0f &&
            g_lastPreset->offset == 0.125f);
  AUD_CHECK(g_lastPreset->name == "Hello" && g_lastPreset->file == dir.path + "/song.txt");
  aud_graph_start(g.graph);
  g.render(256);
  AUD_CHECK(saved(second) == text);
  AUD_CHECK_NEAR(g.out[0][100], 0.5 + 0.3 + 0.125, 1e-6);
  aud_host_destroy(second);
  // The preset of one node, and a node without state.
  size = 0;
  AUD_CHECK(aud_host_node_preset(host, "osc", nullptr, 0, &size) == AUD_ERROR_BUFFER_TOO_SMALL);
  std::string one(size + 1, '\0');
  AUD_CHECK(aud_host_node_preset(host, "osc", &one[0], one.size(), &size) == AUD_OK);
  one.resize(size);
  aud::Json oscPreset;
  AUD_CHECK(aud::parseJson(one.data(), one.size(), &oscPreset, nullptr));
  AUD_CHECK(oscPreset.find("params")->find("frequency")->number == 20);
  AUD_CHECK(oscPreset.find("state") == nullptr);
  AUD_CHECK(aud_host_node_preset(host, "nothing", &one[0], one.size(), &size) == AUD_ERROR_NOT_FOUND);
  AUD_CHECK(contains(aud_host_last_error(host), "unknown node nothing"));
  AUD_CHECK(aud_host_node_preset(host, nullptr, &one[0], one.size(), &size) == AUD_ERROR_INVALID_ARGUMENT);
  aud_host_destroy(host);
}

AUD_TEST(host_refuses_documents_that_do_not_fit) {
  TempDir dir;
  Fixture f;
  AudHost* host = hostOf(f, dir);
  struct Case {
    const char* json;
    int32_t code;
    const char* message;
  };
  const Case cases[] = {
      {"not json", AUD_ERROR_INVALID_ARGUMENT, "invalid JSON"},
      {"[]", AUD_ERROR_INVALID_ARGUMENT, "must be an object"},
      {"{\"schema\": 2}", AUD_ERROR_INVALID_ARGUMENT, "schema must be 1"},
      {"{\"schema\": 1, \"foo\": 1}", AUD_ERROR_INVALID_ARGUMENT, "unknown key foo"},
      {"{\"schema\": 1, \"name\": 3}", AUD_ERROR_INVALID_ARGUMENT, "name must be a string"},
      {"{\"schema\": 1, \"outputChannels\": [0]}", AUD_ERROR_INVALID_ARGUMENT, "channel counts"},
      {"{\"schema\": 1, \"outputChannels\": [1], \"nodes\": 3}", AUD_ERROR_INVALID_ARGUMENT, "nodes must be a list"},
      {"{\"schema\": 1, \"outputChannels\": [1], \"nodes\": [3]}", AUD_ERROR_INVALID_ARGUMENT, "must be an object"},
      {"{\"schema\": 1, \"outputChannels\": [1], \"nodes\": [{\"id\": \"a\", \"type\": \"aud.graph.filter\"}, {\"id\": \"a\", \"type\": \"aud.graph.filter\"}]}",
       AUD_ERROR_INVALID_ARGUMENT, "duplicate node id a"},
      {"{\"schema\": 1, \"outputChannels\": [1], \"nodes\": [{\"id\": \"graph\", \"type\": \"aud.graph.filter\"}]}",
       AUD_ERROR_INVALID_ARGUMENT, "must not be named graph"},
      {"{\"schema\": 1, \"outputChannels\": [1], \"nodes\": [{\"id\": \"1bad\", \"type\": \"aud.graph.filter\"}]}",
       AUD_ERROR_INVALID_ARGUMENT, "needs an id"},
      {"{\"schema\": 1, \"outputChannels\": [1], \"nodes\": [{\"id\": \"a\", \"type\": \"Filter\"}]}",
       AUD_ERROR_INVALID_ARGUMENT, "needs a type"},
      {"{\"schema\": 1, \"outputChannels\": [1], \"nodes\": [{\"id\": \"a\", \"type\": \"aud.nothing.x\"}]}",
       AUD_ERROR_UNKNOWN_TYPE, "unknown node type aud.nothing.x"},
      {"{\"schema\": 1, \"outputChannels\": [1], \"nodes\": [{\"id\": \"a\", \"type\": \"aud.graph.filter\", \"foo\": 1}]}",
       AUD_ERROR_INVALID_ARGUMENT, "unknown key foo in node a"},
      {"{\"schema\": 1, \"outputChannels\": [1], \"nodes\": [{\"id\": \"a\", \"type\": \"aud.graph.filter\", \"delayFrames\": -1}]}",
       AUD_ERROR_INVALID_ARGUMENT, "delayFrames of a"},
      {"{\"schema\": 1, \"outputChannels\": [1], \"nodes\": [{\"id\": \"a\", \"type\": \"aud.graph.filter\", \"inputChannels\": [1, 1]}]}",
       AUD_ERROR_FORMAT, "do not fit"},
      {"{\"schema\": 1, \"outputChannels\": [1], \"nodes\": [{\"id\": \"a\", \"type\": \"aud.graph.filter\", \"outputChannels\": [64]}]}",
       AUD_ERROR_FORMAT, "unsupported channel count"},
      {"{\"schema\": 1, \"outputChannels\": [1], \"nodes\": [{\"id\": \"a\", \"type\": \"aud.graph.filter\", \"preset\": 1}]}",
       AUD_ERROR_INVALID_ARGUMENT, "must be an object"},
      {"{\"schema\": 1, \"outputChannels\": [1], \"nodes\": [{\"id\": \"a\", \"type\": \"aud.graph.filter\", \"preset\": {\"schema\": 1, \"type\": \"aud.graph.gain\"}}]}",
       AUD_ERROR_INVALID_ARGUMENT, "type of the preset of node a"},
      {"{\"schema\": 1, \"outputChannels\": [1], \"nodes\": [{\"id\": \"a\", \"type\": \"aud.graph.filter\", \"preset\": {\"schema\": 1, \"type\": \"aud.graph.filter\", \"params\": {\"nope\": 1}}}]}",
       AUD_ERROR_NOT_FOUND, "unknown param nope of node a"},
      {"{\"schema\": 1, \"outputChannels\": [1], \"nodes\": [{\"id\": \"a\", \"type\": \"aud.graph.filter\", \"preset\": {\"schema\": 1, \"type\": \"aud.graph.filter\", \"params\": {\"cutoff\": 5}}}]}",
       AUD_ERROR_INVALID_ARGUMENT, "outside its range"},
      {"{\"schema\": 1, \"outputChannels\": [1], \"nodes\": [{\"id\": \"a\", \"type\": \"aud.graph.filter\", \"preset\": {\"schema\": 1, \"type\": \"aud.graph.filter\", \"params\": {\"cutoff\": \"x\"}}}]}",
       AUD_ERROR_INVALID_ARGUMENT, "map ids to numbers"},
      {"{\"schema\": 1, \"outputChannels\": [1], \"nodes\": [{\"id\": \"a\", \"type\": \"aud.graph.filter\", \"preset\": {\"schema\": 1, \"type\": \"aud.graph.filter\", \"strings\": {\"x\": \"y\"}}}]}",
       AUD_ERROR_NOT_FOUND, "unknown string key x of node a"},
      {"{\"schema\": 1, \"outputChannels\": [1], \"nodes\": [{\"id\": \"a\", \"type\": \"aud.test.preset\", \"preset\": {\"schema\": 1, \"type\": \"aud.test.preset\", \"strings\": {\"file\": 1}}}]}",
       AUD_ERROR_INVALID_ARGUMENT, "map ids to strings"},
      {"{\"schema\": 1, \"outputChannels\": [1], \"nodes\": [{\"id\": \"a\", \"type\": \"aud.test.preset\", \"preset\": {\"schema\": 1, \"type\": \"aud.test.preset\", \"strings\": {\"file\": \"asset:nope\"}}}]}",
       AUD_ERROR_NOT_FOUND, "unknown asset nope"},
      {"{\"schema\": 1, \"outputChannels\": [1], \"nodes\": [{\"id\": \"a\", \"type\": \"aud.graph.filter\", \"preset\": {\"schema\": 1, \"type\": \"aud.graph.filter\", \"state\": \"AA==\", \"stateVersion\": 1}}]}",
       AUD_ERROR_UNSUPPORTED, "has no state"},
      {"{\"schema\": 1, \"outputChannels\": [1], \"nodes\": [{\"id\": \"a\", \"type\": \"aud.test.preset\", \"preset\": {\"schema\": 1, \"type\": \"aud.test.preset\", \"state\": \"AA==\", \"stateVersion\": 2}}]}",
       AUD_ERROR_STATE_VERSION, "state version of node a is 2, not 3"},
      {"{\"schema\": 1, \"outputChannels\": [1], \"nodes\": [{\"id\": \"a\", \"type\": \"aud.test.preset\", \"preset\": {\"schema\": 1, \"type\": \"aud.test.preset\", \"state\": \"$$\", \"stateVersion\": 3}}]}",
       AUD_ERROR_INVALID_ARGUMENT, "no base64"},
      {"{\"schema\": 1, \"outputChannels\": [1], \"nodes\": [{\"id\": \"a\", \"type\": \"aud.test.preset\", \"preset\": {\"schema\": 1, \"type\": \"aud.test.preset\", \"state\": \"AA==\"}}]}",
       AUD_ERROR_INVALID_ARGUMENT, "integer stateVersion"},
      {"{\"schema\": 1, \"outputChannels\": [1], \"nodes\": [{\"id\": \"a\", \"type\": \"aud.test.preset\", \"preset\": {\"schema\": 1, \"type\": \"aud.test.preset\", \"stateVersion\": 3}}]}",
       AUD_ERROR_INVALID_ARGUMENT, "needs a state"},
      {"{\"schema\": 1, \"outputChannels\": [1], \"nodes\": [{\"id\": \"a\", \"type\": \"aud.test.preset\", \"preset\": {\"schema\": 1, \"type\": \"aud.test.preset\", \"state\": \"AA==\", \"stateVersion\": 3}}]}",
       AUD_ERROR_INVALID_ARGUMENT, "refused its state"},
      {"{\"schema\": 1, \"outputChannels\": [1], \"nodes\": [{\"id\": \"a\", \"type\": \"aud.graph.filter\", \"preset\": {\"schema\": 2, \"type\": \"aud.graph.filter\"}}]}",
       AUD_ERROR_INVALID_ARGUMENT, "schema of the preset of node a must be 1"},
      {"{\"schema\": 1, \"outputChannels\": [1], \"nodes\": [{\"id\": \"a\", \"type\": \"aud.graph.filter\", \"preset\": {\"schema\": 1, \"type\": \"aud.graph.filter\", \"nodeVersion\": -1}}]}",
       AUD_ERROR_INVALID_ARGUMENT, "nodeVersion"},
      {"{\"schema\": 1, \"outputChannels\": [1], \"assets\": 1}", AUD_ERROR_INVALID_ARGUMENT, "assets must be a list"},
      {"{\"schema\": 1, \"outputChannels\": [1], \"assets\": [{\"id\": \"x\"}]}", AUD_ERROR_INVALID_ARGUMENT, "needs an id and a path"},
      {"{\"schema\": 1, \"outputChannels\": [1], \"assets\": [{\"id\": \"x\", \"path\": \"a\"}, {\"id\": \"x\", \"path\": \"b\"}]}",
       AUD_ERROR_INVALID_ARGUMENT, "duplicate asset id x"},
      {"{\"schema\": 1, \"outputChannels\": [1], \"assets\": [{\"id\": \"x\", \"path\": \"missing.txt\"}]}",
       AUD_ERROR_NOT_FOUND, "asset x not found"},
      {"{\"schema\": 1, \"outputChannels\": [2]}", AUD_ERROR_FORMAT, "do not match the graph"},
      {"{\"schema\": 1}", AUD_ERROR_FORMAT, "do not match the graph"},
      {"{\"schema\": 1, \"outputChannels\": [1], \"connections\": [{\"from\": \"z\", \"to\": \"graph\"}]}",
       AUD_ERROR_NOT_FOUND, "unknown node z"},
      {"{\"schema\": 1, \"outputChannels\": [1], \"connections\": [{\"from\": \"graph\"}]}",
       AUD_ERROR_INVALID_ARGUMENT, "needs from and to"},
      {"{\"schema\": 1, \"outputChannels\": [1], \"nodes\": [{\"id\": \"a\", \"type\": \"aud.graph.filter\"}], \"connections\": [{\"from\": \"a\", \"to\": \"graph\", \"fromBus\": 5}]}",
       AUD_ERROR_INVALID_ARGUMENT, "no such bus between a and graph"},
      {"{\"schema\": 1, \"outputChannels\": [1], \"nodes\": [{\"id\": \"a\", \"type\": \"aud.graph.filter\"}], \"connections\": [{\"from\": \"a\", \"to\": \"graph\", \"lowLatency\": 1}]}",
       AUD_ERROR_INVALID_ARGUMENT, "lowLatency must be a boolean"},
      {"{\"schema\": 1, \"outputChannels\": [1], \"nodes\": [{\"id\": \"a\", \"type\": \"aud.graph.filter\"}], \"eventConnections\": [{\"from\": \"graph\", \"to\": \"a\", \"toPort\": 3}]}",
       AUD_ERROR_INVALID_ARGUMENT, "no such event port"},
      {"{\"schema\": 1, \"outputChannels\": [1], \"nodes\": [{\"id\": \"a\", \"type\": \"aud.graph.filter\"}, {\"id\": \"b\", \"type\": \"aud.graph.filter\"}], \"connections\": [{\"from\": \"a\", \"to\": \"b\"}, {\"from\": \"b\", \"to\": \"a\"}, {\"from\": \"b\", \"to\": \"graph\"}]}",
       AUD_ERROR_CYCLE, "does not compile"},
      {"{\"schema\": 1, \"outputChannels\": [1], \"transport\": {\"tempo\": 0}}", AUD_ERROR_INVALID_ARGUMENT, "invalid transport"},
      {"{\"schema\": 1, \"outputChannels\": [1], \"transport\": {\"loopStart\": 1}}", AUD_ERROR_INVALID_ARGUMENT, "invalid transport"},
      {"{\"schema\": 1, \"outputChannels\": [1], \"transport\": {\"loopStart\": 4, \"loopEnd\": 4}}", AUD_ERROR_INVALID_ARGUMENT, "invalid transport"},
      {"{\"schema\": 1, \"outputChannels\": [1], \"transport\": {\"swing\": 1}}", AUD_ERROR_INVALID_ARGUMENT, "unknown key swing"},
      {"{\"schema\": 1, \"outputChannels\": [1], \"transport\": 4}", AUD_ERROR_INVALID_ARGUMENT, "invalid transport"},
  };
  for (const Case& c : cases) {
    const int32_t result = aud_host_load(host, c.json, std::strlen(c.json));
    AUD_CHECK(result == c.code);
    AUD_CHECK(contains(aud_host_last_error(host), c.message));
    AUD_CHECK(aud_host_num_nodes(host) == 0);
    AUD_CHECK(aud_graph_nodes(f.graph, nullptr, 0) == 0);
  }
  AUD_CHECK(aud_host_load(host, nullptr, 0) == AUD_ERROR_INVALID_ARGUMENT);
  AUD_CHECK(contains(aud_host_last_error(host), "no document text"));
  AUD_CHECK(aud_host_load(nullptr, "{}", 2) == AUD_ERROR_INVALID_ARGUMENT);
  // Two node ids whose parameter ids collide, found by a search over
  // n0, n1, ...: FNV-1a over such ids collides first near 750000 of them.
  AUD_CHECK(aud_host_param_id("n512789", "a") == aud_host_param_id("n749192", "a"));
  const char* colliding =
      "{\"schema\": 1, \"outputChannels\": [1], \"nodes\": ["
      "{\"id\": \"n512789\", \"type\": \"aud.test.preset\"}, "
      "{\"id\": \"n749192\", \"type\": \"aud.test.preset\"}]}";
  AUD_CHECK(aud_host_load(host, colliding, std::strlen(colliding)) == AUD_ERROR_DUPLICATE_TYPE);
  AUD_CHECK(contains(aud_host_last_error(host), "n512789/a and n749192/a collide") ||
            contains(aud_host_last_error(host), "n749192/a and n512789/a collide"));
  AUD_CHECK(aud_host_num_nodes(host) == 0);
  // A refused document leaves a loaded one alone.
  AUD_CHECK(load(host, referenceDocument()) == AUD_OK);
  AUD_CHECK(aud_host_load(host, cases[8].json, std::strlen(cases[8].json)) == AUD_ERROR_INVALID_ARGUMENT);
  AUD_CHECK(aud_host_num_nodes(host) == 3 && aud_graph_nodes(f.graph, nullptr, 0) == 3);
  aud_host_destroy(host);
}

AUD_TEST(host_replaces_the_document_while_rendering) {
  TempDir dir;
  Fixture f;
  AudHost* host = hostOf(f, dir);
  const char* loud =
      "{\"schema\": 1, \"outputChannels\": [1], \"nodes\": [{\"id\": \"osc\", "
      "\"type\": \"aud.graph.oscillator\", \"preset\": {\"schema\": 1, \"type\": "
      "\"aud.graph.oscillator\", \"params\": {\"frequency\": 20, \"waveform\": 2, "
      "\"amplitude\": 0.5}}}], \"connections\": [{\"from\": \"osc\", \"to\": \"graph\"}]}";
  const char* quiet =
      "{\"schema\": 1, \"outputChannels\": [1], \"nodes\": [{\"id\": \"osc\", "
      "\"type\": \"aud.graph.oscillator\", \"preset\": {\"schema\": 1, \"type\": "
      "\"aud.graph.oscillator\", \"params\": {\"frequency\": 20, \"waveform\": 2, "
      "\"amplitude\": 0.25}}}], \"connections\": [{\"from\": \"osc\", \"to\": \"graph\"}]}";
  AUD_CHECK(aud_host_load(host, loud, std::strlen(loud)) == AUD_OK);
  const int32_t first = aud_host_node(host, "osc");
  aud_graph_start(f.graph);
  f.render(256);
  AUD_CHECK_NEAR(f.out[0][100], 0.5, 1e-6);
  AUD_CHECK(aud_host_load(host, quiet, std::strlen(quiet)) == AUD_OK);
  const int32_t second = aud_host_node(host, "osc");
  AUD_CHECK(second != first && aud_host_num_nodes(host) == 1);
  // The old chain fades out, the new one fades in: the sum stays smooth.
  f.render(256);
  for (uint32_t i = 1; i < 256; ++i) {
    AUD_CHECK(std::fabs(f.out[0][i] - f.out[0][i - 1]) < 0.01f);
  }
  f.render(256);
  AUD_CHECK_NEAR(f.out[0][100], 0.25, 1e-6);
  AUD_CHECK(f.countNotifications(AUD_NOTIFY_NODE_DONE) == 1);
  AUD_CHECK(aud_graph_nodes(f.graph, nullptr, 0) == 1);
  aud_host_destroy(host);
}

AUD_TEST(host_applies_presets_and_relinks_assets) {
  TempDir dir;
  Fixture f;
  AudHost* host = hostOf(f, dir);
  AUD_CHECK(load(host, referenceDocument()) == AUD_OK);
  Preset* p = g_lastPreset;
  const uint32_t b = aud_host_param_id("preset", "b");
  const char* preset =
      "{\"schema\": 1, \"type\": \"aud.test.preset\", \"params\": {\"b\": 55}, "
      "\"strings\": {\"name\": \"World\"}}";
  AUD_CHECK(aud_host_apply_preset(host, "preset", preset, std::strlen(preset)) == AUD_OK);
  float value = 0;
  AUD_CHECK(aud_host_get_param(host, b, &value) == AUD_OK && value == 55.0f);
  AUD_CHECK(p->name == "World" && p->file == dir.path + "/song.txt");
  AUD_CHECK(aud_host_apply_preset(host, "nothing", preset, std::strlen(preset)) == AUD_ERROR_NOT_FOUND);
  AUD_CHECK(aud_host_apply_preset(host, "preset", "{", 1) == AUD_ERROR_INVALID_ARGUMENT);
  AUD_CHECK(aud_host_apply_preset(host, nullptr, preset, 1) == AUD_ERROR_INVALID_ARGUMENT);
  AUD_CHECK(aud_host_apply_preset(host, "preset", nullptr, 1) == AUD_ERROR_INVALID_ARGUMENT);
  AUD_CHECK(aud_host_apply_preset(nullptr, "preset", preset, 1) == AUD_ERROR_INVALID_ARGUMENT);
  const char* bad =
      "{\"schema\": 1, \"type\": \"aud.test.preset\", \"params\": {\"b\": 1, \"nope\": 2}}";
  AUD_CHECK(aud_host_apply_preset(host, "preset", bad, std::strlen(bad)) == AUD_ERROR_NOT_FOUND);
  AUD_CHECK(aud_host_get_param(host, b, &value) == AUD_OK && value == 55.0f);
  // A state blob through the preset.
  const std::string withState =
      "{\"schema\": 1, \"type\": \"aud.test.preset\", \"state\": \"" +
      presetState(0.5f) + "\", \"stateVersion\": 3}";
  AUD_CHECK(aud_host_apply_preset(host, "preset", withState.data(), withState.size()) == AUD_OK);
  AUD_CHECK(p->offset == 0.5f && p->loads == 2);
  // The preset of the node as it is now.
  size_t size = 0;
  aud_host_node_preset(host, "preset", nullptr, 0, &size);
  std::string text(size + 1, '\0');
  AUD_CHECK(aud_host_node_preset(host, "preset", &text[0], text.size(), &size) == AUD_OK);
  text.resize(size);
  aud::Json json;
  AUD_CHECK(aud::parseJson(text.data(), text.size(), &json, nullptr));
  AUD_CHECK(json.find("params")->find("b")->number == 55);
  AUD_CHECK(json.find("strings")->find("name")->string == "World");
  AUD_CHECK(json.find("strings")->find("file")->string == "asset:song");
  AUD_CHECK(json.find("params")->find("a")->number == 0.25);
  AUD_CHECK(json.find("state")->string == presetState(0.5f));
  // Relinking an asset applies the strings that name it again.
  dir.write("other.txt");
  AUD_CHECK(aud_host_set_asset_path(host, "song", "other.txt") == AUD_OK);
  AUD_CHECK(p->file == dir.path + "/other.txt");
  AudHostAsset asset{};
  asset.struct_size = sizeof(AudHostAsset);
  AUD_CHECK(aud_host_asset(host, 0, &asset) == AUD_OK);
  AUD_CHECK(std::string(asset.path) == "other.txt" && asset.exists == 1);
  // An absolute path inside the base directory is saved relative to it.
  const std::string absolute = dir.path + "/song.txt";
  AUD_CHECK(aud_host_set_asset_path(host, "song", absolute.c_str()) == AUD_OK);
  AUD_CHECK(aud_host_asset(host, 0, &asset) == AUD_OK);
  AUD_CHECK(std::string(asset.path) == "song.txt" && std::string(asset.resolved) == absolute);
  AUD_CHECK(p->file == absolute);
  AUD_CHECK(aud_host_set_asset_path(host, "song", "missing.txt") == AUD_ERROR_NOT_FOUND);
  AUD_CHECK(contains(aud_host_last_error(host), "asset song not found"));
  AUD_CHECK(p->file == absolute);
  AUD_CHECK(aud_host_set_asset_path(host, "nope", "song.txt") == AUD_ERROR_NOT_FOUND);
  AUD_CHECK(aud_host_set_asset_path(host, "song", "") == AUD_ERROR_INVALID_ARGUMENT);
  AUD_CHECK(aud_host_set_asset_path(nullptr, "song", "x") == AUD_ERROR_INVALID_ARGUMENT);
  // The saved document carries the relinked path.
  aud::Json doc;
  const std::string all = saved(host);
  AUD_CHECK(aud::parseJson(all.data(), all.size(), &doc, nullptr));
  AUD_CHECK(at(*doc.find("assets"), 0).find("path")->string == "song.txt");
  aud_host_destroy(host);
}

AUD_TEST(host_inspects_documents_without_a_graph) {
  AudHostDocumentInfo info{};
  info.struct_size = sizeof(AudHostDocumentInfo);
  const std::string text = referenceDocument();
  AUD_CHECK(aud_host_inspect(text.data(), text.size(), &info) == AUD_OK);
  AUD_CHECK(info.schema == 1 && info.num_input_buses == 0 && info.num_output_buses == 1);
  AUD_CHECK(info.output_channels[0] == 1 && info.num_nodes == 3 && info.num_assets == 1);
  AUD_CHECK(std::string(info.name) == "Reference chain");
  const char* bare = "{\"schema\": 1, \"inputChannels\": [2, 1]}";
  AUD_CHECK(aud_host_inspect(bare, std::strlen(bare), &info) == AUD_OK);
  AUD_CHECK(info.num_input_buses == 2 && info.input_channels[1] == 1);
  AUD_CHECK(info.num_output_buses == 1 && info.output_channels[0] == 2);
  AUD_CHECK(info.num_nodes == 0 && info.num_assets == 0 && info.name[0] == '\0');
  const char* bad[] = {"nope", "[]", "{\"schema\": 2}", "{\"schema\": 1, \"outputChannels\": 1}",
                       "{\"schema\": 1, \"nodes\": 1}", "{\"schema\": 1, \"name\": 1}"};
  for (const char* json : bad) {
    AUD_CHECK(aud_host_inspect(json, std::strlen(json), &info) == AUD_ERROR_INVALID_ARGUMENT);
  }
  std::string wide = "{\"schema\": 1, \"outputChannels\": [";
  for (int i = 0; i < 17; ++i) wide += i == 0 ? "1" : ", 1";
  wide += "]}";
  AUD_CHECK(aud_host_inspect(wide.data(), wide.size(), &info) == AUD_ERROR_CAPACITY);
  AUD_CHECK(aud_host_inspect(nullptr, 0, &info) == AUD_ERROR_INVALID_ARGUMENT);
  AUD_CHECK(aud_host_inspect(bare, std::strlen(bare), nullptr) == AUD_ERROR_INVALID_ARGUMENT);
  info.struct_size = 4;
  AUD_CHECK(aud_host_inspect(bare, std::strlen(bare), &info) == AUD_ERROR_INVALID_ARGUMENT);
}

AUD_TEST(host_renders_and_hands_out_the_events) {
  TempDir dir;
  Fixture f;
  AudHost* host = hostOf(f, dir);
  const char* json =
      "{\"schema\": 1, \"outputChannels\": [1], \"nodes\": [{\"id\": \"echo\", "
      "\"type\": \"aud.test.emitter\"}], \"eventConnections\": [{\"from\": "
      "\"graph\", \"to\": \"echo\"}, {\"from\": \"echo\", \"to\": \"graph\"}]}";
  AUD_CHECK(aud_host_load(host, json, std::strlen(json)) == AUD_OK);
  aud_graph_start(f.graph);
  AudEvent in = noteOn(64);
  in.sample_offset = 7;
  AudEvent out[2];
  f.hostRequest = f.renderRequest(256, nullptr, &in, 1);
  AudHostRenderRequest request{sizeof(AudHostRenderRequest), 0, &f.hostRequest, out, 2, 0, 0, 0};
  AUD_CHECK(aud_host_render(host, &request) == AUD_OK);
  AUD_CHECK(request.num_output_events == 1 && out[0].sample_offset == 7);
  AUD_CHECK(aud_ump_note(out[0].words[0]) == 64);
  AUD_CHECK(aud_host_render(nullptr, &request) == AUD_ERROR_INVALID_ARGUMENT);
  aud_host_destroy(host);
}
