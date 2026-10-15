#include "FollowerCore.h"
#include <cassert>
#include <iostream>
#include <string>

using namespace followers;

static bool patch(const std::string& json, const Config& current, Config& output,
                  const char*& error) {
  JsonDocument request;
  assert(!deserializeJson(request, json));
  return applyPatch(request.as<JsonObjectConst>(), current, output, error);
}

static bool profile(const char* json, uint64_t& count, const char*& error) {
  JsonDocument response;
  assert(!deserializeJson(response, json));
  return parseProfile(response.as<JsonObjectConst>(), "william_bao", count, error);
}

int main() {
  Config initial, configured, output;
  const char* error = nullptr;
  assert(!initial.ssid[0] && !initial.account[0]);
  assert(initial.intervalSec == 300);
  assert(patch(R"({"cmd":"set","wifi":{"ssid":"My WiFi","password":"pass word!"},"account":"@william_bao"})",
               initial, configured, error));
  assert(!strcmp(configured.account, "william_bao"));
  assert(patch(R"({"cmd":"set","number_color":"#fFbe55"})", configured, output, error));
  assert(output.numberColor == 0xFFBE55);
  assert(!strcmp(output.ssid, configured.ssid));
  assert(!strcmp(output.password, configured.password));
  assert(!strcmp(output.account, configured.account));
  assert(output.intervalSec == configured.intervalSec);
  assert(output.rotation == configured.rotation);
  assert(patch(R"({"cmd":"set","wifi":{"ssid":"Another"}})", configured, output, error));
  assert(!strcmp(output.password, configured.password));
  assert(patch(R"({"cmd":"set","wifi":{"password":"new password"}})", configured, output, error));
  assert(!strcmp(output.ssid, configured.ssid));
  assert(patch(R"({"cmd":"set","wifi":{}})", configured, output, error));
  assert(sameConfig(configured, output));
  assert(patch(R"({"cmd":"set"})", configured, output, error));
  assert(sameConfig(configured, output));
  assert(patch(R"({"cmd":"set","id":17,"rotation":270,"brightness":1})", configured, output, error));
  assert(output.rotation == 270 && output.brightness == 1);
  assert(patch(R"({"cmd":"set","account":"","wifi":{"ssid":"","password":""}})", configured, output, error));
  assert(!output.ssid[0] && !output.password[0] && !output.account[0]);

  // Invalid fields must not apply any part of a combined update.
  const char* invalid[] = {
    R"({"cmd":"set","number_color":"#000000","rotation":45})",
    R"({"cmd":"set","account":"new_account","interval_sec":59})",
    R"({"cmd":"set","interval_sec":86401})",
    R"({"cmd":"set","interval_sec":300.5})",
    R"({"cmd":"set","interval_sec":true})",
    R"({"cmd":"set","interval_sec":"300"})",
    R"({"cmd":"set","rotation":-90})",
    R"({"cmd":"set","rotation":360})",
    R"({"cmd":"set","rotation":null})",
    R"({"cmd":"set","number_color":"FFBE55"})",
    R"({"cmd":"set","number_color":"#ABCDEG"})",
    R"({"cmd":"set","number_color":16777215})",
    R"({"cmd":"set","number_color":"#ABC"})",
    R"({"cmd":"set","number_color":"#FFFFFF\u0000"})",
    R"({"cmd":"set","account":"@"})",
    R"({"cmd":"set","account":"https://x.com/william_bao"})",
    R"({"cmd":"set","account":"sixteen_character"})",
    R"({"cmd":"set","account":"will\u0000bad"})",
    R"({"cmd":"set","wifi":{"ssid":"123456789012345678901234567890123"}})",
    R"({"cmd":"set","wifi":{"password":"short"}})",
    R"({"cmd":"set","wifi":{"password":null}})",
    R"({"cmd":"set","wifi":null})",
    R"({"cmd":"set","wifi":{"pasword":"long enough"}})",
    R"({"cmd":"set","brightness":0})",
    R"({"cmd":"set","brightness":256})",
    R"({"cmd":"set","rotation_angle":90})"
    ,R"({"cmd":"set","auto_rotation":1})"
    ,R"({"cmd":"set","auto_rotation":null})"
  };
  for (const char* value : invalid) {
    output = configured;
    assert(!patch(value, configured, output, error));
    assert(error && *error);
    assert(sameConfig(configured, output));
  }
  for (int degrees : {0, 90, 180, 270}) {
    assert(patch("{\"cmd\":\"set\",\"rotation\":" + std::to_string(degrees) + "}", configured, output, error));
    assert(output.rotation == degrees);
  }
  assert(patch(R"({"cmd":"set","interval_sec":60,"brightness":255})", configured, output, error));
  assert(patch(R"({"cmd":"set","interval_sec":86400})", configured, output, error));
  assert(patch(R"({"cmd":"set","auto_rotation":false})", configured, output, error));
  assert(!output.autoRotation && output.rotation == configured.rotation);
  assert(patch(R"({"cmd":"set","wifi":{"ssid":"工作室 WiFi"}})", configured, output, error));
  assert(patch(R"({"cmd":"set","wifi":{"password":""}})", configured, output, error));
  assert(patch("{\"cmd\":\"set\",\"wifi\":{\"password\":\"" + std::string(64, 'a') + "\"}}", configured, output, error));
  assert(!patch("{\"cmd\":\"set\",\"wifi\":{\"password\":\"" + std::string(64, 'g') + "\"}}", configured, output, error));

  // NVS serialization roundtrip and public output redaction use the same code.
  JsonDocument stored;
  writeConfig(stored.to<JsonObject>(), configured, true);
  std::string encoded;
  serializeJson(stored, encoded);
  Config restored;
  assert(patch(encoded, initial, restored, error));
  assert(sameConfig(configured, restored));
  JsonDocument visible;
  writeConfig(visible.to<JsonObject>(), configured);
  assert(visible["wifi"]["password"].isUnbound());
  assert(visible["wifi"]["password_set"] == true);
  assert(visible["number_color"] == "#A8FF60");

  uint64_t count = 123;
  assert(profile(R"({"code":200,"user":{"type":"profile","screen_name":"william_bao","followers":742}})", count, error));
  assert(count == 742);
  assert(profile(R"({"code":200,"user":{"type":"profile","screen_name":"@William_Bao","followers":0}})", count, error));
  assert(count == 0);
  assert(profile(R"({"code":200,"user":{"type":"profile","screen_name":"william_bao","followers":5000000000}})", count, error));
  assert(count == 5000000000ULL);
  const char* badProfiles[] = {
    R"({"code":200,"user":{"type":"profile","screen_name":"other","followers":742}})",
    R"({"code":200,"user":{"type":"profile","screen_name":"william_bao\u0000other","followers":742}})",
    R"({"code":200,"user":{"type":"youtube","screen_name":"william_bao","followers":742}})",
    R"({"code":200,"user":{"type":"profile","screen_name":"william_bao","followers":null}})",
    R"({"code":200,"user":{"type":"profile","screen_name":"william_bao"}})",
    R"({"code":200,"user":{"type":"profile","screen_name":"william_bao","followers":-1}})",
    R"({"code":200,"user":{"type":"profile","screen_name":"william_bao","followers":742.5}})",
    R"({"code":200,"user":{"type":"profile","screen_name":"william_bao","followers":"742"}})",
    R"({"code":200,"user":{"type":"profile","screen_name":"william_bao","followers":true}})",
    R"({"code":502,"user":{"type":"profile","screen_name":"william_bao","followers":0}})"
  };
  for (const char* value : badProfiles) {
    count = 742;
    assert(!profile(value, count, error));
    assert(count == 742);  // Failure never invents a zero.
  }
  assert(!due(0xFFFFFFF0, 0x00000010));
  assert(due(0x00000010, 0x00000010));
  assert(due(0x00000011, 0x00000010));
  assert(due(0x00000010, 0xFFFFFFF0));
  assert(retryDelayMs(1) == 15000);
  assert(retryDelayMs(2) == 30000);
  assert(retryDelayMs(6) == 300000);
  assert(retryDelayMs(255) == 300000);
  std::cout << "PASS: optional configuration, atomic rejection, persistence roundtrip, "
               "password redaction, profile validation, rollover and retry limits\n";
}
