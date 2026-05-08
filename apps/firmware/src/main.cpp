#include <Arduino.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include <mbedtls/sha256.h>
#include "esp_task_wdt.h"
#include "secrets.h"

// ── Pin Definitions ──────────────────────────────────────────────────
#define GREEN_LED   25  // Access granted indicator (active-LOW)
#define RED_LED     26  // Access denied indicator  (active-LOW)
#define MQTT_LED    5   // MQTT status indicator     (active-LOW)
#define BUZZER      4
#define BUILTIN_LED 2   // WiFi status indicator     (active-HIGH)

// ── UHF Reader (HW-VX6330K via MAX3232) — Active Mode ────────────────
// Reader auto-scans and sends inventory responses (CMD=0xEE).
// Firmware passively listens — no commands sent to the reader.
//
// Active Mode inventory frame (observed):
//   07 00 EE 00 E2 00 40 D4
//   │  │  │  │  ├────────┤
//   │  │  │  │  EPC (the actual UID, 4 bytes for short tags)
//   │  │  │  status (0x00 = OK)
//   │  │  command (0xEE = inventory notification)
//   │  address
//   length (bytes after this field)
//
// Note: This reader sends compact frames without PC/antenna metadata
// or trailing CRC. EPC length = LEN - HEADER_LEN.
//
#define UHF_BAUD      57600
#define UHF_RX        16      // UART2 RX
#define UHF_TX        17      // UART2 TX
#define READ_TIMEOUT  100     // ms per byte wait
#define MAX_RESPONSE  128     // max frame size
#define HEADER_LEN    4       // [LEN][ADDR][CMD][STATUS]
#define CMD_INVENTORY 0xEE    // auto-inventory response
#define STATUS_OK     0x00

// ── MQTT ─────────────────────────────────────────────────────────────
WiFiClient espClient;
PubSubClient mqttClient(espClient);

// ── Scan Debounce ────────────────────────────────────────────────────
String lastUID          = "";
unsigned long lastScanTime = 0;
const unsigned long SCAN_COOLDOWN_MS = 2000;

// ── Non-blocking Actuator Timing ────────────────────────────────────
unsigned long actuatorEnd = 0;   // millis() when current actuator action expires
bool actuatorActive = false;    // true while LED/buzzer sequence is running
int actuatorNextState = -1;     // -1 = idle, 0 = deny in progress, 1 = grant in progress

// ── Reader Health Check ──────────────────────────────────────────────
// In Active Mode, reader sends data autonomously. We track when we last
// received ANY data. If nothing arrives for >5s, reader may be offline.
// In Active Mode the reader is silent when no tag is in range.
// 30s timeout detects genuine reader failure (power loss, wiring broken).
#define READER_TIMEOUT_MS  30000

bool readerOnline             = false;
unsigned long lastDataTime    = 0;

// ── UART config ──────────────────────────────────────────────────────
int uartBaud         = UHF_BAUD;   // may be overridden by auto-detect
constexpr bool uartInvert = false;

// ── JSON Payload Buffer ──────────────────────────────────────────────
char scanBuffer[256];

// ── MQTT Reconnect Backoff ───────────────────────────────────────────
unsigned long lastReconnectAttempt = 0;
int reconnectDelayMs = 1000;
int mqttFailCount = 0;
const int MQTT_MAX_FAILS = 10;

// ── Prototypes ───────────────────────────────────────────────────────
bool readExact(Stream &s, uint8_t *buf, size_t len, unsigned long timeoutMs);
int autoDetectBaud();
String readUHFTag();
String hashUID(const String &rawUID);
String generateNonce();
void publishScan(const String &rawUID);
void reconnect();
void callback(char *topic, byte *payload, unsigned int length);
void grantAccess();
void denyAccess();
void updateStatusLEDs();

