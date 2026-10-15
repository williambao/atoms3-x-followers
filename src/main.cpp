// AtomS3 X follower counter. Serial protocol: one JSON object per line.
// PlatformIO / Arduino-ESP32, M5Unified, M5GFX and ArduinoJson 7.
#include <Arduino.h>
#include <M5Unified.h>
#include <WiFi.h>
#include <Preferences.h>
#include <esp_crt_bundle.h>
#include <esp_http_client.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>
#include <cmath>
#include <ctime>
#include <strings.h>
#include "FollowerCore.h"
#include "DisplayLogic.h"

using namespace followers;

constexpr uint32_t WIFI_CONNECT_TIMEOUT_MS = 20000;
constexpr uint32_t WIFI_RETRY_MS = 30000;
constexpr uint32_t CLOCK_WAIT_MS = 20000;
constexpr uint32_t HTTP_TIMEOUT_MS = 12000;
constexpr uint32_t STALE_MIN_SEC = 600;
constexpr uint32_t BACKGROUND_RGB = 0x080C12;
constexpr uint32_t MUTED_RGB = 0x9BA6B5;
constexpr uint32_t WARNING_RGB = 0xFFBE55;
constexpr const char* PROFILE_URL = "https://api.fxtwitter.com/2/profile/";

struct FetchRequest {
  uint32_t generation;
  char account[16];
  char cause[16];
};

struct FetchResult {
  uint32_t generation;
  char account[16];
  char cause[16];
  bool ok;
  bool retryable;
  uint64_t followers;
  int httpStatus;
  uint32_t retryAfterSec;
  char fetchedAt[40];
  char error[64];
};

struct HttpBody {
  char text[MAX_BODY_BYTES + 1];
  size_t length;
  bool overflow;
  uint32_t retryAfterSec;
};

Config config;
Preferences preferences;
M5Canvas canvas(&M5.Display);
QueueHandle_t requestQueue = nullptr;
QueueHandle_t resultQueue = nullptr;
bool storageReady = false;
bool configLoadError = false;
bool canvasReady = false;
bool workerReady = false;
bool inFlight = false;
bool refreshPending = true;
bool hasFollowers = false;
bool dirty = true;
bool wasWifiConnected = false;
bool wifiConnecting = false;
bool ntpStarted = false;
bool lastFetchFailed = false;
bool retryBlocked = false;
bool terminalApiError = false;
uint32_t generation = 1;
uint32_t wifiAttemptAt = 0;
uint32_t nextWifiAttemptAt = 0;
uint32_t wifiConnectedAt = 0;
uint32_t nextFetchAt = 0;
int64_t retryAllowedUs = 0;
uint32_t lastFetchStartedAt = 0;
bool hasStartedFetch = false;
uint8_t failures = 0;
uint64_t followerCount = 0;
int64_t lastSuccessUs = 0;
time_t sourceTime = 0;
char sourceFetchedAt[40] = {};
char lastError[64] = {};
int lastHttpStatus = 0;
char pendingCause[16] = "startup";
char serialLine[MAX_COMMAND_BYTES + 2] = {};  // Payload, optional CR, terminator.
size_t serialLength = 0;
bool serialOverflow = false;
RotationFilter rotationFilter;
bool imuAvailable = false;
uint16_t effectiveRotation = 0;
float accelX = 0, accelY = 0, accelZ = 0;

static void emit(JsonDocument& document) {
  serializeJson(document, Serial);
  Serial.println();
}

static void replyBase(JsonDocument& document, JsonObjectConst request, const char* command) {
  if (command) document["cmd"] = command;
  if (!request["id"].isUnbound()) document["id"] = request["id"];
}

static void replyError(JsonObjectConst request, const char* command, const char* error) {
  JsonDocument response;
  replyBase(response, request, command);
  response["ok"] = false;
  response["error"] = error;
  emit(response);
}

static bool clockReady() {
  return time(nullptr) >= 1704067200;  // 2024-01-01 UTC; TLS needs a sane clock.
}

