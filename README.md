# Sistem Kontrol Akses RFID

> **Bahasa:** [English](./README.en.md) | Bahasa Indonesia

> Sistem **Kontrol Akses RFID** berskala enterprise dan event-driven yang dibangun di atas prinsip Pervasive Computing (IoT). ESP32 di sisi edge bertindak sebagai "sensor bodoh" yang hanya membaca tag UHF; seluruh logika bisnis ada di backend Go; dashboard Next.js menyediakan monitoring real-time dan kontrol jarak jauh.

Dunia fisik (tap kartu, pintu, LED) dilepaskan (decouple) dari proses validasi kredensial melalui **protokol MQTT pub/sub** — sehingga perangkat edge tetap sederhana, aman, dan mudah diganti.

---

## ✨ Sorotan Fitur

- **UHF, bukan legacy** — Membaca tag UHF via reader **HW-VX6330K** melalui **RS485 / MAX485** (jangkauan jauh, cepat, kelas industri).
- **Privacy by design** — UID mentah **tidak pernah meninggalkan ESP32**. Hanya hash SHA-256 yang ditransmisikan; database menyimpan hash yang sudah diberi *pepper*, sehingga kebocoran DB tidak bisa diklon ke kartu kosong.
- **Fail-secure** — Tanpa respons dari server = pintu tetap terkunci. ESP32 tidak pernah mengotorisasi sendiri.
- **Anti replay** — `nonce` per scan + window timestamp 30 detik divalidasi pada setiap pesan.
- **Dashboard real-time** — Log akses live via WebSocket, override pintu manual, dan manajemen user.
- **Pola production-ready** — Rate limiting per perangkat, connection pooling DB, watchdog timer, deteksi offline via LWT, dan logging JSON terstruktur.
- **Auto-detect baud rate** — ESP32 mencoba baud rate umum saat boot sehingga penggantian reader bersifat plug-and-play.

---

## 🏗 Arsitektur

### Model Pervasive Computing 5-Lapis

| Lapisan | Peran | Komponen |
|---|---|---|
| **Perception** | Sensor & aktuasi | ESP32, reader UHF HW-VX6330K (MAX485/RS485), LED, buzzer KY-12 |
| **Network** | Transport data | Wi-Fi (802.11), MQTT over TCP |
| **Middleware** | Routing & pemrosesan | Eclipse Mosquitto, backend Go |
| **Data** | Penyimpanan persisten | PostgreSQL 15 |
| **Application** | Antarmuka pengguna | Dashboard Next.js 14 |

### Alur Data

```
 ┌──────────┐   RS485    ┌─────────┐  MQTT door/scan   ┌───────────┐
 │ UHF Tag  │───────────▶│  ESP32  │──────────────────▶│ Mosquitto │
 └──────────┘  /MAX485   │ (edge)  │  (hash SHA-256)   │  broker   │
                          └────┬────┘                   └─────┬─────┘
        ┌──────────────────────│                              │
        │  MQTT door/command   │                              │
        │   {status:1|0}       │                              ▼
        │  ┌───────────────────┘                     ┌─────────────────┐
        │  ▼                                         │   Backend Go    │
 LED hijau/merah + buzzer                            │  rate-limit +   │
 (akses diberikan / ditolak)                         │  anti-replay +  │
                                                     │  validasi DB    │
                                                     └────────┬────────┘
                                          PostgreSQL │  (users / access_logs)
                                                     │        │ WebSocket
                                                     ▼        ▼
                                              ┌─────────────────────┐
                                              │  Dashboard Next.js │
                                              │  (log live + admin)│
                                              └─────────────────────┘
```

---

## 🧰 Tech Stack

| Lapisan | Teknologi |
|---|---|
| Firmware edge | C++ (Arduino), PlatformIO — ESP32 (`lolin32`) |
| Reader | HW-VX6330K UHF via RS485 (transceiver MAX485) |
| Backend | Go 1.25, `paho.mqtt.golang`, `gorilla/mux` + `websocket`, `database/sql`, `zerolog`, `golang.org/x/time` |
| Database | PostgreSQL 15 (Docker) |
| Broker MQTT | Eclipse Mosquitto 2 (Docker) |
| Frontend | Next.js 14, React 18, TypeScript, Tailwind CSS, shadcn/ui (Radix) |
| Infrastruktur | Docker & Docker Compose |

---

