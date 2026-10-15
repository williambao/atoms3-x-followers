# AtomS3 X Followers

A standalone X (Twitter) follower counter for the **M5Stack AtomS3**. A bright green seven-segment display puts the number first, while the built-in IMU keeps the screen upright.

English | [简体中文](README-ZH.md)

<p align="center">
  <img src="docs/images/demo.jpg" alt="AtomS3 displaying a green seven-segment follower count" width="560" />
</p>

Configure it over USB serial, connect it to Wi-Fi, and leave it powered on. No companion application, always-on computer, or user-supplied API key is required.

## Features

- **Direct HTTPS updates** from the FxEmbed / FxTwitter profile API.
- **Automatic and manual refresh:** on startup, every five minutes by default, or when the front button is pressed.
- **Crisp seven-segment digits** drawn on an integer pixel grid, with configurable color and brightness.
- **Adaptive number formatting:** exact counts through five digits; larger counts use two decimal places and a unit such as `K`, `M`, or `B`.
- **IMU auto-rotation:** four orientations, a 600 ms stability delay, and filtering to avoid unwanted changes when flat or moving.
- **Persistent serial configuration:** Wi-Fi, account, refresh interval, color, brightness, and rotation settings survive a restart.
- **Failure handling:** reconnects Wi-Fi, backs off after temporary failures, respects rate-limit waits, and retains the last valid count while running.

## Hardware

