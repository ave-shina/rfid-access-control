// =============================================================================
// RFID Access Control — ESP32 Firmware (Main Entry Point)
// =============================================================================
// Firmware ini menjalankan sistem kontrol akses RFID berbasis ESP32 yang:
//   1. Membaca tag UHF RFID dari reader HW-VX6330K via UART
//   2. Meng-hash UID tag dengan SHA-256 (UID mentah tidak pernah dikirim)
//   3. Mempublikasikan hash ke topik MQTT "door/scan" untuk divalidasi server
//   4. Menerima perintah akses (grant/deny) dari topik MQTT "door/command"
//   5. Menyalakan LED hijau + buzzer 1x untuk akses diterima
//      Menyalakan LED merah + buzzer 3x untuk akses ditolak
//
// Struktur file:
//   main.cpp        — File ini. Variabel global, setup(), loop()
//   config.h        — Semua konstanta dan definisi pin (#define only)
//   crypto.h        — hashUID(), generateNonce()
//   uhf_reader.h    — readExact(), autoDetectBaud(), readUHFTag()
//   mqtt_handler.h  — reconnect(), callback(), publishScan()
//   actuators.h     — grantAccess(), denyAccess(), updateStatusLEDs()
//   secrets.h       — Kredensial WiFi dan MQTT (gitignored)
// =============================================================================

#include <Arduino.h>          // Framework Arduino — menyediakan setup(), loop(), pinMode, dll
#include <WiFi.h>             // Library Wi-Fi ESP32 — untuk koneksi ke jaringan nirkabel
#include <PubSubClient.h>     // Library MQTT client — untuk komunikasi publish/subscribe dengan broker
#include "esp_task_wdt.h"     // Watchdog Timer — reboot otomatis jika firmware hang
#include "secrets.h"          // File header terpisah (gitignored) berisi SSID, password, kredensial MQTT
#include "config.h"           // Semua konstanta dan definisi pin

// =============================================================================
// Variabel Global
// =============================================================================
// Didefinisikan di sini SEBELUM include modul, agar semua fungsi di modul
// bisa mengakses variabel-variabel ini secara langsung.
// =============================================================================

// ── Objek MQTT ──────────────────────────────────────────────────────
WiFiClient espClient;               // Klien TCP yang mendasari koneksi MQTT
PubSubClient mqttClient(espClient); // Klien MQTT yang menggunakan koneksi TCP di atas

// ── Debounce Scan (Pencegahan Pembacaan Ganda) ────────────────────────
// Tanpa cooldown, satu kali tap kartu bisa memicu banyak publish duplikat
// karena reader UHF mengirim frame berulang-ulang saat tag masih di dekatnya.
String lastUID          = "";       // UID terakhir yang berhasil dibaca
unsigned long lastScanTime = 0;     // Waktu millis() saat scan terakhir

// ── Timing Aktuator Non-Blocking ────────────────────────────────────
// Aktuator (LED/buzzer) dinyalakan lalu dimatikan setelah durasi tertentu
// tanpa menggunakan delay() yang memblokir seluruh loop.
unsigned long actuatorEnd = 0;   // Waktu millis() saat aksi aktuator saat ini berakhir
bool actuatorActive = false;    // true saat urutan LED/buzzer sedang berjalan
int actuatorNextState = -1;     // -1 = idle, 0 = deny sedang berjalan, 1 = grant sedang berjalan

// ── Health Check Reader ──────────────────────────────────────────────
bool readerOnline             = false;   // Status online reader
unsigned long lastDataTime    = 0;       // Waktu millis() terakhir kali data diterima dari reader

// ── Konfigurasi UART ──────────────────────────────────────────────────
int uartBaud         = UHF_BAUD;   // Baud rate aktif — bisa di-override oleh auto-detect
constexpr bool uartInvert = false; // Sinyal UART tidak di-invert (normal)

// ── Buffer Payload JSON ──────────────────────────────────────────────
// Buffer statis untuk menyusun payload JSON sebelum publish ke MQTT.
// Menghindari alokasi dinamis (heap fragmentation) pada embedded system.
char scanBuffer[256];             // Buffer untuk menyimpan JSON payload "door/scan"

