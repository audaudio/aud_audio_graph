// @license
// Copyright (c) Audanika. All Rights Reserved.
//
// Use of this source code is governed by terms that can be
// found in the LICENSE file in the root of this package.

// The native tests of the JSON reader and writer of the headless host.

#include <clocale>
#include <cmath>
#include <cstring>
#include <string>

#include "aud_json.hpp"
#include "aud_test.hpp"

namespace {

using aud::Json;

bool parses(const char* text, Json* out, std::string* error = nullptr) {
  return aud::parseJson(text, std::strlen(text), out, error);
}

std::string written(const Json& value, int indent = 0) {
  std::string out;
  aud::writeJson(value, indent, &out);
  return out;
}

}  // namespace

// ############################################################################

AUD_TEST(json_parses_scalars_arrays_and_objects) {
  Json value;
  AUD_CHECK(parses(" { \"a\" : [1, 2.5, -3e2, true, false, null, \"x\"], "
                   "\"b\": {} , \"c\": [] } ",
                   &value));
  AUD_CHECK(value.isObject() && value.object.size() == 3);
  const Json* a = value.find("a");
  AUD_CHECK(a != nullptr && a->isArray() && a->array.size() == 7);
  AUD_CHECK(a->array[0].isInteger() && a->array[0].integer() == 1);
  AUD_CHECK(a->array[1].isNumber() && a->array[1].number == 2.5);
  AUD_CHECK(!a->array[1].isInteger() && a->array[1].integer() == 0);
  AUD_CHECK(a->array[2].number == -300);
  AUD_CHECK(a->array[3].isBool() && a->array[3].boolean);
  AUD_CHECK(a->array[4].isBool() && !a->array[4].boolean);
  AUD_CHECK(a->array[5].isNull());
  AUD_CHECK(a->array[6].isString() && a->array[6].string == "x");
  AUD_CHECK(value.find("b")->isObject() && value.find("b")->object.empty());
  AUD_CHECK(value.find("c")->isArray() && value.find("c")->array.empty());
  AUD_CHECK(value.find("d") == nullptr);
  AUD_CHECK(a->find("x") == nullptr);
  // A later member of the same name replaces the earlier one.
  AUD_CHECK(parses("{\"k\": 1, \"k\": 2}", &value));
  AUD_CHECK(value.object.size() == 1 && value.find("k")->number == 2);
}

AUD_TEST(json_parses_strings_with_escapes) {
  Json value;
  AUD_CHECK(parses("\"a\\\"b\\\\c\\/d\\b\\f\\n\\r\\t\\u00e9\\u20ac\\ud83c\\udfb5\"",
                   &value));
  AUD_CHECK(value.string == "a\"b\\c/d\b\f\n\r\t\xC3\xA9\xE2\x82\xAC\xF0\x9F\x8E\xB5");
  // Raw UTF-8 passes through.
  AUD_CHECK(parses("\"gr\xC3\xBC\xC3\x9F\"", &value));
  AUD_CHECK(value.string == "gr\xC3\xBC\xC3\x9F");
}

AUD_TEST(json_refuses_what_is_no_json) {
  const char* bad[] = {
      "",          "{",          "[1,]",     "{\"a\" 1}", "{\"a\":1,}",
      "01",        "1.",         "1e",       "-",         "\"a",
      "\"\\x\"",   "\"\\u12\"",  "\"\\ud800\"", "\"\\udc00\"",
      "\"\\ud800\\u0041\"", "tru", "nul", "[1] 2", "\"a\tb\"", "1e999",
      "{1:2}",     "x",
  };
  for (const char* text : bad) {
    Json value;
    std::string error;
    AUD_CHECK(!parses(text, &value, &error));
    AUD_CHECK(error.find("at offset") != std::string::npos);
  }
  std::string deep(200, '[');
  Json value;
  std::string error;
  AUD_CHECK(!aud::parseJson(deep.data(), deep.size(), &value, &error));
  AUD_CHECK(error.find("nesting") != std::string::npos);
  AUD_CHECK(!aud::parseJson(nullptr, 0, &value, &error));
  AUD_CHECK(!aud::parseJson("1", 1, nullptr, &error));
}

