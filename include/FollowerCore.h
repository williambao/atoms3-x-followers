#pragma once

#include <ArduinoJson.h>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace followers {

// Factory defaults; saved serial settings override these values.
constexpr uint32_t DEFAULT_UPDATE_INTERVAL_SEC = 300;
constexpr uint32_t DEFAULT_NUMBER_COLOR = 0xA8FF60;
constexpr uint16_t DEFAULT_ROTATION = 0;
constexpr uint8_t DEFAULT_BRIGHTNESS = 160;
constexpr uint32_t MIN_INTERVAL_SEC = 60;
constexpr uint32_t MAX_INTERVAL_SEC = 86400;
constexpr uint32_t MANUAL_COOLDOWN_MS = 5000;
constexpr size_t MAX_COMMAND_BYTES = 1024;
constexpr size_t MAX_BODY_BYTES = 4096;

// ArduinoJson stops after the first complete object. Track consumption so a
// second object or trailing garbage cannot silently execute a partial command.
class CommandReader {
 public:
  CommandReader(const char* data, size_t length) : data_(data), length_(length) {}
  int read() {
    return position_ < length_ ? static_cast<unsigned char>(data_[position_++]) : -1;
  }
  size_t readBytes(char* output, size_t requested) {
    const size_t available = length_ - position_;
    const size_t count = requested < available ? requested : available;
    memcpy(output, data_ + position_, count);
    position_ += count;
    return count;
  }
  size_t position() const { return position_; }
 private:
  const char* data_;
  size_t length_;
  size_t position_ = 0;
};

inline bool parseCommand(const char* line, size_t length, JsonDocument& document) {
  CommandReader reader(line, length);
  if (deserializeJson(document, reader, DeserializationOption::NestingLimit(6)) ||
      !document.is<JsonObject>()) return false;
  for (size_t i = reader.position(); i < length; ++i) {
    const char c = line[i];
    if (c != ' ' && c != '\t' && c != '\r' && c != '\n') return false;
  }
  return true;
}

struct Config {
  char ssid[33] = {};
  char password[65] = {};
  char account[16] = {};  // Explicit setup avoids silently watching the wrong user.
  uint32_t intervalSec = DEFAULT_UPDATE_INTERVAL_SEC;
  uint32_t numberColor = DEFAULT_NUMBER_COLOR;
  uint16_t rotation = DEFAULT_ROTATION;
  bool autoRotation = true;
  uint8_t brightness = DEFAULT_BRIGHTNESS;
};

inline bool sameConfig(const Config& a, const Config& b) {
  return !strcmp(a.ssid, b.ssid) && !strcmp(a.password, b.password) &&
         !strcmp(a.account, b.account) && a.intervalSec == b.intervalSec &&
         a.numberColor == b.numberColor && a.rotation == b.rotation &&
         a.autoRotation == b.autoRotation &&
         a.brightness == b.brightness;
}

inline bool isHex(char c) {
  return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
         (c >= 'A' && c <= 'F');
}

inline bool readText(JsonVariantConst value, char* out, size_t capacity) {
  if (!value.is<const char*>()) return false;
  JsonString str = value.as<JsonString>();
  if (str.size() >= capacity || strlen(str.c_str()) != str.size()) return false;
  // Reject control bytes (including embedded NUL), but allow UTF-8 SSIDs.
  for (size_t i = 0; i < str.size(); ++i) {
    const auto c = static_cast<unsigned char>(str.c_str()[i]);
    if (c < 32 || c == 127) return false;
  }
  memcpy(out, str.c_str(), str.size() + 1);
  return true;
}

inline bool validAccount(const char* value) {
  const size_t len = strlen(value);
  if (!len || len > 15) return false;
  for (size_t i = 0; i < len; ++i) {
    const char c = value[i];
    if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
          (c >= '0' && c <= '9') || c == '_')) return false;
  }
  return true;
}

inline bool validPassword(const char* value) {
  const size_t len = strlen(value);
  if (!len || (len >= 8 && len <= 63)) return true;
  if (len != 64) return false;
  for (size_t i = 0; i < len; ++i) if (!isHex(value[i])) return false;
  return true;
}