// ── Backoff Reconnect MQTT ───────────────────────────────────────────
// Saat koneksi MQTT gagal, delay antar percobaan reconnect dinaikkan
// secara eksponensial (1s → 2s → 4s → 5s max) untuk menghindari spam.
unsigned long lastReconnectAttempt = 0;  // Waktu millis() percobaan reconnect terakhir
int reconnectDelayMs = 1000;             // Delay awal 1 detik, akan dikalikan 2 setiap kegagalan
int mqttFailCount = 0;                   // Penghitung kegagalan berturut-turut

// =============================================================================
// Include Modul (setelah variabel global didefinisikan)
// =============================================================================
// Setiap modul adalah file header (.h) yang berisi implementasi fungsi.
// Karena file-file ini hanya di-include dari main.cpp (satu compilation unit),
// tidak ada risiko definisi ganda. Fungsi-fungsi di modul mengakses
// variabel global di atas secara langsung.
#include "crypto.h"           // hashUID(), generateNonce() — modul kriptografi
#include "uhf_reader.h"       // readExact(), autoDetectBaud(), readUHFTag() — modul pembacaan UHF
#include "actuators.h"        // grantAccess(), denyAccess(), updateStatusLEDs() — modul aktuator
#include "mqtt_handler.h"     // reconnect(), callback(), publishScan() — modul MQTT (butuh actuators.h)

