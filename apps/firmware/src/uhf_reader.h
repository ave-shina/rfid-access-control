// =============================================================================
// uhf_reader.h — Fungsi Pembacaan UHF RFID Reader (HW-VX6330K)
// =============================================================================
// Modul ini menangani semua komunikasi dengan reader UHF via UART2 (RS485):
//   - rs485ReceiveMode():  Set MAX485 ke mode receive (default)
//   - rs485TransmitMode(): Set MAX485 ke mode transmit
//   - readExact():         Utilitas untuk membaca tepat N byte dari UART dengan timeout
//   - autoDetectBaud():    Mencoba baud rate populer sampai menemukan yang menghasilkan
//                          frame valid dari reader (dijalankan sekali saat boot)
//   - readUHFTag():        Membaca satu frame inventory pasif dari reader → EPC hex
//
// Dependensi: Arduino HardwareSerial (Serial2), config.h, esp_task_wdt.h
// Variabel global yang digunakan:
//   - uartBaud, uartInvert        (baca/tulis — auto-detect)
//   - readerOnline, lastDataTime  (tulis — update health check)
// =============================================================================

#pragma once
#include "config.h"

// ── RS485 Direction Control (MAX485) ──────────────────────────────────
// MAX485 adalah half-duplex — DE dan RE di-tie ke GPIO yang sama.
// HIGH = transmit mode, LOW = receive mode (default).
void rs485ReceiveMode()  { digitalWrite(RS485_DE, LOW);  } // MAX485 menerima data dari reader
void rs485TransmitMode() { digitalWrite(RS485_DE, HIGH); } // MAX485 mengirim data ke reader

// ── Fungsi readExact() — Membaca tepat N byte dari stream dengan timeout ──
// Utilitas untuk membaca sejumlah byte tertentu dari UART.
// Mengembalikan true jika semua byte berhasil dibaca, false jika timeout.
// Setiap byte yang diterima me-reset timer timeout.
bool readExact(Stream &s, uint8_t *buf, size_t len, unsigned long timeoutMs) {
  unsigned long start = millis(); // Catat waktu mulai
  size_t i = 0;                  // Index byte yang sudah terbaca
  while (i < len) {              // Terus baca sampai semua byte terkumpul
    if (s.available()) {         // Ada byte tersedia di buffer UART?
      buf[i++] = (uint8_t)s.read(); // Baca 1 byte dan simpan ke buffer
      start = millis();             // Reset timer timeout setelah byte diterima
    } else if (millis() - start > timeoutMs) { // Tidak ada byte selama timeoutMs
      return false;             // Timeout — tidak semua byte bisa dibaca
    }
  }
  return true; // Semua len byte berhasil dibaca
}