## 📂 Struktur Repository

```
.
├── apps/
│   ├── firmware/               # Perangkat edge ESP32 (Arduino / PlatformIO)
│   │   ├── platformio.ini
│   │   └── src/
│   │       ├── main.cpp            # Entry point — setup() + loop()
│   │       ├── config.h            # Pin GPIO, timeout, konstanta
│   │       ├── uhf_reader.h        # Parsing frame RS485 + HW-VX6330K
│   │       ├── crypto.h            # hashUID(), generateNonce()
│   │       ├── mqtt_handler.h      # connect / publish / callback
│   │       ├── actuators.h         # grantAccess() / denyAccess()
│   │       └── secrets.h.example   # salin → secrets.h (gitignored)
│   ├── backend/                # Go — handler MQTT, DB, WebSocket
│   │   ├── cmd/server/main.go      # Routing + wiring
│   │   └── internal/{db,mqtt,ws,models}/
│   └── frontend/               # Dashboard Next.js 14
│       └── src/{app,components,lib}/
├── infrastructure/
│   ├── docker-compose.yml          # Postgres + Mosquitto (dev)
│   ├── docker-compose.prod.yml     # Stack lengkap (backend + frontend)
│   ├── mosquitto.conf
│   ├── init.sql                    # Seed schema database
│   └── .env.example                # Template untuk semua secret
└── package.json                    # Orkestrator root (concurrently)
```

---

## 🚀 Cara Menjalankan

### Prasyarat

- **Docker Desktop**
- **Go 1.25+**
- **Node.js 20+**
- **PlatformIO** (ekstensi VS Code atau CLI) — untuk flash ESP32

### 1 · Konfigurasi secret

```bash
# Infrastruktur
cp infrastructure/.env.example infrastructure/.env
# Isi POSTGRES_PASSWORD, JWT_SECRET, UID_PEPPER, kredensial MQTT…

# Firmware
cp apps/firmware/src/secrets.h.example apps/firmware/src/secrets.h
# Isi WIFI_SSID, WIFI_PASSWORD, MQTT_SERVER, MQTT_USER, MQTT_PASS
```

### 2 · Jalankan infrastruktur (Postgres + Mosquitto)

Dari root repo — `package.json` root sudah otomatis menyertakan file env:

```bash
npm run infra:up      # = docker compose up -d
npm run infra:logs    # tail log (opsional)
```

Postgres di `:5434`, Mosquitto di `:1883` / `:9001`.

### 3 · Jalankan backend + frontend bersamaan

```bash
npm run dev           # menjalankan backend Go (air) + Next.js secara paralel
```

- Backend → `http://localhost:8080` (health check di `/healthz`)
- Dashboard → `http://localhost:3000`

Atau jalankan masing-masing secara terpisah:

```bash
cd apps/backend  && go run cmd/server/main.go
cd apps/frontend && npm install && npm run dev
```

### 4 · Flash ESP32

Buka `apps/firmware/` di VS Code dengan ekstensi **PlatformIO**, pastikan `secrets.h` sudah diisi, lalu **Upload and Monitor**. Dekatkan tag UHF ke reader — serial monitor akan mencetak baud rate yang terdeteksi otomatis dan setiap EPC yang berhasil diparsing.

> Firmware melakukan **auto-discovery broker**: memeriksa IP gateway, lalu memindai subnet lokal untuk port `1883`. Tidak perlu hardcode IP broker.

---

## 🔌 Topik MQTT

| Topik | Publisher → Subscriber | QoS | Tujuan |
|---|---|---|---|
| `door/scan` | ESP32 → Backend | **1** | Edge melaporkan UID hasil scan (sudah di-hash) |
| `door/command` | Backend/UI → ESP32 | **1** | Instruksi beri/tolak akses |
| `door/status` | ESP32 (+ LWT) → UI | 0 | Heartbeat / deteksi offline |

Payload `door/scan` — UID mentah tidak pernah ditransmisikan:

```json
{
  "uid_hash": "64-char-sha256-hex",
  "device_id": "esp32_front_door",
  "nonce": "a3f9c12b",
  "timestamp": 1718000000
}
```

Payload `door/command`:

```json
{ "status": 1, "message": "Access Granted", "action_by": "System" }
```

- `status: 1` → LED hijau + 1 beep
- `status: 0` → LED merah + 3 beep

---

## 🌐 API Backend

