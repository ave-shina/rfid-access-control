// =============================================================================
// mqtt_handler.h — Fungsi Koneksi MQTT dan Publish/Subscribe
// =============================================================================
// Modul ini menangani semua operasi MQTT:
//   - reconnect():    Koneksi ulang Wi-Fi + MQTT dengan exponential backoff.
//                      Dipanggil setiap iterasi loop(). Jika gagal 10x → reboot ESP32.
//   - callback():     Handler pesan MQTT masuk dari topik "door/command".
//                      Parse JSON sederhana → panggil grantAccess() atau denyAccess().
//   - publishScan():   Menyusun payload JSON (hash + nonce + timestamp)
//                      dan mem-publish ke topik "door/scan". Retry sekali jika gagal.
//
// Dependensi: WiFi.h, PubSubClient.h, secrets.h, config.h, crypto.h
// Variabel global yang digunakan:
//   - mqttClient, mqttFailCount, lastReconnectAttempt, reconnectDelayMs  (baca/tulis)
//   - scanBuffer                  (tulis — buffer payload JSON)
//   - actuatorActive, actuatorEnd  (tulis via grantAccess/denyAccess)
// =============================================================================

#pragma once
#include "config.h"
#include "crypto.h"

// ── Broker Auto-Discovery ───────────────────────────────────────────
// Menyimpan IP broker yang ditemukan. Kosong = belum ditemukan.
// Setelah ditemukan, IP ini di-cache dan digunakan untuk semua koneksi
// selanjutnya sampai WiFi terputus (maka cache di-reset).
String discoveredBroker = "";

// Probe satu IP:port dengan TCP connect. Return true jika reachable.
// Menggunakan satu WiFiClient yang di-stop() + delay() antar probe
// untuk mencegah socket exhaustion (ESP32 punya batas fd yang rendah).
bool probeBroker(const char *ip, uint16_t port) {
  WiFiClient probe;
  bool ok = probe.connect(ip, port, DISCOVERY_TIMEOUT_MS);
  probe.stop();
  delay(50);  // Beri waktu socket ditutup sepenuhnya oleh TCP stack
  return ok;
}

// Bangun IP string dari 4 octet
String ipToString(int a, int b, int c, int d) {
  return String(a) + "." + String(b) + "." + String(c) + "." + String(d);
}

// Fungsi utama discovery: cari broker MQTT di jaringan saat ini.
// Dipanggil saat probe ke MQTT_SERVER gagal (broker tidak reachable).
// Mengembalikan IP broker jika ditemukan, atau "" jika tidak.
String discoverBroker() {
  Serial.println("=== Broker Auto-Discovery ===");

  // Ambil IP ESP32 dan hitung subnet prefix
  IPAddress myIP = WiFi.localIP();
  int octet0 = myIP[0];
  int octet1 = myIP[1];
  int octet2 = myIP[2];
  Serial.print("My IP: ");
  Serial.println(myIP);

  // ── Langkah 1: Coba gateway IP ──
  IPAddress gw = WiFi.gatewayIP();
  String gwStr = ipToString(gw[0], gw[1], gw[2], gw[3]);
  Serial.print("Probing gateway ");
  Serial.print(gwStr);
  Serial.print(":1883... ");
  if (probeBroker(gwStr.c_str(), MQTT_PORT)) {
    Serial.println("FOUND!");
    return gwStr;
  }
  Serial.println("no");
  esp_task_wdt_reset();  // Reset watchdog sebelum scan panjang

  // ── Langkah 2: Scan subnet dengan step ──
  // Scan dari x.x.x.1 sampai x.x.x.254, lompat setiap DISCOVERY_SUBNET_STEP
  Serial.println("Scanning subnet...");
  for (int d = 1; d <= 254; d += DISCOVERY_SUBNET_STEP) {
    // Skip IP sendiri
    if (d == (int)myIP[3]) continue;

    String ip = ipToString(octet0, octet1, octet2, d);
    if (probeBroker(ip.c_str(), MQTT_PORT)) {
      Serial.print("FOUND broker at ");
      Serial.println(ip);
      return ip;
    }
    // Reset watchdog setiap beberapa probe agar tidak reboot
    if ((d % 20) == 0) esp_task_wdt_reset();
  }

  Serial.println("Broker NOT FOUND on this subnet");
  return "";
}