static uint32_t staleThresholdSec() {
  const uint32_t threshold = config.intervalSec * 2;
  return threshold < STALE_MIN_SEC ? STALE_MIN_SEC : threshold;
}

static bool dataStale() {
  if (!hasFollowers || lastFetchFailed) return true;
  const int64_t ageSec = (esp_timer_get_time() - lastSuccessUs) / 1000000;
  if (ageSec >= staleThresholdSec()) return true;
  return sourceTime && clockReady() && time(nullptr) - sourceTime >= staleThresholdSec();
}

static const char* screenStatus() {
  if (!workerReady || !canvasReady) return "SYSTEM ERROR";
  if (!storageReady) return "STORAGE ERROR";
  if (configLoadError) return "CONFIG ERROR";
  if (!config.ssid[0]) return "SET WIFI";
  if (!config.account[0]) return "SET ACCOUNT";
  if (WiFi.status() != WL_CONNECTED) return wifiConnecting ? "CONNECTING" : "WIFI OFFLINE";
  if (!clockReady()) {
    return due(millis(), wifiConnectedAt + CLOCK_WAIT_MS) ? "TIME ERROR" : "SYNC TIME";
  }
  if (inFlight) return "REFRESHING";
  if (lastFetchFailed) {
    if (!strcmp(lastError, "rate_limited")) return "RATE LIMITED";
    if (!strcmp(lastError, "content_unavailable")) return "NO PROFILE";
    if (!strcmp(lastError, "login_required")) return "LOGIN REQUIRED";
    if (!strcmp(lastError, "payment_required")) return "API PAYMENT";
    if (!strcmp(lastError, "timeout")) return "TIMEOUT";
    if (!strcmp(lastError, "invalid_json") || !strcmp(lastError, "invalid_profile_code") ||
        !strcmp(lastError, "invalid_fetched_at") ||
        !strcmp(lastError, "response_too_large") ||
        !strcmp(lastError, "invalid_followers") || !strcmp(lastError, "account_mismatch") ||
        !strcmp(lastError, "unexpected_profile")) return "BAD DATA";
    return "API ERROR";
  }
  if (hasFollowers && dataStale()) return "STALE DATA";
  return hasFollowers ? "READY" : "WAITING";
}

static bool isBusyStatus(const char* status) {
  return !strcmp(status, "REFRESHING") || !strcmp(status, "CONNECTING") ||
         !strcmp(status, "SYNC TIME") || !strcmp(status, "WAITING");
}

