#pragma once

#include <array>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace ec_systemcore {

class Json final {
 public:
  using Object = std::map<std::string, Json>;
  using Array = std::vector<Json>;
  struct Number {
    double value = 0;
    std::string lexeme;
  };

  struct Limits {
    std::size_t maximumBytes = 1024U * 1024U;
    std::size_t maximumDepth = 64;
    std::size_t maximumNodes = 16384;
    std::size_t maximumStringBytes = 65536;
  };

  Json() = default;
  explicit Json(std::nullptr_t) {}
  explicit Json(Object object) : value_(std::move(object)) {}
  explicit Json(Array array) : value_(std::move(array)) {}
  explicit Json(std::string text) : value_(std::move(text)) {}
  explicit Json(bool value) : value_(value) {}
  explicit Json(double value) : value_(Number{value, std::to_string(value)}) {}
  explicit Json(Number value) : value_(std::move(value)) {}

  static Json Parse(const std::string& text) {
    return Parse(text, Limits{});
  }

  static Json Parse(const std::string& text, const Limits& limits) {
    if (text.size() > limits.maximumBytes) {
      throw std::runtime_error("JSON input exceeds the configured size limit");
    }
    if (limits.maximumDepth == 0 || limits.maximumNodes == 0 ||
        limits.maximumStringBytes == 0) {
      throw std::runtime_error("invalid JSON parser limits");
    }

    Parser parser{text, limits};
    Json result = parser.parseValue(0);
    parser.skipWhitespace();
    if (!parser.finished()) {
      throw std::runtime_error("unexpected trailing JSON data");
    }
    return result;
  }

  [[nodiscard]] bool isObject() const {
    return std::holds_alternative<Object>(value_);
  }
  [[nodiscard]] bool isArray() const {
    return std::holds_alternative<Array>(value_);
  }
  [[nodiscard]] bool isString() const {
    return std::holds_alternative<std::string>(value_);
  }
  [[nodiscard]] bool isBool() const {
    return std::holds_alternative<bool>(value_);
  }
  [[nodiscard]] bool isNumber() const {
    return std::holds_alternative<Number>(value_);
  }
  [[nodiscard]] bool isNull() const {
    return std::holds_alternative<std::nullptr_t>(value_);
  }

  [[nodiscard]] const Object& object() const {
    return std::get<Object>(value_);
  }
  [[nodiscard]] const Array& array() const { return std::get<Array>(value_); }
  [[nodiscard]] const std::string& string() const {
    return std::get<std::string>(value_);
  }
  [[nodiscard]] bool boolean() const { return std::get<bool>(value_); }
  [[nodiscard]] double number() const { return std::get<Number>(value_).value; }

  [[nodiscard]] std::int64_t integer() const {
    if (!isNumber()) {
      throw std::runtime_error("JSON value must be an integer");
    }
    const Number& numberValue = std::get<Number>(value_);
    if (numberValue.lexeme.empty() ||
        numberValue.lexeme.find_first_of(".eE") != std::string::npos) {
      throw std::runtime_error("JSON integer must use integer syntax");
    }
    std::int64_t parsed = 0;
    const char* begin = numberValue.lexeme.data();
    const char* end = begin + numberValue.lexeme.size();
    const auto result = std::from_chars(begin, end, parsed, 10);
    if (result.ec != std::errc{} || result.ptr != end) {
      throw std::runtime_error(
          "JSON integer is outside the signed 64-bit range");
    }
    return parsed;
  }

  [[nodiscard]] const Json* find(std::string_view key) const {
    if (!isObject()) {
      return nullptr;
    }
    const auto& objectValue = object();
    const auto it = objectValue.find(std::string(key));
    return it == objectValue.end() ? nullptr : &it->second;
  }

  [[nodiscard]] std::string stringValue(
      std::string_view key, const std::string& fallback = "") const {
    const Json* found = find(key);
    if (found == nullptr) {
      return fallback;
    }
    if (!found->isString()) {
      throw std::runtime_error("JSON field '" + std::string(key) +
                               "' must be a string");
    }
    return found->string();
  }

