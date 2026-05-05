#include <Arduino.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include <mbedtls/sha256.h>
#include "esp_task_wdt.h"
#include "secrets.h"

// ── Pin Definitions ──────────────────────────────────────────────────
#define GREEN_LED   25
#define RED_LED     26
#define BUZZER      4
#define BUILTIN_LED 2

// ── UHF Reader (HW-VX6330K via MAX3232) ──────────────────────────────
#define UHF_BAUD      57600
#define UHF_RX        16      // UART2 RX
#define UHF_TX        17      // UART2 TX
#define READ_TIMEOUT  100     // ms per byte wait
#define MAX_RESPONSE  128     // max frame size
#define HEADER_LEN    4       // [LEN][ADDR][CMD][STATUS]
#define CHECKSUM_LEN  2       // trailing checksum
#define STATUS_OK     0x00
#define CMD_INVENTORY 0xEE    // auto-inventory response

// ── MQTT ─────────────────────────────────────────────────────────────
WiFiClient espClient;
PubSubClient mqttClient(espClient);

// ── Scan Debounce ────────────────────────────────────────────────────
String lastUID          = "";
unsigned long lastScanTime = 0;
const unsigned long SCAN_COOLDOWN_MS = 2000;

// ── Reader Health Check ──────────────────────────────────────────────
#define HEALTH_CHECK_INTERVAL_MS  5000  // probe every 5 seconds
#define PROBE_TIMEOUT_MS          300   // wait 300ms for reader response
#define MAX_HEALTH_FAILS          3     // after N consecutive fails → offline
#define CMD_GET_READER_INFO       0x21  // adjust per HW-VX6330K datasheet

bool readerOnline           = false;
unsigned long lastHealthCheckTime = 0;
int healthCheckFailCount    = 0;

// ── JSON Payload Buffer ──────────────────────────────────────────────
char scanBuffer[256];

// ── Prototypes ───────────────────────────────────────────────────────
bool readExact(Stream &s, uint8_t *buf, size_t len, unsigned long timeoutMs);
String readUHFTag();
bool probeReader();
String hashUID(const String &rawUID);
String generateNonce();
void publishScan(const String &rawUID);
void reconnect();
void callback(char *topic, byte *payload, unsigned int length);
void grantAccess();
void denyAccess();
void beep(int times, int durationMs);

// ── Setup ────────────────────────────────────────────────────────────
void setup() {
  Serial.begin(115200);
  while (!Serial) { ; }

  // UART2 for HW-VX6330K UHF reader (via MAX3232 level shifter)
  Serial2.begin(UHF_BAUD, SERIAL_8N1, UHF_RX, UHF_TX);

  // Initial reader probe — verify HW-VX6330K is connected
  Serial.print("Probing HW-VX6330K on UART2 @ ");
  Serial.print(UHF_BAUD);
  Serial.print(" baud... ");
  delay(100);  // let UART stabilize
  readerOnline = probeReader();
  Serial.println(readerOnline ? "DETECTED" : "NO RESPONSE (check wiring/MAX3232)");

  // Actuators
  pinMode(GREEN_LED, OUTPUT);
  pinMode(RED_LED, OUTPUT);
  pinMode(BUZZER, OUTPUT);
  pinMode(BUILTIN_LED, OUTPUT);
  digitalWrite(GREEN_LED, HIGH);
  digitalWrite(RED_LED, HIGH);
  digitalWrite(BUZZER, LOW);
  digitalWrite(BUILTIN_LED, HIGH);

  // Wi-Fi
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.print("Connecting to WiFi");
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println();
  Serial.print("WiFi connected, IP: ");
  Serial.println(WiFi.localIP());
  digitalWrite(BUILTIN_LED, LOW);  // WiFi connected indicator

  // NTP for accurate timestamps
  configTime(0, 0, "pool.ntp.org", "time.nist.gov");
  Serial.print("Waiting for NTP time");
  struct tm timeinfo;
  while (!getLocalTime(&timeinfo)) {
    Serial.print(".");
    delay(500);
  }
  Serial.println(" OK");

  // MQTT
  mqttClient.setServer(MQTT_SERVER, 1883);
  mqttClient.setCallback(callback);

  // Watchdog timer — 10 second timeout, panic on trigger
  esp_task_wdt_init(10, true);
  esp_task_wdt_add(NULL);

  Serial.println("RFID Access Control (UHF) — Ready");
}