static void drawScreen(uint32_t now) {
  if (!canvasReady) return;
  static uint32_t lastDrawAt = 0;
  static const char* lastStatus = nullptr;
  const char* status = screenStatus();
  if (lastStatus != status) dirty = true;
  const bool busy = isBusyStatus(status);
  if (!dirty && (!busy || now - lastDrawAt < 100)) return;
  if (now - lastDrawAt < 30) return;
  dirty = false;
  lastDrawAt = now;
  lastStatus = status;
  canvas.fillScreen(BACKGROUND_RGB);
  canvas.setTextDatum(middle_center);
  canvas.setFont(&fonts::Font0);
  canvas.setTextSize(1);
  canvas.setTextColor(MUTED_RGB);
  char title[20];
  if (config.account[0]) snprintf(title, sizeof(title), "@%s", config.account);
  else strcpy(title, "X FOLLOWERS");
  canvas.drawString(title, 62, 14);

  // Normal state is silent. An animation or amber mark appears only when needed.
  if (busy) {
    const float angle = (now % 1200) * 6.2831853f / 1200.0f;
    canvas.drawCircle(120, 7, 3, 0x263241);
    canvas.fillCircle(120 + lroundf(cosf(angle) * 3), 7 + lroundf(sinf(angle) * 3), 1, MUTED_RGB);
  } else if (strcmp(status, "READY")) {
    canvas.fillCircle(120, 7, 2, WARNING_RGB);
  }

  const bool showCount = hasFollowers && config.ssid[0] && config.account[0] &&
                         workerReady && storageReady && !configLoadError;
  if (showCount) {
    const DisplayNumber number = formatFollowers(followerCount);
    const int unitSpace = number.unit[0] ? 20 : 0;
    const DigitLayout layout = drawSevenSegment(canvas, number.text, config.numberColor, unitSpace);
    if (number.unit[0]) {
      canvas.setFont(&fonts::Font2);
      canvas.setTextSize(1);
      canvas.setTextDatum(bottom_left);
      canvas.setTextColor(config.numberColor);
      canvas.drawString(number.unit, layout.x + layout.totalWidth + 4, layout.y + layout.height);
      canvas.setTextDatum(middle_center);
    }
  } else {
    canvas.setFont(&fonts::FreeSansBold12pt7b);
    canvas.setTextSize(1);
    canvas.setTextColor(isBusyStatus(status) ? MUTED_RGB : WARNING_RGB);
    // Two lines preserve readable lettering on the 128-pixel square.
    char message[24];
    snprintf(message, sizeof(message), "%s", status);
    char* separator = strchr(message, ' ');
    if (separator) {
      *separator = '\0';
      canvas.drawString(message, 64, 53);
      const char* second = separator + 1;
      const int width = canvas.textWidth(second);
      if (width > 116) canvas.setTextSize(116.0f / width);
      canvas.drawString(second, 64, 80);
    } else {
      const int width = canvas.textWidth(message);
      if (width > 116) canvas.setTextSize(116.0f / width);
      canvas.drawString(message, 64, 68);
    }
  }
  canvas.setFont(&fonts::Font0);
  canvas.setTextSize(1);
  const bool countWarning = showCount && strcmp(status, "READY") && strcmp(status, "REFRESHING");
  canvas.setTextColor(countWarning ? WARNING_RGB : MUTED_RGB);
  canvas.drawString(showCount ? (countWarning ? status : "FOLLOWERS") :
                    (isBusyStatus(status) ? "PLEASE WAIT" : "SERIAL: STATUS"), 64, 115);
  canvas.pushSprite(0, 0);
}

static bool saveConfig(const Config& value) {
  if (!storageReady) return false;
  JsonDocument stored;
  stored["schema"] = 1;
  writeConfig(stored["config"].to<JsonObject>(), value, true);
  String encoded;
  serializeJson(stored, encoded);
  // A single NVS value keeps the configuration update atomic across power loss.
  return preferences.putString("config", encoded) == encoded.length();
}

static void loadConfig() {
  storageReady = preferences.begin("x-followers", false);
  if (!storageReady || !preferences.isKey("config")) return;
  String encoded = preferences.getString("config");
  JsonDocument stored;
  const char* error = nullptr;
  Config loaded;
  if (encoded.length() > MAX_COMMAND_BYTES || deserializeJson(stored, encoded) ||
      stored["schema"] != 1 || !stored["config"].is<JsonObjectConst>() ||
      !applyPatch(stored["config"].as<JsonObjectConst>(), config, loaded, error)) {
    configLoadError = true;
    return;
  }
  config = loaded;
}

static uint32_t parseRetryAfter(const char* value) {
  if (!value || !*value) return 0;
  bool numeric = true;
  uint64_t seconds = 0;
  for (const char* p = value; *p; ++p) {
    if (*p < '0' || *p > '9') { numeric = false; break; }
    seconds = seconds * 10 + (*p - '0');
    if (seconds > UINT32_MAX) seconds = UINT32_MAX;
  }
  if (numeric) return static_cast<uint32_t>(seconds);
  tm date = {};
  char* end = strptime(value, "%a, %d %b %Y %H:%M:%S GMT", &date);
  if (!end || *end) return 0;
  const time_t until = mktime(&date);  // configTime(0, 0, ...) sets UTC.
  const time_t now = time(nullptr);
  if (until <= now) return 0;
  const uint64_t difference = static_cast<uint64_t>(until - now);
  return difference > UINT32_MAX ? UINT32_MAX : static_cast<uint32_t>(difference);
}

