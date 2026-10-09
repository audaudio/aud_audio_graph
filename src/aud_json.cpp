// @license
// Copyright (c) Audanika. All Rights Reserved.
//
// Use of this source code is governed by terms that can be
// found in the LICENSE file in the root of this package.

#include "aud_json.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace aud {
namespace {

// Deeper nesting than this is refused instead of exhausting the stack.
constexpr int kMaxDepth = 128;
// Digits a mantissa keeps; more are dropped into the exponent.
constexpr int kMaxMantissaDigits = 19;
// Exact powers of ten: 10^22 is the largest one a double holds exactly.
constexpr int kMaxExactPower = 22;
constexpr double kExactPowers[kMaxExactPower + 1] = {
    1e0,  1e1,  1e2,  1e3,  1e4,  1e5,  1e6,  1e7,  1e8,  1e9,  1e10, 1e11,
    1e12, 1e13, 1e14, 1e15, 1e16, 1e17, 1e18, 1e19, 1e20, 1e21, 1e22};
// The largest mantissa that a double holds exactly: 2^53.
constexpr uint64_t kExactMantissa = uint64_t{1} << 53;

// ............................................................................
// Numbers

// Converts a decimal mantissa and exponent to a double: exactly through
// one rounding when both fit (Clinger's fast path), otherwise through
// strtod on a text without a decimal point, which no locale reads
// differently.
double toDouble(uint64_t mantissa, int exponent, bool negative) {
  double value;
  if (mantissa == 0) {
    value = 0;
  } else if (mantissa < kExactMantissa && exponent >= -kMaxExactPower &&
             exponent <= kMaxExactPower) {
    value = static_cast<double>(mantissa);
    value = exponent < 0 ? value / kExactPowers[-exponent]
                         : value * kExactPowers[exponent];
  } else {
    char text[64];
    std::snprintf(text, sizeof(text), "%llue%d",
                  static_cast<unsigned long long>(mantissa), exponent);
    value = std::strtod(text, nullptr);
  }
  return negative ? -value : value;
}

bool isDigit(char c) { return c >= '0' && c <= '9'; }

// ............................................................................
// The parser

class Parser {
 public:
  Parser(const char* text, size_t length, std::string* error)
      : begin_(text), p_(text), end_(text + length), error_(error) {}

  bool parse(Json* out) {
    skipSpace();
    if (!parseValue(out, 0)) return false;
    skipSpace();
    if (p_ != end_) return fail("unexpected text after the value");
    return true;
  }

 private:
  bool fail(const char* message) {
    if (error_ != nullptr) {
      *error_ = std::string(message) + " at offset " +
                std::to_string(static_cast<long long>(p_ - begin_));
    }
    return false;
  }

  void skipSpace() {
    while (p_ != end_ &&
           (*p_ == ' ' || *p_ == '\t' || *p_ == '\n' || *p_ == '\r')) {
      ++p_;
    }
  }

  bool consume(const char* literal) {
    const size_t length = std::strlen(literal);
    if (static_cast<size_t>(end_ - p_) < length ||
        std::memcmp(p_, literal, length) != 0) {
      return false;
    }
    p_ += length;
    return true;
  }

  bool parseValue(Json* out, int depth) {
    if (depth > kMaxDepth) return fail("nesting too deep");
    if (p_ == end_) return fail("unexpected end of text");
    switch (*p_) {
      case '{':
        return parseObject(out, depth);
      case '[':
        return parseArray(out, depth);
      case '"':
        *out = Json::ofString("");
        return parseString(&out->string);
      case 't':
        if (!consume("true")) return fail("invalid literal");
        *out = Json::ofBool(true);
        return true;
      case 'f':
        if (!consume("false")) return fail("invalid literal");
        *out = Json::ofBool(false);
        return true;
      case 'n':
        if (!consume("null")) return fail("invalid literal");
        *out = Json::ofNull();
        return true;
      default:
        if (*p_ == '-' || isDigit(*p_)) return parseNumber(out);
        return fail("unexpected character");
    }
  }

  bool parseObject(Json* out, int depth) {
    *out = Json::ofObject();
    ++p_;  // {
    skipSpace();
    if (p_ != end_ && *p_ == '}') {
      ++p_;
      return true;
    }
    while (true) {
      skipSpace();
      if (p_ == end_ || *p_ != '"') return fail("expected a member name");
      std::string key;
      if (!parseString(&key)) return false;
      skipSpace();
      if (p_ == end_ || *p_ != ':') return fail("expected ':'");
      ++p_;
      skipSpace();
      Json value;
      if (!parseValue(&value, depth + 1)) return false;
      out->set(key, std::move(value));
      skipSpace();
      if (p_ == end_) return fail("unexpected end of object");
      if (*p_ == ',') {
        ++p_;
        continue;
      }
      if (*p_ == '}') {
        ++p_;
        return true;
      }
      return fail("expected ',' or '}'");
    }
  }

