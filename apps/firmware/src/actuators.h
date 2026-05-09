// =============================================================================
// actuators.h — Kontrol Aktuator (LED dan Buzzer)
// =============================================================================
// Modul ini menangani semua output fisik firmware:
//   - grantAccess():     LED hijau ON + buzzer 1x beep 200ms → LED tetap nyala 1 detik
//   - denyAccess():      LED merah ON + buzzer 3x beep → LED tetap nyala 3 detik
//   - updateStatusLEDs(): Memperbarui LED indikator Wi-Fi dan MQTT secara real-time
//
// Dependensi: config.h
// Variabel global yang digunakan:
//   - actuatorActive, actuatorEnd, actuatorNextState  (tulis — timing aktuator non-blocking)
//
// Catatan desain:
//   LED hijau/merah dimatikan secara non-blocking di loop() (bukan di sini).
//   Fungsi grantAccess/denyAccess hanya MENYALAKAN LED dan mengatur durasi.
//   Loop() yang mematikan LED ketika actuatorEnd tercapai.
// =============================================================================

#pragma once
#include "config.h"

// ── Fungsi grantAccess() — Urutan akses diberikan (non-blocking setelah buzzer) ──
// Menyalakan LED hijau + bunyi buzzer 1x pendek (200ms).
// LED tetap menyala selama 1 detik (dikontrol oleh timing di loop()).
void grantAccess() {
  Serial.println("ACCESS GRANTED");
  digitalWrite(RED_LED, HIGH);     // Matikan LED merah (active-LOW: HIGH = OFF)
  digitalWrite(GREEN_LED, LOW);    // Nyalakan LED hijau (active-LOW: LOW = ON)
  digitalWrite(BUZZER, HIGH);      // Nyalakan buzzer
  delay(200);                      // Bunyi buzzer 200ms — blocking singkat yang dapat diterima
  digitalWrite(BUZZER, LOW);       // Matikan buzzer
  actuatorEnd = millis() + 1000;   // LED hijau akan dimatikan 1 detik dari sekarang
  actuatorActive = true;           // Tandai aktuator sedang aktif
  actuatorNextState = 1;           // State = 1 (grant sedang berjalan)
}

// ── Fungsi denyAccess() — Urutan akses ditolak (3x beep) ──────────────
// Menyalakan LED merah + bunyi buzzer 3x (150ms on, 100ms off).
// LED tetap menyala selama 3 detik. Total blocking: ~650ms.
void denyAccess() {
  Serial.println("ACCESS DENIED");
  digitalWrite(GREEN_LED, HIGH);    // Matikan LED hijau (active-LOW: HIGH = OFF)
  digitalWrite(RED_LED, LOW);       // Nyalakan LED merah (active-LOW: LOW = ON)
  // Bunyikan buzzer 3 kali dengan delay singkat (total ~650ms blocking)
  for (int i = 0; i < 3; i++) {
    digitalWrite(BUZZER, HIGH);     // Nyalakan buzzer
    delay(150);                     // Bunyi selama 150ms
    digitalWrite(BUZZER, LOW);      // Matikan buzzer
    if (i < 2) delay(100);          // Jeda 100ms antar beep (tidak perlu setelah beep terakhir)
  }
  actuatorEnd = millis() + 3000;    // LED merah akan dimatikan 3 detik dari sekarang
  actuatorActive = true;            // Tandai aktuator sedang aktif
  actuatorNextState = 0;            // State = 0 (deny sedang berjalan)
}

// ── Fungsi updateStatusLEDs() — Memperbarui LED indikator sistem ──────
// Dua LED indikator yang selalu menampilkan status real-time:
//   - LED biru (BUILTIN_LED, GPIO 2) = status WiFi — active-HIGH
//     NYALA = WiFi terhubung, MATI = WiFi terputus
//   - LED terpisah (MQTT_LED, GPIO 5) = status MQTT — active-LOW
//     NYALA = MQTT terhubung, MATI = MQTT terputus
void updateStatusLEDs() {
  // LED biru — status WiFi (active-HIGH: HIGH = nyala)
  digitalWrite(BUILTIN_LED, WiFi.status() == WL_CONNECTED ? HIGH : LOW);

  // LED terpisah — status MQTT (active-LOW: LOW = nyala)
  digitalWrite(MQTT_LED, mqttClient.connected() ? LOW : HIGH);
}
