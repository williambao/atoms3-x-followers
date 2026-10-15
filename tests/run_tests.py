#!/usr/bin/env python3
"""Run shared firmware logic on the host, with ASan and UBSan."""
import argparse
import pathlib
import subprocess
import tempfile

parser = argparse.ArgumentParser()
parser.add_argument("--arduinojson", type=pathlib.Path,
                    default=pathlib.Path.home() / "Documents/Arduino/libraries/ArduinoJson")
args = parser.parse_args()
project = pathlib.Path(__file__).resolve().parents[1]
json_include = args.arduinojson / "src"
if not (json_include / "ArduinoJson.h").exists():
    parser.error("Pass --arduinojson /path/to/ArduinoJson (library root)")
with tempfile.TemporaryDirectory(prefix="atoms3-followers-tests-") as temp:
    doubles = pathlib.Path(temp) / "device-doubles"
    doubles.mkdir()
    for header in ("Arduino.h", "M5Unified.h", "WiFi.h", "Preferences.h",
                   "esp_crt_bundle.h", "esp_http_client.h", "esp_timer.h",
                   "freertos/FreeRTOS.h", "freertos/queue.h", "freertos/task.h"):
        path = doubles / header
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text('#include "MockHardware.h"\n')
    for suite in ("core_test", "runtime_test", "display_test"):
        executable = pathlib.Path(temp) / suite
        subprocess.run([
            "c++", "-std=c++17", "-Wall", "-Wextra", "-Werror", "-g",
            "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
            "-I", str(project / "include"), "-I", str(json_include),
            "-I", str(doubles), "-I", str(project / "tests"),
            str(project / f"tests/{suite}.cpp"), "-o", str(executable)
        ], check=True)
        subprocess.run([str(executable)], check=True)
