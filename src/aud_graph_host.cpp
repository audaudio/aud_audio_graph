// @license
// Copyright (c) Audanika. All Rights Reserved.
//
// Use of this source code is governed by terms that can be
// found in the LICENSE file in the root of this package.

// The headless host (plugin-002, ticket 20): loads a graph document into a
// graph without Dart, applies and saves the presets, resolves the assets,
// derives the stable parameter ids and reports latency and tail. Every
// document is validated completely before anything is applied, so that a
// refused document leaves the graph as it was. Control thread only; the
// render call forwards to the engine.

#include <sys/stat.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <functional>
#include <new>
#include <string>
#include <utility>
#include <vector>

#include "aud_audio_graph.h"
#include "aud_json.hpp"
#include "aud_transport.h"

namespace aud {
namespace {

constexpr double kDocumentSchema = 1;
constexpr double kPresetSchema = 1;
constexpr const char* kGraphId = "graph";
constexpr const char* kAssetPrefix = "asset:";
constexpr size_t kAssetPrefixLength = 6;
// The output buses of a document that names none: one stereo bus, as
// AudGraphDocument of the Dart side has it.
constexpr uint32_t kDefaultOutputChannels = 2;
// The largest channel count of a bus, as the engine has it.
constexpr int64_t kMaxBusChannels = 64;
// The largest bus or port index a document may name.
constexpr int64_t kMaxIndex = 0xFFFF;
// A state blob is read into this many bytes first; a larger one asks
// again with its size, which parks the node a second time.
constexpr size_t kStateProbeBytes = 1024;
// FNV-1a, 32 bits; the top bit of an id is cleared.
constexpr uint32_t kFnvOffset = 2166136261u;
constexpr uint32_t kFnvPrime = 16777619u;
constexpr uint32_t kParamIdMask = 0x7FFFFFFFu;

// ............................................................................
// Errors

struct Error {
  int32_t code = AUD_OK;
  std::string message;

  bool fail(int32_t newCode, std::string newMessage) {
    code = newCode;
    message = std::move(newMessage);
    return false;
  }
};

std::string withCode(const std::string& message, int32_t code) {
  return message + " (" + std::to_string(code) + ")";
}

// ............................................................................
// The loaded document

struct HostConnection {
  std::string from;
  uint32_t fromBus = 0;
  std::string to;
  uint32_t toBus = 0;
  bool lowLatency = false;
};

struct HostEventConnection {
  std::string from;
  uint32_t fromPort = 0;
  std::string to;
  uint32_t toPort = 0;
};

struct HostAssetEntry {
  std::string id;
  std::string path;      // as the document names it
  std::string resolved;  // what the nodes receive
  bool exists = false;
};

// What a preset sets, read and checked against the node's type.
struct PresetParts {
  std::vector<std::pair<uint32_t, float>> params;         // index, value
  std::vector<std::pair<uint32_t, std::string>> strings;  // slot, raw value
  bool hasState = false;
  std::vector<uint8_t> state;
  uint32_t stateVersion = 0;
};

struct HostNode {
  std::string id;
  std::string type;
  const AudNodeDescriptor* descriptor = nullptr;
  int32_t handle = 0;
  std::vector<uint32_t> inputChannels;
  std::vector<uint32_t> outputChannels;
  uint32_t delayFrames = 0;
  std::vector<float> params;         // the current values by index
  std::vector<std::string> strings;  // by string key slot, raw values
  std::vector<bool> stringSet;
  PresetParts preset;  // what the document's preset sets
};

struct HostParamEntry {
  uint32_t id = 0;
  uint32_t node = 0;  // index into the nodes
  uint32_t index = 0;
};

struct HostTransport {
  double tempo = 120;
  uint32_t numerator = 4;
  uint32_t denominator = 4;
  bool looping = false;
  double loopStart = 0;
  double loopEnd = 0;
};

struct Document {
  std::string name;
  std::vector<uint32_t> inputChannels;
  std::vector<uint32_t> outputChannels;
  std::vector<HostAssetEntry> assets;
  std::vector<HostNode> nodes;
  std::vector<HostConnection> audio;
  std::vector<HostEventConnection> events;
  HostTransport transport;
  std::vector<HostParamEntry> params;  // sorted by id
};

}  // namespace
}  // namespace aud

struct AudHost {
  AudGraph* graph = nullptr;
  std::string baseDirectory;
  std::string lastError;
  aud::Document document;
};