// ── Baud Rate Auto-Detection ────────────────────────────────────────
// Tries common baud rates, sends Answer Mode inventory command,
// and checks for a valid response. Hold a tag near the reader during
// boot for best results.
int autoDetectBaud() {
  static const int rates[] = {9600, 19200, 38400, 57600, 115200};
  static const int numRates = 5;
  // Answer Mode inventory command
  static const uint8_t invCmd[] = {0x04, 0xFF, 0x01, 0x1B, 0xB4};

  Serial.println();
  Serial.println("=== UART Baud Auto-Detect ===");
  Serial.println("Hold a tag near the reader for best detection...");
  delay(2000);

  for (int r = 0; r < numRates; r++) {
    esp_task_wdt_reset();
    Serial2.end();
    Serial2.begin(rates[r], SERIAL_8N1, UHF_RX, UHF_TX, uartInvert);
    delay(100);
    while (Serial2.available()) Serial2.read();

    Serial.print("Trying ");
    Serial.print(rates[r]);
    Serial.print(" baud... ");

    // Send inventory command
    Serial2.write(invCmd, sizeof(invCmd));
    Serial2.flush();

    // Wait for response
    delay(200);

    if (!Serial2.available()) {
      // No response — try passive listen for Active Mode data
      esp_task_wdt_reset();
      delay(800);
      if (!Serial2.available()) {
        Serial.println("no data");
        while (Serial2.available()) Serial2.read();
        continue;
      }
    }

    // We got data — try to read a frame
    uint8_t buf[MAX_RESPONSE];
    size_t totalRead = 0;

    // Read all available bytes
    unsigned long start = millis();
    while (totalRead < MAX_RESPONSE && millis() - start < 300) {
      if (Serial2.available()) {
        buf[totalRead++] = (uint8_t)Serial2.read();
        start = millis();
      }
    }

    if (totalRead == 0) {
      Serial.println("no frame");
      continue;
    }

    // Dump raw bytes
    Serial.print("got ");
    Serial.print(totalRead);
    Serial.print(" bytes: ");
    for (size_t i = 0; i < totalRead && i < 20; i++) {
      if (buf[i] < 0x10) Serial.print("0");
      Serial.print(buf[i], HEX);
      Serial.print(" ");
    }
    if (totalRead > 20) Serial.print("...");
    Serial.println();

    // Check for valid frame patterns:
    // Active Mode: [LEN][ADR][0xEE][0x00]...
    // Answer Mode: [LEN][ADR][0x01][STATUS]...
    if (totalRead >= 4) {
      uint8_t len = buf[0];
      uint8_t cmd = buf[2];

      // Check if LEN is consistent with actual data received
      if (len > 0 && (size_t)(1 + len) == totalRead) {
        if (cmd == 0xEE || cmd == 0x01) {
          Serial.print("  >>> VALID frame! CMD=0x");
          Serial.println(cmd, HEX);
          Serial.print("  >>> Using baud rate: ");
          Serial.println(rates[r]);
          while (Serial2.available()) Serial2.read();
          return rates[r];
        }
      }

      // Even if LEN doesn't match exactly, check for 0xEE
      for (size_t i = 2; i < totalRead; i++) {
        if (buf[i] == 0xEE && i > 0 && buf[i + 1] == 0x00) {
          Serial.println("  >>> Found 0xEE in data stream");
          Serial.print("  >>> Using baud rate: ");
          Serial.println(rates[r]);
          while (Serial2.available()) Serial2.read();
          return rates[r];
        }
      }
    }

    Serial.println("  not a valid frame");
    while (Serial2.available()) Serial2.read();
  }

  // Fallback: just pick the rate with most non-zero data
  Serial.println();
  Serial.println("No valid frame found at any baud rate.");
  Serial.println("Check: reader power, MAX3232 wiring, TX/RX swap");
  return 0;
}