static esp_err_t httpEvent(esp_http_client_event_t* event) {
  auto* body = static_cast<HttpBody*>(event->user_data);
  if (event->event_id == HTTP_EVENT_ON_HEADER &&
      !strcasecmp(event->header_key, "Retry-After")) {
    body->retryAfterSec = parseRetryAfter(event->header_value);
  }
  if (event->event_id == HTTP_EVENT_ON_DATA) {
    if (event->data_len < 0 || body->length + static_cast<size_t>(event->data_len) > MAX_BODY_BYTES) {
      body->overflow = true;
      return ESP_FAIL;
    }
    memcpy(body->text + body->length, event->data, event->data_len);
    body->length += event->data_len;
    body->text[body->length] = '\0';
  }
  return ESP_OK;
}

static const char* httpError(int status) {
  switch (status) {
    case 400: return "bad_request";
    case 401: case 403: return "access_denied";
    case 402: return "payment_required";
    case 404: return "content_unavailable";
    case 422: return "login_required";
    case 429: return "rate_limited";
    default: return "http_error";
  }
}

static time_t parseSourceTime(const char* value);

static void fetchWorker(void*) {
  FetchRequest request;
  while (true) {
    if (xQueueReceive(requestQueue, &request, portMAX_DELAY) != pdTRUE) continue;
    FetchResult result = {};
    result.generation = request.generation;
    strcpy(result.account, request.account);
    strcpy(result.cause, request.cause);
    result.retryable = true;
    HttpBody body = {};
    char url[160];
    snprintf(url, sizeof(url), "%s%s", PROFILE_URL, request.account);
    esp_http_client_config_t settings = {};
    settings.url = url;
    settings.timeout_ms = HTTP_TIMEOUT_MS;
    settings.crt_bundle_attach = esp_crt_bundle_attach;
    settings.event_handler = httpEvent;
    settings.user_data = &body;
    settings.user_agent = "AtomS3-X-Followers/1.1.1";
    settings.disable_auto_redirect = true;
    settings.buffer_size = 1024;
    esp_http_client_handle_t client = esp_http_client_init(&settings);
    esp_err_t transport = ESP_ERR_NO_MEM;
    if (client) {
      transport = esp_http_client_perform(client);
      result.httpStatus = esp_http_client_get_status_code(client);
      esp_http_client_cleanup(client);
    }
    result.retryAfterSec = body.retryAfterSec;
    if (result.httpStatus >= 400) {
      strcpy(result.error, httpError(result.httpStatus));
      result.retryable = result.httpStatus == 404 || result.httpStatus == 408 || result.httpStatus == 429 || result.httpStatus >= 500;
    } else if (body.overflow) strcpy(result.error, "response_too_large");
    else if (transport != ESP_OK) {
      strcpy(result.error, transport == ESP_ERR_TIMEOUT ? "timeout" : "network_or_tls_error");
    } else if (result.httpStatus != 200) {
      strcpy(result.error, httpError(result.httpStatus));
      result.retryable = result.httpStatus == 404 || result.httpStatus == 408 || result.httpStatus == 429 || result.httpStatus >= 500;
    } else {
      JsonDocument document;
      // Filter keeps unused fields (avatar, bio, etc.) out of the parsed document.
      JsonDocument filter;
      filter["code"] = true;
      filter["user"]["type"] = true;
      filter["user"]["screen_name"] = true;
      filter["user"]["followers"] = true;
      const char* error = nullptr;
      if (deserializeJson(document, body.text, body.length,
                          DeserializationOption::Filter(filter), DeserializationOption::NestingLimit(8)) ||
          !document.is<JsonObject>()) {
        strcpy(result.error, "invalid_json");
      } else if (!parseProfile(document.as<JsonObjectConst>(), request.account, result.followers, error)) {
        snprintf(result.error, sizeof(result.error), "%s", error);
        const int apiCode = document["code"] | 0;
        result.retryable = apiCode == 0 || apiCode == 200 || apiCode == 404 || apiCode == 408 || apiCode == 429 || apiCode >= 500;
      } else result.ok = true;
    }
    // Only the main loop touches the display, config, serial or NVS.
    xQueueSend(resultQueue, &result, portMAX_DELAY);
  }
}

