// =============================================================================
// config.h — Konstanta dan Pin Definitions
// =============================================================================
// File ini berisi semua #define untuk pin GPIO, konfigurasi UHF reader,
// parameter debounce, timeout, dan batas reconnect MQTT.
// Tidak mengandung variabel atau fungsi — hanya konstanta preprocessor.
// =============================================================================

#pragma once

// ── Definisi Pin GPIO ──────────────────────────────────────────────────
// LED eksternal dan buzzer menggunakan logika active-LOW (HIGH = mati, LOW = nyala).
// LED built-in ESP32 menggunakan logika active-HIGH (HIGH = nyala, LOW = mati).
#define GREEN_LED   25  // LED hijau — indikator akses diberikan (active-LOW)
#define RED_LED     26  // LED merah — indikator akses ditolak (active-LOW)
#define MQTT_LED    5   // LED indikator status koneksi MQTT (active-LOW)
#define BUZZER      4   // Buzzer aktif KY-12 — bunyi saat akses grant/deny
#define BUILTIN_LED 2   // LED built-in ESP32 — indikator status Wi-Fi (active-HIGH)

// ── Konfigurasi UHF Reader (HW-VX6330K via MAX3232) — Active Mode ────
// Reader beroperasi dalam "Active Mode": secara otomatis memindai tag
// dan mengirimkan frame inventory (CMD=0xEE) tanpa perlu diperintah.
//
// Format frame Active Mode yang diamati:
//   07 00 EE 00 E2 00 40 D4
//   │  │  │  │  ├────────┤
//   │  │  │  │  EPC (UID sebenarnya, 4 byte untuk tag pendek)
//   │  │  │  status (0x00 = OK, pembacaan berhasil)
//   │  │  command (0xEE = notifikasi inventory otomatis)
//   │  address (alamat reader, biasanya 0x00)
//   length (jumlah byte setelah field ini)
#define UHF_BAUD      57600   // Baud rate default komunikasi UART dengan reader UHF
#define UHF_RX        16      // GPIO 16 — pin RX UART2 (menerima data dari reader)
#define UHF_TX        17      // GPIO 17 — pin TX UART2 (mengirim data ke reader, tidak digunakan di Active Mode)
#define READ_TIMEOUT  100     // Timeout dalam ms saat menunggu setiap byte dari UART
#define MAX_RESPONSE  128     // Ukuran maksimum buffer untuk menyimpan satu frame respons
#define HEADER_LEN    4       // Panjang header frame: [LEN][ADDR][CMD][STATUS]
#define CMD_INVENTORY 0xEE    // Kode perintah inventory otomatis dari reader
#define STATUS_OK     0x00    // Status byte yang menandakan pembacaan berhasil

// ── Reader Health Check ──────────────────────────────────────────────
// Di Active Mode reader diam saat tidak ada tag di jangkauan, jadi
// timeout 30 detik mendeteksi kegagalan reader yang sebenarnya.
#define READER_TIMEOUT_MS  30000   // Timeout 30 detik — jika tidak ada data, reader dianggap offline

// ── Scan Debounce ────────────────────────────────────────────────────
// Cooldown mencegah publish duplikat dari reader UHF yang mengirim
// frame berulang saat tag masih di dekatnya.
#define SCAN_COOLDOWN_MS 2000      // Cooldown 2 detik antar scan untuk UID yang sama

// ── MQTT Reconnect ──────────────────────────────────────────────────
// Jika gagal 10x berturut-turut, ESP32 di-reboot untuk recovery total.
#define MQTT_MAX_FAILS 10