// ── Setup ────────────────────────────────────────────────────────────
void setup() {
  Serial.begin(115200);
  delay(1000);

  // Watchdog FIRST — 30s to allow for baud auto-detect + WiFi setup
  esp_task_wdt_init(30, true);
  esp_task_wdt_add(NULL);

  // UART2 — auto-detect baud rate
  uartBaud = autoDetectBaud();
  if (uartBaud == 0) {
    Serial.println("!!! FAILED to detect reader baud rate — using default 57600");
    uartBaud = 57600;
  }
  Serial2.begin(uartBaud, SERIAL_8N1, UHF_RX, UHF_TX, uartInvert);
  delay(100);
  while (Serial2.available()) Serial2.read();
  Serial.print("UART2 locked: ");
  Serial.print(uartBaud);
  Serial.println(" baud");

  // Actuators — BUILTIN active-HIGH, external LEDs active-LOW
  pinMode(GREEN_LED, OUTPUT);
  pinMode(RED_LED, OUTPUT);
  pinMode(MQTT_LED, OUTPUT);
  pinMode(BUZZER, OUTPUT);
  pinMode(BUILTIN_LED, OUTPUT);
  digitalWrite(GREEN_LED, HIGH);   // active-LOW: HIGH = OFF
  digitalWrite(RED_LED, HIGH);     // active-LOW: HIGH = OFF
  digitalWrite(MQTT_LED, HIGH);    // active-LOW: HIGH = OFF
  digitalWrite(BUZZER, LOW);
  digitalWrite(BUILTIN_LED, LOW);  // active-HIGH: LOW = OFF

  // Wi-Fi
  WiFi.mode(WIFI_STA);
  WiFi.setAutoConnect(false);
  WiFi.setAutoReconnect(true);
  delay(100);

  Serial.println();
  Serial.println("=== WiFi Debug ===");
  Serial.print("SSID:     "); Serial.println(WIFI_SSID);
  Serial.print("Password: "); Serial.println("********");
  Serial.print("MAC:      "); Serial.println(WiFi.macAddress());

  Serial.println("Scanning nearby networks...");
  int netCount = WiFi.scanNetworks();
  bool ssidFound = false;
  for (int i = 0; i < netCount; i++) {
    Serial.print("  ");
    Serial.print(i);
    Serial.print(": ");
    Serial.print(WiFi.SSID(i));
    Serial.print(" (ch ");
    Serial.print(WiFi.channel(i));
    Serial.print(", RSSI ");
    Serial.print(WiFi.RSSI(i));
    Serial.print(" dBm, ");
    Serial.print(WiFi.encryptionType(i) == WIFI_AUTH_OPEN ? "OPEN" : "ENCRYPTED");
    Serial.println(")");
    if (WiFi.SSID(i) == WIFI_SSID) {
      ssidFound = true;
    }
  }
  if (!ssidFound) {
    Serial.print("!!! SSID '");
    Serial.print(WIFI_SSID);
    Serial.println("' NOT FOUND in scan!");
    Serial.println("!!! ESP32 only supports 2.4 GHz — check if your router uses 5 GHz only");
  } else {
    Serial.print("SSID '");
    Serial.print(WIFI_SSID);
    Serial.println("' found in scan");
  }
  WiFi.scanDelete();

  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.print("Connecting to WiFi");
  int wifiAttempts = 0;
  wl_status_t lastStatus = WL_IDLE_STATUS;
  while (WiFi.status() != WL_CONNECTED && wifiAttempts < 40) {
    delay(500);
    wl_status_t curStatus = WiFi.status();
    if (curStatus != lastStatus) {
      Serial.println();
      Serial.print("  status=");
      Serial.print(curStatus);
      Serial.print(" (");
      switch (curStatus) {
        case WL_IDLE_STATUS:     Serial.print("IDLE"); break;
        case WL_NO_SSID_AVAIL:   Serial.print("NO_SSID_AVAIL"); break;
        case WL_SCAN_COMPLETED:  Serial.print("SCAN_COMPLETED"); break;
        case WL_CONNECTED:       Serial.print("CONNECTED"); break;
        case WL_CONNECT_FAILED:  Serial.print("CONNECT_FAILED"); break;
        case WL_CONNECTION_LOST:  Serial.print("CONNECTION_LOST"); break;
        case WL_DISCONNECTED:    Serial.print("DISCONNECTED"); break;
        default:                 Serial.print("UNKNOWN"); break;
      }
      Serial.println(")");
      lastStatus = curStatus;
    } else {
      Serial.print(".");
    }
    wifiAttempts++;
    esp_task_wdt_reset();
  }
  Serial.println();
  if (WiFi.status() == WL_CONNECTED) {
    Serial.print("WiFi connected, IP: ");
    Serial.println(WiFi.localIP());
    Serial.print("Gateway: ");
    Serial.println(WiFi.gatewayIP());
  } else {
    Serial.print("WiFi FAILED after ");
    Serial.print(wifiAttempts);
    Serial.print(" attempts, final status=");
    Serial.println(WiFi.status());
    Serial.println("!!! Check: wrong password, 5 GHz only, or MAC filter");
  }

  if (WiFi.status() == WL_CONNECTED) {
    configTime(0, 0, "pool.ntp.org", "time.nist.gov");
    Serial.println("NTP sync started (background)");
    Serial.println("Waiting for TCP stack to stabilize...");
    delay(1500);
  }

  // MQTT
  mqttClient.setServer(MQTT_SERVER, 1883);
  mqttClient.setCallback(callback);

  lastDataTime = millis();
  lastReconnectAttempt = millis();  // grace period before first MQTT attempt
  Serial.println("RFID Access Control (UHF Active Mode) — Ready");
}