  [[nodiscard]] bool boolValue(std::string_view key,
                               bool fallback = false) const {
    const Json* found = find(key);
    if (found == nullptr) {
      return fallback;
    }
    if (!found->isBool()) {
      throw std::runtime_error("JSON field '" + std::string(key) +
                               "' must be a boolean");
    }
    return found->boolean();
  }

  [[nodiscard]] std::int64_t integerValue(std::string_view key,
                                          std::int64_t fallback = 0) const {
    const Json* found = find(key);
    if (found == nullptr) {
      return fallback;
    }
    if (!found->isNumber()) {
      throw std::runtime_error("JSON field '" + std::string(key) +
                               "' must be an integer");
    }

    try {
      return found->integer();
    } catch (const std::runtime_error&) {
      throw std::runtime_error("JSON field '" + std::string(key) +
                               "' is outside the signed 64-bit integer range");
    }
  }

 private:
  class Parser final {
   public:
    Parser(const std::string& text, const Limits& limits)
        : text_(text), limits_(limits) {}

    Json parseValue(std::size_t depth) {
      if (depth > limits_.maximumDepth) {
        throw std::runtime_error("JSON nesting exceeds the configured limit");
      }
      if (++nodeCount_ > limits_.maximumNodes) {
        throw std::runtime_error("JSON node count exceeds the configured limit");
      }

      skipWhitespace();
      if (finished()) {
        throw std::runtime_error("unexpected end of JSON input");
      }

      switch (peek()) {
        case '{':
          return parseObject(depth);
        case '[':
          return parseArray(depth);
        case '"':
          return Json{parseString()};
        case 't':
          consumeLiteral("true");
          return Json{true};
        case 'f':
          consumeLiteral("false");
          return Json{false};
        case 'n':
          consumeLiteral("null");
          return Json{nullptr};
        default:
          if (peek() == '-' ||
              IsDigit(peek())) {
            return Json{parseNumber()};
          }
          throw std::runtime_error("unexpected JSON token");
      }
    }

    void skipWhitespace() {
      while (!finished() &&
             IsJsonWhitespace(text_[offset_])) {
        ++offset_;
      }
    }

    [[nodiscard]] bool finished() const { return offset_ >= text_.size(); }

   private:
    [[nodiscard]] static bool IsDigit(char character) {
      return character >= '0' && character <= '9';
    }

    [[nodiscard]] static bool IsJsonWhitespace(char character) {
      return character == ' ' || character == '\t' || character == '\r' ||
             character == '\n';
    }

    [[nodiscard]] char peek() const { return text_[offset_]; }

    char get() {
      if (finished()) {
        throw std::runtime_error("unexpected end of JSON input");
      }
      return text_[offset_++];
    }

    Json parseObject(std::size_t depth) {
      expect('{');
      Object objectValue;
      skipWhitespace();
      if (!finished() && peek() == '}') {
        get();
        return Json{std::move(objectValue)};
      }

      while (true) {
        skipWhitespace();
        if (finished() || peek() != '"') {
          throw std::runtime_error("JSON object key must be a string");
        }
        std::string key = parseString();
        skipWhitespace();
        expect(':');
        auto inserted =
            objectValue.emplace(std::move(key), parseValue(depth + 1U));
        if (!inserted.second) {
          throw std::runtime_error("duplicate JSON object key");
        }
        skipWhitespace();
        const char separator = get();
        if (separator == '}') {
          break;
        }
        if (separator != ',') {
          throw std::runtime_error("expected ',' or '}' in JSON object");
        }
      }
      return Json{std::move(objectValue)};
    }

    Json parseArray(std::size_t depth) {
      expect('[');
      Array arrayValue;
      skipWhitespace();
      if (!finished() && peek() == ']') {
        get();
        return Json{std::move(arrayValue)};
      }

      while (true) {
        arrayValue.emplace_back(parseValue(depth + 1U));
        skipWhitespace();
        const char separator = get();
        if (separator == ']') {
          break;
        }
        if (separator != ',') {
          throw std::runtime_error("expected ',' or ']' in JSON array");
        }
      }
      return Json{std::move(arrayValue)};
    }