// ── Main Loop ────────────────────────────────────────────────────────
void loop() {
  esp_task_wdt_reset();
  reconnect();

  // Read UHF tag via UART2
  String uid = readUHFTag();
  if (uid.length() == 0) {
    // Periodic reader health check when idle
    unsigned long nowHC = millis();
    if (nowHC - lastHealthCheckTime >= HEALTH_CHECK_INTERVAL_MS) {
      lastHealthCheckTime = nowHC;
      bool wasOnline = readerOnline;
      bool probeOk = probeReader();

      if (probeOk) {
        healthCheckFailCount = 0;
        if (!wasOnline) {
          Serial.println("HW-VX6330K: ONLINE (reader responded to probe)");
        }
        readerOnline = true;
      } else {
        healthCheckFailCount++;
        if (healthCheckFailCount >= MAX_HEALTH_FAILS) {
          if (wasOnline) {
            Serial.print("HW-VX6330K: OFFLINE (no response after ");
            Serial.print(MAX_HEALTH_FAILS);
            Serial.println(" probes)");
            // Brief RED LED blink to indicate reader offline
            digitalWrite(RED_LED, LOW);
            delay(100);
            digitalWrite(RED_LED, HIGH);
          }
          readerOnline = false;
        }
      }
    }
    mqttClient.loop();
    return;
  }

  // Tag was successfully read — reader is confirmed online
  if (!readerOnline) {
    readerOnline = true;
    healthCheckFailCount = 0;
    Serial.println("HW-VX6330K: ONLINE (tag data received)");
  }
  lastHealthCheckTime = millis();  // reset health check timer on activity

  unsigned long now = millis();

  // Debounce — same card within cooldown window is ignored
  if (uid == lastUID && (now - lastScanTime) < SCAN_COOLDOWN_MS) {
    mqttClient.loop();
    return;
  }

  lastUID = uid;
  lastScanTime = now;

  publishScan(uid);

  mqttClient.loop();
}

// ── UHF Reader: read exact N bytes with timeout ──────────────────────
bool readExact(Stream &s, uint8_t *buf, size_t len, unsigned long timeoutMs) {
  unsigned long start = millis();
  size_t i = 0;
  while (i < len) {
    if (s.available()) {
      buf[i++] = (uint8_t)s.read();
      start = millis();  // reset timeout on each byte
    } else if (millis() - start > timeoutMs) {
      return false;
    }
  }
  return true;
}

// ── UHF Reader: parse HW-VX6330K frame → hex UID string ─────────────
String readUHFTag() {
  if (!Serial2.available()) return "";

  uint8_t resp[MAX_RESPONSE];
  size_t respLen = 0;

  // 1) Read length field (1 byte)
  if (!readExact(Serial2, &resp[0], 1, READ_TIMEOUT)) {
    Serial.println("UHF: timeout reading length");
    return "";
  }

  const uint8_t dataLen = resp[0];
  respLen = 1 + dataLen;

  if (respLen > MAX_RESPONSE) {
    Serial.println("UHF: frame too large, draining");
    for (uint8_t j = 0; j < dataLen && Serial2.available(); ++j) Serial2.read();
    return "";
  }

  // 2) Read remaining dataLen bytes
  if (!readExact(Serial2, &resp[1], dataLen, READ_TIMEOUT)) {
    Serial.println("UHF: timeout reading data");
    return "";
  }

  // 3) Validate frame structure
  if (respLen < (HEADER_LEN + CHECKSUM_LEN)) {
    Serial.println("UHF: frame too short");
    return "";
  }

  const uint8_t command = resp[2];
  const uint8_t status  = resp[3];

  if (status != STATUS_OK) return "";
  if (command != CMD_INVENTORY) return "";

  // 4) Extract tag payload (between header and checksum)
  const int payloadLen = respLen - HEADER_LEN - CHECKSUM_LEN;
  if (payloadLen <= 0) return "";

  const uint8_t *tag = &resp[HEADER_LEN];

  // 5) Convert to hex string (same format as old getRawUID)
  String uid = "";
  for (int i = 0; i < payloadLen; i++) {
    if (tag[i] < 0x10) uid += "0";
    uid += String(tag[i], HEX);
  }
  uid.toUpperCase();
  return uid;
}