// Mendapatkan IP broker yang akan digunakan.
// Logika:
//   1. Jika discovery sudah pernah berhasil → gunakan cached IP
//   2. Jika belum → jalankan auto-discovery di subnet saat ini
String getBrokerIP() {
#ifdef MQTT_SERVER
  // Broker statis (disarankan): langsung gunakan IP dari secrets.h,
  // tanpa auto-discovery yang mempercayai host mana pun di subnet.
  if (String(MQTT_SERVER).length() > 0) {
    return String(MQTT_SERVER);
  }
#endif
  // Jika sudah pernah ditemukan, gunakan cache
  if (discoveredBroker.length() > 0) {
    return discoveredBroker;
  }

  // Langsung lakukan auto-discovery di subnet saat ini
  String found = discoverBroker();
  if (found.length() > 0) {
    discoveredBroker = found;
  }
  return found;
}

// ── Fungsi reconnect() — Koneksi ulang Wi-Fi + MQTT dengan Exponential Backoff ──
// Dipanggil setiap iterasi loop(). Jika MQTT sudah terhubung, langsung kembali.
// Jika terputus, mencoba reconnect dengan delay yang meningkat setiap gagal.
// Jika gagal 10x berturut-turut, ESP32 di-reboot untuk memulai dari kondisi bersih.
void reconnect() {
  if (mqttClient.connected()) return; // Sudah terhubung — tidak perlu melakukan apa-apa

  unsigned long now = millis();
  // Cek apakah sudah cukup waktu sejak percobaan terakhir (respect backoff delay)
  if (now - lastReconnectAttempt < (unsigned long)reconnectDelayMs) return;
  lastReconnectAttempt = now; // Catat waktu percobaan ini

  // ── Cek koneksi WiFi terlebih dahulu ──
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("WiFi lost, reconnecting...");
    discoveredBroker = "";  // Reset cache discovery karena ganti jaringan
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD); // Coba koneksi ulang WiFi
    return;                               // Tunggu iterasi berikutnya untuk MQTT
  }

  // ── Auto-discovery: cari broker di subnet saat ini ──
  String brokerIP = getBrokerIP();
  if (brokerIP.length() == 0) {
    mqttFailCount++;
    Serial.print("No broker found (");
    Serial.print(mqttFailCount);
    Serial.print("/");
    Serial.print(MQTT_MAX_FAILS);
    Serial.println(")");
    if (mqttFailCount >= MQTT_MAX_FAILS) {
      Serial.println("!!! MQTT failed 10 times — resetting ESP32 !!!");
      Serial.flush();
      ESP.restart();
    }
    reconnectDelayMs = 2000;
    return;
  }

  // ── Set broker MQTT ──
  mqttClient.setServer(brokerIP.c_str(), MQTT_PORT);

  // ── Koneksi MQTT ──
  Serial.print("Connecting MQTT...");
  // Parameter connect():
  //   "esp32_front_door"              — Client ID unik di broker
  //   MQTT_USER, MQTT_PASS            — Kredensial dari secrets.h
  //   "door/status"                   — Topik LWT (Last Will and Testament)
  //   0                               — QoS 0 untuk LWT
  //   true                            — retained = true (pesan LWT disimpan broker)
  //   "{\"status\":\"offline\"}"      — Pesan LWT: otomatis dikirim jika ESP32 disconnect
  if (mqttClient.connect("esp32_front_door", MQTT_USER, MQTT_PASS,
                         "door/status", 0, true,
                         "{\"status\":\"offline\"}")) {
    Serial.println(" connected");
    mqttFailCount = 0;                                    // Reset penghitung kegagalan
    mqttClient.subscribe("door/command", 1);              // Subscribe ke topik perintah (QoS 1)
    mqttClient.publish("door/status", "{\"status\":\"online\"}", true); // Publish status online (retained)
    reconnectDelayMs = 1000;                              // Reset delay backoff ke 1 detik
  } else {
    mqttFailCount++;                                      // Tambah penghitung kegagalan
    Serial.print(" failed (");
    Serial.print(mqttFailCount);
    Serial.print("/");
    Serial.print(MQTT_MAX_FAILS);
    Serial.print("), rc=");
    Serial.print(mqttClient.state()); // Cetak kode error MQTT (reason code)

    if (mqttFailCount >= MQTT_MAX_FAILS) { // Sudah gagal 10x?
      Serial.println();
      Serial.println("!!! MQTT failed 10 times — resetting ESP32 !!!");
      Serial.flush();
      ESP.restart();                       // Reboot ESP32
    }

    Serial.println(" — retrying");
    reconnectDelayMs = min(reconnectDelayMs * 2, 5000); // Exponential backoff: 1s→2s→4s→5s (maks)
  }
}