    std::string parseString() {
      expect('"');
      std::string value;
      while (!finished()) {
        const char ch = get();
        if (ch == '"') {
          if (!isValidUtf8(value)) {
            throw std::runtime_error(
                "JSON string contains malformed UTF-8");
          }
          return value;
        }
        if (static_cast<unsigned char>(ch) < 0x20U) {
          throw std::runtime_error("invalid control character in JSON string");
        }
        if (ch != '\\') {
          appendByte(value, ch);
          continue;
        }

        const char escaped = get();
        switch (escaped) {
          case '"':
          case '\\':
          case '/':
            appendByte(value, escaped);
            break;
          case 'b':
            appendByte(value, '\b');
            break;
          case 'f':
            appendByte(value, '\f');
            break;
          case 'n':
            appendByte(value, '\n');
            break;
          case 'r':
            appendByte(value, '\r');
            break;
          case 't':
            appendByte(value, '\t');
            break;
          case 'u':
            appendUnicodeEscape(value);
            break;
          default:
            throw std::runtime_error("unsupported JSON string escape");
        }
      }
      throw std::runtime_error("unterminated JSON string");
    }

    void appendByte(std::string& value, char byte) const {
      if (value.size() >= limits_.maximumStringBytes) {
        throw std::runtime_error(
            "JSON string exceeds the configured size limit");
      }
      value.push_back(byte);
    }

    [[nodiscard]] static bool isValidUtf8(std::string_view value) {
      std::size_t offset = 0;
      while (offset < value.size()) {
        const auto first =
            static_cast<std::uint8_t>(value[offset]);
        if (first <= 0x7fU) {
          ++offset;
          continue;
        }
        std::size_t continuations = 0;
        std::uint32_t codePoint = 0;
        if ((first & 0xe0U) == 0xc0U) {
          continuations = 1;
          codePoint = first & 0x1fU;
        } else if ((first & 0xf0U) == 0xe0U) {
          continuations = 2;
          codePoint = first & 0x0fU;
        } else if ((first & 0xf8U) == 0xf0U) {
          continuations = 3;
          codePoint = first & 0x07U;
        } else {
          return false;
        }
        if (value.size() - offset <= continuations) {
          return false;
        }
        for (std::size_t index = 1; index <= continuations;
             ++index) {
          const auto byte =
              static_cast<std::uint8_t>(value[offset + index]);
          if ((byte & 0xc0U) != 0x80U) {
            return false;
          }
          codePoint = (codePoint << 6U) | (byte & 0x3fU);
        }
        if ((continuations == 1 && codePoint < 0x80U) ||
            (continuations == 2 && codePoint < 0x800U) ||
            (continuations == 3 && codePoint < 0x10000U) ||
            codePoint > 0x10ffffU ||
            (codePoint >= 0xd800U && codePoint <= 0xdfffU)) {
          return false;
        }
        offset += continuations + 1U;
      }
      return true;
    }

    [[nodiscard]] std::uint16_t parseUnicodeCodeUnit() {
      std::uint16_t value = 0;
      for (int index = 0; index < 4; ++index) {
        value = static_cast<std::uint16_t>(value << 4U);
        const char ch = get();
        if (ch >= '0' && ch <= '9') {
          value = static_cast<std::uint16_t>(value + (ch - '0'));
        } else if (ch >= 'a' && ch <= 'f') {
          value = static_cast<std::uint16_t>(value + (ch - 'a' + 10));
        } else if (ch >= 'A' && ch <= 'F') {
          value = static_cast<std::uint16_t>(value + (ch - 'A' + 10));
        } else {
          throw std::runtime_error("invalid JSON unicode escape");
        }
      }
      return value;
    }

