#pragma once

#include <ArduinoJson.h>

#include <array>
#include <functional>
#include <string>
#include <string_view>

// One bounded foreground form, owned by the existing setup-hotspot Activity.
// No new background service; labels and values must survive HTTP/input frames.
struct PhoneSetupForm {
  static constexpr size_t MAX_FIELDS = 6;
  static constexpr size_t MAX_FIELD_BYTES = 512;
  enum class Result { Saved, Invalid, SaveFailed };
  enum class Kind { Text, Password, Url };
  struct Field {
    const char* label = "";
    std::string value;
    size_t maxBytes = MAX_FIELD_BYTES;
    Kind kind = Kind::Text;
    bool required = false;
  };
  using Values = std::array<std::string, MAX_FIELDS>;

  std::string title;
  std::array<Field, MAX_FIELDS> fields;
  size_t count = 0;
  std::function<Result(const Values&)> submit;

  void describe(JsonDocument& doc) const {
    doc["title"] = title;
    auto list = doc["fields"].to<JsonArray>();
    for (size_t i = 0; i < count; ++i) {
      const auto& field = fields[i];
      auto item = list.add<JsonObject>();
      item["label"] = field.label;
      item["maxBytes"] = field.maxBytes;
      item["required"] = field.required;
      item["secret"] = field.kind == Kind::Password;
      item["url"] = field.kind == Kind::Url;
      if (field.kind == Kind::Password)
        item["hasValue"] = !field.value.empty();
      else
        item["value"] = field.value;
    }
  }

  static bool validValue(const Field& field, const std::string_view value) {
    if (value.size() > field.maxBytes || value.size() > MAX_FIELD_BYTES || (field.required && value.empty()) ||
        value.find('\0') != value.npos || value.find_first_of("\r\n") != value.npos)
      return false;
    if (field.kind != Kind::Url || value.empty()) return true;
    const size_t prefix = value.substr(0, 8) == "https://" ? 8 : value.substr(0, 7) == "http://" ? 7 : 0;
    if (!prefix || value.find_first_of(" \t") != value.npos) return false;
    const auto host = value.substr(prefix);
    return !host.empty() && host.find_first_of("/?#") != 0;
  }

  Result accept(const JsonDocument& doc) {
    const auto list = doc["values"].as<JsonArrayConst>();
    if (!count || count > MAX_FIELDS || list.size() != count || !submit) return Result::Invalid;
    // Six strings are bounded to 3 KiB of character data. This cold-path
    // candidate survives validation/save without mutating the current form.
    Values next;
    for (size_t i = 0; i < count; ++i) {
      if (list[i].isNull() && fields[i].kind == Kind::Password) {
        if (!validValue(fields[i], fields[i].value)) return Result::Invalid;
        next[i] = fields[i].value;  // Untouched secrets never travel back to the browser.
        continue;
      }
      if (!list[i].is<const char*>()) return Result::Invalid;
      const auto value = list[i].as<JsonString>();
      if (!validValue(fields[i], {value.c_str(), value.size()})) return Result::Invalid;
      next[i].assign(value.c_str(), value.size());
    }
    return submit(next);
  }
};
