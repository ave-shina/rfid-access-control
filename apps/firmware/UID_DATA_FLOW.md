# UID Data Flow: From Reader to Backend via MQTT

This document traces the complete lifecycle of a RFID tag UID — from the moment the HW-VX6330K reader detects a tag, through hashing and security processing on the ESP32, to the final MQTT publish to the Go backend.

---

## Overview Diagram

```
┌──────────────┐    RS485     ┌─────────┐    UART2    ┌───────────────┐
│  HW-VX6330K  │─────────────▶│ MAX485  │───────────▶│     ESP32     │
│  UHF Reader  │  A+/B- diff  │Transcvr │  GPIO16/RX │               │
└──────────────┘              └─────────┘             │  1. Read EPC  │
                                                      │  2. Debounce  │
                                                      │  3. SHA-256   │
                                                      │  4. Build JSON│
                                                      └──────┬────────┘
                                                             │ MQTT publish
                                                             │ "door/scan"
                                                             ▼
                                                      ┌──────────────┐
                                                      │   Mosquitto  │
                                                      │   Broker     │
                                                      │  :1883       │
                                                      └──────┬───────┘
                                                             │ MQTT deliver
                                                             ▼
                                                      ┌──────────────┐
                                                      │  Go Backend  │
                                                      │  Validate    │
                                                      │  Query DB    │
                                                      │  Respond     │
                                                      └──────────────┘
```

---

## Step-by-Step Process

### Step 1: Reader Detects a Tag (Physical Layer)

The HW-VX6330K operates in **Active Mode** — it continuously scans for UHF RFID tags without needing a command from the ESP32. When a tag enters the reader's electromagnetic field, the reader automatically transmits an inventory frame over RS485.

**File:** `config.h:21-31`

The reader sends a binary frame with this structure:

```
Byte Index:  [0]    [1]    [2]    [3]    [4]   [5]   [6]   [7]
Meaning:     LEN    ADDR   CMD    STATUS EPC   EPC   EPC   EPC
Example:     0x07   0x00   0xEE   0x00   0xE2  0x00  0x40  0xD4
```

| Field | Size | Description |
|---|---|---|
| `LEN` | 1 byte | Number of bytes **after** this field. In the example: `0x07` = 7 bytes follow. |
| `ADDR` | 1 byte | Reader address. `0x00` = default. |
| `CMD` | 1 byte | Command code. `0xEE` = automatic inventory notification (Active Mode). |
| `STATUS` | 1 byte | Read result. `0x00` = success. |
| `EPC` | variable | The actual tag UID (EPC memory bank contents). Length = `LEN - 3` bytes. |

**Example raw frame (hex):**
```
07 00 EE 00 E2 00 40 D4
```

This frame has `LEN=7`, so total bytes = 1 + 7 = **8 bytes**. The EPC is the last 4 bytes: `E2 00 40 D4`.

---

### Step 2: RS485 Signal Conversion (MAX485 Transceiver)

The reader sends RS485 **differential signals** (A+/B-). A MAX485 transceiver converts these to 3.3V TTL levels for the ESP32.

**File:** `uhf_reader.h:22-25`

| MAX485 Pin | Connects To | Direction | Notes |
|---|---|---|---|
| RO | ESP32 GPIO 16 (RX2) | Reader → ESP32 | Received data |
| DI | ESP32 GPIO 17 (TX2) | ESP32 → Reader | Transmit data (unused in Active Mode) |
| DE + RE | ESP32 GPIO 27 | ESP32 control | LOW = receive (default), HIGH = transmit |

Since Active Mode only requires receiving, the DE/RE pin stays LOW (receive mode) most of the time:

```cpp
void rs485ReceiveMode()  { digitalWrite(RS485_DE, LOW);  }  // Default state
```

---

### Step 3: ESP32 Reads the UART Frame

**File:** `uhf_reader.h:172-258` — `readUHFTag()`

The function reads one complete frame from UART2:

**3a. Fast path check** — If no data is available on UART, return immediately (non-blocking):
```cpp
if (!Serial2.available()) return "";
```

**3b. Read LEN byte** — Read exactly 1 byte (the length field):
```cpp
if (!readExact(Serial2, &resp[0], 1, READ_TIMEOUT)) return "";
const uint8_t dataLen = resp[0];
```