AUD_TEST(json_converts_numbers_exactly_and_independent_of_the_locale) {
  Json value;
  AUD_CHECK(parses("0.1", &value) && value.number == 0.1);
  AUD_CHECK(parses("220.5", &value) && value.number == 220.5);
  AUD_CHECK(parses("-0", &value) && value.number == 0);
  AUD_CHECK(parses("1E+2", &value) && value.number == 100);
  AUD_CHECK(parses("123456789012345678901234567890", &value) &&
            value.number == 1.2345678901234568e29);
  AUD_CHECK(parses("0.000000000000000000000001", &value) && value.number == 1e-24);
  AUD_CHECK(parses("1.7976931348623157e308", &value) &&
            value.number == 1.7976931348623157e308);
  AUD_CHECK(parses("9007199254740993", &value) && value.isInteger());
  AUD_CHECK(parses("1e-400", &value) && value.number == 0);
  // A locale with a decimal comma changes nothing.
  const char* locale = std::setlocale(LC_NUMERIC, "de_DE.UTF-8");
  AUD_CHECK(parses("2.75", &value) && value.number == 2.75);
  AUD_CHECK(written(Json::ofNumber(2.75)) == "2.75");
  AUD_CHECK(written(Json::ofNumber(0.1)) == "0.1");
  if (locale != nullptr) std::setlocale(LC_NUMERIC, "C");
}

AUD_TEST(json_writes_compact_and_pretty_text_that_reads_back) {
  Json value = Json::ofObject();
  value.set("name", Json::ofString("Reference \"chain\"\n"));
  value.set("tempo", Json::ofNumber(120));
  value.set("gain", Json::ofNumber(0.1f));
  value.set("huge", Json::ofNumber(1e21));
  value.set("tiny", Json::ofNumber(1e-7));
  value.set("neg", Json::ofNumber(-2.5));
  value.set("nan", Json::ofNumber(std::nan("")));
  value.set("flag", Json::ofBool(true));
  value.set("none", Json::ofNull());
  Json& list = value.set("list", Json::ofArray());
  list.push(Json::ofNumber(1));
  list.push(Json::ofObject());
  list.push(Json::ofArray());
  value.set("tempo", Json::ofNumber(90.5));  // replaces in place
  const std::string compact = written(value);
  AUD_CHECK(compact ==
            "{\"name\":\"Reference \\\"chain\\\"\\n\",\"tempo\":90.5,"
            "\"gain\":0.10000000149011612,\"huge\":1e+21,\"tiny\":1e-07,"
            "\"neg\":-2.5,\"nan\":null,\"flag\":true,\"none\":null,"
            "\"list\":[1,{},[]]}");
  Json back;
  AUD_CHECK(aud::parseJson(compact.data(), compact.size(), &back, nullptr));
  AUD_CHECK(back.find("gain")->number == static_cast<double>(0.1f));
  AUD_CHECK(back.find("huge")->number == 1e21);
  AUD_CHECK(back.find("tiny")->number == 1e-7);
  AUD_CHECK(back.find("name")->string == "Reference \"chain\"\n");
  const std::string pretty = written(value, 2);
  AUD_CHECK(pretty.rfind("{\n  \"name\": \"Reference", 0) == 0);
  AUD_CHECK(pretty.find("\"list\": [\n    1,\n    {},\n    []\n  ]\n}") !=
            std::string::npos);
  AUD_CHECK(aud::parseJson(pretty.data(), pretty.size(), &back, nullptr));
  AUD_CHECK(written(back) == compact);
  AUD_CHECK(written(Json::ofString(std::string("\x01", 1))) == "\"\\u0001\"");
  aud::writeJson(value, 0, nullptr);
}

AUD_TEST(json_encodes_and_decodes_base64) {
  const uint8_t bytes[] = {0x00, 0xff, 0x10, 0x20, 0x30};
  AUD_CHECK(aud::base64Encode(bytes, 0) == "");
  AUD_CHECK(aud::base64Encode(bytes, 1) == "AA==");
  AUD_CHECK(aud::base64Encode(bytes, 2) == "AP8=");
  AUD_CHECK(aud::base64Encode(bytes, 3) == "AP8Q");
  AUD_CHECK(aud::base64Encode(bytes, 5) == "AP8QIDA=");
  std::vector<uint8_t> out;
  AUD_CHECK(aud::base64Decode("AP8QIDA=", &out) && out.size() == 5 &&
            std::memcmp(out.data(), bytes, 5) == 0);
  AUD_CHECK(aud::base64Decode("AA==", &out) && out.size() == 1 && out[0] == 0);
  AUD_CHECK(aud::base64Decode("", &out) && out.empty());
  AUD_CHECK(!aud::base64Decode("AAA", &out));
  AUD_CHECK(!aud::base64Decode("A=AA", &out));
  AUD_CHECK(!aud::base64Decode("AA==AAAA", &out));
  AUD_CHECK(!aud::base64Decode("AA$=", &out));
  AUD_CHECK(!aud::base64Decode("AAAA", nullptr));
  // Every byte value survives a round trip.
  std::vector<uint8_t> all;
  for (int i = 0; i < 256; ++i) all.push_back(static_cast<uint8_t>(i));
  AUD_CHECK(aud::base64Decode(aud::base64Encode(all.data(), all.size()), &out) &&
            out == all);
}