// =============================================================================
// Fungsi setup() — Inisialisasi Perangkat
// =============================================================================
// Dijalankan sekali saat ESP32 boot. Menginisialisasi semua subsistem:
// Serial debug, UART reader, GPIO aktuator, Wi-Fi, NTP, dan MQTT.
// =============================================================================
void setup() {
  Serial.begin(115200);  // Mulai Serial USB untuk debug output (baud 115200)
  delay(1000);           // Tunggu 1 detik agar Serial Monitor sempat terhubung

  // Watchdog Timer — diinisialisasi PERTAMA sebelum proses yang memakan waktu
  // Timeout 30 detik (longgar) untuk mengakomodasi auto-detect baud + koneksi WiFi
  esp_task_wdt_init(30, true); // 30 detik timeout, panic=true → reboot otomatis jika hang
  esp_task_wdt_add(NULL);      // Daftarkan task utama (loop) ke watchdog

  // ── UART2: Auto-detect baud rate reader UHF ──
  uartBaud = autoDetectBaud();        // Coba deteksi baud rate reader secara otomatis
  if (uartBaud == 0) {                // Jika deteksi gagal (return 0)
    Serial.println("!!! FAILED to detect reader baud rate — using default 57600");
    uartBaud = 57600;                 // Gunakan baud rate default 57600 sebagai fallback
  }
  Serial2.begin(uartBaud, SERIAL_8N1, UHF_RX, UHF_TX, uartInvert); // Inisialisasi UART2 dengan baud yang terdeteksi
  delay(100);                        // Tunggu UART stabil
  while (Serial2.available()) Serial2.read(); // Buang data sampah di buffer
  Serial.print("UART2 locked: ");
  Serial.print(uartBaud);
  Serial.println(" baud");

  // ── Inisialisasi pin aktuator ──
  // LED eksternal active-LOW (HIGH=mati, LOW=nyala), LED built-in active-HIGH
  pinMode(GREEN_LED, OUTPUT);       // Set pin LED hijau sebagai output
  pinMode(RED_LED, OUTPUT);         // Set pin LED merah sebagai output
  pinMode(MQTT_LED, OUTPUT);        // Set pin LED indikator MQTT sebagai output
  pinMode(BUZZER, OUTPUT);          // Set pin buzzer sebagai output
  pinMode(BUILTIN_LED, OUTPUT);     // Set pin LED built-in sebagai output
  digitalWrite(GREEN_LED, HIGH);    // Matikan LED hijau (active-LOW: HIGH = OFF)
  digitalWrite(RED_LED, HIGH);      // Matikan LED merah (active-LOW: HIGH = OFF)
  digitalWrite(MQTT_LED, HIGH);     // Matikan LED MQTT (active-LOW: HIGH = OFF)
  digitalWrite(BUZZER, LOW);        // Matikan buzzer (active-HIGH: LOW = OFF)
  digitalWrite(BUILTIN_LED, LOW);   // Matikan LED built-in (active-HIGH: LOW = OFF)

  // ── Koneksi Wi-Fi ──
  WiFi.mode(WIFI_STA);              // Set mode Station (bukan Access Point)
  WiFi.setAutoConnect(false);       // Jangan auto-connect — kita kontrol manual
  WiFi.setAutoReconnect(true);      // Izinkan reconnect otomatis jika terputus
  delay(100);                       // Tunggu konfigurasi WiFi selesai

  // Cetak informasi debug Wi-Fi
  Serial.println();
  Serial.println("=== WiFi Debug ===");
  Serial.print("SSID:     "); Serial.println(WIFI_SSID);
  Serial.print("Password: "); Serial.println("********"); // Sensor password di log
  Serial.print("MAC:      "); Serial.println(WiFi.macAddress()); // Tampilkan MAC address ESP32

  // Scan jaringan WiFi di sekitar untuk memverifikasi SSID target ada
  Serial.println("Scanning nearby networks...");
  int netCount = WiFi.scanNetworks(); // Scan semua access point yang terdeteksi
  bool ssidFound = false;             // Flag: apakah SSID target ditemukan
  for (int i = 0; i < netCount; i++) {
    // Cetak info setiap jaringan: nomor, SSID, channel, RSSI, enkripsi
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
    if (WiFi.SSID(i) == WIFI_SSID) {  // Periksa apakah SSID target ada di hasil scan
      ssidFound = true;
    }
  }
  if (!ssidFound) {
    // SSID tidak ditemukan — kemungkinan router 5 GHz only (ESP32 hanya mendukung 2.4 GHz)
    Serial.print("!!! SSID '");
    Serial.print(WIFI_SSID);
    Serial.println("' NOT FOUND in scan!");
    Serial.println("!!! ESP32 only supports 2.4 GHz — check if your router uses 5 GHz only");
  } else {
    Serial.print("SSID '");
    Serial.print(WIFI_SSID);
    Serial.println("' found in scan");
  }
  WiFi.scanDelete(); // Hapus hasil scan dari memori

  // Mulai koneksi ke WiFi
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.print("Connecting to WiFi");
  int wifiAttempts = 0;                  // Penghitung percobaan koneksi
  wl_status_t lastStatus = WL_IDLE_STATUS; // Status WiFi sebelumnya (untuk deteksi perubahan)
  while (WiFi.status() != WL_CONNECTED && wifiAttempts < 40) { // Maks 40 percobaan (20 detik)
    delay(500);                          // Tunggu 500ms antar pengecekan
    wl_status_t curStatus = WiFi.status(); // Baca status WiFi saat ini
    if (curStatus != lastStatus) {       // Jika status berubah, cetak detail
      Serial.println();
      Serial.print("  status=");
      Serial.print(curStatus);
      Serial.print(" (");
      switch (curStatus) {
        case WL_IDLE_STATUS:     Serial.print("IDLE"); break;           // Belum mencoba connect
        case WL_NO_SSID_AVAIL:   Serial.print("NO_SSID_AVAIL"); break; // SSID tidak ditemukan
        case WL_SCAN_COMPLETED:  Serial.print("SCAN_COMPLETED"); break; // Scan selesai
        case WL_CONNECTED:       Serial.print("CONNECTED"); break;     // Terhubung!
        case WL_CONNECT_FAILED:  Serial.print("CONNECT_FAILED"); break; // Gagal autentikasi
        case WL_CONNECTION_LOST:  Serial.print("CONNECTION_LOST"); break; // Koneksi terputus
        case WL_DISCONNECTED:    Serial.print("DISCONNECTED"); break;  // Terputus
        default:                 Serial.print("UNKNOWN"); break;       // Status tidak dikenali
      }
      Serial.println(")");
      lastStatus = curStatus;            // Simpan status untuk perbandingan berikutnya
    } else {
      Serial.print(".");                 // Status belum berubah, cetak titik sebagai indikator progres
    }
    wifiAttempts++;                      // Tambah penghitung percobaan
    esp_task_wdt_reset();                // Reset watchdog agar tidak reboot selama proses koneksi
  }
  Serial.println();
  if (WiFi.status() == WL_CONNECTED) {   // Berhasil terhubung
    Serial.print("WiFi connected, IP: ");
    Serial.println(WiFi.localIP());      // Cetak alamat IP yang diperoleh
    Serial.print("Gateway: ");
    Serial.println(WiFi.gatewayIP());    // Cetak IP gateway/router
  } else {                               // Gagal terhubung setelah semua percobaan
    Serial.print("WiFi FAILED after ");
    Serial.print(wifiAttempts);
    Serial.print(" attempts, final status=");
    Serial.println(WiFi.status());
    Serial.println("!!! Check: wrong password, 5 GHz only, or MAC filter");
  }

  // ── Sinkronisasi waktu via NTP ──
  // Hanya dilakukan jika WiFi sudah terhubung. NTP diperlukan untuk
  // menghasilkan timestamp yang akurat pada payload scan MQTT.
  if (WiFi.status() == WL_CONNECTED) {
    configTime(0, 0, "pool.ntp.org", "time.nist.gov"); // Sinkron waktu dari server NTP (UTC, tanpa DST)
    Serial.println("NTP sync started (background)");    // NTP berjalan di background, non-blocking
    Serial.println("Waiting for TCP stack to stabilize...");
    delay(1500); // Tunggu stack TCP/IP stabil sebelum koneksi MQTT
  }

  // ── Konfigurasi MQTT ──
  mqttClient.setServer(MQTT_SERVER, 1883); // Set alamat broker MQTT dan port (1883 = non-TLS)
  mqttClient.setCallback(callback);         // Daftarkan fungsi callback untuk menangani pesan masuk

  // Inisialisasi timer untuk health check dan reconnect
  lastDataTime = millis();                  // Catat waktu sekarang sebagai referensi awal
  lastReconnectAttempt = millis();          // Grace period sebelum percobaan MQTT pertama
  Serial.println("RFID Access Control (UHF Active Mode) — Ready");
}