    void appendUnicodeEscape(std::string& value) {
      std::uint32_t codePoint = parseUnicodeCodeUnit();
      if (codePoint >= 0xD800U && codePoint <= 0xDBFFU) {
        if (get() != '\\' || get() != 'u') {
          throw std::runtime_error("invalid JSON unicode surrogate pair");
        }
        const std::uint32_t low = parseUnicodeCodeUnit();
        if (low < 0xDC00U || low > 0xDFFFU) {
          throw std::runtime_error("invalid JSON unicode surrogate pair");
        }
        codePoint =
            0x10000U + ((codePoint - 0xD800U) << 10U) + (low - 0xDC00U);
      } else if (codePoint >= 0xDC00U && codePoint <= 0xDFFFU) {
        throw std::runtime_error("unpaired JSON unicode low surrogate");
      }

      std::array<char, 4> encoded{};
      std::size_t length = 0;
      if (codePoint <= 0x7FU) {
        encoded[0] = static_cast<char>(codePoint);
        length = 1;
      } else if (codePoint <= 0x7FFU) {
        encoded[0] = static_cast<char>(0xC0U | (codePoint >> 6U));
        encoded[1] = static_cast<char>(0x80U | (codePoint & 0x3FU));
        length = 2;
      } else if (codePoint <= 0xFFFFU) {
        encoded[0] = static_cast<char>(0xE0U | (codePoint >> 12U));
        encoded[1] =
            static_cast<char>(0x80U | ((codePoint >> 6U) & 0x3FU));
        encoded[2] = static_cast<char>(0x80U | (codePoint & 0x3FU));
        length = 3;
      } else {
        encoded[0] = static_cast<char>(0xF0U | (codePoint >> 18U));
        encoded[1] =
            static_cast<char>(0x80U | ((codePoint >> 12U) & 0x3FU));
        encoded[2] =
            static_cast<char>(0x80U | ((codePoint >> 6U) & 0x3FU));
        encoded[3] = static_cast<char>(0x80U | (codePoint & 0x3FU));
        length = 4;
      }
      if (length > limits_.maximumStringBytes ||
          value.size() > limits_.maximumStringBytes - length) {
        throw std::runtime_error(
            "JSON string exceeds the configured size limit");
      }
      value.append(encoded.data(), length);
    }

    Number parseNumber() {
      const std::size_t start = offset_;
      if (peek() == '-') {
        get();
      }
      if (finished() ||
          !IsDigit(peek())) {
        throw std::runtime_error("invalid JSON number");
      }
      if (peek() == '0') {
        get();
        if (!finished() && IsDigit(peek())) {
          throw std::runtime_error("invalid leading zero in JSON number");
        }
      } else {
        while (!finished() &&
               IsDigit(peek())) {
          get();
        }
      }
      if (!finished() && peek() == '.') {
        get();
        if (finished() ||
            !IsDigit(peek())) {
          throw std::runtime_error("invalid JSON fraction");
        }
        while (!finished() &&
               IsDigit(peek())) {
          get();
        }
      }
      if (!finished() && (peek() == 'e' || peek() == 'E')) {
        get();
        if (!finished() && (peek() == '+' || peek() == '-')) {
          get();
        }
        if (finished() ||
            !IsDigit(peek())) {
          throw std::runtime_error("invalid JSON exponent");
        }
        while (!finished() &&
               IsDigit(peek())) {
          get();
        }
      }

      std::string lexeme = text_.substr(start, offset_ - start);
      double parsed = 0;
      const char* begin = lexeme.data();
      const char* end = begin + lexeme.size();
      const auto result = std::from_chars(
          begin, end, parsed, std::chars_format::general);
      if (result.ec != std::errc{} || result.ptr != end ||
          !std::isfinite(parsed)) {
        throw std::runtime_error("non-finite JSON number");
      }
      return Number{parsed, std::move(lexeme)};
    }

    void consumeLiteral(std::string_view literal) {
      for (const char expected : literal) {
        if (get() != expected) {
          throw std::runtime_error("invalid JSON literal");
        }
      }
    }

    void expect(char expected) {
      if (get() != expected) {
        throw std::runtime_error("unexpected JSON character");
      }
    }

    const std::string& text_;
    const Limits& limits_;
    std::size_t offset_ = 0;
    std::size_t nodeCount_ = 0;
  };

  std::variant<std::nullptr_t, Object, Array, std::string, bool, Number>
      value_ = nullptr;
};

}  // namespace ec_systemcore