static time_t parseSourceTime(const char* value) {
  if (!value[0]) return 0;
  tm date = {};
  char* end = strptime(value, "%Y-%m-%dT%H:%M:%S", &date);
  if (!end) return 0;
  if (*end == '.') {
    ++end;
    if (*end < '0' || *end > '9') return 0;
    while (*end >= '0' && *end <= '9') ++end;
  }
  if (*end != 'Z' || end[1]) return 0;
  const tm expected = date;
  const time_t result = mktime(&date);
  if (result <= 0 || date.tm_year != expected.tm_year || date.tm_mon != expected.tm_mon ||
      date.tm_mday != expected.tm_mday || date.tm_hour != expected.tm_hour ||
      date.tm_min != expected.tm_min || date.tm_sec != expected.tm_sec) return 0;
  return result;
}

static void resetFetchState() {
  lastFetchFailed = false;
  terminalApiError = false;
  retryBlocked = false;
  failures = 0;
  lastError[0] = '\0';
  lastHttpStatus = 0;
  hasStartedFetch = false;
  refreshPending = true;
  strcpy(pendingCause, "config");
  nextFetchAt = millis();
}

static void restartWifi() {
  // Credentials are managed exclusively by our atomic NVS config record.
  WiFi.disconnect(false, false);
  wifiConnecting = false;
  wasWifiConnected = false;
  ntpStarted = false;
  nextWifiAttemptAt = millis();
  dirty = true;
}

static void serviceWifi(uint32_t now) {
  if (!config.ssid[0]) return;
  const bool connected = WiFi.status() == WL_CONNECTED;
  if (connected) {
    if (!wasWifiConnected) {
      wifiConnectedAt = now;
      wifiConnecting = false;
      refreshPending = true;
      strcpy(pendingCause, "wifi");
      dirty = true;
    }
    if (!ntpStarted) {
      configTime(0, 0, "time.cloudflare.com", "pool.ntp.org", "time.google.com");
      ntpStarted = true;
    }
  } else {
    if (wasWifiConnected) {
      wifiConnecting = false;
      nextWifiAttemptAt = now;
      dirty = true;
    }
    if (wifiConnecting && now - wifiAttemptAt >= WIFI_CONNECT_TIMEOUT_MS) {
      WiFi.disconnect(false, false);
      wifiConnecting = false;
      nextWifiAttemptAt = now + WIFI_RETRY_MS;
      dirty = true;
    }
    if (!wifiConnecting && due(now, nextWifiAttemptAt)) {
      WiFi.begin(config.ssid, config.password);
      wifiAttemptAt = now;
      wifiConnecting = true;
      dirty = true;
    }
  }
  wasWifiConnected = connected;
}

static void serviceFetch(uint32_t now) {
  if (!workerReady || !storageReady || configLoadError || !config.ssid[0] ||
      !config.account[0] || WiFi.status() != WL_CONNECTED || !clockReady() || inFlight) return;
  if (retryBlocked) {
    if (esp_timer_get_time() < retryAllowedUs) return;
    retryBlocked = false;
    nextFetchAt = now;
  }
  if (!refreshPending && (terminalApiError || !due(now, nextFetchAt))) return;
  if (hasStartedFetch && now - lastFetchStartedAt < MANUAL_COOLDOWN_MS) return;
  FetchRequest request = {};
  request.generation = generation;
  strcpy(request.account, config.account);
  strcpy(request.cause, refreshPending ? pendingCause : "timer");
  if (xQueueSend(requestQueue, &request, 0) != pdTRUE) return;
  inFlight = true;
  refreshPending = false;
  hasStartedFetch = true;
  lastFetchStartedAt = now;
  dirty = true;
}

