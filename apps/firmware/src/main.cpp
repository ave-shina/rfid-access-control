#include <Arduino.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include <MFRC522.h>
#include <SPI.h>
#include <mbedtls/sha256.h>
#include "esp_task_wdt.h"
#include "secrets.h"

// ── Pin Definitions ──────────────────────────────────────────────────
#define SS_PIN    5
#define RST_PIN   22
#define GREEN_LED 25
#define RED_LED   26
#define BUZZER    4
#define BUILTIN_LED 2

// ── RFID & MQTT Objects ──────────────────────────────────────────────
MFRC522 mfrc522(SS_PIN, RST_PIN);
WiFiClient espClient;
PubSubClient mqttClient(espClient);

// ── Scan Debounce ────────────────────────────────────────────────────
String lastUID          = "";
unsigned long lastScanTime = 0;
const unsigned long SCAN_COOLDOWN_MS = 2000;

// ── JSON Payload Buffer ──────────────────────────────────────────────
// Pre-allocated buffer for scan payloads
char scanBuffer[256];

// ── Prototypes ───────────────────────────────────────────────────────
String getRawUID();
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

  // Actuators
  pinMode(GREEN_LED, OUTPUT);
  pinMode(RED_LED, OUTPUT);
  pinMode(BUZZER, OUTPUT);
  pinMode(BUILTIN_LED, OUTPUT);
  digitalWrite(GREEN_LED, HIGH);
  digitalWrite(RED_LED, HIGH);
  digitalWrite(BUZZER, LOW);
  digitalWrite(BUILTIN_LED, HIGH);

  // SPI + RFID
  SPI.begin(18, 19, 23, SS_PIN);  // SCK=18, MISO=19, MOSI=23, SS=5
  mfrc522.PCD_Init();
  delay(4);
  mfrc522.PCD_DumpVersionToSerial();

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

  Serial.println("RFID Access Control — Ready");
}

// ── Main Loop ────────────────────────────────────────────────────────
void loop() {
  esp_task_wdt_reset();
  reconnect();

  // Check for new RFID card
  if (!mfrc522.PICC_IsNewCardPresent() || !mfrc522.PICC_ReadCardSerial()) {
    mqttClient.loop();
    return;
  }

  String uid = getRawUID();
  unsigned long now = millis();

  // Debounce — same card within cooldown window is ignored
  if (uid == lastUID && (now - lastScanTime) < SCAN_COOLDOWN_MS) {
    mqttClient.loop();
    return;
  }

  lastUID = uid;
  lastScanTime = now;

  publishScan(uid);

  // Halt PICC and stop crypto — ready for next scan
  mfrc522.PICC_HaltA();
  mfrc522.PCD_StopCrypto1();

  mqttClient.loop();
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
    // Simple parsing since we control the format
    int status = -1;

    // Look for "status": followed by a digit
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

// ── UID Helpers ──────────────────────────────────────────────────────
String getRawUID() {
  String uid = "";
  for (byte i = 0; i < mfrc522.uid.size; i++) {
    if (mfrc522.uid.uidByte[i] < 0x10) uid += "0";
    uid += String(mfrc522.uid.uidByte[i], HEX);
  }
  uid.toUpperCase();
  return uid;
}

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

  mqttClient.publish("door/scan", scanBuffer, false);  // QoS handled by client config
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