// ── Auto-Deteksi Baud Rate UART ────────────────────────────────────────
// Mencoba baud rate yang umum (9600 s/d 115200) satu per satu.
// Mengirim perintah inventory Answer Mode ke reader, lalu memeriksa
// apakah responsnya valid. Letakkan tag di dekat reader saat boot
// untuk hasil terbaik.
int autoDetectBaud() {
  // Daftar baud rate yang akan dicoba secara berurutan
  static const int rates[] = {9600, 19200, 38400, 57600, 115200};
  static const int numRates = 5;
  // Perintah inventory Answer Mode: [LEN=0x04][ADDR=0xFF][CMD=0x01][CHK_L=0x1B][CHK_H=0xB4]
  static const uint8_t invCmd[] = {0x04, 0xFF, 0x01, 0x1B, 0xB4};

  Serial.println();
  Serial.println("=== UART Baud Auto-Detect ===");
  Serial.println("Hold a tag near the reader for best detection...");
  delay(2000); // Tunggu 2 detik agar reader siap setelah power-on

  // Iterasi setiap baud rate
  for (int r = 0; r < numRates; r++) {
    esp_task_wdt_reset();                    // Reset watchdog agar tidak reboot selama proses deteksi
    Serial2.end();                           // Tutup koneksi UART2 sebelumnya
    Serial2.begin(rates[r], SERIAL_8N1, UHF_RX, UHF_TX, uartInvert); // Buka UART2 dengan baud rate baru
    delay(100);                              // Tunggu stabilisasi UART
    while (Serial2.available()) Serial2.read(); // Buang data sisa di buffer

    Serial.print("Trying ");
    Serial.print(rates[r]);
    Serial.print(" baud... ");

    // Kirim perintah inventory ke reader
    rs485TransmitMode();                           // MAX485 → transmit mode
    Serial2.write(invCmd, sizeof(invCmd));
    Serial2.flush();                               // Pastikan semua byte terkirim
    rs485ReceiveMode();                            // MAX485 → receive mode (kembali mendengarkan)

    // Tunggu respons dari reader
    delay(200);

    if (!Serial2.available()) {
      // Tidak ada respons langsung — coba dengarkan pasif untuk data Active Mode
      esp_task_wdt_reset();
      delay(800); // Tunggu lebih lama karena Active Mode mungkin tidak segera mengirim
      if (!Serial2.available()) {
        Serial.println("no data");           // Tidak ada data sama sekali di baud rate ini
        while (Serial2.available()) Serial2.read(); // Bersihkan buffer
        continue;                            // Lanjut ke baud rate berikutnya
      }
    }

    // Ada data masuk — coba baca seluruh frame
    uint8_t buf[MAX_RESPONSE];
    size_t totalRead = 0;

    // Baca semua byte yang tersedia dengan timeout 300ms antar byte
    unsigned long start = millis();
    while (totalRead < MAX_RESPONSE && millis() - start < 300) {
      if (Serial2.available()) {
        buf[totalRead++] = (uint8_t)Serial2.read(); // Simpan byte ke buffer
        start = millis();                            // Reset timeout setelah setiap byte diterima
      }
    }

    if (totalRead == 0) {
      Serial.println("no frame"); // Tidak ada byte yang terbaca
      continue;
    }

    // Dump byte mentah ke Serial untuk debugging (maks 20 byte ditampilkan)
    Serial.print("got ");
    Serial.print(totalRead);
    Serial.print(" bytes: ");
    for (size_t i = 0; i < totalRead && i < 20; i++) {
      if (buf[i] < 0x10) Serial.print("0"); // Tambahkan leading zero untuk byte < 0x10
      Serial.print(buf[i], HEX);
      Serial.print(" ");
    }
    if (totalRead > 20) Serial.print("..."); // Indikasi ada byte lagi yang tidak ditampilkan
    Serial.println();

    // Periksa apakah data cocok dengan pola frame yang valid:
    // Active Mode: [LEN][ADR][0xEE][0x00]...
    // Answer Mode: [LEN][ADR][0x01][STATUS]...
    if (totalRead >= 4) {                     // Minimal 4 byte untuk header valid
      uint8_t len = buf[0];                  // Byte pertama = panjang data setelah byte LEN
      uint8_t cmd = buf[2];                  // Byte ketiga = kode perintah

      // Periksa apakah LEN konsisten dengan jumlah byte yang diterima
      // Frame total = 1 byte LEN + len byte data
      if (len > 0 && (size_t)(1 + len) == totalRead) {
        if (cmd == 0xEE || cmd == 0x01) {    // 0xEE = Active Mode inventory, 0x01 = Answer Mode inventory
          Serial.print("  >>> VALID frame! CMD=0x");
          Serial.println(cmd, HEX);
          Serial.print("  >>> Using baud rate: ");
          Serial.println(rates[r]);
          while (Serial2.available()) Serial2.read(); // Bersihkan sisa buffer
          return rates[r];                             // Kembalikan baud rate yang berhasil
        }
      }

      // Jika LEN tidak cocok persis, tetap coba cari byte 0xEE di data
      // (kadang reader mengirim frame dengan format sedikit berbeda)
      for (size_t i = 2; i + 1 < totalRead; i++) {
        if (buf[i] == 0xEE && buf[i + 1] == 0x00) { // 0xEE diikuti status 0x00
          Serial.println("  >>> Found 0xEE in data stream");
          Serial.print("  >>> Using baud rate: ");
          Serial.println(rates[r]);
          while (Serial2.available()) Serial2.read();
          return rates[r]; // Kembalikan baud rate yang berhasil
        }
      }
    }

    Serial.println("  not a valid frame"); // Frame tidak valid, coba baud rate berikutnya
    while (Serial2.available()) Serial2.read(); // Bersihkan buffer sebelum lanjut
  }

  // Semua baud rate sudah dicoba — tidak ada yang menghasilkan frame valid
  Serial.println();
  Serial.println("No valid frame found at any baud rate.");
  Serial.println("Check: reader power, MAX485 wiring, A/B polarity, TX/RX swap");
  return 0; // Kembalikan 0 menandakan gagal deteksi
}