// ── Main Loop ────────────────────────────────────────────────────────
void loop() {
  esp_task_wdt_reset();
  reconnect();

  // Non-blocking actuator: check if current action has expired
  if (actuatorActive && millis() >= actuatorEnd) {
    digitalWrite(RED_LED, HIGH);    // active-LOW: HIGH = OFF
    actuatorActive = false;
    actuatorNextState = -1;
  }

  unsigned long now = millis();
  bool inCooldown = (lastUID.length() > 0 && (now - lastScanTime) < SCAN_COOLDOWN_MS);

  // Drain UART during cooldown or actuator to prevent buffer spam
  if (inCooldown || actuatorActive) {
    while (Serial2.available()) Serial2.read();
  }

  // Only read tags when not in cooldown and not actuating
  if (!actuatorActive && !inCooldown) {
    String uid = readUHFTag();

    if (uid.length() > 0) {
      lastDataTime = millis();
      if (!readerOnline) {
        readerOnline = true;
        Serial.println("HW-VX6330K: ONLINE (tag data received)");
      }

      lastUID = uid;
      lastScanTime = millis();

      publishScan(uid);
    }
  }

  // Health check: no data from reader for too long → offline
  if (readerOnline && millis() - lastDataTime > READER_TIMEOUT_MS) {
    readerOnline = false;
    Serial.println("HW-VX6330K: OFFLINE (no data for 5s)");
  }

  // Status LEDs: Blue = WiFi, Green = MQTT
  updateStatusLEDs();

  // ALWAYS process MQTT — no early return before this
  mqttClient.loop();
}

// ── UHF Reader: read exact N bytes with timeout ──────────────────────
bool readExact(Stream &s, uint8_t *buf, size_t len, unsigned long timeoutMs) {
  unsigned long start = millis();
  size_t i = 0;
  while (i < len) {
    if (s.available()) {
      buf[i++] = (uint8_t)s.read();
      start = millis();
    } else if (millis() - start > timeoutMs) {
      return false;
    }
  }
  return true;
}

// ── UHF Reader: passively read an auto-inventory frame → EPC hex ────
// Follows the official Electron.id HW-VX6330K Active Mode example.
String readUHFTag() {
  // Fast path — no data available
  if (!Serial2.available()) return "";

  uint8_t resp[MAX_RESPONSE];
  size_t respLen = 0;

  // 1) Read length byte with timeout
  if (!readExact(Serial2, &resp[0], 1, READ_TIMEOUT)) {
    return "";
  }

  const uint8_t dataLen = resp[0];
  if (dataLen == 0 || (size_t)(dataLen + 1) > MAX_RESPONSE) {
    // Invalid LEN — drain and discard
    while (Serial2.available()) Serial2.read();
    return "";
  }
  respLen = 1 + dataLen;

  // 2) Read remaining dataLen bytes
  if (!readExact(Serial2, &resp[1], dataLen, READ_TIMEOUT)) {
    Serial.println("UHF: timeout reading frame data");
    return "";
  }

  // Update last-data timestamp (any frame = reader alive)
  lastDataTime = millis();
  if (!readerOnline) {
    readerOnline = true;
    Serial.println("HW-VX6330K: ONLINE (frame received)");
  }

  // Dump frame for debug
  Serial.print("UHF FRAME (");
  Serial.print(respLen);
  Serial.print(" bytes): ");
  for (size_t i = 0; i < respLen; i++) {
    if (resp[i] < 0x10) Serial.print("0");
    Serial.print(resp[i], HEX);
    Serial.print(" ");
  }
  Serial.println();

  // 3) Structural check
  if (respLen < HEADER_LEN) {
    Serial.println("UHF: frame too short");
    return "";
  }

  // 4) Validate header: CMD must be 0xEE, STATUS must be 0x00
  const uint8_t command = resp[2];
  const uint8_t status  = resp[3];

  if (command != CMD_INVENTORY) {
    Serial.print("UHF: CMD=0x");
    Serial.print(command, HEX);
    Serial.println(" (expected 0xEE)");
    return "";
  }

  if (status != STATUS_OK) {
    Serial.print("UHF: STATUS=0x");
    Serial.println(status, HEX);
    return "";
  }

  // 5) Extract EPC — everything after header (no PC/antenna meta, no CRC)
  //    Frame: [LEN][ADR][CMD][STATUS][EPC...]
  //    EPC length = total frame - header
  const int epcLen = respLen - HEADER_LEN;
  if (epcLen <= 0) {
    Serial.println("UHF: no EPC data");
    return "";
  }

  const uint8_t *epc = &resp[HEADER_LEN];
  String uid = "";
  for (int i = 0; i < epcLen; i++) {
    if (epc[i] < 0x10) uid += "0";
    uid += String(epc[i], HEX);
  }
  uid.toUpperCase();
  Serial.print("UHF EPC: ");
  Serial.println(uid);
  return uid;
}