**3c. Read remaining bytes** — Read exactly `dataLen` more bytes:
```cpp
if (!readExact(Serial2, &resp[1], dataLen, READ_TIMEOUT)) return "";
```

**3d. Validate header** — Check that CMD is `0xEE` (inventory) and STATUS is `0x00` (OK):
```cpp
if (command != CMD_INVENTORY) return "";  // Not an inventory frame
if (status != STATUS_OK) return "";       // Read failed
```

**3e. Extract EPC** — Everything after the 4-byte header is the tag's EPC:
```cpp
const int epcLen = respLen - HEADER_LEN;  // EPC length
const uint8_t *epc = &resp[HEADER_LEN];   // Pointer to EPC bytes
```

**3f. Convert to hex string** — Each EPC byte becomes 2 hex characters, uppercase:
```cpp
String uid = "";
for (int i = 0; i < epcLen; i++) {
    if (epc[i] < 0x10) uid += "0";
    uid += String(epc[i], HEX);
}
uid.toUpperCase();
```

**Result for our example frame:**
```
Raw frame:  07 00 EE 00 E2 00 40 D4
EPC bytes:  E2 00 40 D4
UID string: "E20040D4"
```

---

### Step 4: Scan Debounce

**File:** `main.cpp:266-273`

Before processing, the firmware checks a 2-second cooldown to prevent duplicate publishes from the same tag (UHF readers send repeated frames while a tag is in range):

```cpp
unsigned long now = millis();
bool inCooldown = (lastUID.length() > 0 && (now - lastScanTime) < SCAN_COOLDOWN_MS);
```

- If the **same UID** was scanned within the last **2000ms**, the scan is **silently dropped**.
- UART buffer is flushed during cooldown to prevent backlog.

---

### Step 5: SHA-256 Hashing

**File:** `crypto.h:22-39` — `hashUID()`

The raw UID is **never transmitted** over the network. Instead, it is hashed with SHA-256:

```cpp
String hashUID(const String &rawUID) {
    uint8_t digest[32];
    mbedtls_sha256_context ctx;
    mbedtls_sha256_init(&ctx);
    mbedtls_sha256_starts(&ctx, 0);                    // SHA-256 mode
    mbedtls_sha256_update(&ctx, rawUID.c_str(), rawUID.length()); // Hash the UID string
    mbedtls_sha256_finish(&ctx, digest);               // Get 32-byte digest
    mbedtls_sha256_free(&ctx);

    // Convert 32 bytes → 64 hex characters (lowercase)
    String hex = "";
    for (int i = 0; i < 32; i++) {
        if (digest[i] < 16) hex += "0";
        hex += String(digest[i], HEX);
    }
    hex.toLowerCase();
    return hex;
}
```

**Important:** The firmware hashes the raw UID **without** pepper. The server-side pepper (`UID_PEPPER`) is applied by the Go backend when comparing against the database.

**Example:**
```
Input:  "E20040D4"                    (raw UID, 8 hex chars)
Output: "a1b2c3d4e5f6..."             (SHA-256 hash, 64 hex chars)
```

The actual SHA-256 of `"E20040D4"`:
```
Input:  "E20040D4"
Hash:   "3f7c3e0a1b8d9c4e5f6a2b8c9d0e1f2a3b4c5d6e7f8a9b0c1d2e3f4a5b6c7d8e"
```

---

### Step 6: Nonce Generation

**File:** `crypto.h:45-54` — `generateNonce()`

A random nonce is generated per scan for anti-replay protection. The backend rejects any message with a nonce it has seen within the last 60 seconds:

```cpp
String generateNonce() {
    String nonce = "";
    for (int i = 0; i < 4; i++) {       // 4 random bytes
        byte b = random(256);
        if (b < 16) nonce += "0";
        nonce += String(b, HEX);
    }
    nonce.toLowerCase();
    return nonce;                        // 8 hex characters
}
```

**Example output:** `"a3f9c12b"`

---

### Step 7: Timestamp from NTP

**File:** `mqtt_handler.h:244-246`

The ESP32 syncs time via NTP during boot (`configTime(0, 0, "pool.ntp.org")`). Each scan payload includes the current Unix epoch:

```cpp
time_t now;
time(&now);
unsigned long ts = (unsigned long)now;
```

**Example:** `1718000000`

The backend rejects messages with a timestamp older than 30 seconds.

---

### Step 8: Build and Publish MQTT Payload

**File:** `mqtt_handler.h:236-268` — `publishScan()`

The function assembles the final JSON payload and publishes it:

```cpp
snprintf(scanBuffer, sizeof(scanBuffer),
    "{\"uid_hash\":\"%s\",\"device_id\":\"esp32_front_door\",\"nonce\":\"%s\",\"timestamp\":%lu}",
    hash.c_str(), nonce.c_str(), ts);

mqttClient.publish("door/scan", scanBuffer, false);
```

If the publish fails, it retries **once** after a 50ms delay.

---

## Complete Payload Example

### ESP32 → Mosquitto Broker (topic: `door/scan`)

```json
{
    "uid_hash": "3f7c3e0a1b8d9c4e5f6a2b8c9d0e1f2a3b4c5d6e7f8a9b0c1d2e3f4a5b6c7d8e",
    "device_id": "esp32_front_door",
    "nonce": "a3f9c12b",
    "timestamp": 1718000000
}
```

| Field | Type | Size | Description |
|---|---|---|---|
| `uid_hash` | string | 64 chars | SHA-256 hash of raw UID (lowercase hex). The raw UID never leaves the ESP32. |
| `device_id` | string | variable | Identifies which door/device sent the scan. Supports multi-door deployment. |
| `nonce` | string | 8 chars | Random value unique per scan. Backend rejects duplicate nonces (anti-replay). |
| `timestamp` | integer | variable | Unix epoch (seconds). Backend rejects timestamps older than 30 seconds. |

### Broker → Go Backend (same topic: `door/scan`, delivered by subscription)

The Mosquitto broker delivers the identical payload to the Go backend, which has subscribed to `door/scan` at QoS 1.

### Go Backend → Broker → ESP32 (topic: `door/command`)

After validating the hash against PostgreSQL, the backend responds:

**Access Granted:**
```json
{
    "status": 1,
    "message": "Access Granted",
    "action_by": "John Doe"
}
```

**Access Denied:**
```json
{
    "status": 0,
    "message": "Access Denied",
    "action_by": "Unknown"
}
```

| Field | Type | Values | Description |
|---|---|---|---|
| `status` | integer | `1` or `0` | `1` = grant access, `0` = deny access |
| `message` | string | variable | Human-readable result |
| `action_by` | string | variable | User name from DB, or `"Unknown"` if hash not found |

---

## End-to-End Data Transformation Summary