static void serviceResult(uint32_t now) {
  if (!resultQueue) return;
  FetchResult result;
  if (xQueueReceive(resultQueue, &result, 0) != pdTRUE) return;
  inFlight = false;
  dirty = true;
  // A result from a previous account/Wi-Fi configuration cannot update the UI.
  if (result.generation != generation) return;
  lastHttpStatus = result.httpStatus;
  lastFetchFailed = !result.ok;
  snprintf(lastError, sizeof(lastError), "%s", result.error);
  if (result.ok) {
    followerCount = result.followers;
    hasFollowers = true;
    lastSuccessUs = esp_timer_get_time();
    strcpy(sourceFetchedAt, result.fetchedAt);
    sourceTime = parseSourceTime(result.fetchedAt);
    failures = 0;
    retryBlocked = false;
    terminalApiError = false;
    nextFetchAt = now + config.intervalSec * 1000;
  } else {
    if (failures < 255) ++failures;
    terminalApiError = !result.retryable;
    uint32_t wait = retryDelayMs(failures);
    if (!strcmp(result.error, "content_unavailable") && wait < config.intervalSec * 1000) {
      wait = config.intervalSec * 1000;
    }
    nextFetchAt = now + wait;
    retryBlocked = result.retryAfterSec > 0 || result.httpStatus == 429 || !strcmp(result.error, "rate_limited");
    const int64_t serverWaitUs = static_cast<int64_t>(result.retryAfterSec) * 1000000;
    const int64_t backoffUs = static_cast<int64_t>(wait) * 1000;
    retryAllowedUs = esp_timer_get_time() + (serverWaitUs > backoffUs ? serverWaitUs : backoffUs);
  }
  JsonDocument event;
  event["event"] = "refresh";
  event["account"] = result.account;
  event["cause"] = result.cause;
  event["ok"] = result.ok;
  event["http_status"] = result.httpStatus;
  event["stale"] = dataStale();
  if (result.ok) {
    event["followers"] = followerCount;
    if (sourceFetchedAt[0]) event["fetched_at"] = sourceFetchedAt;
  } else {
    event["error"] = result.error;
    event["retryable"] = result.retryable;
    if (!terminalApiError) {
      event["retry_in_sec"] = retryBlocked ?
        (retryAllowedUs - esp_timer_get_time() + 999999) / 1000000 :
        (nextFetchAt - now + 999) / 1000;
    }
  }
  emit(event);
}

static const char* requestManualRefresh(const char* cause) {
  if (!workerReady || !canvasReady) return "system_error";
  if (!storageReady) return "storage_error";
  if (configLoadError) return "config_error";
  if (!config.ssid[0]) return "wifi_not_configured";
  if (!config.account[0]) return "account_not_configured";
  if (inFlight) return "busy";
  const uint32_t now = millis();
  if (retryBlocked && esp_timer_get_time() < retryAllowedUs) return "rate_limit_cooldown";
  if (hasStartedFetch && now - lastFetchStartedAt < MANUAL_COOLDOWN_MS) return "refresh_cooldown";
  refreshPending = true;
  strcpy(pendingCause, cause);
  terminalApiError = false;
  if (WiFi.status() != WL_CONNECTED && !wifiConnecting) nextWifiAttemptAt = now;
  dirty = true;
  return nullptr;
}