// ── Reader Health Check: probe HW-VX6330K ───────────────────────────
bool probeReader() {
  // Drain stale data from Serial2 RX buffer
  while (Serial2.available()) Serial2.read();

  // Build "Get Reader Info" command frame
  // Format: [LEN][ADR][CMD][CHK_L][CHK_H]
  // NOTE: verify these bytes against your HW-VX6330K datasheet/protocol doc.
  //       The command code (0x21) and checksum method may differ.
  uint8_t frame[5];
  frame[0] = 0x04;                   // LEN = bytes after this field
  frame[1] = 0xFF;                   // ADR = broadcast address
  frame[2] = CMD_GET_READER_INFO;    // CMD = Get Reader Info

  // Checksum: 16-bit sum of ADR + CMD
  uint16_t chk = (uint16_t)frame[1] + (uint16_t)frame[2];
  frame[3] = (uint8_t)(chk & 0xFF);
  frame[4] = (uint8_t)((chk >> 8) & 0xFF);

  Serial2.write(frame, sizeof(frame));
  Serial2.flush();

  // Wait for any response from the reader
  unsigned long start = millis();
  while (millis() - start < PROBE_TIMEOUT_MS) {
    if (Serial2.available()) {
      // Reader responded — drain the response
      while (Serial2.available()) {
        Serial2.read();
      }
      return true;
    }
    delay(1);
  }
  return false;
}

// ── Wi-Fi + MQTT Reconnection with Exponential Backoff ───────────────
void reconnect() {
  if (mqttClient.connected()) return;

  int delayMs = 1000;
  while (!mqttClient.connected()) {
    // Ensure Wi-Fi is up
    if (WiFi.status() != WL_CONNECTED) {
      Serial.println("WiFi lost, reconnecting...");
      digitalWrite(BUILTIN_LED, HIGH);  // WiFi lost indicator
      WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
      delay(delayMs);
      delayMs = min(delayMs * 2, 30000);
      continue;
    }

    // Attempt MQTT connection with LWT
    Serial.print("Connecting MQTT...");
    if (mqttClient.connect("esp32_front_door", MQTT_USER, MQTT_PASS,
                           "door/status", 0, true,
                           "{\"status\":\"offline\"}")) {
      Serial.println(" connected");
      digitalWrite(BUILTIN_LED, LOW);  // WiFi+MQTT connected indicator
      mqttClient.subscribe("door/command", 1);  // QoS 1
      mqttClient.publish("door/status", "{\"status\":\"online\"}", true);
      delayMs = 1000;  // reset backoff
    } else {
      Serial.print(" failed, rc=");
      Serial.print(mqttClient.state());
      Serial.println(" — retrying");
      // Feed watchdog during long backoff delays
      unsigned long waited = 0;
      while (waited < (unsigned long)delayMs) {
        esp_task_wdt_reset();
        delay(100);
        waited += 100;
      }
      delayMs = min(delayMs * 2, 30000);
    }
  }
}

// ── MQTT Message Callback ────────────────────────────────────────────
void callback(char *topic, byte *payload, unsigned int length) {
  // Build the payload string
  char msg[128];
  unsigned int len = min(length, (unsigned int)(sizeof(msg) - 1));
  memcpy(msg, payload, len);
  msg[len] = '\0';

  if (String(topic) == "door/command") {
    // Parse status field — find "status":N in JSON
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
  mbedtls_sha256_starts(&ctx, 0);  // 0 = SHA-256
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
  // 4-byte random hex
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
  String hash = hashUID(rawUID);
  String nonce = generateNonce();
  time_t now;
  time(&now);
  unsigned long ts = (unsigned long)now;

  // Build JSON manually
  snprintf(scanBuffer, sizeof(scanBuffer),
    "{\"uid_hash\":\"%s\",\"device_id\":\"esp32_front_door\",\"nonce\":\"%s\",\"timestamp\":%lu}",
    hash.c_str(), nonce.c_str(), ts);

  Serial.print("Raw UID: ");
  Serial.println(rawUID);
  Serial.print("Publishing scan: ");
  Serial.println(scanBuffer);

  mqttClient.publish("door/scan", scanBuffer, false);
}

// ── Actuator Control ─────────────────────────────────────────────────
void grantAccess() {
  Serial.println("ACCESS GRANTED");
  digitalWrite(RED_LED, HIGH);
  digitalWrite(GREEN_LED, LOW);
  beep(1, 200);
  delay(3000);
  digitalWrite(GREEN_LED, HIGH);
}

void denyAccess() {
  Serial.println("ACCESS DENIED");
  digitalWrite(GREEN_LED, HIGH);
  digitalWrite(RED_LED, LOW);
  beep(3, 150);
  delay(3000);
  digitalWrite(RED_LED, HIGH);
}

void beep(int times, int durationMs) {
  for (int i = 0; i < times; i++) {
    digitalWrite(BUZZER, HIGH);
    delay(durationMs);
    digitalWrite(BUZZER, LOW);
    if (i < times - 1) delay(100);
  }
}