  bool parseArray(Json* out, int depth) {
    *out = Json::ofArray();
    ++p_;  // [
    skipSpace();
    if (p_ != end_ && *p_ == ']') {
      ++p_;
      return true;
    }
    while (true) {
      skipSpace();
      Json value;
      if (!parseValue(&value, depth + 1)) return false;
      out->push(std::move(value));
      skipSpace();
      if (p_ == end_) return fail("unexpected end of array");
      if (*p_ == ',') {
        ++p_;
        continue;
      }
      if (*p_ == ']') {
        ++p_;
        return true;
      }
      return fail("expected ',' or ']'");
    }
  }

  bool parseHex4(uint32_t* code) {
    if (end_ - p_ < 4) return fail("truncated \\u escape");
    uint32_t value = 0;
    for (int i = 0; i < 4; ++i) {
      const char c = p_[i];
      value <<= 4;
      if (c >= '0' && c <= '9') {
        value |= static_cast<uint32_t>(c - '0');
      } else if (c >= 'a' && c <= 'f') {
        value |= static_cast<uint32_t>(c - 'a' + 10);
      } else if (c >= 'A' && c <= 'F') {
        value |= static_cast<uint32_t>(c - 'A' + 10);
      } else {
        return fail("invalid \\u escape");
      }
    }
    p_ += 4;
    *code = value;
    return true;
  }

  static void appendUtf8(uint32_t code, std::string* out) {
    if (code < 0x80) {
      out->push_back(static_cast<char>(code));
    } else if (code < 0x800) {
      out->push_back(static_cast<char>(0xC0 | (code >> 6)));
      out->push_back(static_cast<char>(0x80 | (code & 0x3F)));
    } else if (code < 0x10000) {
      out->push_back(static_cast<char>(0xE0 | (code >> 12)));
      out->push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
      out->push_back(static_cast<char>(0x80 | (code & 0x3F)));
    } else {
      out->push_back(static_cast<char>(0xF0 | (code >> 18)));
      out->push_back(static_cast<char>(0x80 | ((code >> 12) & 0x3F)));
      out->push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
      out->push_back(static_cast<char>(0x80 | (code & 0x3F)));
    }
  }

  // Parses a string at the opening quote into `out`.
  bool parseString(std::string* out) {
    ++p_;  // "
    while (true) {
      if (p_ == end_) return fail("unterminated string");
      const unsigned char c = static_cast<unsigned char>(*p_);
      if (c == '"') {
        ++p_;
        return true;
      }
      if (c < 0x20) return fail("control character in string");
      if (c != '\\') {
        out->push_back(static_cast<char>(c));
        ++p_;
        continue;
      }
      ++p_;
      if (p_ == end_) return fail("unterminated escape");
      const char e = *p_++;
      switch (e) {
        case '"':
        case '\\':
        case '/':
          out->push_back(e);
          break;
        case 'b':
          out->push_back('\b');
          break;
        case 'f':
          out->push_back('\f');
          break;
        case 'n':
          out->push_back('\n');
          break;
        case 'r':
          out->push_back('\r');
          break;
        case 't':
          out->push_back('\t');
          break;
        case 'u': {
          uint32_t code = 0;
          if (!parseHex4(&code)) return false;
          if (code >= 0xD800 && code <= 0xDBFF) {
            // A high surrogate needs its low surrogate.
            if (end_ - p_ < 6 || p_[0] != '\\' || p_[1] != 'u') {
              return fail("lone high surrogate");
            }
            p_ += 2;
            uint32_t low = 0;
            if (!parseHex4(&low)) return false;
            if (low < 0xDC00 || low > 0xDFFF) return fail("invalid surrogate pair");
            code = 0x10000 + ((code - 0xD800) << 10) + (low - 0xDC00);
          } else if (code >= 0xDC00 && code <= 0xDFFF) {
            return fail("lone low surrogate");
          }
          appendUtf8(code, out);
          break;
        }
        default:
          return fail("invalid escape");
      }
    }
  }