static void writeStatus(JsonDocument& response) {
  response["account"] = config.account;
  response["state"] = screenStatus();
  response["wifi_configured"] = config.ssid[0] != '\0';
  response["account_configured"] = config.account[0] != '\0';
  response["wifi_connected"] = WiFi.status() == WL_CONNECTED;
  response["clock_synced"] = clockReady();
  response["busy"] = inFlight;
  response["refresh_pending"] = refreshPending;
  response["stale"] = dataStale();
  response["last_fetch_failed"] = lastFetchFailed;
  response["storage_ready"] = storageReady;
  response["data_source"] = "fxembed";
  response["auto_rotation"] = config.autoRotation;
  response["effective_rotation"] = effectiveRotation;
  response["imu_available"] = imuAvailable;
  if (imuAvailable) {
    JsonObject accel = response["accel_g"].to<JsonObject>();
    accel["x"] = accelX;
    accel["y"] = accelY;
    accel["z"] = accelZ;
  }
  response["http_status"] = lastHttpStatus;
  response["uptime_sec"] = esp_timer_get_time() / 1000000;
  if (hasFollowers) {
    response["followers"] = followerCount;
    response["last_success_age_sec"] = (esp_timer_get_time() - lastSuccessUs) / 1000000;
  } else response["followers"] = nullptr;
  if (sourceFetchedAt[0]) response["fetched_at"] = sourceFetchedAt;
  if (lastError[0]) response["error"] = lastError;
  response["automatic_retry_paused"] = terminalApiError;
  if (retryBlocked && esp_timer_get_time() < retryAllowedUs) {
    response["retry_after_sec"] = (retryAllowedUs - esp_timer_get_time() + 999999) / 1000000;
  }
  if (WiFi.status() == WL_CONNECTED) {
    response["rssi"] = WiFi.RSSI();
    response["ip"] = WiFi.localIP().toString();
  }
}

static void processCommand(const char* line, size_t length) {
  JsonDocument document;
  if (!parseCommand(line, length, document)) {
    replyError(JsonObjectConst(), nullptr, "invalid_json_object");
    return;
  }
  JsonObjectConst request = document.as<JsonObjectConst>();
  char command[16];
  if (!readText(request["cmd"], command, sizeof(command)) || !command[0]) {
    replyError(request, nullptr, "missing_or_invalid_cmd");
    return;
  }
  JsonDocument response;
  replyBase(response, request, command);
  response["ok"] = true;
  if (!strcmp(command, "set")) {
    Config next;
    const char* error = nullptr;
    if (!applyPatch(request, config, next, error)) {
      replyError(request, command, error);
      return;
    }
    const bool changed = !sameConfig(next, config);
    if ((changed || configLoadError) && !saveConfig(next)) {
      replyError(request, command, "storage_write_failed");
      return;
    }
    const bool wifiChanged = strcmp(next.ssid, config.ssid) || strcmp(next.password, config.password);
    const bool accountChanged = strcmp(next.account, config.account);
    const bool intervalChanged = next.intervalSec != config.intervalSec;
    const bool autoModeChanged = next.autoRotation != config.autoRotation;
    const bool rotationChanged = next.rotation != config.rotation || autoModeChanged;
    const bool repairingConfig = configLoadError;
    config = next;
    configLoadError = false;
    if (wifiChanged || accountChanged || repairingConfig) {
      ++generation;
      resetFetchState();
    }
    if (accountChanged) {
      hasFollowers = false;
      followerCount = 0;
      sourceTime = 0;
      sourceFetchedAt[0] = '\0';
    }
    if (wifiChanged) restartWifi();
    if (intervalChanged && !lastFetchFailed && !refreshPending) {
      nextFetchAt = millis() + config.intervalSec * 1000;
    }
    if (!config.autoRotation && rotationChanged) {
      effectiveRotation = config.rotation;
      M5.Display.setRotation(effectiveRotation / 90);
    }
    if (rotationChanged) rotationFilter.reset(effectiveRotation);
    M5.Display.setBrightness(config.brightness);
    dirty = true;
    response["changed"] = changed;
    response["saved"] = storageReady;
  } else if (!strcmp(command, "get")) {
    writeConfig(response["config"].to<JsonObject>(), config);
  } else if (!strcmp(command, "status")) {
    writeStatus(response);
  } else if (!strcmp(command, "refresh")) {
    const char* error = requestManualRefresh("serial");
    if (error) {
      replyError(request, command, error);
      return;
    }
    response["state"] = "queued";
  } else if (!strcmp(command, "help")) {
    response["protocol"] = "JSON Lines";
    response["id_optional"] = true;
    response["commands"] = "set,get,status,refresh,help";
    response["set_parameters"] = "wifi.ssid,wifi.password,account,interval_sec,number_color,rotation,auto_rotation,brightness";
    response["set_semantics"] = "Only supplied parameters change; validated and saved atomically";
  } else {
    replyError(request, command, "unknown_cmd");
    return;
  }
  emit(response);
}

