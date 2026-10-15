// Compile the actual main.cpp against device doubles: tests serial responses,
// persistence failures and asynchronous request lifecycle, not a reimplementation.
#include "MockHardware.h"
#include "../src/main.cpp"
#include <cassert>
#include <iostream>

static JsonDocument command(const char* request) {
  Serial.output.clear();
  processCommand(request, strlen(request));
  JsonDocument response;
  assert(!deserializeJson(response, Serial.output));
  return response;
}

static FetchResult runWorker(const char* response, int status = 200, int transport = ESP_OK) {
  requestQueue->values.clear();
  resultQueue->values.clear();
  mockHttpResponse = response;
  mockHttpStatus = status;
  mockTransportResult = transport;
  FetchRequest request = {};
  request.generation = generation;
  strcpy(request.account, config.account);
  strcpy(request.cause, "test");
  assert(xQueueSend(requestQueue, &request, 0) == pdTRUE);
  try { fetchWorker(nullptr); } catch (const StopWorker&) {}
  FetchResult result;
  assert(xQueueReceive(resultQueue, &result, 0) == pdTRUE);
  assert(mockHttpSettings.crt_bundle_attach == esp_crt_bundle_attach);
  assert(mockHttpSettings.disable_auto_redirect);
  return result;
}

int main() {
  setenv("TZ", "UTC0", 1);
  tzset();
  setup();
  assert(Serial.rxBufferSize >= MAX_COMMAND_BYTES + 2);
  assert(!strcmp(screenStatus(), "SET WIFI"));
  assert(command(R"({"cmd":"refresh"})")["error"] == "wifi_not_configured");
  auto response = command(R"({"cmd":"set","wifi":{"ssid":"My WiFi","password":"test-password"}})");
  assert(response["ok"] == true && response["saved"] == true);
  assert(response["id"].isUnbound());
  assert(!strcmp(screenStatus(), "SET ACCOUNT"));
  assert(command(R"({"cmd":"refresh"})")["error"] == "account_not_configured");
  assert(command(R"({"cmd":"set","account":"william_bao","id":"setup"})")["id"] == "setup");
  assert(!strcmp(screenStatus(), "WIFI OFFLINE"));

  const int wifiCalls = WiFi.disconnectCalls;
  response = command(R"({"cmd":"set","number_color":"#FFBE55","auto_rotation":false,"rotation":90})");
  assert(response["ok"] == true);
  assert(!strcmp(config.ssid, "My WiFi") && !strcmp(config.password, "test-password"));
  assert(WiFi.disconnectCalls == wifiCalls);
  assert(M5.Display.rotation == 1 && config.numberColor == 0xFFBE55);
  const int writes = Preferences::writes;
  assert(command(R"({"cmd":"set","rotation":90})")["changed"] == false);
  assert(Preferences::writes == writes);
  Config saved = config;
  const std::string stored = Preferences::values["config"];
  Preferences::failWrite = true;
  assert(command(R"({"cmd":"set","account":"another"})")["error"] == "storage_write_failed");
  assert(sameConfig(config, saved) && Preferences::values["config"] == stored);
  Preferences::failWrite = false;
  assert(command(R"({"cmd":"set","rotation":45,"number_color":"#000000"})")["ok"] == false);
  assert(sameConfig(config, saved));
  config = Config{};
  loadConfig();
  assert(sameConfig(config, saved));
  response = command(R"({"cmd":"get","id":7})");
  assert(response["id"] == 7);
  assert(response["config"]["wifi"]["password"].isUnbound());
  assert(Serial.output.find("test-password") == std::string::npos);
  assert(command(R"({"account":"william_bao"})")["error"] == "missing_or_invalid_cmd");
  assert(command("[]")["error"] == "invalid_json_object");
  assert(command(R"({"cmd":"unknown"})")["error"] == "unknown_cmd");
  const Config beforeInvalidLine = config;
  const int beforeInvalidWrites = Preferences::writes;
  for (const char* invalid : {
         R"({"cmd":"set","brightness":42}x)",
         R"({"cmd":"set","brightness":42}trailing)",
         R"({"cmd":"set","brightness":42}{"cmd":"get"})"}) {
    assert(command(invalid)["error"] == "invalid_json_object");
    assert(sameConfig(config, beforeInvalidLine));
    assert(Preferences::writes == beforeInvalidWrites);
  }
  assert(command(" \t{\"cmd\":\"get\"} \t")["ok"] == true);

  serviceWifi(0);
  assert(!strcmp(screenStatus(), "CONNECTING"));
  mockMillis = WIFI_CONNECT_TIMEOUT_MS;
  mockMicros = static_cast<int64_t>(mockMillis) * 1000;
  serviceWifi(mockMillis);
  assert(!strcmp(screenStatus(), "WIFI OFFLINE"));
  WiFi.state = WL_CONNECTED;
  serviceWifi(mockMillis);
  serviceFetch(mockMillis);
  assert(inFlight);
  assert(command(R"({"cmd":"refresh"})")["error"] == "busy");

  FetchResult ok = runWorker(R"({"code":200,"user":{"type":"profile","screen_name":"william_bao","followers":742}})");
  assert(ok.ok && ok.followers == 742);
  assert(parseSourceTime("2024-02-29T12:00:00Z") != 0);
  assert(parseSourceTime("2025-02-29T12:00:00Z") == 0);
  assert(parseSourceTime("2026-10-14T16:26:34.699Z") != 0);
  assert(parseSourceTime("garbage") == 0);
  assert(parseSourceTime("2026-10-14T16:26:34Zbad") == 0);
  assert(parseRetryAfter("86401") == 86401);
  assert(parseRetryAfter("4294967296") == UINT32_MAX);
  assert(parseRetryAfter("abc") == 0);
  assert(xQueueSend(resultQueue, &ok, 0) == pdTRUE);
  serviceResult(mockMillis);
  assert(hasFollowers && followerCount == 742 && !inFlight);
  assert(nextFetchAt == mockMillis + config.intervalSec * 1000);
  assert(command(R"({"cmd":"refresh"})")["error"] == "refresh_cooldown");

  FetchResult bad = runWorker(R"({"code":200,"user":{"type":"profile","screen_name":"william_bao","followers":null}})");
  assert(!bad.ok && !strcmp(bad.error, "invalid_followers"));
  assert(xQueueSend(resultQueue, &bad, 0) == pdTRUE);
  serviceResult(mockMillis);
  assert(followerCount == 742 && hasFollowers && dataStale());
  assert(!strcmp(screenStatus(), "BAD DATA"));
  mockMillis += 100;
  drawScreen(mockMillis);
  assert(canvas.segments > 0);
  assert(std::find(canvas.strings.begin(), canvas.strings.end(), "BAD DATA") != canvas.strings.end());
  const int drawn = canvas.pushes;
  for (int i = 0; i < 20; ++i) drawScreen(mockMillis + 200 + i * 50);
  assert(canvas.pushes == drawn);  // Polling an unchanged screen does not redraw it.

  bad = runWorker("<html>challenge</html>");
  assert(!bad.ok && !strcmp(bad.error, "invalid_json"));
  std::string large(MAX_BODY_BYTES + 1, 'a');
  bad = runWorker(large.c_str());
  assert(!bad.ok && !strcmp(bad.error, "response_too_large"));
  // Cloudflare's HTML 502 page may exceed our body cap. It remains an upstream
  // HTTP error rather than being mislabelled BAD DATA by the buffer check.
  bad = runWorker(large.c_str(), 502);
  assert(!bad.ok && bad.retryable && !strcmp(bad.error, "http_error"));
  bad = runWorker("", 200, ESP_ERR_TIMEOUT);
  assert(!bad.ok && !strcmp(bad.error, "timeout"));
  bad = runWorker("{}", 404);
  assert(!bad.ok && bad.retryable && !strcmp(bad.error, "content_unavailable"));
  bad = runWorker(R"({"code":404,"message":"User not found"})");
  assert(!bad.ok && bad.retryable && !strcmp(bad.error, "content_unavailable"));
  assert(xQueueSend(resultQueue, &bad, 0) == pdTRUE);
  serviceResult(mockMillis);
  assert(!terminalApiError && !strcmp(screenStatus(), "NO PROFILE"));
  assert(nextFetchAt - mockMillis >= config.intervalSec * 1000);
  bad = runWorker(R"({"code":429,"message":"Rate limited"})");
  assert(!bad.ok && bad.retryable && !strcmp(bad.error, "rate_limited"));

  bad = runWorker("{}", 429);
  bad.retryAfterSec = 86401;
  assert(xQueueSend(resultQueue, &bad, 0) == pdTRUE);
  serviceResult(mockMillis);
  assert(!strcmp(screenStatus(), "RATE LIMITED"));
  assert(command(R"({"cmd":"refresh"})")["error"] == "rate_limit_cooldown");
  assert(command(R"({"cmd":"status"})")["retry_after_sec"] == 86401);
  mockMillis += 6000;
  serviceFetch(mockMillis);
  assert(!inFlight);  // Server cooldown cannot be bypassed by the normal timer.

  // An old request finishing after a config change must never publish its count.
  ok.generation = generation;
  inFlight = true;
  assert(command(R"({"cmd":"set","account":"another"})")["ok"] == true);
  assert(!hasFollowers);
  assert(xQueueSend(resultQueue, &ok, 0) == pdTRUE);
  serviceResult(mockMillis);
  assert(!hasFollowers && !inFlight && refreshPending);
  assert(command(R"({"cmd":"set","account":""})")["ok"] == true);
  assert(!strcmp(screenStatus(), "SET ACCOUNT"));

  // Serial framing supports CRLF, multiple commands and oversized-line recovery.
  Serial.output.clear();
  std::string lines = "{\"cmd\":\"get\"}\r\n{\"cmd\":\"status\",\"id\":9}\n";
  Serial.input.insert(Serial.input.end(), lines.begin(), lines.end());
  while (Serial.available()) serviceSerial();
  assert(std::count(Serial.output.begin(), Serial.output.end(), '\n') == 2);
  // CRLF does not reduce the documented maximum JSON payload length.
  Serial.output.clear();
  lines = R"({"cmd":"get"})";
  lines.append(MAX_COMMAND_BYTES - lines.size(), ' ');
  lines += "\r\n";
  Serial.input.insert(Serial.input.end(), lines.begin(), lines.end());
  while (Serial.available()) serviceSerial();
  JsonDocument boundary;
  assert(!deserializeJson(boundary, Serial.output));
  assert(boundary["ok"] == true && boundary["cmd"] == "get");
  Serial.output.clear();
  lines = std::string(MAX_COMMAND_BYTES + 1, 'x') + "\n{\"cmd\":\"help\"}\n";
  Serial.input.insert(Serial.input.end(), lines.begin(), lines.end());
  while (Serial.available()) serviceSerial();
  assert(Serial.output.find("command_too_long") != std::string::npos);
  assert(Serial.output.find("JSON Lines") != std::string::npos);
  for (char c : serialLine) assert(c == 0);
  delete requestQueue;
  delete resultQueue;
  std::cout << "PASS: actual serial handler, optional IDs, storage failures, screen error states, "
               "HTTP validation, cooldown, late-result discard and serial framing\n";
}