  bool parseNumber(Json* out) {
    const char* start = p_;
    bool negative = false;
    if (*p_ == '-') {
      negative = true;
      ++p_;
    }
    if (p_ == end_ || !isDigit(*p_)) return fail("expected a digit");
    uint64_t mantissa = 0;
    int digits = 0;    // significant digits kept
    int exponent = 0;  // decimal exponent of the mantissa
    auto addDigit = [&](char c) {
      if (digits < kMaxMantissaDigits) {
        mantissa = mantissa * 10 + static_cast<uint64_t>(c - '0');
        if (mantissa != 0) digits += 1;
      } else {
        exponent += 1;  // the digit is dropped
      }
    };
    if (*p_ == '0') {
      ++p_;
      if (p_ != end_ && isDigit(*p_)) return fail("leading zero");
    } else {
      while (p_ != end_ && isDigit(*p_)) addDigit(*p_++);
    }
    if (p_ != end_ && *p_ == '.') {
      ++p_;
      if (p_ == end_ || !isDigit(*p_)) return fail("expected a fraction");
      while (p_ != end_ && isDigit(*p_)) {
        if (digits < kMaxMantissaDigits) {
          mantissa = mantissa * 10 + static_cast<uint64_t>(*p_ - '0');
          if (mantissa != 0) digits += 1;
          exponent -= 1;
        }
        ++p_;
      }
    }
    if (p_ != end_ && (*p_ == 'e' || *p_ == 'E')) {
      ++p_;
      bool negativeExponent = false;
      if (p_ != end_ && (*p_ == '+' || *p_ == '-')) {
        negativeExponent = *p_ == '-';
        ++p_;
      }
      if (p_ == end_ || !isDigit(*p_)) return fail("expected an exponent");
      int value = 0;
      while (p_ != end_ && isDigit(*p_)) {
        if (value < 100000) value = value * 10 + (*p_ - '0');
        ++p_;
      }
      exponent += negativeExponent ? -value : value;
    }
    const double number = toDouble(mantissa, exponent, negative);
    if (!std::isfinite(number)) {
      p_ = start;
      return fail("number out of range");
    }
    *out = Json::ofNumber(number);
    return true;
  }