// ── Wi-Fi + MQTT Reconnection with Exponential Backoff ───────────────
void reconnect() {
  if (mqttClient.connected()) return;

  unsigned long now = millis();
  if (now - lastReconnectAttempt < (unsigned long)reconnectDelayMs) return;
  lastReconnectAttempt = now;

  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("WiFi lost, reconnecting...");
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    return;
  }

  // TCP probe — check if broker is reachable before wasting an MQTT attempt
  WiFiClient probe;
  probe.setTimeout(500);
  Serial.print("Probing broker ");
  Serial.print(MQTT_SERVER);
  Serial.print(":1883... ");
  if (!probe.connect(MQTT_SERVER, 1883, 500)) {
    mqttFailCount++;
    Serial.print("unreachable (");
    Serial.print(mqttFailCount);
    Serial.print("/");
    Serial.print(MQTT_MAX_FAILS);
    Serial.println(")");
    probe.stop();

    if (mqttFailCount >= MQTT_MAX_FAILS) {
      Serial.println("!!! MQTT failed 10 times — resetting ESP32 !!!");
      Serial.flush();
      ESP.restart();
    }

    reconnectDelayMs = 2000;
    return;
  }
  probe.stop();
  Serial.println("ok");

  Serial.print("Connecting MQTT...");
  if (mqttClient.connect("esp32_front_door", MQTT_USER, MQTT_PASS,
                         "door/status", 0, true,
                         "{\"status\":\"offline\"}")) {
    Serial.println(" connected");
    mqttFailCount = 0;
    mqttClient.subscribe("door/command", 1);
    mqttClient.publish("door/status", "{\"status\":\"online\"}", true);
    reconnectDelayMs = 1000;
  } else {
    mqttFailCount++;
    Serial.print(" failed (");
    Serial.print(mqttFailCount);
    Serial.print("/");
    Serial.print(MQTT_MAX_FAILS);
    Serial.print("), rc=");
    Serial.print(mqttClient.state());

    if (mqttFailCount >= MQTT_MAX_FAILS) {
      Serial.println();
      Serial.println("!!! MQTT failed 10 times — resetting ESP32 !!!");
      Serial.flush();
      ESP.restart();
    }

    Serial.println(" — retrying");
    reconnectDelayMs = min(reconnectDelayMs * 2, 5000);
  }
}

// ── MQTT Message Callback ────────────────────────────────────────────
void callback(char *topic, byte *payload, unsigned int length) {
  char msg[128];
  unsigned int len = min(length, (unsigned int)(sizeof(msg) - 1));
  memcpy(msg, payload, len);
  msg[len] = '\0';

  if (String(topic) == "door/command") {
    int status = -1;

    const char *key = "\"status\":";
    const char *p = strstr(msg, key);
    if (p) {
      p += strlen(key);
      while (*p == ' ') p++;
      if (*p >= '0' && *p <= '9') {
        status = *p - '0';
      }
    }

    Serial.print("Command received: status=");
    Serial.println(status);

    if (status == 1) {
      grantAccess();
    } else if (status == 0) {
      denyAccess();
    }
  }
}