static void serviceSerial() {
  // Bound work per loop so a stream of serial input cannot freeze the display.
  size_t budget = 256;
  while (budget-- && Serial.available()) {
    const int value = Serial.read();
    if (value < 0) break;
    const char c = static_cast<char>(value);
    if (c == '\n') {
      if (serialLength && serialLine[serialLength - 1] == '\r') --serialLength;
      if (serialOverflow || serialLength > MAX_COMMAND_BYTES) replyError(JsonObjectConst(), nullptr, "command_too_long");
      else if (serialLength) {
        serialLine[serialLength] = '\0';
        processCommand(serialLine, serialLength);
      }
      // Avoid retaining a password in the serial buffer after processing.
      memset(serialLine, 0, sizeof(serialLine));
      serialLength = 0;
      serialOverflow = false;
    } else if (!serialOverflow) {
      if (serialLength >= MAX_COMMAND_BYTES + 1) serialOverflow = true;
      else serialLine[serialLength++] = c;
    }
  }
}

void setup() {
  // A USB command burst can exceed HWCDC's 256-byte default RX queue.
  Serial.setRxBufferSize(4096);
  Serial.begin(115200);
  auto hardware = M5.config();
  hardware.clear_display = true;
  hardware.internal_imu = true;
  hardware.internal_mic = false;
  hardware.internal_spk = false;
  M5.begin(hardware);
  imuAvailable = M5.Imu.isEnabled();
  loadConfig();
  effectiveRotation = config.rotation;
  rotationFilter.reset(effectiveRotation);
  M5.Display.setRotation(effectiveRotation / 90);
  M5.Display.setBrightness(config.brightness);
  canvas.setColorDepth(16);
  canvasReady = canvas.createSprite(128, 128) != nullptr;
  WiFi.persistent(false);
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(false);
  requestQueue = xQueueCreate(1, sizeof(FetchRequest));
  resultQueue = xQueueCreate(1, sizeof(FetchResult));
  if (requestQueue && resultQueue) {
    workerReady = xTaskCreate(fetchWorker, "profile-fetch", 16384, nullptr, 1, nullptr) == pdPASS;
  }
  if (!canvasReady) {
    M5.Display.fillScreen(BACKGROUND_RGB);
    M5.Display.setTextColor(WARNING_RGB);
    M5.Display.setTextSize(1);
    M5.Display.setCursor(12, 55);
    M5.Display.print("DISPLAY ERROR");
  }
  JsonDocument ready;
  ready["event"] = "ready";
  ready["firmware"] = "atoms3-x-followers/1.1.1";
  ready["protocol_version"] = 1;
  ready["state"] = screenStatus();
  emit(ready);
}

static void serviceImu(uint32_t now) {
  static uint32_t lastReadAt = 0;
  if (!imuAvailable || now - lastReadAt < 50) return;
  lastReadAt = now;
  if (!M5.Imu.getAccel(&accelX, &accelY, &accelZ)) return;
  if (config.autoRotation && rotationFilter.sample(accelX, accelY, accelZ, now, effectiveRotation)) {
    M5.Display.setRotation(effectiveRotation / 90);
    dirty = true;
  }
}

void loop() {
  M5.update();
  serviceSerial();
  if (M5.BtnA.wasPressed()) {
    const char* error = requestManualRefresh("button");
    if (error) {
      JsonDocument event;
      event["event"] = "button";
      event["ok"] = false;
      event["error"] = error;
      emit(event);
    }
  }
  const uint32_t now = millis();
  serviceImu(now);
  serviceResult(now);
  serviceWifi(now);
  serviceFetch(now);
  drawScreen(now);
  delay(2);
}