  const char* begin_;
  const char* p_;
  const char* end_;
  std::string* error_;
};

// ............................................................................
// The writer

void writeString(const std::string& value, std::string* out) {
  out->push_back('"');
  for (const char c : value) {
    const unsigned char u = static_cast<unsigned char>(c);
    switch (c) {
      case '"':
        out->append("\\\"");
        break;
      case '\\':
        out->append("\\\\");
        break;
      case '\b':
        out->append("\\b");
        break;
      case '\f':
        out->append("\\f");
        break;
      case '\n':
        out->append("\\n");
        break;
      case '\r':
        out->append("\\r");
        break;
      case '\t':
        out->append("\\t");
        break;
      default:
        if (u < 0x20) {
          char escaped[8];
          std::snprintf(escaped, sizeof(escaped), "\\u%04x", u);
          out->append(escaped);
        } else {
          out->push_back(c);
        }
    }
  }
  out->push_back('"');
}

// Writes a number in the shortest form that parses back to the same
// double. snprintf writes the decimal point of the C locale; it is
// replaced, so the text stays JSON whatever the locale is.
void writeNumber(double value, std::string* out) {
  if (!std::isfinite(value)) {
    out->append("null");
    return;
  }
  if (value == std::floor(value) && std::fabs(value) < 9007199254740992.0) {
    out->append(std::to_string(static_cast<long long>(value)));
    return;
  }
  char text[40];
  for (int precision = 1; precision <= 17; ++precision) {
    std::snprintf(text, sizeof(text), "%.*g", precision, value);
    for (char* c = text; *c != '\0'; ++c) {
      if (*c == ',') *c = '.';
    }
    Json parsed;
    if (parseJson(text, std::strlen(text), &parsed, nullptr) &&
        parsed.number == value) {
      break;
    }
  }
  out->append(text);
}

void newline(int indent, int depth, std::string* out) {
  if (indent <= 0) return;
  out->push_back('\n');
  out->append(static_cast<size_t>(indent * depth), ' ');
}

void writeValue(const Json& value, int indent, int depth, std::string* out) {
  switch (value.kind) {
    case Json::Kind::null:
      out->append("null");
      break;
    case Json::Kind::boolean:
      out->append(value.boolean ? "true" : "false");
      break;
    case Json::Kind::number:
      writeNumber(value.number, out);
      break;
    case Json::Kind::string:
      writeString(value.string, out);
      break;
    case Json::Kind::array:
      out->push_back('[');
      for (size_t i = 0; i < value.array.size(); ++i) {
        if (i > 0) out->push_back(',');
        newline(indent, depth + 1, out);
        writeValue(value.array[i], indent, depth + 1, out);
      }
      if (!value.array.empty()) newline(indent, depth, out);
      out->push_back(']');
      break;
    case Json::Kind::object:
      out->push_back('{');
      for (size_t i = 0; i < value.object.size(); ++i) {
        if (i > 0) out->push_back(',');
        newline(indent, depth + 1, out);
        writeString(value.object[i].first, out);
        out->push_back(':');
        if (indent > 0) out->push_back(' ');
        writeValue(value.object[i].second, indent, depth + 1, out);
      }
      if (!value.object.empty()) newline(indent, depth, out);
      out->push_back('}');
      break;
  }
}

// ............................................................................
// Base64

constexpr char kBase64Alphabet[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

int base64Digit(char c) {
  if (c >= 'A' && c <= 'Z') return c - 'A';
  if (c >= 'a' && c <= 'z') return c - 'a' + 26;
  if (c >= '0' && c <= '9') return c - '0' + 52;
  if (c == '+') return 62;
  if (c == '/') return 63;
  return -1;
}

}  // namespace

// ............................................................................
// Json

Json Json::ofNull() { return Json(); }

Json Json::ofBool(bool value) {
  Json json;
  json.kind = Kind::boolean;
  json.boolean = value;
  return json;
}

Json Json::ofNumber(double value) {
  Json json;
  json.kind = Kind::number;
  json.number = value;
  return json;
}

Json Json::ofString(std::string value) {
  Json json;
  json.kind = Kind::string;
  json.string = std::move(value);
  return json;
}

Json Json::ofArray() {
  Json json;
  json.kind = Kind::array;
  return json;
}

Json Json::ofObject() {
  Json json;
  json.kind = Kind::object;
  return json;
}

bool Json::isInteger() const {
  return kind == Kind::number && number == std::floor(number) &&
         std::fabs(number) < 9223372036854775808.0;
}

int64_t Json::integer() const {
  return isInteger() ? static_cast<int64_t>(number) : 0;
}

const Json* Json::find(const char* key) const {
  if (kind != Kind::object) return nullptr;
  for (const auto& member : object) {
    if (member.first == key) return &member.second;
  }
  return nullptr;
}

Json& Json::set(const std::string& key, Json value) {
  kind = Kind::object;
  for (auto& member : object) {
    if (member.first != key) continue;
    member.second = std::move(value);
    return member.second;
  }
  object.emplace_back(key, std::move(value));
  return object.back().second;
}

Json& Json::push(Json value) {
  kind = Kind::array;
  array.push_back(std::move(value));
  return array.back();
}

// ............................................................................
// The functions

bool parseJson(const char* text, size_t length, Json* out, std::string* error) {
  if (text == nullptr || out == nullptr) {
    if (error != nullptr) *error = "no text";
    return false;
  }
  Parser parser(text, length, error);
  return parser.parse(out);
}

void writeJson(const Json& value, int indent, std::string* out) {
  if (out == nullptr) return;
  writeValue(value, indent, 0, out);
}

std::string base64Encode(const uint8_t* data, size_t size) {
  std::string out;
  out.reserve((size + 2) / 3 * 4);
  for (size_t i = 0; i < size; i += 3) {
    const uint32_t a = data[i];
    const uint32_t b = i + 1 < size ? data[i + 1] : 0;
    const uint32_t c = i + 2 < size ? data[i + 2] : 0;
    const uint32_t triple = (a << 16) | (b << 8) | c;
    out.push_back(kBase64Alphabet[(triple >> 18) & 0x3F]);
    out.push_back(kBase64Alphabet[(triple >> 12) & 0x3F]);
    out.push_back(i + 1 < size ? kBase64Alphabet[(triple >> 6) & 0x3F] : '=');
    out.push_back(i + 2 < size ? kBase64Alphabet[triple & 0x3F] : '=');
  }
  return out;
}

bool base64Decode(const std::string& text, std::vector<uint8_t>* out) {
  if (out == nullptr || text.size() % 4 != 0) return false;
  out->clear();
  out->reserve(text.size() / 4 * 3);
  for (size_t i = 0; i < text.size(); i += 4) {
    const int digits[4] = {base64Digit(text[i]), base64Digit(text[i + 1]),
                           base64Digit(text[i + 2]), base64Digit(text[i + 3])};
    const bool lastGroup = i + 4 == text.size();
    const int padding = text[i + 3] == '=' ? (text[i + 2] == '=' ? 2 : 1) : 0;
    if (digits[0] < 0 || digits[1] < 0 || (padding > 0 && !lastGroup) ||
        (padding < 2 && digits[2] < 0) || (padding < 1 && digits[3] < 0)) {
      return false;
    }
    const uint32_t triple = (static_cast<uint32_t>(digits[0]) << 18) |
                            (static_cast<uint32_t>(digits[1]) << 12) |
                            (padding < 2 ? static_cast<uint32_t>(digits[2]) << 6 : 0) |
                            (padding < 1 ? static_cast<uint32_t>(digits[3]) : 0);
    out->push_back(static_cast<uint8_t>((triple >> 16) & 0xFF));
    if (padding < 2) out->push_back(static_cast<uint8_t>((triple >> 8) & 0xFF));
    if (padding < 1) out->push_back(static_cast<uint8_t>(triple & 0xFF));
  }
  return true;
}

}  // namespace aud