```
TAG IN RANGE
    │
    ▼
┌──────────────────────────────────────────────────────────────────┐
│  READER (HW-VX6330K)                                            │
│  Binary RS485 frame: 07 00 EE 00 E2 00 40 D4                   │
└──────────────────────────────┬───────────────────────────────────┘
                               │ RS485 differential (A+/B-)
                               ▼
┌──────────────────────────────────────────────────────────────────┐
│  MAX485 Transceiver                                              │
│  Converts RS485 → 3.3V TTL, passes to ESP32 GPIO 16 (RX2)      │
└──────────────────────────────┬───────────────────────────────────┘
                               │ UART TTL (3.3V)
                               ▼
┌──────────────────────────────────────────────────────────────────┐
│  ESP32 — readUHFTag()                                           │
│  Parses frame, extracts EPC bytes → hex string                  │
│  Result: "E20040D4"                                             │
└──────────────────────────────┬───────────────────────────────────┘
                               │ String (RAM)
                               ▼
┌──────────────────────────────────────────────────────────────────┐
│  ESP32 — Debounce Check                                         │
│  Same UID within 2 seconds? → DROP (prevents duplicate publish) │
└──────────────────────────────┬───────────────────────────────────┘
                               │ (if not in cooldown)
                               ▼
┌──────────────────────────────────────────────────────────────────┐
│  ESP32 — hashUID()                                              │
│  SHA-256("E20040D4") → "3f7c3e0a...b6c7d8e" (64 hex chars)     │
│  Raw UID is NEVER transmitted — only the hash leaves the device  │
└──────────────────────────────┬───────────────────────────────────┘
                               │ String (RAM)
                               ▼
┌──────────────────────────────────────────────────────────────────┐
│  ESP32 — generateNonce()                                        │
│  4 random bytes → "a3f9c12b" (8 hex chars, unique per scan)    │
└──────────────────────────────┬───────────────────────────────────┘
                               │ String (RAM)
                               ▼
┌──────────────────────────────────────────────────────────────────┐
│  ESP32 — time(&now)                                             │
│  NTP-synced Unix epoch → 1718000000                            │
└──────────────────────────────┬───────────────────────────────────┘
                               │
                               ▼
┌──────────────────────────────────────────────────────────────────┐
│  ESP32 — publishScan()                                          │
│  Assembles JSON:                                                │
│  {                                                              │
│    "uid_hash": "3f7c...d8e",                                    │
│    "device_id": "esp32_front_door",                             │
│    "nonce": "a3f9c12b",                                         │
│    "timestamp": 1718000000                                      │
│  }                                                              │
│  Publishes to MQTT topic "door/scan" (QoS 0, not retained)     │
└──────────────────────────────┬───────────────────────────────────┘
                               │ MQTT over Wi-Fi (TCP :1883)
                               ▼
┌──────────────────────────────────────────────────────────────────┐
│  MOSQUITTO BROKER                                               │
│  Receives on "door/scan", delivers to all subscribers            │
└──────────────────────────────┬───────────────────────────────────┘
                               │ MQTT deliver to subscriber
                               ▼
┌──────────────────────────────────────────────────────────────────┐
│  GO BACKEND                                                     │
│  1. Rate-limit check (1 msg / 2 sec per device_id)              │
│  2. Validate uid_hash = 64 hex chars, device_id not empty       │
│  3. Anti-replay: reject if nonce seen in last 60 sec            │
│  4. Timestamp: reject if older than 30 seconds                  │
│  5. DB query: SELECT name FROM users WHERE rfid_uid_hash = $1   │
│     (Backend applies pepper: sha256(incoming_hash + PEPPER))    │
│  6. Publish result to "door/command"                            │
│     {"status":1,"message":"Access Granted","action_by":"John"}  │
└──────────────────────────────┬───────────────────────────────────┘
                               │ MQTT "door/command"
                               ▼
┌──────────────────────────────────────────────────────────────────┐
│  ESP32 — callback()                                             │
│  Parses "status" field from JSON                                │
│  status=1 → grantAccess() → Green LED ON + 1 beep              │
│  status=0 → denyAccess()  → Red LED ON   + 3 beeps             │
└──────────────────────────────────────────────────────────────────┘
```

---

## Security Properties at Each Step

| Step | Security Measure | Purpose |
|---|---|---|
| Reader → MAX485 | RS485 differential signaling | Noise immunity, up to 1200m cable |
| Raw UID extraction | Never stored, never transmitted | UID privacy — only hash leaves device |
| SHA-256 hashing | One-way cryptographic hash | Cannot recover raw UID from hash |
| Nonce | 4 random bytes per scan | Anti-replay — backend rejects duplicates |
| Timestamp | NTP-synced Unix epoch | Reject stale/delayed messages (>30s old) |
| MQTT auth | Username/password per client | Prevents unauthorized broker access |
| Server-side pepper | Backend adds `UID_PEPPER` before DB compare | Even if hash is intercepted, cannot match DB |

---

## Source File Reference

| Step | File | Function |
|---|---|---|
| 1-2. Reader → RS485 → MAX485 | `config.h:32-40` | Pin and protocol definitions |
| 3. UART frame parsing | `uhf_reader.h:172` | `readUHFTag()` |
| 4. Debounce | `main.cpp:266` | Cooldown check in `loop()` |
| 5. SHA-256 hashing | `crypto.h:22` | `hashUID()` |
| 6. Nonce generation | `crypto.h:45` | `generateNonce()` |
| 7. NTP timestamp | `mqtt_handler.h:244` | `time(&now)` |
| 8. JSON assembly + publish | `mqtt_handler.h:236` | `publishScan()` |
| Command callback | `mqtt_handler.h:195` | `callback()` |