| Method | Endpoint | Keterangan |
|---|---|---|
| `GET` | `/healthz` | Cek hidup (memeriksa ping DB) |
| `GET` | `/api/ws` | WebSocket — stream real-time scan + status |
| `GET` | `/api/logs` | Daftar log akses (dengan filter) |
| `POST` | `/api/logs` | Buat entri log |
| `GET` | `/api/users` | Daftar user terdaftar |
| `POST` | `/api/users` | Daftarkan user baru (hash UID + pepper) |
| `PUT` | `/api/users/{id}` | Update user |
| `DELETE` | `/api/users/{id}` | Hapus user |
| `POST` | `/api/door/override` | Beri/tolak akses manual dari dashboard |

---

## 🗄 Skema Database

```sql
CREATE TABLE users (
  id            SERIAL PRIMARY KEY,
  name          VARCHAR(100) NOT NULL,
  rfid_uid_hash VARCHAR(64) UNIQUE NOT NULL,   -- sha256(rawUID + pepper)
  role          VARCHAR(50) DEFAULT 'employee',
  is_active     BOOLEAN DEFAULT TRUE,
  created_at    TIMESTAMP DEFAULT CURRENT_TIMESTAMP
);

CREATE TABLE access_logs (
  id            SERIAL PRIMARY KEY,
  rfid_uid_hash VARCHAR(64) NOT NULL,
  status        VARCHAR(20) NOT NULL,           -- AUTHORIZED | DENIED
  action_by     VARCHAR(100),
  device_id     VARCHAR(50),
  timestamp     TIMESTAMP DEFAULT CURRENT_TIMESTAMP
);
```

Di-index pada `(device_id, timestamp DESC)`, `(timestamp DESC)`, dan `(status)` — tabel `access_logs` bersifat write-heavy dan tumbuh tak terbatas; jadwalkan pembersihan berkala sesuai kebijakan retensi Anda.

---

## 🔐 Model Keamanan

- ✅ UID di-hash di perangkat (SHA-256) sebelum ditransmisikan — UID mentah tidak pernah ke jaringan
- ✅ DB menyimpan `sha256(rawUID + UID_PEPPER)` — kebocoran DB tidak bisa dibalik menjadi kartu yang bisa diklon
- ✅ Broker MQTT mewajibkan kredensial per klien — koneksi anonim dinonaktifkan
- ✅ Anti replay: `nonce` (di-dedup 60 detik) + window timestamp 30 detik
- ✅ Rate limiting per `device_id` (maks 1 scan / 2 detik) di backend
- ✅ Watchdog timer ESP32 + backoff eksponensial Wi-Fi/MQTT
- ✅ LWT mempublikasikan `offline` otomatis jika perangkat terputus
- ✅ Fail-secure: tanpa respons server yang tervalidasi → pintu tetap terkunci

**Sebelum produksi**, amankan juga: MQTTS/TLS (`:8883`), autentikasi JWT di semua route, pembatasan role NextAuth pada override, agregasi log (Loki/Grafana), dan cron retensi log. Lihat [`CLAUDE.md`](./CLAUDE.md) untuk checklist pra-produksi lengkap.

---

## 🔧 Wiring Hardware (ESP32 ↔ HW-VX6330K via MAX485)

| MAX485 | → | ESP32 | Keterangan |
|---|---|---|---|
| DI | ← | GPIO 17 (TX2) | TX ESP32 → data-in MAX485 |
| RO | → | GPIO 16 (RX2) | receiver-out MAX485 → RX ESP32 |
| DE + RE | ← | GPIO 27 | Kontrol arah (di-tie): HIGH=TX, LOW=RX |
| A / B | → | Reader A+/B- | Pasangan diferensial RS485, terminasi 120Ω di kedua ujung |
| VCC / GND | → | 3V3 / GND | Reader diberi daya eksternal terpisah (12V) |

** Aktuator:** LED hijau `GPIO 25`, LED merah `GPIO 26`, buzzer KY-12 `GPIO 4`, LED built-in `GPIO 2`.

> ⚠️ **MAX485 wajib.** HW-VX6330K menggunakan RS485 — sambungan langsung ke GPIO ESP32 tidak akan berfungsi dan dapat merusak chip.

---

## 📜 Lisensi

Proyek ini merupakan bagian dari tugas akademik Magister. Hak cipta dilindungi kecuali dinyatakan lain.
