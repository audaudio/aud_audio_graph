// @license
// Copyright (c) Audanika. All Rights Reserved.
//
// Use of this source code is governed by terms that can be
// found in the LICENSE file in the root of this package.

// A small JSON reader and writer for the graph documents of the headless
// host (ticket 20, plugin-002): RFC 8259 with UTF-8 strings, the escapes
// including surrogate pairs, numbers converted independently of the C
// locale, and base64 for the state blobs of the node presets. The tree is
// built and walked on the control thread only; nothing here is realtime.
// A vendored library would carry a notice for a schema this small.

#ifndef AUD_JSON_HPP
#define AUD_JSON_HPP

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace aud {

// A JSON value: a scalar, an array of values or an object whose members
// keep their document order.
struct Json {
  enum class Kind : uint8_t { null, boolean, number, string, array, object };

  Kind kind = Kind::null;
  bool boolean = false;
  double number = 0;
  std::string string;
  std::vector<Json> array;
  std::vector<std::pair<std::string, Json>> object;

  static Json ofNull();
  static Json ofBool(bool value);
  static Json ofNumber(double value);
  static Json ofString(std::string value);
  static Json ofArray();
  static Json ofObject();

  bool isNull() const { return kind == Kind::null; }
  bool isBool() const { return kind == Kind::boolean; }
  bool isNumber() const { return kind == Kind::number; }
  bool isString() const { return kind == Kind::string; }
  bool isArray() const { return kind == Kind::array; }
  bool isObject() const { return kind == Kind::object; }

  // Whether the value is a number without a fraction that fits an int64.
  bool isInteger() const;

  // The number as an integer; 0 unless isInteger().
  int64_t integer() const;

  // The member `key` of an object, or NULL.
  const Json* find(const char* key) const;

  // Sets the member `key` of an object, replacing an existing one.
  Json& set(const std::string& key, Json value);

  // Appends to an array.
  Json& push(Json value);
};

// Parses `length` bytes of JSON text into `out`; false with a message and
// the byte offset of the problem in `error`.
bool parseJson(const char* text, size_t length, Json* out, std::string* error);

// Appends `value` as JSON text to `out`: compact when `indent` is 0,
// otherwise one member per line with `indent` spaces per level. Numbers
// take the shortest form that reads back exactly; a number that is not
// finite is written as null.
void writeJson(const Json& value, int indent, std::string* out);

// Encodes bytes as base64 with padding.
std::string base64Encode(const uint8_t* data, size_t size);

// Decodes base64 with padding; false for text that is no base64.
bool base64Decode(const std::string& text, std::vector<uint8_t>* out);

}  // namespace aud

#endif  // AUD_JSON_HPP