This project targets the original **[M5Stack AtomS3](https://docs.m5stack.com/en/core/AtomS3)**, SKU **C123**:

- ESP32-S3 with 8 MB Flash
- 128 × 128 display
- MPU6886 IMU
- Front programmable button and USB-C power/programming

AtomS3 Lite and AtomS3R are not the tested targets. Use a data-capable USB cable and a **2.4 GHz Wi-Fi network**.

## Build and flash

Install [PlatformIO](https://platformio.org/), then clone the repository:

```sh
git clone https://github.com/williambao/atoms3-x-followers.git
cd atoms3-x-followers
pio run -e atoms3
pio device list
```

Replace `YOUR_PORT` below with the device listed by PlatformIO, for example `/dev/cu.usbmodem1101` on macOS, `/dev/ttyACM0` on Linux, or `COM3` on Windows:

```sh
pio run -e atoms3 -t upload --upload-port YOUR_PORT
pio device monitor --port YOUR_PORT --baud 115200
```

Close other serial monitors before uploading. Flashing replaces the current firmware. If automatic upload does not work, follow the [AtomS3 download-mode instructions](https://docs.m5stack.com/en/core/AtomS3): hold the rear reset button for about two seconds until the internal green LED lights, then release it and retry.

The project uses the Arduino framework with a standard C++ entry point in [`src/main.cpp`](src/main.cpp). It does not use an `.ino` sketch. [`platformio.ini`](platformio.ini) pins the development platform and libraries:

| Dependency | Version |
| --- | --- |
| pioarduino ESP32 platform | 55.03.38 |
| Arduino-ESP32 | 3.3.8 |
| M5Unified | 0.2.19 |
| M5GFX | 0.2.26 |
| ArduinoJson | 7.4.3 |

The first build downloads the required dependencies. M5GFX is fetched from the source archive of its official Git tag.

## First-time setup

The device starts without a Wi-Fi network or account configured. The screen shows `SET WIFI`, followed by `SET ACCOUNT` if Wi-Fi has been configured but the account is still empty.

Open the serial monitor at **115200 baud** and send this as a **single line**, followed by a newline. Replace the placeholder values with your own:

```json
{"cmd":"set","wifi":{"ssid":"YOUR_WIFI_SSID","password":"YOUR_WIFI_PASSWORD"},"account":"your_handle"}
```

A successful change returns:

```json
{"cmd":"set","ok":true,"changed":true,"saved":true}
```

The device connects to Wi-Fi, synchronizes its clock using NTP, and fetches the follower count. It needs access to `api.fxtwitter.com` over HTTPS and to an NTP server. Your computer's proxy settings are not automatically applied to the board.

Settings are stored in **NVS** and loaded after a restart. Wi-Fi credentials do not need to be compiled into the firmware. The follower count itself stays in RAM and is fetched again after a reboot.

## Display and controls

Press the **front button** to request a refresh. The **rear button** is for reset/download mode.

The normal screen contains the account, a large follower count, and a small `FOLLOWERS` label. Refreshing uses a small indicator; errors use a short message. An unsuccessful fetch never replaces a valid count with a fabricated zero.

| Follower count | Display |
| --- | --- |
| `749` | `749` |
| `12345` | `12345` |
| `99999` | `99999` |
| `100000` | `100.00K` |
| `1234567` | `1.23M` |
| `1000000000` | `1.00B` |

Rounding can promote the unit: `999995` displays as `1.00M`. The digits, decimal point, and small unit label fit within 116 pixels. Serial responses always report the full integer count, regardless of its display format.

Auto-rotation is enabled by default. A new orientation must remain stable for 600 ms before the display changes. When the device is flat, near a diagonal, or being shaken, it holds its previous orientation. Automatically detected angles are not repeatedly written to Flash.

To use a fixed orientation:

```json
{"cmd":"set","auto_rotation":false,"rotation":90}
```

To enable automatic rotation again:

```json
{"cmd":"set","auto_rotation":true}
```

The saved `rotation` is the manual orientation and the fallback when the IMU is unavailable. An unavailable IMU does not stop follower updates.

## Serial protocol

Send **one JSON object per line**, using LF or CRLF. The maximum command length is **1024 bytes**, excluding the line ending.

- `cmd` is required and case-sensitive.
- `id` is optional. When present, it is echoed in the response; otherwise it is omitted.
- Asynchronous notifications use `event`, so they can be distinguished from command responses.

| Command | Purpose |
| --- | --- |
| `set` | Validate, save, and apply a partial configuration update |
| `get` | Read configuration, excluding the Wi-Fi password |
| `status` | Read connectivity, count, errors, and IMU state |
| `refresh` | Queue a manual refresh |
| `help` | Read a protocol summary |

### Configuration

**Every configuration field in `set` is optional. Omitted fields keep their current values**, including individual fields inside `wifi`. All supplied fields are validated before any change is saved or applied. Invalid values, unknown fields, and `null` reject the whole update.

| Field | Accepted value | Factory default |
| --- | --- | --- |
| `wifi.ssid` | String, up to 32 bytes; `""` clears the SSID | Empty |
| `wifi.password` | 8–63 bytes, a 64-character hexadecimal PSK, or `""` for an open network | Empty |
| `account` | 1–15 letters, digits, or underscores; an initial `@` is accepted; `""` clears it | Empty |
| `interval_sec` | Integer from 60 to 86400 | `300` |
| `number_color` | Hex string `"#RRGGBB"`, case-insensitive | `"#A8FF60"` |
| `brightness` | Integer from 1 to 255 | `160` |
| `auto_rotation` | Boolean | `true` |
| `rotation` | `0`, `90`, `180`, or `270` degrees | `0` |

For example, changing only the color does not alter the network, account, or other settings:

```json
{"cmd":"set","number_color":"#FFBE55"}
```

Other independent examples:

```json
{"cmd":"set","interval_sec":600,"brightness":120}
{"cmd":"set","wifi":{"password":"another-password"}}
{"cmd":"set","account":"@your_handle"}
```

Changing an SSID without supplying a password preserves the old password. Set `"password":""` explicitly when switching to an open network. `{"cmd":"set"}` and `{"cmd":"set","wifi":{}}` are successful no-ops.

Configuration is saved as a single NVS record before it becomes active. Identical settings do not cause another Flash write. Saved settings override the factory defaults in [`include/FollowerCore.h`](include/FollowerCore.h).

`get` returns the SSID and a `password_set` flag, but never the password. NVS storage is not additionally encrypted by this firmware.

### Status and refresh

```json
{"cmd":"get"}
{"cmd":"status","id":7}
{"cmd":"refresh"}
```

`status` includes `state`, `wifi_connected`, `clock_synced`, `followers`, `stale`, `http_status`, `data_source`, `auto_rotation`, `effective_rotation`, and `imu_available`. When available, `accel_g` contains the IMU's acceleration readings. A missing count is returned as `null`.

A refresh acknowledgement means the request was accepted, not that the network operation has succeeded:

```json
{"cmd":"refresh","ok":true,"state":"queued"}
```

Completion is reported separately, for example:

```json
{"event":"refresh","account":"your_handle","cause":"serial","ok":true,"http_status":200,"stale":false,"followers":12345}
```

Only one fetch runs at a time. Manual requests have a five-second cooldown after a fetch starts; requests during an active fetch return `busy`. Server-imposed waits cannot be bypassed by pressing the button or sending `refresh`.

## Troubleshooting

| Screen message | Meaning / next step |
| --- | --- |
| `SET WIFI` / `SET ACCOUNT` | Supply the missing setting with `set`. |
| `CONNECTING` / `WIFI OFFLINE` | Check the SSID, password, and 2.4 GHz coverage. Connection attempts time out after 20 seconds and retry automatically. |
| `SYNC TIME` / `TIME ERROR` | NTP synchronization is pending or delayed. A valid clock is needed for TLS verification. |
| `TIMEOUT` / `API ERROR` | A request, TLS connection, or upstream service failed. Use `status` for details. |
| `BAD DATA` | The response could not be validated as the requested profile and an integer follower count. |
| `RATE LIMITED` | Wait for the server's retry window. |
| `NO PROFILE` | The account is unavailable or was not found by the provider. |
| `STALE DATA` | The last successful result is older than the greater of ten minutes or twice the refresh interval. |
| `CONFIG ERROR` | The saved record could not be loaded. Submit a valid configuration with `set`. |
| `STORAGE ERROR` / `SYSTEM ERROR` | Storage initialization or resource allocation failed. Inspect serial output. |

Temporary fetch failures retry with increasing delays of 15, 30, 60, 120, 240, and up to 300 seconds. Non-retryable errors pause automatic queries until a configuration change, manual refresh, or network recovery permits another attempt. Unavailable profiles are retried no more frequently than the configured update interval, so a temporary 404 does not stop monitoring permanently. Existing valid counts are retained while the firmware is running.

If uploading reports a busy port, close the program holding the serial connection. Some serial tools reset the board when opening a connection; saved settings survive this reset.

## Data source

The firmware uses the public [FxEmbed profile API](https://docs.fxembed.com/api/twitter/operations/2profilehandle/):

```text
GET https://api.fxtwitter.com/2/profile/{account}
```

It validates the API result code, profile type, account name, and `user.followers`. HTTPS uses the ESP32 CA certificate bundle. The firmware does not use X login cookies or store X API tokens.

FxEmbed is a third-party service. Availability and upstream caching are outside this project's control. The API does not provide a collection timestamp for this response, so a successful device refresh does not establish when X last updated the underlying data. `last_success_age_sec` measures time since the device's last successful request.

## Tests

On macOS or Linux, host tests require Python 3, a C++17 compiler with AddressSanitizer/UndefinedBehaviorSanitizer support, and ArduinoJson 7 headers:

```sh
python3 tests/run_tests.py --arduinojson /path/to/ArduinoJson
```

Pass the ArduinoJson library root containing its `src` directory. After a PlatformIO dependency installation, this is normally `.pio/libdeps/atoms3/ArduinoJson`.

The three suites cover configuration patches and persistence serialization, the actual main program with hardware doubles, and number layout/formatting plus rotation filtering. Hardware doubles do not establish real Wi-Fi, display, or NVS behavior.

On a real AtomS3, verify Wi-Fi setup, HTTPS retrieval, button and timed refresh, settings retained after restart, and display orientation in all four directions.

## License

[Apache License 2.0](LICENSE).