// ── Fungsi callback() — Handler Pesan MQTT Masuk ──────────────────────
// Dipanggil secara otomatis oleh PubSubClient ketika ada pesan baru
// di topik yang sudah di-subscribe ("door/command").
// Mem-parsing JSON secara sederhana (tanpa library JSON) untuk mencari
// field "status" dan memanggil grantAccess() atau denyAccess().
void callback(char *topic, byte *payload, unsigned int length) {
  char msg[128]; // Buffer lokal untuk pesan (maks 127 byte + null terminator)
  unsigned int len = min(length, (unsigned int)(sizeof(msg) - 1)); // Batasi panjang agar tidak overflow
  memcpy(msg, payload, len); // Salin payload MQTT ke buffer lokal
  msg[len] = '\0';           // Tambahkan null terminator untuk string C

  if (String(topic) == "door/command") { // Hanya proses pesan dari topik "door/command"
    int status = -1; // Nilai default: tidak diketahui (-1)

    // Parsing manual: cari substring "\"status\":" dalam payload JSON
    const char *key = "\"status\":";           // Kunci yang dicari
    const char *p = strstr(msg, key);           // Cari posisi key dalam string
    if (p) {                                    // Key ditemukan
      p += strlen(key);                        // Pindahkan pointer setelah key
      while (*p == ' ') p++;                   // Lewati spasi setelah titik dua
      if (*p >= '0' && *p <= '9') {            // Karakter berikutnya adalah angka?
        status = *p - '0';                     // Konversi karakter angka ke integer (0 atau 1)
      }
    }

    Serial.print("Command received: status=");
    Serial.println(status); // Cetak status yang diterima untuk debugging

    // Eksekusi aksi berdasarkan status
    if (status == 1) {        // status=1 → Akses diberikan
      grantAccess();
    } else if (status == 0) { // status=0 → Akses ditolak
      denyAccess();
    }
    // status=-1 atau nilai lain: abaikan (pesan tidak valid)
  }
}

// ── Fungsi publishScan() — Menyusun dan mem-publish payload scan ke MQTT ──
// Alur:
//   1. Cek koneksi MQTT (jika terputus, scan dibuang)
//   2. Hash UID mentah → SHA-256 hex
//   3. Generate nonce acak
//   4. Ambil timestamp dari NTP
//   5. Susun JSON payload
//   6. Publish ke topik "door/scan" dengan QoS 1 — jika gagal, coba sekali lagi
void publishScan(const String &rawUID) {
  if (!mqttClient.connected()) {      // Jika MQTT tidak terhubung
    Serial.println("Scan dropped — MQTT not connected"); // Scan dibuang
    return;                           // Jangan publish
  }

  String hash = hashUID(rawUID);      // Hash UID mentah → 64 karakter hex SHA-256
  String nonce = generateNonce();     // Generate nonce acak 8 karakter hex
  time_t now;
  time(&now);                         // Ambil waktu saat ini dari NTP (epoch Unix)
  unsigned long ts = (unsigned long)now; // Konversi ke unsigned long untuk JSON

  // Susun payload JSON menggunakan snprintf (aman dari buffer overflow)
  // Format: {"uid_hash":"<64 hex>","device_id":"esp32_front_door","nonce":"<8 hex>","timestamp":<epoch>}
  snprintf(scanBuffer, sizeof(scanBuffer),
    "{\"uid_hash\":\"%s\",\"device_id\":\"esp32_front_door\",\"nonce\":\"%s\",\"timestamp\":%lu}",
    hash.c_str(), nonce.c_str(), ts);

  Serial.print("Raw UID: ");
  Serial.println(rawUID);             // Cetak UID mentah (hanya untuk debug lokal)
  Serial.print("Publishing scan: ");
  Serial.println(scanBuffer);         // Cetak payload yang akan dikirim

  // Publish ke topik "door/scan" dengan QoS 1 (at-least-once) — sesuai spec:
  // packet yang hilang tidak boleh membuat scan tidak diproses diam-diam
  if (!mqttClient.publish("door/scan", scanBuffer, false, 1)) { // false = tidak retained, 1 = QoS 1
    Serial.println("Publish FAILED — retrying once..."); // Gagal publish
    delay(50);                        // Tunggu sebentar
    mqttClient.loop();                // Proses internal MQTT (mungkin perlu untuk recovery)
    if (!mqttClient.publish("door/scan", scanBuffer, false, 1)) { // Coba sekali lagi
      Serial.println("Publish FAILED again — scan lost"); // Gagal lagi — scan hilang
    }
  }
}