// Stage a whole patch before touching live state or NVS. Missing fields retain
// their old values, including each individual member of the wifi object.
inline bool applyPatch(JsonObjectConst request, const Config& current,
                       Config& output, const char*& error) {
  Config next = current;
  error = nullptr;
  for (JsonPairConst pair : request) {
    const char* key = pair.key().c_str();
    if (strcmp(key, "cmd") && strcmp(key, "id") && strcmp(key, "wifi") &&
        strcmp(key, "account") && strcmp(key, "interval_sec") &&
        strcmp(key, "number_color") && strcmp(key, "rotation") && strcmp(key, "auto_rotation") &&
        strcmp(key, "brightness")) {
      error = "unknown_parameter";
      return false;
    }
  }
  if (!request["wifi"].isUnbound()) {
    if (!request["wifi"].is<JsonObjectConst>()) {
      error = "wifi_must_be_object";
      return false;
    }
    JsonObjectConst wifi = request["wifi"];
    for (JsonPairConst pair : wifi) {
      if (strcmp(pair.key().c_str(), "ssid") && strcmp(pair.key().c_str(), "password")) {
        error = "unknown_wifi_parameter";
        return false;
      }
    }
    if (!wifi["ssid"].isUnbound() && !readText(wifi["ssid"], next.ssid, sizeof(next.ssid))) {
      error = "invalid_wifi_ssid";
      return false;
    }
    if (!wifi["password"].isUnbound() &&
        (!readText(wifi["password"], next.password, sizeof(next.password)) ||
         !validPassword(next.password))) {
      error = "invalid_wifi_password";
      return false;
    }
  }
  if (!request["account"].isUnbound()) {
    char account[17] = {};
    if (!readText(request["account"], account, sizeof(account))) {
      error = "invalid_account";
      return false;
    }
    const char* normalized = account[0] == '@' ? account + 1 : account;
    // An explicit empty string clears the account; '@' alone is invalid.
    if (*account && !validAccount(normalized)) {
      error = "invalid_account";
      return false;
    }
    strcpy(next.account, normalized);
  }
  if (!request["interval_sec"].isUnbound()) {
    auto value = request["interval_sec"];
    if (!value.is<uint32_t>() || value.as<uint32_t>() < MIN_INTERVAL_SEC ||
        value.as<uint32_t>() > MAX_INTERVAL_SEC) {
      error = "interval_sec_must_be_60_to_86400";
      return false;
    }
    next.intervalSec = value.as<uint32_t>();
  }
  if (!request["number_color"].isUnbound()) {
    char color[8] = {};
    if (!readText(request["number_color"], color, sizeof(color)) ||
        strlen(color) != 7 || color[0] != '#') {
      error = "number_color_must_be_hex_RRGGBB";
      return false;
    }
    uint32_t rgb = 0;
    for (size_t i = 1; i < 7; ++i) {
      char c = color[i];
      if (!isHex(c)) {
        error = "number_color_must_be_hex_RRGGBB";
        return false;
      }
      uint32_t digit = c <= '9' ? c - '0' : (c <= 'F' ? c - 'A' + 10 : c - 'a' + 10);
      rgb = (rgb << 4) | digit;
    }
    next.numberColor = rgb;
  }
  if (!request["rotation"].isUnbound()) {
    auto value = request["rotation"];
    if (!value.is<uint16_t>() || value.as<uint16_t>() > 270 || value.as<uint16_t>() % 90) {
      error = "rotation_must_be_0_90_180_270";
      return false;
    }
    next.rotation = value.as<uint16_t>();
  }
  if (!request["auto_rotation"].isUnbound()) {
    if (!request["auto_rotation"].is<bool>()) {
      error = "auto_rotation_must_be_boolean";
      return false;
    }
    next.autoRotation = request["auto_rotation"].as<bool>();
  }
  if (!request["brightness"].isUnbound()) {
    auto value = request["brightness"];
    if (!value.is<uint16_t>() || value.as<uint16_t>() < 1 || value.as<uint16_t>() > 255) {
      error = "brightness_must_be_1_to_255";
      return false;
    }
    next.brightness = value.as<uint8_t>();
  }
  output = next;
  return true;
}

inline void writeConfig(JsonObject target, const Config& config, bool includePassword = false) {
  JsonObject wifi = target["wifi"].to<JsonObject>();
  wifi["ssid"] = config.ssid;
  if (includePassword) wifi["password"] = config.password;
  else wifi["password_set"] = config.password[0] != '\0';
  target["account"] = config.account;
  target["interval_sec"] = config.intervalSec;
  char color[8];
  snprintf(color, sizeof(color), "#%06lX", static_cast<unsigned long>(config.numberColor));
  target["number_color"] = color;
  target["rotation"] = config.rotation;
  target["auto_rotation"] = config.autoRotation;
  target["brightness"] = config.brightness;
}

inline bool accountEquals(const char* a, const char* b) {
  while (*a && *b) {
    char ca = *a++, cb = *b++;
    if (ca >= 'A' && ca <= 'Z') ca += 'a' - 'A';
    if (cb >= 'A' && cb <= 'Z') cb += 'a' - 'A';
    if (ca != cb) return false;
  }
  return *a == *b;
}

inline bool parseProfile(JsonObjectConst profile, const char* expectedAccount,
                         uint64_t& count, const char*& error) {
  error = nullptr;
  if (!profile["code"].is<int>()) {
    error = "invalid_profile_code";
    return false;
  }
  const int code = profile["code"].as<int>();
  if (code != 200) {
    switch (code) {
      case 404: error = "content_unavailable"; break;
      case 401: case 403: error = "access_denied"; break;
      case 429: error = "rate_limited"; break;
      default: error = "api_error"; break;
    }
    return false;
  }
  JsonObjectConst user = profile["user"].as<JsonObjectConst>();
  char type[16] = {}, handle[17] = {};
  if (!readText(user["type"], type, sizeof(type)) || strcmp(type, "profile")) {
    error = "unexpected_profile";
    return false;
  }
  if (!readText(user["screen_name"], handle, sizeof(handle)) ||
      !validAccount(handle[0] == '@' ? handle + 1 : handle) ||
      !accountEquals(handle[0] == '@' ? handle + 1 : handle, expectedAccount)) {
    error = "account_mismatch";
    return false;
  }
  if (!user["followers"].is<uint64_t>()) {
    error = "invalid_followers";
    return false;
  }
  count = user["followers"].as<uint64_t>();
  return true;
}

// Signed difference handles millis() wrapping every ~49.7 days. All scheduled
// delays are less than INT32_MAX, so this comparison is unambiguous.
inline bool due(uint32_t now, uint32_t deadline) {
  return static_cast<int32_t>(now - deadline) >= 0;
}

inline uint32_t retryDelayMs(uint8_t failures) {
  const uint8_t shift = failures > 6 ? 5 : (failures ? failures - 1 : 0);
  const uint32_t delay = 15000u << shift;
  return delay > 300000u ? 300000u : delay;
}

}  // namespace followers