// =============================================================================
// Fungsi loop() — Loop Utama (dijalankan berulang tanpa henti)
// =============================================================================
// Urutan eksekusi setiap iterasi:
//   1. Reset watchdog
//   2. Reconnect MQTT jika terputus
//   3. Matikan aktuator jika waktunya habis
//   4. Baca tag RFID (jika tidak dalam cooldown/aktuator aktif)
//   5. Cek health reader
//   6. Update LED status
//   7. Proses pesan MQTT masuk
// =============================================================================
void loop() {
  esp_task_wdt_reset(); // Reset watchdog — HARUS dipanggil setiap iterasi agar tidak reboot
  reconnect();          // Pastikan MQTT terhubung, reconnect jika terputus (dengan backoff)

  // ── Aktuator non-blocking: matikan LED jika durasi sudah habis ──
  if (actuatorActive && millis() >= actuatorEnd) {
    digitalWrite(GREEN_LED, HIGH);  // Matikan LED hijau (active-LOW: HIGH = OFF)
    digitalWrite(RED_LED, HIGH);    // Matikan LED merah (active-LOW: HIGH = OFF)
    actuatorActive = false;         // Tandai aktuator sudah tidak aktif
    actuatorNextState = -1;         // Reset state ke idle
  }

  // ── Cek cooldown debounce ──
  unsigned long now = millis();
  bool inCooldown = (lastUID.length() > 0 && (now - lastScanTime) < SCAN_COOLDOWN_MS);
  // inCooldown = true jika UID yang sama di-scan lagi dalam waktu < 2 detik

  // Buang data UART saat cooldown atau aktuator aktif untuk mencegah buffer penuh
  if (inCooldown || actuatorActive) {
    while (Serial2.available()) Serial2.read(); // Buang semua byte di buffer UART
  }

  // ── Baca tag RFID hanya jika tidak dalam cooldown dan aktuator tidak aktif ──
  if (!actuatorActive && !inCooldown) {
    String uid = readUHFTag(); // Coba baca frame inventory dari reader UHF

    if (uid.length() > 0) {             // Tag terdeteksi
      lastDataTime = millis();          // Update timestamp data terakhir
      if (!readerOnline) {              // Jika reader sebelumnya offline, sekarang online
        readerOnline = true;
        Serial.println("HW-VX6330K: ONLINE (tag data received)");
      }

      lastUID = uid;                    // Simpan UID untuk debounce
      lastScanTime = millis();          // Catat waktu scan ini

      publishScan(uid);                 // Hash UID, susun JSON, publish ke MQTT "door/scan"
    }
  }

  // ── Health check reader: tidak ada data selama > 30 detik → offline ──
  if (readerOnline && millis() - lastDataTime > READER_TIMEOUT_MS) {
    readerOnline = false;               // Tandai reader offline
    Serial.println("HW-VX6330K: OFFLINE (no data for 5s)");
  }

  // ── Update LED indikator status ──
  updateStatusLEDs(); // LED biru = Wi-Fi, LED terpisah = MQTT

  // ── Proses pesan MQTT masuk ──
  // HARUS dipanggil setiap iterasi — menangani keepalive, pesan masuk, dll.
  mqttClient.loop();
}
