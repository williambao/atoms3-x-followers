#pragma once
// Host-only device doubles. Production builds use the real ESP32/M5 headers.
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <deque>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

using String = std::string;
inline uint32_t mockMillis = 0;
inline int64_t mockMicros = 0;
inline uint32_t millis() { return mockMillis; }
inline int64_t esp_timer_get_time() { return mockMicros; }
inline void delay(uint32_t ms) { mockMillis += ms; mockMicros += static_cast<int64_t>(ms) * 1000; }
inline void configTime(int, int, const char*, const char*, const char*) {}

struct MockSerial {
  size_t rxBufferSize = 256;
  std::string output;
  std::deque<char> input;
  void begin(int) {}
  size_t setRxBufferSize(size_t size) { rxBufferSize = size; return size; }
  int available() const { return static_cast<int>(input.size()); }
  int read() { if (input.empty()) return -1; int c = static_cast<unsigned char>(input.front()); input.pop_front(); return c; }
  size_t write(uint8_t c) { output.push_back(static_cast<char>(c)); return 1; }
  size_t write(const uint8_t* data, size_t size) { output.append(reinterpret_cast<const char*>(data), size); return size; }
  void println() { output.push_back('\n'); }
};
inline MockSerial Serial;

constexpr int middle_center = 0;
constexpr int bottom_left = 1;
namespace fonts {
inline int Font0, Font2, FreeSansBold24pt7b, FreeSansBold12pt7b;
}
struct MockDisplay {
  int rotation = 0, brightness = 0;
  void setRotation(int value) { rotation = value; }
  void setBrightness(int value) { brightness = value; }
  void fillScreen(uint32_t) {}
  void setTextColor(uint32_t) {}
  void setTextSize(float) {}
  void setCursor(int, int) {}
  void print(const char*) {}
};
struct MockM5 {
  struct Config { bool clear_display, internal_imu, internal_mic, internal_spk; };
  struct Button { bool pressed = false; bool wasPressed() { bool value = pressed; pressed = false; return value; } } BtnA;
  MockDisplay Display;
  struct IMU {
    bool isEnabled() { return true; }
    bool getAccel(float* x, float* y, float* z) { *x = 0; *y = 0; *z = 1; return true; }
  } Imu;
  Config config() { return {}; }
  void begin(Config) {}
  void update() {}
};
inline MockM5 M5;
struct M5Canvas {
  std::vector<std::string> strings;
  int pushes = 0, segments = 0;
  M5Canvas(MockDisplay*) {}
  void setColorDepth(int) {}
  void* createSprite(int, int) { return this; }
  void fillScreen(uint32_t) { strings.clear(); }
  void setTextDatum(int) {}
  void setFont(const int*) {}
  void setTextSize(float) {}
  void setTextColor(uint32_t) {}
  void drawString(const char* value, int, int) { strings.emplace_back(value); }
  void drawCircle(int, int, int, uint32_t) {}
  void fillCircle(int, int, int, uint32_t) {}
  void fillRect(int, int, int, int, uint32_t) { ++segments; }
  void fillTriangle(int, int, int, int, int, int, uint32_t) {}
  int textWidth(const char* value) { return static_cast<int>(strlen(value) * 10); }
  int fontHeight() { return 48; }
  void pushSprite(int, int) { ++pushes; }
};

constexpr int WL_CONNECTED = 3;
constexpr int WIFI_STA = 1;
struct MockWifi {
  int state = 0, beginCalls = 0, disconnectCalls = 0;
  int status() const { return state; }
  void disconnect(bool, bool) { state = 0; ++disconnectCalls; }
  void begin(const char*, const char*) { ++beginCalls; }
  void persistent(bool) {}
  void mode(int) {}
  void setAutoReconnect(bool) {}
  int RSSI() const { return -42; }
  struct IP { std::string toString() const { return "192.0.2.1"; } };
  IP localIP() const { return {}; }
};
inline MockWifi WiFi;

struct Preferences {
  inline static std::map<std::string, std::string> values;
  inline static bool failWrite = false;
  inline static int writes = 0;
  bool begin(const char*, bool) { return true; }
  bool isKey(const char* key) { return values.count(key); }
  String getString(const char* key) { return values[key]; }
  size_t putString(const char* key, const String& value) {
    if (failWrite) return 0;
    values[key] = value;
    ++writes;
    return value.length();
  }
};

constexpr int pdTRUE = 1;
constexpr int pdPASS = 1;
constexpr uint32_t portMAX_DELAY = UINT32_MAX;
struct MockQueue { size_t capacity, size; std::deque<std::vector<uint8_t>> values; };
using QueueHandle_t = MockQueue*;
struct StopWorker : std::exception {};
inline QueueHandle_t xQueueCreate(size_t capacity, size_t size) { return new MockQueue{capacity, size, {}}; }
inline int xQueueSend(QueueHandle_t queue, const void* data, uint32_t) {
  if (queue->values.size() >= queue->capacity) return 0;
  auto first = static_cast<const uint8_t*>(data);
  queue->values.emplace_back(first, first + queue->size);
  return pdTRUE;
}
inline int xQueueReceive(QueueHandle_t queue, void* data, uint32_t wait) {
  if (queue->values.empty()) { if (wait == portMAX_DELAY) throw StopWorker{}; return 0; }
  memcpy(data, queue->values.front().data(), queue->size);
  queue->values.pop_front();
  return pdTRUE;
}
inline int xTaskCreate(void (*)(void*), const char*, size_t, void*, int, void*) { return pdPASS; }

using esp_err_t = int;
constexpr int ESP_OK = 0, ESP_FAIL = -1, ESP_ERR_NO_MEM = 0x101, ESP_ERR_TIMEOUT = 0x107;
constexpr int HTTP_EVENT_ON_HEADER = 1, HTTP_EVENT_ON_DATA = 2;
struct esp_http_client_event_t { int event_id; void* user_data; const char* header_key; const char* header_value; void* data; int data_len; };
inline int esp_crt_bundle_attach(void*) { return ESP_OK; }
struct esp_http_client_config_t {
  const char* url;
  int timeout_ms;
  int (*crt_bundle_attach)(void*);
  int (*event_handler)(esp_http_client_event_t*);
  void* user_data;
  const char* user_agent;
  bool disable_auto_redirect;
  int buffer_size;
};
inline esp_http_client_config_t mockHttpSettings = {};
inline std::string mockHttpResponse = R"({"code":200,"user":{"type":"profile","screen_name":"william_bao","followers":742}})";
inline int mockHttpStatus = 200, mockTransportResult = ESP_OK;
using esp_http_client_handle_t = esp_http_client_config_t*;
inline esp_http_client_handle_t esp_http_client_init(const esp_http_client_config_t* settings) {
  mockHttpSettings = *settings;
  return &mockHttpSettings;
}
inline int esp_http_client_perform(esp_http_client_handle_t client) {
  if (mockTransportResult != ESP_OK) return mockTransportResult;
  esp_http_client_event_t event = {HTTP_EVENT_ON_DATA, client->user_data, nullptr, nullptr,
    const_cast<char*>(mockHttpResponse.data()), static_cast<int>(mockHttpResponse.size())};
  return client->event_handler(&event);
}
inline int esp_http_client_get_status_code(esp_http_client_handle_t) { return mockHttpStatus; }
inline void esp_http_client_cleanup(esp_http_client_handle_t) {}
