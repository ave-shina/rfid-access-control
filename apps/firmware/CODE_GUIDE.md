# Firmware Code Guide — `main.cpp`

Detailed walkthrough of the ESP32 firmware source code. Line numbers reference the current version of `src/main.cpp`.

---

## Table of Contents

1. [Includes & Secrets](#1-includes--secrets)
2. [Pin Definitions](#2-pin-definitions)
3. [UHF Reader Protocol Constants](#3-uhf-reader-protocol-constants)
4. [Global State Variables](#4-global-state-variables)
5. [Baud Rate Auto-Detection](#5-baud-rate-auto-detection)
6. [Setup](#6-setup)
7. [Main Loop](#7-main-loop)
8. [UHF Tag Reading](#8-uhf-tag-reading)
9. [MQTT Reconnection](#9-mqtt-reconnection)
10. [MQTT Message Callback](#10-mqtt-message-callback)
11. [UID Hashing & Nonce Generation](#11-uid-hashing--nonce-generation)
12. [Publishing a Scan](#12-publishing-a-scan)
13. [Actuator Control](#13-actuator-control)
14. [Status LEDs](#14-status-leds)

---

## 1. Includes & Secrets

```cpp
#include <Arduino.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include <mbedtls/sha256.h>
#include "esp_task_wdt.h"
#include "secrets.h"
```

| Include | Purpose |
|---|---|
| `Arduino.h` | Core Arduino framework (GPIO, Serial, delay, etc.) |
| `WiFi.h` | ESP32 Wi-Fi STA mode |
| `PubSubClient.h` | MQTT client (publish/subscribe) |
| `mbedtls/sha256.h` | ESP-IDF built-in SHA-256 (no external crypto library needed) |
| `esp_task_wdt.h` | Hardware watchdog timer — auto-reboots if `loop()` hangs |
| `secrets.h` | **gitignored** — contains `WIFI_SSID`, `WIFI_PASSWORD`, `MQTT_SERVER`, `MQTT_USER`, `MQTT_PASS` |

> The firmware uses ESP-IDF's built-in `mbedtls` for SHA-256 instead of an external Arduino crypto library. This avoids adding `Arduino_CryptoLibrary` to `lib_deps`.

---

## 2. Pin Definitions

```cpp
#define GREEN_LED   25   // Access granted indicator (active-LOW)
#define RED_LED     26   // Access denied indicator  (active-LOW)
#define MQTT_LED    5    // MQTT connection status    (active-LOW)
#define BUZZER      4    // KY-12 active buzzer
#define BUILTIN_LED 2    // WiFi status indicator     (active-HIGH)
```

**Active-LOW vs Active-HIGH:**

- External LEDs (GREEN, RED, MQTT) are wired between the GPIO pin and GND via a resistor. Setting the pin `LOW` (0V) turns the LED **ON** because current flows from 3V3 through the LED to the LOW pin. Setting `HIGH` turns it **OFF**.
- Built-in LED is active-HIGH — `HIGH` = ON, `LOW` = OFF.

---

## 3. UHF Reader Protocol Constants

```cpp
#define UHF_BAUD      57600
#define UHF_RX        16       // UART2 RX (ESP32 receives from MAX3232 TX)
#define UHF_TX        17       // UART2 TX (ESP32 sends to MAX3232 RX)
#define READ_TIMEOUT  100      // ms to wait per byte
#define MAX_RESPONSE  128      // max frame size in bytes
#define HEADER_LEN    4        // [LEN][ADDR][CMD][STATUS]
#define CMD_INVENTORY 0xEE     // auto-inventory notification
#define STATUS_OK     0x00
```

The HW-VX6330K operates in **Active Mode** — it continuously scans for UHF tags and autonomously sends inventory frames over UART whenever a tag is in range. The ESP32 passively listens; no commands are sent to the reader.

**Frame structure:**

```
 Byte 0   Byte 1   Byte 2   Byte 3   Byte 4...
 [LEN]    [ADDR]   [CMD]    [STATUS]  [EPC bytes...]
                             0xEE     0x00 = success
```

- `LEN` = number of bytes **after** this field (i.e., total frame = LEN + 1)
- `CMD` = `0xEE` for auto-inventory
- `STATUS` = `0x00` for success
- EPC (Electronic Product Code) = the tag's UID, variable length = LEN - 3

---

## 4. Global State Variables

### MQTT Client

```cpp
WiFiClient espClient;
PubSubClient mqttClient(espClient);
```

`PubSubClient` wraps a `WiFiClient` for TCP transport. All MQTT operations go through `mqttClient`.

### Scan Debounce

```cpp
String lastUID            = "";
unsigned long lastScanTime = 0;
const unsigned long SCAN_COOLDOWN_MS = 2000;
```

Prevents the same card from being published multiple times during a single tap. If the same UID is read within 2 seconds of the last scan, it is silently ignored.

### Non-blocking Actuator Timing

```cpp
unsigned long actuatorEnd     = 0;
bool          actuatorActive  = false;
int           actuatorNextState = -1;   // -1=idle, 0=deny, 1=grant
```

LEDs and buzzer are timed non-blockingly. When an actuator sequence starts, `actuatorEnd` is set to `millis() + duration`. Each `loop()` iteration checks if the duration has expired and turns LEDs off if so. This avoids `delay()` blocking the main loop for long periods.

- **Grant:** green LED ON for 1 second, then OFF
- **Deny:** red LED ON for 3 seconds, then OFF

### Reader Health Check

```cpp
#define READER_TIMEOUT_MS  30000
bool          readerOnline  = false;
unsigned long lastDataTime  = 0;
```

In Active Mode, the reader is silent when no tag is present. A 30-second timeout detects genuine hardware failure (power loss, wiring broken). If no UART data arrives for 30 seconds, the reader is marked offline.

### MQTT Reconnect Backoff

```cpp
unsigned long lastReconnectAttempt = 0;
int  reconnectDelayMs = 1000;
int  mqttFailCount    = 0;
const int MQTT_MAX_FAILS = 10;
```

Exponential backoff prevents flooding the broker with connection attempts. If MQTT fails 10 consecutive times, the ESP32 reboots (assumes a deeper system issue).

### JSON Payload Buffer

```cpp
char scanBuffer[256];
```

Pre-allocated buffer for constructing MQTT JSON payloads. Avoids heap fragmentation from `String` concatenation.

---

## 5. Baud Rate Auto-Detection

```cpp
int autoDetectBaud();
```

Called once during `setup()`. Tries baud rates `[9600, 19200, 38400, 57600, 115200]` in order:

1. Opens `Serial2` at the candidate baud rate
2. Sends an Answer Mode inventory command `0x04 0xFF 0x01 0x1B 0xB4`
3. Waits for a response (or passively listens for Active Mode data)
4. Validates the frame — checks for `CMD=0xEE` or `CMD=0x01`
5. Returns the first working baud rate, or `0` if none work

If detection fails, the firmware falls back to 57600 and prints a warning.

---

## 6. Setup

`setup()` runs once at boot:

1. **Serial** — `Serial.begin(115200)` for debug output
2. **Watchdog** — `esp_task_wdt_init(30, true)` — 30-second timeout (generous to allow for baud detection + WiFi)
3. **Baud auto-detect** — determines reader baud rate, then locks `Serial2` to it
4. **GPIO** — configures all outputs. LEDs start **OFF** (HIGH for active-LOW, LOW for active-HIGH)
5. **WiFi** — STA mode, auto-reconnect enabled. Scans nearby networks to verify the SSID is visible (helpful debug for 5 GHz vs 2.4 GHz issues). Blocks until connected (up to 20 seconds).
6. **NTP** — `configTime(0, 0, "pool.ntp.org", "time.nist.gov")` for Unix timestamps in scan payloads
7. **MQTT** — sets server and callback, but does **not** connect yet (that happens in `reconnect()` during `loop()`)

---

## 7. Main Loop

`loop()` runs continuously:

```
1. reset_watchdog()
2. reconnect()                     ← ensure MQTT is connected
3. check actuator expiry           ← turn off LEDs after timeout
4. check scan cooldown
5. drain UART if in cooldown or actuating
6. read UHF tag (only if idle + not in cooldown)
7. if tag found:
     - update reader health timestamp
     - update debounce state
     - publish scan to MQTT
8. check reader health timeout
9. update WiFi/MQTT status LEDs
10. mqttClient.loop()              ← process incoming MQTT messages
```

**Key design decisions:**

- Steps 3-5 ensure the UART buffer is drained during cooldown/actuation to prevent stale data from triggering false scans.
- Step 10 (`mqttClient.loop()`) is **always** reached — no early return skips it. This is required for PubSubClient to process incoming messages and maintain the connection.
- Tag reading is blocked during cooldown AND during actuator sequences, preventing a scan from interrupting an ongoing LED/buzzer response.

---

## 8. UHF Tag Reading

### `readExact()` — Read N Bytes with Timeout

```cpp
bool readExact(Stream &s, uint8_t *buf, size_t len, unsigned long timeoutMs);
```

Reads exactly `len` bytes from a `Stream` (Serial2). Returns `false` if timeout expires before all bytes arrive. Resets the timeout clock on each received byte.

### `readUHFTag()` — Parse a Complete UHF Frame

```cpp
String readUHFTag();
```

Returns the EPC (tag UID) as an uppercase hex string, or `""` if no valid tag:

1. **Fast path** — if `Serial2` has no data, return `""` immediately (no blocking)
2. **Read length byte** — first byte = `LEN` (bytes following)
3. **Read remaining data** — `readExact()` reads `dataLen` more bytes
4. **Structural validation** — frame must be at least `HEADER_LEN` (4 bytes)
5. **Command check** — byte[2] must be `0xEE` (inventory notification)
6. **Status check** — byte[3] must be `0x00` (success)
7. **Extract EPC** — bytes 4 onwards, converted to uppercase hex string

Debug output includes a hex dump of every received frame.

---

## 9. MQTT Reconnection

```cpp
void reconnect();
```

Non-blocking reconnection with exponential backoff and TCP probing:

1. **Return immediately** if already connected
2. **Throttle attempts** — skip if less than `reconnectDelayMs` since last attempt
3. **WiFi check** — if WiFi is down, trigger `WiFi.begin()` and return
4. **TCP probe** — attempt a raw TCP connection to broker:1883 with 500ms timeout. If unreachable, increment fail counter. After 10 failures, `ESP.restart()`.
5. **MQTT connect** — connect with credentials and LWT (Last Will and Testament):
   - Client ID: `"esp32_front_door"`
   - LWT topic: `door/status`, payload: `{"status":"offline"}`, retained
   - On success: subscribe to `door/command` (QoS 1), publish `{"status":"online"}` (retained), reset backoff
   - On failure: increment fail counter, double backoff (capped at 5 seconds)

The LWT ensures the broker automatically publishes an "offline" message if the ESP32 disconnects unexpectedly (power loss, crash, network drop).

---

## 10. MQTT Message Callback

```cpp
void callback(char *topic, byte *payload, unsigned int length);
```

Called by `mqttClient.loop()` when a message arrives:

1. Copy payload into a local buffer (max 127 bytes)
2. Check if topic is `"door/command"`
3. Parse `"status":` value from the JSON using string search (no JSON library)
4. Dispatch:
   - `status == 1` → `grantAccess()`
   - `status == 0` → `denyAccess()`

> JSON parsing uses `strstr()` instead of a JSON library to save flash/RAM on the ESP32. The payload schema is simple and well-known.

---

## 11. UID Hashing & Nonce Generation

### `hashUID()`

```cpp
String hashUID(const String &rawUID);
```

Hashes the raw EPC string using `mbedtls_sha256`:

1. Initialize SHA-256 context
2. Update with the raw UID string bytes
3. Finalize into a 32-byte digest
4. Convert to lowercase 64-character hex string

The raw UID **never leaves the ESP32**. Only the SHA-256 hash is transmitted. The backend applies a server-side pepper (`sha256(incoming_hash + pepper)`) before database lookup.

### `generateNonce()`

```cpp
String generateNonce();
```

Generates a random 8-character lowercase hex string (4 random bytes). Used as an anti-replay token — the backend caches nonces and rejects duplicates within 60 seconds.

---

## 12. Publishing a Scan

```cpp
void publishScan(const String &rawUID);
```

Called when a valid, non-debounced UHF tag is read:

1. **Check MQTT** — if not connected, drop the scan and log a warning
2. **Hash UID** — `sha256(rawUID)`
3. **Generate nonce** — random 4-byte hex
4. **Get timestamp** — from NTP-synced system clock
5. **Build JSON** — using `snprintf` into the pre-allocated `scanBuffer`:
   ```json
   {"uid_hash":"abc...","device_id":"esp32_front_door","nonce":"a3f9c12b","timestamp":1718000000}
   ```
6. **Publish** — `mqttClient.publish("door/scan", scanBuffer, false)` at QoS 1
7. **Retry once** — if publish fails, wait 50ms, call `mqttClient.loop()`, retry. If that also fails, log the failure.

> `false` in `publish()` means the message is NOT retained by the broker — scans are transient events.

---

## 13. Actuator Control

### `grantAccess()`

```cpp
void grantAccess();
```

1. Turn red LED OFF, green LED ON
2. Beep buzzer for 200ms (brief blocking delay — acceptable)
3. Set actuator timer: green LED stays ON for **1 second**
4. Set `actuatorActive = true`, `actuatorNextState = 1`

The main loop's actuator expiry check turns the green LED OFF when the timer expires.

### `denyAccess()`

```cpp
void denyAccess();
```

1. Turn green LED OFF, red LED ON
2. Beep buzzer 3 times (150ms on, 100ms off — brief blocking delays)
3. Set actuator timer: red LED stays ON for **3 seconds**
4. Set `actuatorActive = true`, `actuatorNextState = 0`

### Actuator Expiry (in `loop()`)

```cpp
if (actuatorActive && millis() >= actuatorEnd) {
    digitalWrite(GREEN_LED, HIGH);   // OFF (active-LOW)
    digitalWrite(RED_LED, HIGH);     // OFF (active-LOW)
    actuatorActive = false;
    actuatorNextState = -1;
}
```

Both LEDs are turned OFF on expiry regardless of which one was active. This is safe because only one LED is ON at any time (grant = green, deny = red).

---

## 14. Status LEDs

```cpp
void updateStatusLEDs();
```

| LED | GPIO | Active | Logic |
|---|---|---|---|
| Built-in (blue) | 2 | HIGH | ON when WiFi connected |
| MQTT (external) | 5 | LOW | ON when MQTT connected |

Called every `loop()` iteration. Provides at-a-glance hardware status:
- Both on = fully connected
- Only blue = WiFi OK, MQTT down
- Both off = no connectivity

---

## Execution Flow Summary

```
Boot
  │
  ▼
setup()
  ├── Serial debug (115200)
  ├── Watchdog (30s)
  ├── Baud auto-detect → Serial2 locked
  ├── GPIO init (LEDs OFF)
  ├── WiFi connect (blocking, up to 20s)
  ├── NTP sync (background)
  └── MQTT server + callback set
      │
      ▼
loop() ─────────────────────────────┐
  ├── Watchdog reset                 │
  ├── reconnect() if needed          │
  ├── Actuator expiry check          │
  ├── Read UHF tag (if idle)         │
  │   └── publishScan() → MQTT       │
  ├── Reader health check            │
  ├── updateStatusLEDs()             │
  └── mqttClient.loop()              │
      └── callback()                 │
          ├── status=1 → grantAccess()
          └── status=0 → denyAccess()
                                    ──┘ (repeats forever)
```

---

## Fail-Secure Behavior

The system is **fail-secure** — the door stays locked unless the backend explicitly sends `{"status": 1}`:

- If MQTT is down → scans are dropped, no command received, door stays locked
- If backend is down → no response to scans, door stays locked
- If WiFi is down → no MQTT connection possible, door stays locked
- If ESP32 crashes → watchdog reboots it, door stays locked during reboot
- Physical key backup is required for emergency access during extended outages
