// =============================================================================
// crypto.h — Fungsi Hash SHA-256 dan Nonce Generator
// =============================================================================
// Modul ini menangani keamanan kriptografi pada sisi firmware:
//   - hashUID():    Meng-hash UID mentah dengan SHA-256 sebelum dikirim ke jaringan.
//                    UID mentah TIDAK PERNAH dikirim — hanya hash-nya.
//   - generateNonce(): Menghasilkan nonce acak per scan untuk anti-replay.
//                       Backend menolak pesan dengan nonce yang sama dalam window waktu.
//
// Dependensi: mbedtls/sha256.h (bundled ESP-IDF), Arduino String
// Variabel global yang digunakan: (tidak ada — modul ini self-contained)
// =============================================================================

#pragma once
#include <mbedtls/sha256.h>

// ── Fungsi hashUID() — Hash UID mentah dengan SHA-256 ─────────────────
// UID mentah (plaintext) TIDAK PERNAH dikirim ke jaringan.
// Yang dikirim adalah hash SHA-256 dari UID. Ini melindungi privasi:
// jika pesan MQTT di-intercept, penyerang tidak bisa meng-clone kartu.
// Pepper (rahasia server-side) ditambahkan oleh backend saat membandingkan.
String hashUID(const String &rawUID) {
  uint8_t digest[32];              // Buffer untuk menyimpan hasil hash (SHA-256 = 32 byte)
  mbedtls_sha256_context ctx;      // Context untuk operasi hashing
  mbedtls_sha256_init(&ctx);       // Inisialisasi context
  mbedtls_sha256_starts(&ctx, 0);  // Mulai session SHA-256 (0 = SHA-256, bukan SHA-224)
  mbedtls_sha256_update(&ctx, (const unsigned char *)rawUID.c_str(), rawUID.length()); // Masukkan data UID
  mbedtls_sha256_finish(&ctx, digest); // Finalisasi — hasil disimpan di digest[]
  mbedtls_sha256_free(&ctx);       // Bersihkan context (free memory)

  // Konversi 32 byte binary digest → 64 karakter string hex lowercase
  String hex = "";
  for (int i = 0; i < 32; i++) {
    if (digest[i] < 16) hex += "0";  // Tambahkan leading zero untuk byte < 0x10
    hex += String(digest[i], HEX);    // Konversi byte ke string hex
  }
  hex.toLowerCase(); // Pastikan lowercase untuk konsistensi dengan backend
  return hex;
}

// ── Fungsi generateNonce() — Menghasilkan nonce acak 8 karakter hex ──
// Nonce (number used once) adalah nilai acak yang unik per scan.
// Tujuan: mencegah serangan replay — backend menolak pesan yang memiliki
// nonce yang sama dengan pesan sebelumnya dalam window waktu tertentu.
String generateNonce() {
  String nonce = "";
  for (int i = 0; i < 4; i++) {       // Generate 4 byte acak → 8 karakter hex
    byte b = random(256);              // Random byte antara 0-255 (PRNG ESP32)
    if (b < 16) nonce += "0";          // Leading zero untuk byte < 0x10
    nonce += String(b, HEX);           // Konversi byte ke string hex
  }
  nonce.toLowerCase();                 // Pastikan lowercase untuk konsistensi
  return nonce;
}