// ── Fungsi readUHFTag() — Membaca frame inventory UHF secara pasif → EPC hex ──
// Membaca satu frame auto-inventory dari reader HW-VX6330K.
// Frame format: [LEN][ADR][CMD=0xEE][STATUS=0x00][EPC bytes...]
// Mengembalikan string EPC dalam format hex uppercase, atau "" jika gagal.
String readUHFTag() {
  // Fast path — jika tidak ada data di UART, langsung kembali
  if (!Serial2.available()) return "";

  uint8_t resp[MAX_RESPONSE];   // Buffer untuk menyimpan frame lengkap
  size_t respLen = 0;            // Panjang frame yang terbaca

  // Langkah 1: Baca byte pertama (LEN — panjang data setelah byte ini)
  if (!readExact(Serial2, &resp[0], 1, READ_TIMEOUT)) {
    return "";  // Timeout menunggu byte pertama
  }

  const uint8_t dataLen = resp[0]; // LEN = jumlah byte setelah byte ini
  if (dataLen == 0 || (size_t)(dataLen + 1) > MAX_RESPONSE) {
    // LEN = 0 (tidak valid) atau frame terlalu besar untuk buffer
    while (Serial2.available()) Serial2.read(); // Buang data yang tersisa
    return "";
  }
  respLen = 1 + dataLen; // Total frame = 1 byte LEN + dataLen byte data

  // Langkah 2: Baca sisa dataLen byte dari UART
  if (!readExact(Serial2, &resp[1], dataLen, READ_TIMEOUT)) {
    Serial.println("UHF: timeout reading frame data");
    return ""; // Timeout sebelum semua byte terbaca
  }

  // Update timestamp data terakhir (setiap frame = reader hidup)
  lastDataTime = millis();
  if (!readerOnline) { // Jika reader baru saja kembali online
    readerOnline = true;
    Serial.println("HW-VX6330K: ONLINE (frame received)");
  }

  // Dump frame mentah ke Serial untuk debugging
  Serial.print("UHF FRAME (");
  Serial.print(respLen);
  Serial.print(" bytes): ");
  for (size_t i = 0; i < respLen; i++) {
    if (resp[i] < 0x10) Serial.print("0"); // Leading zero untuk byte < 0x10
    Serial.print(resp[i], HEX);
    Serial.print(" ");
  }
  Serial.println();

  // Langkah 3: Pemeriksaan struktural — frame harus minimal sepanjang header
  if (respLen < HEADER_LEN) { // HEADER_LEN = 4 byte
    Serial.println("UHF: frame too short"); // Frame terlalu pendek, tidak punya header lengkap
    return "";
  }

  // Langkah 4: Validasi header — CMD harus 0xEE, STATUS harus 0x00
  const uint8_t command = resp[2]; // Byte ketiga = kode perintah
  const uint8_t status  = resp[3]; // Byte keempat = status

  if (command != CMD_INVENTORY) { // Apakah ini frame inventory (0xEE)?
    Serial.print("UHF: CMD=0x");
    Serial.print(command, HEX);
    Serial.println(" (expected 0xEE)"); // Bukan frame inventory — abaikan
    return "";
  }

  if (status != STATUS_OK) { // Apakah status OK (0x00)?
    Serial.print("UHF: STATUS=0x");
    Serial.println(status, HEX); // Status error — abaikan
    return "";
  }

  // Langkah 5: Ekstrak EPC — semua byte setelah header
  // Frame: [LEN][ADR][CMD][STATUS][EPC...]
  // Panjang EPC = total frame - 4 byte header
  const int epcLen = respLen - HEADER_LEN;
  if (epcLen <= 0) {
    Serial.println("UHF: no EPC data"); // Tidak ada data EPC dalam frame
    return "";
  }

  const uint8_t *epc = &resp[HEADER_LEN]; // Pointer ke byte EPC pertama
  String uid = "";
  for (int i = 0; i < epcLen; i++) {
    if (epc[i] < 0x10) uid += "0";    // Tambahkan leading zero untuk byte < 0x10
    uid += String(epc[i], HEX);        // Konversi byte ke string hex
  }
  uid.toUpperCase();                    // Konversi ke huruf kapital untuk konsistensi
  Serial.print("UHF EPC: ");
  Serial.println(uid);
  return uid;                           // Kembalikan EPC sebagai string hex uppercase
}