// ── UID Hashing ──────────────────────────────────────────────────────
String hashUID(const String &rawUID) {
  uint8_t digest[32];
  mbedtls_sha256_context ctx;
  mbedtls_sha256_init(&ctx);
  mbedtls_sha256_starts(&ctx, 0);
  mbedtls_sha256_update(&ctx, (const unsigned char *)rawUID.c_str(), rawUID.length());
  mbedtls_sha256_finish(&ctx, digest);
  mbedtls_sha256_free(&ctx);

  String hex = "";
  for (int i = 0; i < 32; i++) {
    if (digest[i] < 16) hex += "0";
    hex += String(digest[i], HEX);
  }
  hex.toLowerCase();
  return hex;
}

String generateNonce() {
  String nonce = "";
  for (int i = 0; i < 4; i++) {
    byte b = random(256);
    if (b < 16) nonce += "0";
    nonce += String(b, HEX);
  }
  nonce.toLowerCase();
  return nonce;
}

// ── Publish Scan ─────────────────────────────────────────────────────
void publishScan(const String &rawUID) {
  if (!mqttClient.connected()) {
    Serial.println("Scan dropped — MQTT not connected");
    return;
  }

  String hash = hashUID(rawUID);
  String nonce = generateNonce();
  time_t now;
  time(&now);
  unsigned long ts = (unsigned long)now;

  snprintf(scanBuffer, sizeof(scanBuffer),
    "{\"uid_hash\":\"%s\",\"device_id\":\"esp32_front_door\",\"nonce\":\"%s\",\"timestamp\":%lu}",
    hash.c_str(), nonce.c_str(), ts);

  Serial.print("Raw UID: ");
  Serial.println(rawUID);
  Serial.print("Publishing scan: ");
  Serial.println(scanBuffer);

  if (!mqttClient.publish("door/scan", scanBuffer, false)) {
    Serial.println("Publish FAILED — retrying once...");
    delay(50);
    mqttClient.loop();
    if (!mqttClient.publish("door/scan", scanBuffer, false)) {
      Serial.println("Publish FAILED again — scan lost");
    }
  }
}

// ── Actuator Control (non-blocking) ─────────────────────────────────
void grantAccess() {
  Serial.println("ACCESS GRANTED");
  digitalWrite(RED_LED, HIGH);     // active-LOW: HIGH = OFF
  digitalWrite(GREEN_LED, LOW);    // active-LOW: LOW = ON
  digitalWrite(BUZZER, HIGH);
  delay(200);  // short beep — brief block is acceptable
  digitalWrite(BUZZER, LOW);
  actuatorEnd = millis() + 3000;
  actuatorActive = true;
  actuatorNextState = 1;
}

void denyAccess() {
  Serial.println("ACCESS DENIED");
  digitalWrite(GREEN_LED, HIGH);    // active-LOW: HIGH = OFF
  digitalWrite(RED_LED, LOW);       // active-LOW: LOW = ON
  // Beep 3 times with brief blocking delays (total ~650ms)
  for (int i = 0; i < 3; i++) {
    digitalWrite(BUZZER, HIGH);
    delay(150);
    digitalWrite(BUZZER, LOW);
    if (i < 2) delay(100);
  }
  actuatorEnd = millis() + 3000;
  actuatorActive = true;
  actuatorNextState = 0;
}

// ── Status LEDs ──────────────────────────────────────────────────────
// Blue LED (BUILTIN_LED) = WiFi   — active-HIGH (HIGH = ON)
// MQTT LED (GPIO 5)      = MQTT   — active-LOW  (LOW = ON)
// Green LED (GPIO 25)    = Access granted only (active-LOW: LOW = ON)
// Red LED   (GPIO 26)    = Access denied only   (active-LOW: LOW = ON)
void updateStatusLEDs() {
  // Blue LED — WiFi (active-HIGH)
  digitalWrite(BUILTIN_LED, WiFi.status() == WL_CONNECTED ? HIGH : LOW);

  // MQTT LED — MQTT status (active-LOW: LOW = ON)
  digitalWrite(MQTT_LED, mqttClient.connected() ? LOW : HIGH);
}