namespace aud {
namespace {

// ............................................................................
// Helpers

bool isLetter(char c) { return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z'); }

bool isDigit(char c) { return c >= '0' && c <= '9'; }

// ^[A-Za-z][A-Za-z0-9_-]*$
bool isIdentifier(const std::string& text) {
  if (text.empty() || !isLetter(text[0])) return false;
  for (const char c : text) {
    if (!isLetter(c) && !isDigit(c) && c != '_' && c != '-') return false;
  }
  return true;
}

// ^[a-z][a-z0-9_]*(\.[a-z][a-z0-9_]*)+$
bool isTypeId(const std::string& text) {
  size_t parts = 0;
  size_t i = 0;
  while (true) {
    if (i >= text.size() || !(text[i] >= 'a' && text[i] <= 'z')) return false;
    ++i;
    while (i < text.size() && ((text[i] >= 'a' && text[i] <= 'z') ||
                               isDigit(text[i]) || text[i] == '_')) {
      ++i;
    }
    parts += 1;
    if (i == text.size()) return parts >= 2;
    if (text[i] != '.') return false;
    ++i;
  }
}

bool isAbsolute(const std::string& path) {
  if (path.empty()) return false;
  if (path[0] == '/') return true;
#if _WIN32
  if (path[0] == '\\') return true;
  if (path.size() > 1 && path[1] == ':') return true;
#endif
  return false;
}

std::string resolvePath(const std::string& base, const std::string& path) {
  if (base.empty() || isAbsolute(path)) return path;
  return base + "/" + path;
}

// The path relative to the base directory when it lies inside it.
std::string relativeToBase(const std::string& base, const std::string& path) {
  if (base.empty() || path.size() <= base.size() + 1) return path;
  if (path.compare(0, base.size(), base) != 0 || path[base.size()] != '/') {
    return path;
  }
  return path.substr(base.size() + 1);
}

bool fileExists(const std::string& path) {
#if _WIN32
  struct _stat info;
  return _stat(path.c_str(), &info) == 0;
#else
  struct stat info;
  return stat(path.c_str(), &info) == 0;
#endif
}

uint32_t fnv1a(uint32_t hash, const char* text) {
  for (; *text != '\0'; ++text) {
    hash ^= static_cast<unsigned char>(*text);
    hash *= kFnvPrime;
  }
  return hash;
}

// Writes a text into the caller's buffer with a terminator.
int32_t writeOut(const std::string& text, char* buffer, size_t capacity,
                 size_t* size) {
  if (size == nullptr || (buffer == nullptr && capacity > 0)) {
    return AUD_ERROR_INVALID_ARGUMENT;
  }
  *size = text.size();
  if (buffer == nullptr || capacity < text.size() + 1) {
    return AUD_ERROR_BUFFER_TOO_SMALL;
  }
  std::memcpy(buffer, text.data(), text.size());
  buffer[text.size()] = '\0';
  return AUD_OK;
}

const HostAssetEntry* assetOf(const std::vector<HostAssetEntry>& assets,
                              const std::string& id) {
  for (const HostAssetEntry& asset : assets) {
    if (asset.id == id) return &asset;
  }
  return nullptr;
}

bool isAssetReference(const std::string& value) {
  return value.compare(0, kAssetPrefixLength, kAssetPrefix) == 0;
}

std::string assetIdOf(const std::string& value) {
  return value.substr(kAssetPrefixLength);
}

// The value a node receives for a string setting: the resolved path of an
// asset reference, otherwise the value itself.
std::string stringValue(const std::vector<HostAssetEntry>& assets,
                        const std::string& raw) {
  if (!isAssetReference(raw)) return raw;
  const HostAssetEntry* asset = assetOf(assets, assetIdOf(raw));
  return asset == nullptr ? raw : asset->resolved;
}

HostNode* nodeOf(Document& document, const std::string& id) {
  for (HostNode& node : document.nodes) {
    if (node.id == id) return &node;
  }
  return nullptr;
}

int32_t handleOf(const Document& document, const std::string& id) {
  if (id == kGraphId) return AUD_GRAPH_NODE;
  for (const HostNode& node : document.nodes) {
    if (node.id == id) return node.handle;
  }
  return AUD_ERROR_NOT_FOUND;
}

bool inRange(const AudParamDescriptor& param, double value) {
  return value >= param.min_value && value <= param.max_value;
}

// ............................................................................
// Reading a document

bool readChannels(const Json* json, const std::string& what,
                  std::vector<uint32_t>* out, Error* error) {
  out->clear();
  if (json == nullptr) return true;
  if (!json->isArray()) {
    return error->fail(AUD_ERROR_INVALID_ARGUMENT,
                       what + " must list channel counts");
  }
  for (const Json& item : json->array) {
    if (!item.isInteger() || item.integer() < 1 ||
        item.integer() > kMaxBusChannels) {
      return error->fail(AUD_ERROR_INVALID_ARGUMENT,
                         what + " must list channel counts");
    }
    out->push_back(static_cast<uint32_t>(item.integer()));
  }
  return true;
}

bool readIndex(const Json* json, const std::string& what, uint32_t* out,
               Error* error) {
  *out = 0;
  if (json == nullptr) return true;
  if (!json->isInteger() || json->integer() < 0 || json->integer() > kMaxIndex) {
    return error->fail(AUD_ERROR_INVALID_ARGUMENT, what + " must be an index");
  }
  *out = static_cast<uint32_t>(json->integer());
  return true;
}

bool knownKeys(const Json& object, const std::vector<const char*>& keys,
               const std::string& what, Error* error) {
  for (const auto& member : object.object) {
    const bool known =
        std::any_of(keys.begin(), keys.end(),
                    [&](const char* key) { return member.first == key; });
    if (!known) {
      return error->fail(AUD_ERROR_INVALID_ARGUMENT,
                         "unknown key " + member.first + " in " + what);
    }
  }
  return true;
}

bool readPresetParams(const Json& json, const std::string& what,
                      const std::string& nodeId, const AudNodeDescriptor* d,
                      PresetParts* parts, Error* error) {
  const Json* params = json.find("params");
  if (params == nullptr) return true;
  if (!params->isObject()) {
    return error->fail(AUD_ERROR_INVALID_ARGUMENT,
                       "params of " + what + " must map ids to numbers");
  }
  for (const auto& member : params->object) {
    if (!member.second.isNumber()) {
      return error->fail(AUD_ERROR_INVALID_ARGUMENT,
                         "params of " + what + " must map ids to numbers");
    }
    uint32_t index = UINT32_MAX;
    for (uint32_t i = 0; i < d->num_params; ++i) {
      if (member.first == d->params[i].id) index = i;
    }
    if (index == UINT32_MAX) {
      return error->fail(AUD_ERROR_NOT_FOUND, "unknown param " + member.first +
                                                  " of node " + nodeId);
    }
    if (!inRange(d->params[index], member.second.number)) {
      return error->fail(AUD_ERROR_INVALID_ARGUMENT,
                         "param " + member.first + " of node " + nodeId +
                             " is outside its range");
    }
    parts->params.emplace_back(index, static_cast<float>(member.second.number));
  }
  return true;
}

bool readPresetStrings(const Json& json, const std::string& what,
                       const std::string& nodeId, const AudNodeDescriptor* d,
                       const std::vector<HostAssetEntry>& assets,
                       PresetParts* parts, Error* error) {
  const Json* strings = json.find("strings");
  if (strings == nullptr) return true;
  if (!strings->isObject()) {
    return error->fail(AUD_ERROR_INVALID_ARGUMENT,
                       "strings of " + what + " must map ids to strings");
  }
  for (const auto& member : strings->object) {
    if (!member.second.isString()) {
      return error->fail(AUD_ERROR_INVALID_ARGUMENT,
                         "strings of " + what + " must map ids to strings");
    }
    uint32_t slot = UINT32_MAX;
    for (uint32_t i = 0; i < d->num_string_keys; ++i) {
      if (member.first == d->string_keys[i].id) slot = i;
    }
    if (slot == UINT32_MAX || (d->capabilities & AUD_NODE_CAP_STRINGS) == 0) {
      return error->fail(AUD_ERROR_NOT_FOUND, "unknown string key " +
                                                  member.first + " of node " +
                                                  nodeId);
    }
    const std::string& raw = member.second.string;
    if (isAssetReference(raw) && assetOf(assets, assetIdOf(raw)) == nullptr) {
      return error->fail(AUD_ERROR_NOT_FOUND,
                         "unknown asset " + assetIdOf(raw) + " in string " +
                             member.first + " of node " + nodeId);
    }
    parts->strings.emplace_back(slot, raw);
  }
  return true;
}

bool readPresetState(const Json& json, const std::string& what,
                     const std::string& nodeId, const AudNodeDescriptor* d,
                     PresetParts* parts, Error* error) {
  const Json* state = json.find("state");
  const Json* version = json.find("stateVersion");
  if (state == nullptr) {
    if (version != nullptr) {
      return error->fail(AUD_ERROR_INVALID_ARGUMENT,
                         "stateVersion of " + what + " needs a state");
    }
    return true;
  }
  if (!state->isString() || version == nullptr || !version->isInteger() ||
      version->integer() < 0 || version->integer() > UINT32_MAX) {
    return error->fail(AUD_ERROR_INVALID_ARGUMENT,
                       "state of " + what +
                           " must be base64 with an integer stateVersion");
  }
  if ((d->capabilities & AUD_NODE_CAP_STATE) == 0) {
    return error->fail(AUD_ERROR_UNSUPPORTED,
                       std::string("node type ") + d->type_id + " has no state");
  }
  if (static_cast<uint32_t>(version->integer()) != d->state_version) {
    return error->fail(AUD_ERROR_STATE_VERSION,
                       "state version of node " + nodeId + " is " +
                           std::to_string(version->integer()) + ", not " +
                           std::to_string(d->state_version));
  }
  if (!base64Decode(state->string, &parts->state)) {
    return error->fail(AUD_ERROR_INVALID_ARGUMENT,
                       "state of " + what + " is no base64");
  }
  parts->hasState = true;
  parts->stateVersion = static_cast<uint32_t>(version->integer());
  return true;
}

// Reads a node preset against the node's type.
bool readPreset(const Json& json, const std::string& nodeId,
                const std::string& type, const AudNodeDescriptor* d,
                const std::vector<HostAssetEntry>& assets, PresetParts* parts,
                Error* error) {
  const std::string what = "the preset of node " + nodeId;
  *parts = PresetParts();
  if (!json.isObject()) {
    return error->fail(AUD_ERROR_INVALID_ARGUMENT, what + " must be an object");
  }
  if (!knownKeys(json,
                 {"schema", "type", "name", "nodeVersion", "params", "strings",
                  "state", "stateVersion"},
                 what, error)) {
    return false;
  }
  const Json* schema = json.find("schema");
  if (schema == nullptr || !schema->isNumber() ||
      schema->number != kPresetSchema) {
    return error->fail(AUD_ERROR_INVALID_ARGUMENT,
                       "schema of " + what + " must be 1");
  }
  const Json* presetType = json.find("type");
  if (presetType == nullptr || !presetType->isString() ||
      presetType->string != type) {
    return error->fail(AUD_ERROR_INVALID_ARGUMENT,
                       "type of " + what + " is not " + type);
  }
  const Json* nodeVersion = json.find("nodeVersion");
  if (nodeVersion != nullptr &&
      (!nodeVersion->isInteger() || nodeVersion->integer() < 0)) {
    return error->fail(AUD_ERROR_INVALID_ARGUMENT,
                       "nodeVersion of " + what + " must be an integer");
  }
  const Json* name = json.find("name");
  if (name != nullptr && !name->isString()) {
    return error->fail(AUD_ERROR_INVALID_ARGUMENT,
                       "name of " + what + " must be a string");
  }
  return readPresetParams(json, what, nodeId, d, parts, error) &&
         readPresetStrings(json, what, nodeId, d, assets, parts, error) &&
         readPresetState(json, what, nodeId, d, parts, error);
}

bool checkBusChannels(const std::vector<uint32_t>& channels,
                      const AudBusDescriptor* buses, const char* direction,
                      const std::string& nodeId, Error* error) {
  for (size_t b = 0; b < channels.size(); ++b) {
    if (channels[b] < buses[b].min_channels ||
        channels[b] > buses[b].max_channels) {
      return error->fail(AUD_ERROR_FORMAT,
                         std::string(direction) + " bus " + std::to_string(b) +
                             " of node " + nodeId +
                             " has an unsupported channel count");
    }
  }
  return true;
}

bool readNode(AudGraph* graph, const Json& json, Document* document,
              Error* error) {
  if (!json.isObject()) {
    return error->fail(AUD_ERROR_INVALID_ARGUMENT, "a node must be an object");
  }
  const Json* id = json.find("id");
  if (id == nullptr || !id->isString() || !isIdentifier(id->string)) {
    return error->fail(AUD_ERROR_INVALID_ARGUMENT,
                       "a node needs an id like osc1");
  }
  HostNode node;
  node.id = id->string;
  if (node.id == kGraphId) {
    return error->fail(AUD_ERROR_INVALID_ARGUMENT,
                       "a node must not be named graph");
  }
  if (nodeOf(*document, node.id) != nullptr) {
    return error->fail(AUD_ERROR_INVALID_ARGUMENT,
                       "duplicate node id " + node.id);
  }
  if (!knownKeys(json,
                 {"id", "type", "inputChannels", "outputChannels",
                  "delayFrames", "preset"},
                 "node " + node.id, error)) {
    return false;
  }
  const Json* type = json.find("type");
  if (type == nullptr || !type->isString() || !isTypeId(type->string)) {
    return error->fail(AUD_ERROR_INVALID_ARGUMENT,
                       "node " + node.id + " needs a type like aud.graph.filter");
  }
  node.type = type->string;
  node.descriptor = aud_graph_node_type_by_id(graph, node.type.c_str());
  if (node.descriptor == nullptr) {
    return error->fail(AUD_ERROR_UNKNOWN_TYPE, "unknown node type " +
                                                   node.type + " of node " +
                                                   node.id);
  }
  const AudNodeDescriptor* d = node.descriptor;
  if (!readChannels(json.find("inputChannels"), "inputChannels of " + node.id,
                    &node.inputChannels, error) ||
      !readChannels(json.find("outputChannels"),
                    "outputChannels of " + node.id, &node.outputChannels,
                    error)) {
    return false;
  }
  if ((!node.inputChannels.empty() &&
       node.inputChannels.size() != d->num_input_buses) ||
      (!node.outputChannels.empty() &&
       node.outputChannels.size() != d->num_output_buses)) {
    return error->fail(AUD_ERROR_FORMAT, "the buses of node " + node.id +
                                             " do not fit " + node.type);
  }
  if (!checkBusChannels(node.inputChannels, d->input_buses, "input", node.id,
                        error) ||
      !checkBusChannels(node.outputChannels, d->output_buses, "output",
                        node.id, error)) {
    return false;
  }
  if (const Json* delay = json.find("delayFrames")) {
    if (!delay->isInteger() || delay->integer() < 0 ||
        delay->integer() > UINT32_MAX) {
      return error->fail(AUD_ERROR_INVALID_ARGUMENT,
                         "delayFrames of " + node.id + " must be a count");
    }
    node.delayFrames = static_cast<uint32_t>(delay->integer());
  }
  node.params.assign(d->num_params, 0.0f);
  for (uint32_t i = 0; i < d->num_params; ++i) {
    node.params[i] = d->params[i].default_value;
  }
  node.strings.assign(d->num_string_keys, std::string());
  node.stringSet.assign(d->num_string_keys, false);
  if (const Json* preset = json.find("preset")) {
    if (!readPreset(*preset, node.id, node.type, d, document->assets,
                    &node.preset, error)) {
      return false;
    }
  }
  document->nodes.push_back(std::move(node));
  return true;
}

bool knownEnd(const Document& document, const std::string& id) {
  if (id == kGraphId) return true;
  return std::any_of(document.nodes.begin(), document.nodes.end(),
                     [&](const HostNode& node) { return node.id == id; });
}

// The buses or event ports of an end of a connection: of the graph for
// "graph", otherwise of the node.
uint32_t endCount(const Document& document, const std::string& id,
                  bool output, bool events) {
  if (id == kGraphId) {
    if (events) return 1;
    return static_cast<uint32_t>(output ? document.inputChannels.size()
                                        : document.outputChannels.size());
  }
  for (const HostNode& node : document.nodes) {
    if (node.id != id) continue;
    const AudNodeDescriptor* d = node.descriptor;
    if (events) return output ? d->num_event_outputs : d->num_event_inputs;
    return output ? d->num_output_buses : d->num_input_buses;
  }
  return 0;
}

bool readEnds(const Json& json, const Document& document,
              const std::string& what, std::string* from, std::string* to,
              Error* error) {
  if (!json.isObject()) {
    return error->fail(AUD_ERROR_INVALID_ARGUMENT, what + " must be an object");
  }
  const Json* f = json.find("from");
  const Json* t = json.find("to");
  if (f == nullptr || t == nullptr || !f->isString() || !t->isString()) {
    return error->fail(AUD_ERROR_INVALID_ARGUMENT, what + " needs from and to");
  }
  if (!knownEnd(document, f->string)) {
    return error->fail(AUD_ERROR_NOT_FOUND, "unknown node " + f->string);
  }
  if (!knownEnd(document, t->string)) {
    return error->fail(AUD_ERROR_NOT_FOUND, "unknown node " + t->string);
  }
  *from = f->string;
  *to = t->string;
  return true;
}

bool readConnection(const Json& json, Document* document, Error* error) {
  HostConnection c;
  if (!readEnds(json, *document, "a connection", &c.from, &c.to, error) ||
      !knownKeys(json, {"from", "fromBus", "to", "toBus", "lowLatency"},
                 "a connection", error) ||
      !readIndex(json.find("fromBus"), "fromBus", &c.fromBus, error) ||
      !readIndex(json.find("toBus"), "toBus", &c.toBus, error)) {
    return false;
  }
  if (const Json* low = json.find("lowLatency")) {
    if (!low->isBool()) {
      return error->fail(AUD_ERROR_INVALID_ARGUMENT,
                         "lowLatency must be a boolean");
    }
    c.lowLatency = low->boolean;
  }
  if (c.fromBus >= endCount(*document, c.from, true, false) ||
      c.toBus >= endCount(*document, c.to, false, false)) {
    return error->fail(AUD_ERROR_INVALID_ARGUMENT,
                       "no such bus between " + c.from + " and " + c.to);
  }
  document->audio.push_back(c);
  return true;
}

bool readEventConnection(const Json& json, Document* document, Error* error) {
  HostEventConnection c;
  if (!readEnds(json, *document, "an event connection", &c.from, &c.to,
                error) ||
      !knownKeys(json, {"from", "fromPort", "to", "toPort"},
                 "an event connection", error) ||
      !readIndex(json.find("fromPort"), "fromPort", &c.fromPort, error) ||
      !readIndex(json.find("toPort"), "toPort", &c.toPort, error)) {
    return false;
  }
  if (c.fromPort >= endCount(*document, c.from, true, true) ||
      c.toPort >= endCount(*document, c.to, false, true)) {
    return error->fail(AUD_ERROR_INVALID_ARGUMENT,
                       "no such event port between " + c.from + " and " + c.to);
  }
  document->events.push_back(c);
  return true;
}

bool readTransport(const Json* json, HostTransport* transport, Error* error) {
  *transport = HostTransport();
  if (json == nullptr) return true;
  if (!json->isObject()) {
    return error->fail(AUD_ERROR_INVALID_ARGUMENT, "invalid transport settings");
  }
  if (!knownKeys(*json,
                 {"tempo", "numerator", "denominator", "loopStart", "loopEnd"},
                 "transport", error)) {
    return false;
  }
  const Json* tempo = json->find("tempo");
  const Json* numerator = json->find("numerator");
  const Json* denominator = json->find("denominator");
  const Json* loopStart = json->find("loopStart");
  const Json* loopEnd = json->find("loopEnd");
  const auto positiveInteger = [](const Json* value) {
    return value == nullptr ||
           (value->isInteger() && value->integer() >= 1 &&
            value->integer() <= UINT32_MAX);
  };
  if ((tempo != nullptr && (!tempo->isNumber() || !(tempo->number > 0))) ||
      !positiveInteger(numerator) || !positiveInteger(denominator) ||
      (loopStart != nullptr && !loopStart->isNumber()) ||
      (loopEnd != nullptr && !loopEnd->isNumber()) ||
      ((loopStart == nullptr) != (loopEnd == nullptr)) ||
      (loopStart != nullptr && !(loopEnd->number > loopStart->number))) {
    // The engine loops only when the end lies after the start; a document
    // that names another loop would lose it on the next save.
    return error->fail(AUD_ERROR_INVALID_ARGUMENT, "invalid transport settings");
  }
  if (tempo != nullptr) transport->tempo = tempo->number;
  if (numerator != nullptr) {
    transport->numerator = static_cast<uint32_t>(numerator->integer());
  }
  if (denominator != nullptr) {
    transport->denominator = static_cast<uint32_t>(denominator->integer());
  }
  if (loopStart != nullptr) {
    transport->looping = true;
    transport->loopStart = loopStart->number;
    transport->loopEnd = loopEnd->number;
  }
  return true;
}

bool readAsset(const Json& json, Document* document, Error* error) {
  if (!json.isObject()) {
    return error->fail(AUD_ERROR_INVALID_ARGUMENT, "an asset must be an object");
  }
  if (!knownKeys(json, {"id", "path"}, "an asset", error)) return false;
  const Json* id = json.find("id");
  const Json* path = json.find("path");
  if (id == nullptr || !id->isString() || !isIdentifier(id->string) ||
      path == nullptr || !path->isString() || path->string.empty()) {
    return error->fail(AUD_ERROR_INVALID_ARGUMENT,
                       "an asset needs an id and a path");
  }
  if (assetOf(document->assets, id->string) != nullptr) {
    return error->fail(AUD_ERROR_INVALID_ARGUMENT,
                       "duplicate asset id " + id->string);
  }
  HostAssetEntry asset;
  asset.id = id->string;
  asset.path = path->string;
  document->assets.push_back(asset);
  return true;
}

bool readList(const Json* json, const char* what, Error* error,
              const std::function<bool(const Json&)>& each) {
  if (json == nullptr) return true;
  if (!json->isArray()) {
    return error->fail(AUD_ERROR_INVALID_ARGUMENT,
                       std::string(what) + " must be a list");
  }
  return std::all_of(json->array.begin(), json->array.end(), each);
}

// Reads and validates a whole document against the graph's node types.
bool readDocument(AudGraph* graph, const Json& root, Document* document,
                  Error* error) {
  if (!root.isObject()) {
    return error->fail(AUD_ERROR_INVALID_ARGUMENT,
                       "a document must be an object");
  }
  if (!knownKeys(root,
                 {"schema", "name", "inputChannels", "outputChannels", "assets",
                  "nodes", "connections", "eventConnections", "transport"},
                 "the document", error)) {
    return false;
  }
  const Json* schema = root.find("schema");
  if (schema == nullptr || !schema->isNumber() ||
      schema->number != kDocumentSchema) {
    return error->fail(AUD_ERROR_INVALID_ARGUMENT, "schema must be 1");
  }
  if (const Json* name = root.find("name")) {
    if (!name->isString()) {
      return error->fail(AUD_ERROR_INVALID_ARGUMENT, "name must be a string");
    }
    document->name = name->string;
  }
  if (!readChannels(root.find("inputChannels"), "inputChannels",
                    &document->inputChannels, error) ||
      !readChannels(root.find("outputChannels"), "outputChannels",
                    &document->outputChannels, error)) {
    return false;
  }
  if (root.find("outputChannels") == nullptr) {
    document->outputChannels.assign(1, kDefaultOutputChannels);
  }
  return readList(root.find("assets"), "assets", error,
                  [&](const Json& item) {
                    return readAsset(item, document, error);
                  }) &&
         readList(root.find("nodes"), "nodes", error,
                  [&](const Json& item) {
                    return readNode(graph, item, document, error);
                  }) &&
         readList(root.find("connections"), "connections", error,
                  [&](const Json& item) {
                    return readConnection(item, document, error);
                  }) &&
         readList(root.find("eventConnections"), "eventConnections", error,
                  [&](const Json& item) {
                    return readEventConnection(item, document, error);
                  }) &&
         readTransport(root.find("transport"), &document->transport, error);
}

// The buses of the graph: node 0's outputs are the graph's inputs.
std::vector<uint32_t> graphBuses(AudGraph* graph, bool inputs) {
  std::vector<uint32_t> channels;
  for (uint32_t b = 0;; ++b) {
    const int32_t count =
        aud_graph_node_channels(graph, AUD_GRAPH_NODE, inputs ? 1 : 0, b);
    if (count < 0) return channels;
    channels.push_back(static_cast<uint32_t>(count));
  }
}

bool checkBuses(AudGraph* graph, const Document& document, Error* error) {
  if (graphBuses(graph, true) != document.inputChannels ||
      graphBuses(graph, false) != document.outputChannels) {
    return error->fail(AUD_ERROR_FORMAT,
                       "the buses of the document do not match the graph");
  }
  return true;
}

// Resolves the assets and checks that every file exists.
bool checkAssets(const std::string& base, std::vector<HostAssetEntry>* assets,
                 Error* error) {
  for (HostAssetEntry& asset : *assets) {
    asset.resolved = resolvePath(base, asset.path);
    asset.exists = fileExists(asset.resolved);
    if (!asset.exists) {
      return error->fail(AUD_ERROR_NOT_FOUND,
                         "asset " + asset.id + " not found: " + asset.resolved);
    }
  }
  return true;
}

// Derives the stable parameter ids and refuses collisions.
bool buildParams(Document* document, Error* error) {
  document->params.clear();
  for (uint32_t n = 0; n < document->nodes.size(); ++n) {
    const HostNode& node = document->nodes[n];
    for (uint32_t i = 0; i < node.descriptor->num_params; ++i) {
      HostParamEntry entry;
      entry.id =
          aud_host_param_id(node.id.c_str(), node.descriptor->params[i].id);
      entry.node = n;
      entry.index = i;
      document->params.push_back(entry);
    }
  }
  std::sort(document->params.begin(), document->params.end(),
            [](const HostParamEntry& a, const HostParamEntry& b) {
              return a.id < b.id;
            });
  for (size_t i = 1; i < document->params.size(); ++i) {
    const HostParamEntry& a = document->params[i - 1];
    const HostParamEntry& b = document->params[i];
    if (a.id != b.id) continue;
    const HostNode& na = document->nodes[a.node];
    const HostNode& nb = document->nodes[b.node];
    return error->fail(AUD_ERROR_DUPLICATE_TYPE,
                       "the parameter ids of " + na.id + "/" +
                           na.descriptor->params[a.index].id + " and " +
                           nb.id + "/" + nb.descriptor->params[b.index].id +
                           " collide");
  }
  return true;
}

// ............................................................................
// Applying a document

// Sends the transport settings of a document.
int32_t applyTransport(AudGraph* graph, const HostTransport& t) {
  AudTransportRequest request{};
  request.struct_size = sizeof(AudTransportRequest);
  request.at.struct_size = sizeof(AudTimestamp);
  request.at.domain = AUD_TIME_IMMEDIATE;
  request.type = AUD_TRANSPORT_REQUEST_SET_TEMPO;
  request.value = t.tempo;
  int32_t result = aud_graph_transport(graph, &request);
  if (result != AUD_OK) return result;
  request.type = AUD_TRANSPORT_REQUEST_SET_TIME_SIGNATURE;
  request.numerator = t.numerator;
  request.denominator = t.denominator;
  result = aud_graph_transport(graph, &request);
  if (result != AUD_OK) return result;
  request.type = AUD_TRANSPORT_REQUEST_SET_LOOP;
  request.beat = t.looping ? aud_ticks_from_beats(t.loopStart) : 0;
  request.beat_end = t.looping ? aud_ticks_from_beats(t.loopEnd) : 0;
  return aud_graph_transport(graph, &request);
}

// Frees instances that were created but never committed.
void dropCreated(AudGraph* graph, const std::vector<int32_t>& created) {
  if (created.empty() || aud_graph_begin(graph) != AUD_OK) return;
  for (const int32_t handle : created) aud_graph_remove_node(graph, handle);
  aud_graph_commit(graph);
}

// Applies what a preset sets in the order strings, state, parameters and
// records the values in the host's tables.
bool applyParts(AudGraph* graph, const PresetParts& parts,
                const std::vector<HostAssetEntry>& assets, HostNode* node,
                Error* error) {
  const AudNodeDescriptor* d = node->descriptor;
  for (const auto& string : parts.strings) {
    const uint32_t slot = string.first;
    const std::string value = stringValue(assets, string.second);
    const int32_t result = aud_graph_set_string(
        graph, node->handle, d->string_keys[slot].key, value.c_str());
    if (result != AUD_OK) {
      return error->fail(result, withCode("node " + node->id +
                                              " refused string " +
                                              d->string_keys[slot].id,
                                          result));
    }
    node->strings[slot] = string.second;
    node->stringSet[slot] = true;
  }
  if (parts.hasState) {
    const int32_t result = aud_graph_node_load_state(
        graph, node->handle, parts.state.data(), parts.state.size(),
        parts.stateVersion);
    if (result != AUD_OK) {
      return error->fail(
          result, withCode("node " + node->id + " refused its state", result));
    }
  }
  for (const auto& param : parts.params) {
    const int32_t result =
        aud_graph_set_param(graph, node->handle, param.first, param.second, 0);
    if (result != AUD_OK) {
      return error->fail(result, withCode("node " + node->id +
                                              " refused param " +
                                              d->params[param.first].id,
                                          result));
    }
    node->params[param.first] = param.second;
  }
  return true;
}

int32_t createNode(AudGraph* graph, HostNode* node) {
  AudNodeConfig config{sizeof(AudNodeConfig), 0, nullptr, 0, nullptr,
                       node->delayFrames, 0};
  if (!node->inputChannels.empty()) {
    config.num_input_buses = static_cast<uint32_t>(node->inputChannels.size());
    config.input_channels = node->inputChannels.data();
  }
  if (!node->outputChannels.empty()) {
    config.num_output_buses =
        static_cast<uint32_t>(node->outputChannels.size());
    config.output_channels = node->outputChannels.data();
  }
  const int32_t handle =
      aud_graph_create_node(graph, node->type.c_str(), &config);
  if (handle < 0) return handle;
  node->handle = handle;
  // The channels as the engine made them.
  node->inputChannels.clear();
  node->outputChannels.clear();
  for (uint32_t b = 0; b < node->descriptor->num_input_buses; ++b) {
    node->inputChannels.push_back(
        static_cast<uint32_t>(aud_graph_node_channels(graph, handle, 0, b)));
  }
  for (uint32_t b = 0; b < node->descriptor->num_output_buses; ++b) {
    node->outputChannels.push_back(
        static_cast<uint32_t>(aud_graph_node_channels(graph, handle, 1, b)));
  }
  return handle;
}

bool connectAll(AudGraph* graph, const Document& document, Error* error) {
  for (const HostConnection& c : document.audio) {
    const int32_t result = aud_graph_connect(
        graph, handleOf(document, c.from), c.fromBus, handleOf(document, c.to),
        c.toBus, c.lowLatency ? AUD_CONNECTION_LOW_LATENCY : 0);
    if (result != AUD_OK) {
      return error->fail(
          result, withCode("could not connect " + c.from + " to " + c.to, result));
    }
  }
  for (const HostEventConnection& c : document.events) {
    const int32_t result = aud_graph_connect_events(
        graph, handleOf(document, c.from), c.fromPort, handleOf(document, c.to),
        c.toPort);
    if (result != AUD_OK) {
      return error->fail(result, withCode("could not connect the events of " +
                                              c.from + " to " + c.to,
                                          result));
    }
  }
  return true;
}

// Creates the nodes, applies their presets and commits the topology in one
// transaction while the nodes of the previous document retire; on a
// failure the graph is left as it was.
bool applyDocument(AudHost* host, Document* document, Error* error) {
  AudGraph* graph = host->graph;
  if (aud_graph_begin(graph) != AUD_OK) {
    return error->fail(AUD_ERROR_STATE, "a transaction is open on the graph");
  }
  std::vector<int32_t> created;
  const auto undo = [&]() {
    aud_graph_rollback(graph);
    dropCreated(graph, created);
    return false;
  };
  for (HostNode& node : document->nodes) {
    const int32_t handle = createNode(graph, &node);
    if (handle < 0) {
      error->fail(handle, withCode("could not create node " + node.id, handle));
      return undo();
    }
    created.push_back(handle);
    if (!applyParts(graph, node.preset, document->assets, &node, error)) {
      return undo();
    }
  }
  for (const HostNode& previous : host->document.nodes) {
    aud_graph_remove_node(graph, previous.handle);
  }
  if (!connectAll(graph, *document, error)) return undo();
  const int32_t revision = aud_graph_commit(graph);
  if (revision < 0) {
    error->fail(revision, withCode("the document does not compile", revision));
    return undo();
  }
  return true;
}

// ............................................................................
// Writing a document

Json channelsJson(const std::vector<uint32_t>& channels) {
  Json list = Json::ofArray();
  for (const uint32_t c : channels) list.push(Json::ofNumber(c));
  return list;
}

bool saveNodeState(AudGraph* graph, const HostNode& node,
                   std::vector<uint8_t>* blob, Error* error) {
  blob->assign(kStateProbeBytes, 0);
  size_t size = 0;
  int32_t result =
      aud_graph_node_save_state(graph, node.handle, blob->data(), blob->size(), &size);
  if (result == AUD_ERROR_BUFFER_TOO_SMALL) {
    blob->assign(size, 0);
    result = aud_graph_node_save_state(graph, node.handle, blob->data(),
                                       blob->size(), &size);
  }
  if (result != AUD_OK) {
    return error->fail(
        result, withCode("node " + node.id + " did not save its state", result));
  }
  blob->resize(size);
  return true;
}

bool presetJson(AudHost* host, const HostNode& node, Json* out, Error* error) {
  const AudNodeDescriptor* d = node.descriptor;
  Json preset = Json::ofObject();
  preset.set("schema", Json::ofNumber(kPresetSchema));
  preset.set("type", Json::ofString(node.type));
  preset.set("nodeVersion", Json::ofNumber(d->version));
  Json params = Json::ofObject();
  for (uint32_t i = 0; i < d->num_params; ++i) {
    params.set(d->params[i].id, Json::ofNumber(node.params[i]));
  }
  preset.set("params", std::move(params));
  Json strings = Json::ofObject();
  for (uint32_t i = 0; i < d->num_string_keys; ++i) {
    if (!node.stringSet[i]) continue;
    strings.set(d->string_keys[i].id, Json::ofString(node.strings[i]));
  }
  if (!strings.object.empty()) preset.set("strings", std::move(strings));
  if (d->capabilities & AUD_NODE_CAP_STATE) {
    std::vector<uint8_t> blob;
    if (!saveNodeState(host->graph, node, &blob, error)) return false;
    preset.set("state", Json::ofString(base64Encode(blob.data(), blob.size())));
    preset.set("stateVersion", Json::ofNumber(d->state_version));
  }
  *out = std::move(preset);
  return true;
}

Json connectionsJson(const Document& document) {
  Json connections = Json::ofArray();
  for (const HostConnection& c : document.audio) {
    Json entry = Json::ofObject();
    entry.set("from", Json::ofString(c.from));
    if (c.fromBus != 0) entry.set("fromBus", Json::ofNumber(c.fromBus));
    entry.set("to", Json::ofString(c.to));
    if (c.toBus != 0) entry.set("toBus", Json::ofNumber(c.toBus));
    if (c.lowLatency) entry.set("lowLatency", Json::ofBool(true));
    connections.push(std::move(entry));
  }
  return connections;
}

Json eventConnectionsJson(const Document& document) {
  Json events = Json::ofArray();
  for (const HostEventConnection& c : document.events) {
    Json entry = Json::ofObject();
    entry.set("from", Json::ofString(c.from));
    if (c.fromPort != 0) entry.set("fromPort", Json::ofNumber(c.fromPort));
    entry.set("to", Json::ofString(c.to));
    if (c.toPort != 0) entry.set("toPort", Json::ofNumber(c.toPort));
    events.push(std::move(entry));
  }
  return events;
}

Json transportJson(AudGraph* graph) {
  AudGraphTransportState state{};
  state.struct_size = sizeof(AudGraphTransportState);
  aud_graph_transport_state(graph, &state);
  Json transport = Json::ofObject();
  transport.set("tempo", Json::ofNumber(state.tempo));
  transport.set("numerator", Json::ofNumber(state.numerator));
  transport.set("denominator", Json::ofNumber(state.denominator));
  if (state.looping) {
    transport.set("loopStart",
                  Json::ofNumber(aud_beats_from_ticks(state.loop_start)));
    transport.set("loopEnd",
                  Json::ofNumber(aud_beats_from_ticks(state.loop_end)));
  }
  return transport;
}

bool documentJson(AudHost* host, Json* out, Error* error) {
  const Document& document = host->document;
  Json root = Json::ofObject();
  root.set("schema", Json::ofNumber(kDocumentSchema));
  if (!document.name.empty()) root.set("name", Json::ofString(document.name));
  root.set("inputChannels", channelsJson(graphBuses(host->graph, true)));
  root.set("outputChannels", channelsJson(graphBuses(host->graph, false)));
  if (!document.assets.empty()) {
    Json assets = Json::ofArray();
    for (const HostAssetEntry& asset : document.assets) {
      Json entry = Json::ofObject();
      entry.set("id", Json::ofString(asset.id));
      entry.set("path", Json::ofString(asset.path));
      assets.push(std::move(entry));
    }
    root.set("assets", std::move(assets));
  }
  Json nodes = Json::ofArray();
  for (const HostNode& node : document.nodes) {
    Json entry = Json::ofObject();
    entry.set("id", Json::ofString(node.id));
    entry.set("type", Json::ofString(node.type));
    entry.set("inputChannels", channelsJson(node.inputChannels));
    entry.set("outputChannels", channelsJson(node.outputChannels));
    if (node.delayFrames != 0) {
      entry.set("delayFrames", Json::ofNumber(node.delayFrames));
    }
    Json preset;
    if (!presetJson(host, node, &preset, error)) return false;
    entry.set("preset", std::move(preset));
    nodes.push(std::move(entry));
  }
  root.set("nodes", std::move(nodes));
  root.set("connections", connectionsJson(document));
  root.set("eventConnections", eventConnectionsJson(document));
  root.set("transport", transportJson(host->graph));
  *out = std::move(root);
  return true;
}

std::string pretty(const Json& json) {
  std::string text;
  writeJson(json, 2, &text);
  text.push_back('\n');
  return text;
}

const HostParamEntry* paramById(AudHost* host, uint32_t id) {
  const auto& params = host->document.params;
  const auto it = std::lower_bound(
      params.begin(), params.end(), id,
      [](const HostParamEntry& entry, uint32_t value) {
        return entry.id < value;
      });
  if (it == params.end() || it->id != id) return nullptr;
  return &*it;
}

int32_t refuse(AudHost* host, int32_t code, const std::string& message) {
  host->lastError = message;
  return code;
}

int32_t refuse(AudHost* host, const Error& error) {
  return refuse(host, error.code, error.message);
}

}  // namespace
}  // namespace aud

using namespace aud;

// ############################################################################
// The C API

AUD_EXPORT AudHost* aud_host_create(AudGraph* graph,
                                    const AudHostOptions* options) {
  if (graph == nullptr ||
      (options != nullptr && options->struct_size < sizeof(AudHostOptions))) {
    return nullptr;
  }
  auto* host = new (std::nothrow) AudHost();
  if (host == nullptr) return nullptr;
  host->graph = graph;
  if (options != nullptr && options->base_directory != nullptr) {
    host->baseDirectory = options->base_directory;
    while (host->baseDirectory.size() > 1 && host->baseDirectory.back() == '/') {
      host->baseDirectory.pop_back();
    }
  }
  return host;
}

AUD_EXPORT void aud_host_destroy(AudHost* host) { delete host; }

AUD_EXPORT AudGraph* aud_host_graph(AudHost* host) {
  return host == nullptr ? nullptr : host->graph;
}

AUD_EXPORT const char* aud_host_last_error(AudHost* host) {
  return host == nullptr ? "" : host->lastError.c_str();
}

AUD_EXPORT int32_t aud_host_inspect(const char* json, size_t length,
                                    AudHostDocumentInfo* info) {
  if (json == nullptr || info == nullptr ||
      info->struct_size < sizeof(AudHostDocumentInfo)) {
    return AUD_ERROR_INVALID_ARGUMENT;
  }
  Json root;
  if (!parseJson(json, length, &root, nullptr) || !root.isObject()) {
    return AUD_ERROR_INVALID_ARGUMENT;
  }
  const Json* schema = root.find("schema");
  if (schema == nullptr || !schema->isNumber() ||
      schema->number != kDocumentSchema) {
    return AUD_ERROR_INVALID_ARGUMENT;
  }
  Error error;
  std::vector<uint32_t> inputs;
  std::vector<uint32_t> outputs;
  if (!readChannels(root.find("inputChannels"), "inputChannels", &inputs,
                    &error) ||
      !readChannels(root.find("outputChannels"), "outputChannels", &outputs,
                    &error)) {
    return AUD_ERROR_INVALID_ARGUMENT;
  }
  if (root.find("outputChannels") == nullptr) {
    outputs.assign(1, kDefaultOutputChannels);
  }
  if (inputs.size() > AUD_HOST_MAX_BUSES || outputs.size() > AUD_HOST_MAX_BUSES) {
    return AUD_ERROR_CAPACITY;
  }
  const Json* nodes = root.find("nodes");
  const Json* assets = root.find("assets");
  const Json* name = root.find("name");
  if ((nodes != nullptr && !nodes->isArray()) ||
      (assets != nullptr && !assets->isArray()) ||
      (name != nullptr && !name->isString())) {
    return AUD_ERROR_INVALID_ARGUMENT;
  }
  const uint32_t structSize = info->struct_size;
  *info = AudHostDocumentInfo{};
  info->struct_size = structSize;
  info->schema = static_cast<uint32_t>(schema->number);
  info->num_input_buses = static_cast<uint32_t>(inputs.size());
  info->num_output_buses = static_cast<uint32_t>(outputs.size());
  std::copy(inputs.begin(), inputs.end(), info->input_channels);
  std::copy(outputs.begin(), outputs.end(), info->output_channels);
  info->num_nodes =
      nodes == nullptr ? 0 : static_cast<uint32_t>(nodes->array.size());
  info->num_assets =
      assets == nullptr ? 0 : static_cast<uint32_t>(assets->array.size());
  if (name != nullptr) {
    std::snprintf(info->name, sizeof(info->name), "%s", name->string.c_str());
  }
  return AUD_OK;
}

AUD_EXPORT int32_t aud_host_load(AudHost* host, const char* json,
                                 size_t length) {
  if (host == nullptr) return AUD_ERROR_INVALID_ARGUMENT;
  host->lastError.clear();
  if (json == nullptr) {
    return refuse(host, AUD_ERROR_INVALID_ARGUMENT, "no document text");
  }
  Json root;
  std::string parseError;
  if (!parseJson(json, length, &root, &parseError)) {
    return refuse(host, AUD_ERROR_INVALID_ARGUMENT,
                  "invalid JSON: " + parseError);
  }
  Document document;
  Error error;
  if (!readDocument(host->graph, root, &document, &error) ||
      !checkBuses(host->graph, document, &error) ||
      !checkAssets(host->baseDirectory, &document.assets, &error) ||
      !buildParams(&document, &error) ||
      !applyDocument(host, &document, &error)) {
    return refuse(host, error);
  }
  host->document = std::move(document);
  const int32_t transport =
      applyTransport(host->graph, host->document.transport);
  if (transport != AUD_OK) {
    return refuse(host, transport,
                  withCode("the document is loaded, but the transport "
                           "refused its settings",
                           transport));
  }
  return AUD_OK;
}

AUD_EXPORT int32_t aud_host_save(AudHost* host, char* buffer, size_t capacity,
                                 size_t* size) {
  if (host == nullptr) return AUD_ERROR_INVALID_ARGUMENT;
  host->lastError.clear();
  Json root;
  Error error;
  if (!documentJson(host, &root, &error)) return refuse(host, error);
  return writeOut(pretty(root), buffer, capacity, size);
}

AUD_EXPORT int32_t aud_host_num_nodes(AudHost* host) {
  return host == nullptr ? AUD_ERROR_INVALID_ARGUMENT
                         : static_cast<int32_t>(host->document.nodes.size());
}

AUD_EXPORT int32_t aud_host_node_at(AudHost* host, uint32_t index) {
  if (host == nullptr) return AUD_ERROR_INVALID_ARGUMENT;
  if (index >= host->document.nodes.size()) return AUD_ERROR_NOT_FOUND;
  return host->document.nodes[index].handle;
}

AUD_EXPORT int32_t aud_host_node(AudHost* host, const char* id) {
  if (host == nullptr || id == nullptr) return AUD_ERROR_INVALID_ARGUMENT;
  const HostNode* node = nodeOf(host->document, id);
  return node == nullptr ? AUD_ERROR_NOT_FOUND : node->handle;
}

AUD_EXPORT const char* aud_host_node_id(AudHost* host, int32_t node) {
  if (host == nullptr) return nullptr;
  for (const HostNode& entry : host->document.nodes) {
    if (entry.handle == node) return entry.id.c_str();
  }
  return nullptr;
}

AUD_EXPORT int32_t aud_host_apply_preset(AudHost* host, const char* node_id,
                                         const char* json, size_t length) {
  if (host == nullptr || node_id == nullptr || json == nullptr) {
    return AUD_ERROR_INVALID_ARGUMENT;
  }
  host->lastError.clear();
  HostNode* node = nodeOf(host->document, node_id);
  if (node == nullptr) {
    return refuse(host, AUD_ERROR_NOT_FOUND,
                  std::string("unknown node ") + node_id);
  }
  Json root;
  std::string parseError;
  if (!parseJson(json, length, &root, &parseError)) {
    return refuse(host, AUD_ERROR_INVALID_ARGUMENT,
                  "invalid JSON: " + parseError);
  }
  // Read completely first, so that a refused preset changes nothing.
  PresetParts parts;
  Error error;
  if (!readPreset(root, node->id, node->type, node->descriptor,
                  host->document.assets, &parts, &error) ||
      !applyParts(host->graph, parts, host->document.assets, node, &error)) {
    return refuse(host, error);
  }
  return AUD_OK;
}

AUD_EXPORT int32_t aud_host_node_preset(AudHost* host, const char* node_id,
                                        char* buffer, size_t capacity,
                                        size_t* size) {
  if (host == nullptr || node_id == nullptr) return AUD_ERROR_INVALID_ARGUMENT;
  host->lastError.clear();
  const HostNode* node = nodeOf(host->document, node_id);
  if (node == nullptr) {
    return refuse(host, AUD_ERROR_NOT_FOUND,
                  std::string("unknown node ") + node_id);
  }
  Json preset;
  Error error;
  if (!presetJson(host, *node, &preset, &error)) return refuse(host, error);
  return writeOut(pretty(preset), buffer, capacity, size);
}

AUD_EXPORT int32_t aud_host_num_params(AudHost* host) {
  return host == nullptr ? AUD_ERROR_INVALID_ARGUMENT
                         : static_cast<int32_t>(host->document.params.size());
}

AUD_EXPORT int32_t aud_host_param(AudHost* host, uint32_t index,
                                  AudHostParam* out) {
  if (host == nullptr || out == nullptr ||
      out->struct_size < sizeof(AudHostParam)) {
    return AUD_ERROR_INVALID_ARGUMENT;
  }
  if (index >= host->document.params.size()) return AUD_ERROR_NOT_FOUND;
  const HostParamEntry& entry = host->document.params[index];
  const HostNode& node = host->document.nodes[entry.node];
  out->id = entry.id;
  out->node = node.handle;
  out->index = entry.index;
  out->node_id = node.id.c_str();
  out->param_id = node.descriptor->params[entry.index].id;
  out->descriptor = &node.descriptor->params[entry.index];
  out->value = node.params[entry.index];
  return AUD_OK;
}

AUD_EXPORT int32_t aud_host_param_index(AudHost* host, uint32_t id) {
  if (host == nullptr) return AUD_ERROR_INVALID_ARGUMENT;
  const HostParamEntry* entry = paramById(host, id);
  if (entry == nullptr) return AUD_ERROR_NOT_FOUND;
  return static_cast<int32_t>(entry - host->document.params.data());
}

AUD_EXPORT uint32_t aud_host_param_id(const char* node_id,
                                      const char* param_id) {
  if (node_id == nullptr || param_id == nullptr) return 0;
  uint32_t hash = fnv1a(kFnvOffset, node_id);
  hash = fnv1a(hash, "/");
  hash = fnv1a(hash, param_id);
  return hash & kParamIdMask;
}

AUD_EXPORT int32_t aud_host_set_param(AudHost* host, uint32_t id, float value,
                                      uint32_t ramp_frames) {
  if (host == nullptr) return AUD_ERROR_INVALID_ARGUMENT;
  const HostParamEntry* entry = paramById(host, id);
  if (entry == nullptr) return AUD_ERROR_NOT_FOUND;
  HostNode& node = host->document.nodes[entry->node];
  if (!inRange(node.descriptor->params[entry->index], value)) {
    return AUD_ERROR_INVALID_ARGUMENT;
  }
  const int32_t result = aud_graph_set_param(host->graph, node.handle,
                                             entry->index, value, ramp_frames);
  if (result == AUD_OK) node.params[entry->index] = value;
  return result;
}

AUD_EXPORT int32_t aud_host_get_param(AudHost* host, uint32_t id,
                                      float* value) {
  if (host == nullptr || value == nullptr) return AUD_ERROR_INVALID_ARGUMENT;
  const HostParamEntry* entry = paramById(host, id);
  if (entry == nullptr) return AUD_ERROR_NOT_FOUND;
  *value = host->document.nodes[entry->node].params[entry->index];
  return AUD_OK;
}

AUD_EXPORT int32_t aud_host_latency(AudHost* host) {
  return host == nullptr ? AUD_ERROR_INVALID_ARGUMENT
                         : aud_graph_output_latency(host->graph);
}

AUD_EXPORT uint32_t aud_host_tail(AudHost* host) {
  return host == nullptr ? 0 : aud_graph_output_tail(host->graph);
}

AUD_EXPORT int32_t aud_host_num_assets(AudHost* host) {
  return host == nullptr ? AUD_ERROR_INVALID_ARGUMENT
                         : static_cast<int32_t>(host->document.assets.size());
}

AUD_EXPORT int32_t aud_host_asset(AudHost* host, uint32_t index,
                                  AudHostAsset* out) {
  if (host == nullptr || out == nullptr ||
      out->struct_size < sizeof(AudHostAsset)) {
    return AUD_ERROR_INVALID_ARGUMENT;
  }
  if (index >= host->document.assets.size()) return AUD_ERROR_NOT_FOUND;
  const HostAssetEntry& asset = host->document.assets[index];
  out->exists = asset.exists ? 1 : 0;
  out->id = asset.id.c_str();
  out->path = asset.path.c_str();
  out->resolved = asset.resolved.c_str();
  return AUD_OK;
}

AUD_EXPORT int32_t aud_host_set_asset_path(AudHost* host, const char* id,
                                           const char* path) {
  if (host == nullptr || id == nullptr || path == nullptr || *path == '\0') {
    return AUD_ERROR_INVALID_ARGUMENT;
  }
  host->lastError.clear();
  auto& assets = host->document.assets;
  const auto asset =
      std::find_if(assets.begin(), assets.end(),
                   [&](const HostAssetEntry& entry) { return entry.id == id; });
  if (asset == assets.end()) {
    return refuse(host, AUD_ERROR_NOT_FOUND, std::string("unknown asset ") + id);
  }
  const std::string resolved = resolvePath(host->baseDirectory, path);
  if (!fileExists(resolved)) {
    return refuse(host, AUD_ERROR_NOT_FOUND,
                  std::string("asset ") + id + " not found: " + resolved);
  }
  asset->path = relativeToBase(host->baseDirectory, resolved);
  asset->resolved = resolved;
  asset->exists = true;
  const std::string reference = std::string(kAssetPrefix) + id;
  for (HostNode& node : host->document.nodes) {
    for (uint32_t slot = 0; slot < node.strings.size(); ++slot) {
      if (!node.stringSet[slot] || node.strings[slot] != reference) continue;
      const int32_t result = aud_graph_set_string(
          host->graph, node.handle, node.descriptor->string_keys[slot].key,
          resolved.c_str());
      if (result != AUD_OK) {
        return refuse(host, result,
                      withCode("node " + node.id +
                                   " refused the relinked asset " + id,
                               result));
      }
    }
  }
  return AUD_OK;
}

AUD_EXPORT int32_t aud_host_render(AudHost* host,
                                   AudHostRenderRequest* request) {
  if (host == nullptr) return AUD_ERROR_INVALID_ARGUMENT;
  return aud_graph_render_host(host->graph, request);
}
