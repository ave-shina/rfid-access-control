# Sistem Kontrol Akses RFID — Dokumentasi Lengkap

---

## 1. Tentang Proyek

**Sistem Kontrol Akses RFID** adalah sistem kontrol akses berbasis IoT (Pervasive Computing) yang memisahkan perangkat fisik (ESP32 + pembaca RFID) dari logika bisnis (backend Go) menggunakan protokol MQTT. Prinsip utama: **perangkat edge hanya membaca kartu dan merespons perintah** — semua keputusan otorisasi dibuat oleh backend.

Sistem ini mengontrol akses pintu melalui kartu RFID UHF. Ketika seseorang mengetuk kartu, ESP32 membaca UID dari modul HW-VX6330K melalui UART, meng-hash-nya, dan mengirimkannya ke backend melalui MQTT. Backend memvalidasinya terhadap database PostgreSQL dan mengirimkan perintah kembali ke ESP32 untuk memberikan akses (LED hijau + 1 bip) atau menolak (LED merah + 3 bip).

---

## 2. Tech Stack

| Layer | Teknologi |
|---|---|
| **Firmware (Edge)** | C++ (framework Arduino), PlatformIO, ESP32 |
| **Protokol Komunikasi** | MQTT (Eclipse Mosquitto 2), Wi-Fi (IEEE 802.11) |
| **Backend** | Go 1.22, `paho.mqtt.golang`, `database/sql`, `gorilla/mux`, `gorilla/websocket`, `zerolog` |
| **Database** | PostgreSQL 15 (Docker) |
| **Frontend** | Next.js 14 (App Router), React, TypeScript, Tailwind CSS, shadcn/ui |
| **Infrastruktur** | Docker, Docker Compose |

---

## 3. Arsitektur 5 Lapis (Pervasive Computing)

```
┌─────────────────────────────────────────────────────────┐
│                 LAPISAN 5: APLIKASI                      │
│             Dashboard Next.js (port 3000)                │
│        LiveLogTable │ DoorControl │ Halaman Users        │
│        WebSocket ←─── Backend (:8080/api/ws)             │
├─────────────────────────────────────────────────────────┤
│                  LAPISAN 4: DATA                         │
│             PostgreSQL 15 (port 5434)                     │
│          tabel users │ tabel access_logs                  │
├─────────────────────────────────────────────────────────┤
│                LAPISAN 3: MIDDLEWARE                      │
│      Go Backend (:8080) + Mosquitto Broker (:1883)       │
│  MQTT Handler │ Rate Limiter │ Anti-Replay │ WS Hub      │
├─────────────────────────────────────────────────────────┤
│                 LAPISAN 2: JARINGAN                      │
│         Wi-Fi (ESP32) + MQTT over TCP                    │
│          Topics: door/scan, door/command, door/status     │
├─────────────────────────────────────────────────────────┤
│               LAPISAN 1: PERSEPSI (DIPERBARUI)          │
│      ESP32 + HW-VX6330K (UHF) + MAX3232 + LED           │
│      UART2: RX=16, TX=17                                │
│      LED Hijau=25, LED Merah=26, Buzzer=4, Built-in=2   │
└─────────────────────────────────────────────────────────┘
```

---

## 4. Struktur Direktori Monorepo

```
rfid-esp32/
├── apps/
│   ├── firmware/                          # Perangkat Edge ESP32
│   │   ├── platformio.ini                 # Konfigurasi board + dependensi
│   │   └── src/
│   │       ├── main.cpp                   # Firmware utama (~296 baris)
│   │       ├── secrets.h                  # gitignored — kredensial Wi-Fi/MQTT
│   │       └── secrets.h.example          # Template (dicommit)
│   ├── backend/                           # Layanan Backend Go
│   │   ├── cmd/server/
│   │   │   └── main.go                    # Entry point, router HTTP, endpoint API
│   │   ├── internal/
│   │   │   ├── db/
│   │   │   │   └── postgres.go            # Connection pool, query CRUD
│   │   │   ├── mqtt/
│   │   │   │   ├── handler.go             # Subscripsi MQTT, pemrosesan scan
│   │   │   │   └── nonce.go              # Cache nonce anti-replay (TTL 60 detik)
│   │   │   ├── models/
│   │   │   │   └── schema.go             # Definisi struct
│   │   │   └── ws/
│   │   │       └── hub.go                # Manajemen klien WebSocket + broadcast
│   │   ├── go.mod
│   │   └── go.sum
│   └── frontend/                          # Dashboard Next.js
│       └── src/
│           ├── app/
│           │   ├── layout.tsx             # Layout root + navigasi sidebar
│           │   ├── page.tsx               # Dashboard (log langsung + kontrol pintu)
│           │   ├── users/page.tsx         # Halaman manajemen pengguna
│           │   ├── globals.css            # Variabel tema, utilitas retro
│           │   └── api/                   # Route handler BFF proxy
│           │       ├── logs/route.ts
│           │       ├── users/route.ts
│           │       ├── users/[id]/route.ts
│           │       └── door/override/route.ts
│           ├── components/
│           │   ├── LiveLogTable.tsx       # Tabel log akses real-time
│           │   ├── DoorControl.tsx        # Override manual (GRANT/DENY)
│           │   └── ui/                    # Komponen shadcn/ui (tema retro)
│           └── lib/
│               ├── websocket.ts           # Hook WebSocket + utilitas fetchAPI
│               └── utils.ts              # Utilitas cn()
└── infrastructure/
    ├── docker-compose.yml                 # Container PostgreSQL + Mosquitto
    ├── mosquitto.conf                     # Konfigurasi broker (autentikasi wajib)
    ├── init.sql                           # Schema seed database
    ├── .env                               # gitignored — rahasia
    └── .env.example                       # Template (dicommit)
```

---

## 5. Alur Kerja Sistem (Alur End-to-End)

### 5.1 Alur Utama: Ketukan Kartu RFID UHF

```
[Pengguna mengetuk kartu UHF]
      │
      ▼
┌──────────────────────────────────────┐
│  ESP32 (main.cpp:loop)              │
│  1. HW-VX6330K membaca tag UHF     │
│     melalui UART2 (RX=16, TX=17)    │
│  2. Parse EPC/UID dari respons      │
│  3. Debounce: cooldown 2 detik      │
│  4. Hash UID → SHA-256 (64 hex)     │
│  5. Generate nonce (random 4 byte)  │
│  6. Ambil timestamp dari NTP        │
│  7. Publish ke MQTT "door/scan"     │
│     {uid_hash, device_id, nonce,    │
│      timestamp}                      │
└──────────────┬───────────────────────┘
               │ MQTT QoS 1
               ▼
┌──────────────────────────────────────┐
│  Mosquitto Broker (:1883)           │
│  Rute: door/scan → Go Backend       │
└──────────────┬───────────────────────┘
               │
               ▼
┌──────────────────────────────────────┐
│  Go Backend (mqtt/handler.go)       │
│  1. Parse payload JSON              │
│  2. Cek rate limit (1 msg/2 detik)  │
│  3. Validasi: uid_hash 64 karakter, │
│     device_id != "", timestamp <30d │
│  4. Anti-replay: nonce unik?        │
│  5. Terapkan pepper: sha256(hash+p) │
│  6. Query DB: SELECT dari users     │
│  7. Evaluasi:                       │
│     - Ditemukan + aktif → AUTHORIZED│
│     - Ditemukan + nonaktif → DENIED │
│     - Tidak ditemukan → DENIED      │
│  8. INSERT ke access_logs           │
│  9. Broadcast via WebSocket         │
│  10. Publish perintah ke "door/cmd" │
│      {status:1/0, message, by}      │
└──────────────┬───────────────────────┘
               │ MQTT QoS 1
               ▼
┌──────────────────────────────────────┐
│  ESP32 (callback)                   │
│  Parse "door/command":              │
│  - status=1 → LED Hijau + 1 bip    │
│  - status=0 → LED Merah + 3 bip    │
└──────────────────────────────────────┘
```

### 5.2 Alur Dashboard Real-Time

```
┌──────────────────────────────────────┐
│  Go Backend (ws/hub.go)             │
│  - Setiap access_log → Broadcast    │
│    {type:"access_log", data:log}    │
│  - Status perangkat → Broadcast     │
│    {type:"device_status", data}     │
└──────────────┬───────────────────────┘
               │ WebSocket (:8080/api/ws)
               ▼
┌──────────────────────────────────────┐
│  Dashboard Next.js                  │
│  Hook useWebSocket() menerima pesan │
│  - access_log → tambah ke tabel    │
│  - device_status → update indikator│
│  Tabel auto-update tanpa refresh    │
└──────────────────────────────────────┘
```

### 5.3 Alur Override Manual dari Dashboard

```
Dashboard ──POST──▶ Next.js BFF ──POST──▶ Go Backend (:8080/api/door/override)
                                                    │
                                                    ├── Publish ke MQTT "door/command"
                                                    ├── INSERT ke access_logs
                                                    ├── Broadcast via WebSocket
                                                    ▼
                                              ESP32 menerima perintah
```

### 5.4 Alur Status Perangkat (LWT)

```
ESP32 terhubung ke MQTT:
  ├── Publish "door/status" → {"status":"online"} (retained)
  └── Daftarkan LWT → {"status":"offline"} (otomatis saat terputus)

Mosquitto ──door/status──▶ Go Backend ──WebSocket──▶ Dashboard
```

---

## 6. Topik MQTT dan Skema Payload

| Topik | Publisher | Subscriber | QoS | Payload |
|---|---|---|---|---|
| `door/scan` | ESP32 | Go Backend | **1** | `{uid_hash, device_id, nonce, timestamp}` |
| `door/command` | Go Backend | ESP32 | **1** | `{status, message, action_by}` |
| `door/status` | ESP32 (LWT) | Go Backend | **0** | `{status: "online"/"offline"}` |

### Skema `door/scan` (ESP32 → Backend)

```json
{
  "uid_hash": "64-karakter-sha256-hex",
  "device_id": "esp32_front_door",
  "nonce": "a3f9c12b",
  "timestamp": 1718000000
}
```

### Skema `door/command` (Backend → ESP32)

```json
{
  "status": 1,
  "message": "Access Granted",
  "action_by": "John Doe"
}
```

- `status: 1` → LED Hijau + 1 bip
- `status: 0` → LED Merah + 3 bip

---

## 7. Skema Database

```sql
CREATE TABLE users (
  id            SERIAL PRIMARY KEY,
  name          VARCHAR(100) NOT NULL,
  rfid_uid_hash VARCHAR(64) UNIQUE NOT NULL,  -- sha256(sha256(rawUID) + pepper)
  role          VARCHAR(50) DEFAULT 'employee',
  is_active     BOOLEAN DEFAULT TRUE,
  created_at    TIMESTAMP DEFAULT CURRENT_TIMESTAMP
);

CREATE TABLE access_logs (
  id            SERIAL PRIMARY KEY,
  rfid_uid_hash VARCHAR(64) NOT NULL,
  status        VARCHAR(20) NOT NULL,          -- 'AUTHORIZED' atau 'DENIED'
  action_by     VARCHAR(100),                  -- Nama pengguna atau 'Unknown'
  device_id     VARCHAR(50),                   -- ID perangkat (multi-pintu)
  timestamp     TIMESTAMP DEFAULT CURRENT_TIMESTAMP
);

-- Indeks untuk performa query
CREATE INDEX idx_access_logs_device_time ON access_logs(device_id, timestamp DESC);
CREATE INDEX idx_access_logs_time        ON access_logs(timestamp DESC);
CREATE INDEX idx_access_logs_status      ON access_logs(status);
```

### Proses Hashing UID (Double Hash + Pepper)

```
UID Mentah (dari kartu): "A1B2C3D4"
         │
         ▼ sha256()
Hash firmware: "8f7a3b...64char"
         │ (dikirim via MQTT, UID mentah TIDAK PERNAH ditransmisikan)
         ▼
Go Backend: sha256(hash_firmware + pepper)
         │
         ▼
Hash terpepper: "e2d9f1...64char"  ← disimpan di DB, digunakan untuk pencarian
```

---

## 8. Endpoint API (Go Backend)

| Method | Endpoint | Deskripsi |
|---|---|---|
| `GET` | `/healthz` | Health check (ping DB) |
| `GET` | `/api/ws` | Upgrade WebSocket untuk event real-time |
| `GET` | `/api/logs?limit=N` | Ambil N log terbaru (default 50) |
| `DELETE` | `/api/logs` | Hapus semua log akses |
| `GET` | `/api/users` | Daftar semua pengguna |
| `POST` | `/api/users` | Registrasi pengguna baru (body: `{name, rfid_uid, role}`) |
| `PATCH` | `/api/users/{id}` | Toggle is_active (body: `{is_active: bool}`) |
| `DELETE` | `/api/users/{id}` | Hapus pengguna |
| `POST` | `/api/door/override` | Override manual (body: `{status: 0/1, message, action_by}`) |

### Layer BFF (Next.js Route Handlers)

Frontend berkomunikasi dengan backend melalui route handler di `src/app/api/` yang mem-proxy request ke `http://localhost:8080`. Pola ini memisahkan frontend dari URL backend dan memungkinkan penambahan autentikasi di masa depan.

---

## 9. Pseudocode

### 9.1 Firmware ESP32 (`main.cpp`)

```
// SETUP
function setup():
    init_serial(115200)
    init_GPIO(green_led=25 OUTPUT, red_led=26 OUTPUT, buzzer=4 OUTPUT, built_in_led=2 OUTPUT)
    init_UART2(rx=16, tx=17, baud=57600)  // Pembaca UHF HW-VX6330K via MAX3232

    connect_wifi(SSID, PASSWORD)       // blocking sampai terhubung
    sync_NTP_time()                     // untuk timestamp akurat

    mqtt.set_server(BROKER, 1883)
    mqtt.set_callback(on_message)

    init_watchdog(timeout=10s, panic=true)

// LOOP UTAMA
function loop():
    reset_watchdog()
    ensure_mqtt_connected()

    if no_uhf_response():
        mqtt.loop()
        return

    uid = parse_epc_from_uart()        // parse EPC dari respons HW-VX6330K

    if uid == last_uid AND (now - last_scan_time) < 2000ms:
        mqtt.loop()                     // debounce: kartu yang sama dalam 2 detik diabaikan
        return

    last_uid = uid
    last_scan_time = now

    hashed = sha256(uid)                // hash UID mentah
    nonce = random_4byte_hex()
    timestamp = current_unix_time()

    payload = json{
        "uid_hash": hashed,
        "device_id": "esp32_front_door",
        "nonce": nonce,
        "timestamp": timestamp
    }

    mqtt.publish("door/scan", payload, QoS=1)
    clear_uart_buffer()                 // siap untuk scan berikutnya

// CALLBACK PESAN MQTT
function on_message(topic, payload):
    if topic == "door/command":
        status = parse_json(payload).status

        if status == 1:
            grant_access()              // LED Hijau ON, 1 bip, delay 3s, LED OFF
        else if status == 0:
            deny_access()               // LED Merah ON, 3 bip, delay 3s, LED OFF

// RECONNECT DENGAN EXPONENTIAL BACKOFF
function ensure_mqtt_connected():
    if mqtt.connected(): return

    backoff = 1000ms
    while not mqtt.connected():
        if wifi_disconnected():
            reconnect_wifi()
            wait(backoff)
            backoff = min(backoff * 2, 30000ms)
            continue

        // koneksi dengan LWT (Last Will and Testament)
        success = mqtt.connect(
            client_id="esp32_front_door",
            auth=(user, pass),
            lwt_topic="door/status",
            lwt_payload={"status":"offline"}
        )

        if success:
            mqtt.subscribe("door/command", QoS=1)
            mqtt.publish("door/status", {"status":"online"}, retained=true)
            backoff = 1000ms           // reset backoff
        else:
            wait(backoff)
            backoff = min(backoff * 2, 30000ms)
```

### 9.2 Go Backend — MQTT Handler (`handler.go`)

```
// DIPANGGIL SETIAP KALI PESAN TIBA DI "door/scan"
function handle_scan(raw_message):
    go process_scan(raw_message)        // jalankan dalam goroutine terpisah

function process_scan(raw_message):
    payload = parse_json(raw_message)

    // 1. CEK RATE LIMIT
    if not rate_limiter.allow(payload.device_id, rate=1_per_2s):
        log.warn("rate limit terlampaui")
        return

    // 2. VALIDASI INPUT
    if len(payload.uid_hash) != 64:
        return                          // panjang hash tidak valid
    if payload.device_id == "":
        return                          // device_id kosong
    if now() - payload.timestamp > 30s:
        return                          // timestamp kedaluwarsa

    // 3. CEK ANTI-REPLAY
    if not nonce_cache.is_fresh(payload.nonce):
        log.warn("replay terdeteksi")
        return                          // nonce sudah terlihat dalam 60 detik terakhir

    // 4. TERAPKAN PEPPER + LOOKUP DATABASE
    peppered_hash = sha256(payload.uid_hash + PEPPER)
    user = db.query("SELECT * FROM users WHERE rfid_uid_hash = ?", peppered_hash)

    // 5. EVALUASI AKSES
    if user != null AND user.is_active:
        status = "AUTHORIZED"
        cmd_status = 1
        message = "Access Granted"
        action_by = user.name
    else if user != null AND NOT user.is_active:
        status = "DENIED"
        cmd_status = 0
        message = "Access Denied — User Inactive"
        action_by = user.name
    else:
        status = "DENIED"
        cmd_status = 0
        message = "Access Denied — Unknown Card"
        action_by = "Unknown"

    // 6. LOG KE DATABASE (kegagalan logging TIDAK BOLEH memblokir aktuator)
    access_log = {peppered_hash, status, action_by, device_id, now()}
    try:
        db.insert("access_logs", access_log)
        ws_hub.broadcast({type: "access_log", data: access_log})
    catch error:
        log.error("gagal logging", error)
        // TETAP LANJUTKAN ke aktuator

    // 7. AKTUATKAN PINTU
    command = {status: cmd_status, message, action_by}
    mqtt.publish("door/command", command, QoS=1)
```

### 9.3 Go Backend — HTTP API (`main.go`)

```
// POST /api/users — Registrasi pengguna baru
function create_user(request):
    body = parse_json(request)
    validate(body.name != "" AND body.rfid_uid != "")

    // Double hash: sha256(rawUID) lalu sha256(hash + pepper)
    firmware_hash = sha256(body.rfid_uid)
    peppered_hash = sha256(firmware_hash + PEPPER)

    user = db.insert("users", {
        name: body.name,
        rfid_uid_hash: peppered_hash,
        role: body.role || "employee"
    })

    return json(user)

// POST /api/door/override — Override manual dari dashboard
function door_override(request):
    body = parse_json(request)
    validate(body.status == 0 OR body.status == 1)

    command = {status: body.status, message, action_by: "Dashboard"}
    mqtt.publish("door/command", command, QoS=1)

    log = {rfid_hash: "override", status, "Dashboard", "dashboard_override", now()}
    db.insert("access_logs", log)
    ws_hub.broadcast({type: "access_log", data: log})

    return json(command)
```

### 9.4 Go Backend — WebSocket Hub (`hub.go`)

```
class Hub:
    clients: map[WebSocketConnection]bool
    cached_device_status: bytes          // cache status terakhir

    function register(conn):
        clients[conn] = true
        if cached_device_status != null:
            conn.send(cached_device_status)   // kirim status langsung ke klien baru

    function unregister(conn):
        delete clients[conn]
        conn.close()

    function broadcast(message):
        data = json.encode(message)

        if message.type == "device_status":
            cached_device_status = data        // cache untuk klien baru

        for each conn in clients:
            try:
                conn.send(data)
            catch:
                go unregister(conn)           // koneksi mati, hapus

    function serve_websocket(http_request):
        conn = upgrade_to_websocket(http_request)
        register(conn)

        // Read loop — deteksi putus koneksi
        go function():
            while true:
                try: conn.read()
                catch: break                  // klien terputus
            unregister(conn)

        // Ping loop — keep-alive
        go function():
            every 30 seconds:
                try: conn.ping()
                catch: break                  // koneksi mati
            unregister(conn)
```

### 9.5 Go Backend — Cache Nonce Anti-Replay (`nonce.go`)

```
class NonceCache:
    entries: map[nonce_string → timestamp]
    ttl: 60 detik
    mutex: lock

    function check_and_store(nonce):
        lock(mutex)
        if nonce in entries AND (now - entries[nonce]) < ttl:
            unlock(mutex)
            return false                   // REPLAY TERDETEKSI
        entries[nonce] = now()
        unlock(mutex)
        return true                        // nonce segar, disimpan

    // Pembersihan background setiap 30 detik
    function cleanup_loop():
        every 30 seconds:
            lock(mutex)
            for nonce, timestamp in entries:
                if now - timestamp > ttl:
                    delete entries[nonce]
            unlock(mutex)
```

### 9.6 Frontend — Hook WebSocket (`websocket.ts`)

```
const BACKEND_URL = env.BACKEND_URL || "http://localhost:8080"

function useWebSocket(on_message_callback):
    ws = null
    reconnect_timer = null

    function connect():
        ws_url = BACKEND_URL.replace("http", "ws") + "/api/ws"
        ws = new WebSocket(ws_url)

        ws.on_message = (event):
            msg = json.parse(event.data)
            on_message_callback(msg)     // {type: "access_log"/"device_status", data}

        ws.on_close = ():
            if ws masih aktif:
                reconnect_timer = setTimeout(connect, 3000ms)

        ws.on_error = ():
            ws.close()                   // trigger on_close → reconnect

    on_mount: connect()
    on_unmount: clearTimeout(reconnect_timer), ws.close()

    return ws_ref

function fetchAPI(path, options):
    response = fetch(path, options)
    if not response.ok:
        throw Error(response.text())
    if response.status == 204:
        return undefined
    return response.json()
```

### 9.7 Frontend — Komponen LiveLogTable

```
component LiveLogTable():
    state logs = []
    state device_status = "unknown"

    // Muat data awal
    on_mount:
        logs = fetchAPI("/api/logs?limit=50")

    // Berlangganan update real-time
    useWebSocket(on_message):
        if message.type == "access_log":
            logs = [message.data, ...logs].slice(0, 100)    // tambah di depan, maks 100
        if message.type == "device_status":
            device_status = message.data.status

    render:
        // Indikator status perangkat (kotak hitam/putih berdasarkan status)
        <status_indicator status={device_status} />

        // Tabel log
        <table columns=[Waktu, Status, Pengguna, Perangkat, Hash UID]>
            for each log in logs:
                <row>
                    <cell>{log.timestamp.toTimeString()}</cell>
                    <cell><badge>{log.status}</badge></cell>
                    <cell bold>{log.action_by}</cell>
                    <cell mono>{log.device_id}</cell>
                    <cell mono truncated>{log.uid_hash[:16]}...</cell>
                </row>
```

### 9.8 Frontend — Komponen DoorControl

```
component DoorControl():
    state loading = false

    function override(status):
        action = status == 1 ? "GRANT" : "DENY"
        if not confirm("Apakah Anda yakin ingin {action} akses?"):
            return

        loading = true
        try:
            fetchAPI("/api/door/override", {
                method: POST,
                body: {status, message: "Manual Override — ...", action_by: "Dashboard"}
            })
        catch error:
            alert("Override gagal: " + error)
        finally:
            loading = false

    render:
        <button onClick={() => override(1)>[ GRANT ]</button>
        <button onClick={() => override(0)>[ DENY ]</button>
```

---

## 10. Fitur Keamanan yang Diimplementasikan

| Fitur | Implementasi | Lokasi |
|---|---|---|
| **Hashing UID** | SHA-256 di firmware, double-hash + pepper di backend | `main.cpp:hashUID()` (EPC UHF di-hash), `handler.go:pepperHash()` |
| **Anti-Replay** | Nonce random 4 byte + cache TTL 60 detik | `nonce.go:CheckAndStore()` |
| **Validasi Timestamp** | Tolak jika >30 detik dari waktu server | `handler.go:processScan()` |
| **Rate Limiting** | 1 pesan per 2 detik per device_id | `handler.go:getLimiter()` menggunakan `golang.org/x/time/rate` |
| **Autentikasi MQTT** | Username/password wajib, anonim dinonaktifkan | `mosquitto.conf` |
| **LWT (Last Will)** | Otomatis publish offline jika ESP32 terputus | `main.cpp:reconnect()` |
| **Watchdog Timer** | ESP32 auto-reboot jika hang (timeout 10s) | `main.cpp:setup()` |
| **Validasi Input** | uid_hash harus 64 karakter hex, device_id tidak boleh kosong | `handler.go:processScan()` |
| **Connection Pool** | Maks 25 terbuka, 10 idle, masa hidup 5 menit | `postgres.go:New()` |
| **Debounce Scan** | Kartu yang sama dalam 2 detik diabaikan | `main.cpp:loop()` — debounce pada parsing UART UHF |

---

## 11. Diagram Pengkabelan Perangkat Keras

### 11.1 Pengkabelan Sistem Lengkap

```
                         ┌─────────────────────────────────────────────────────┐
                         │              ESP32 (LoLin32)                        │
                         │                                                     │
   ┌──────────────┐      │  GPIO 16 (RX2) ──── MAX3232 TTL TX ──┐            │
   │ HW-VX6330K   │      │  GPIO 17 (TX2) ──── MAX3232 TTL RX ──┤            │
   │ Pembaca UHF  │      │  3V3/5V          ──── MAX3232 VCC    │            │
   │ (Mode Aktif) │      │  GND             ──── MAX3232 GND    │            │
   │              │      │                                     │            │
   │  Power Ekst. │      │  GPIO 25 ──── 330Ω ──── LED Hijau ── GND         │
   │  (12V DC)    │      │  GPIO 26 ──── 330Ω ──── LED Merah ── GND         │
   └──────┬───────┘      │  GPIO 4  ──────────── Buzzer KY-12 ── GND         │
          │              │  GPIO 2  ──────────── LED Built-in                │
          │              └─────────────────────────────────────────────────────┘
          │
          ▼
   ┌──────────────┐      ┌──────────────────┐      ┌──────────────────┐
   │ DB9 Male     │      │ DB9 Female-to-   │      │ Kabel Jumper     │
   │ (di kabel    │──────│ Female Converter │──────│ (Male-to-Male)   │──┐
   │  reader)     │      │ (straight)       │      │ Pin 3 → Pin 2   │  │
   │              │      │                  │      │ Pin 2 → Pin 3   │  │
   │ Pin 3 = TXD  │      │                  │      │ Pin 5 → Pin 5   │  │
   │ Pin 2 = RXD  │      │                  │      │                  │  │
   │ Pin 5 = GND  │      └──────────────────┘      └──────────────────┘  │
   └──────────────┘                                                        │
                                                                            │
                                                             ┌──────────────┘
                                                             │
                                                    ┌────────▼────────┐
                                                    │ Modul MAX3232   │
                                                    │ (RS232 ↔ TTL)   │
                                                    │                 │
                                                    │ DB9 Female:     │
                                                    │  Pin 2 = RX in  │
                                                    │  Pin 3 = TX out │
                                                    │  Pin 5 = GND    │
                                                    │                 │
                                                    │ Sisi TTL:       │
                                                    │  TX  → GPIO 16  │
                                                    │  RX  ← GPIO 17  │
                                                    │  VCC → 3V3/5V   │
                                                    │  GND → GND      │
                                                    └─────────────────┘
```

### 11.2 Pinout Kabel Pembaca (HW-VX6330K)

Pembaca HW-VX6330K memiliki konektor DB9 male tetap pada kabelnya dengan pinout berikut:

| Warna Kabel | Pin DB9 Male | Sinyal | Arah |
|---|---|---|---|
| **Pink** | Pin 3 | TXD (Transmit Data) | Reader → MAX3232 |
| **Putih** | Pin 2 | RXD (Receive Data) | MAX3232 → Reader |
| **Coklat** | Pin 5 | GND (Signal Ground) | Common |

> **Catatan:** Pembaca memiliki catu daya eksternal tersendiri (12V DC). Daya TIDAK disediakan melalui konektor DB9.

### 11.3 Pengkabelan RS232 Null-Modem (Cross)

Komunikasi RS232 memerlukan **pengkabelan silang** antara pembaca (DCE) dan modul MAX3232. TX di satu sisi harus terhubung ke RX di sisi lain:

```
Reader DB9 Male          MAX3232 DB9 Female
Pin 3 (TXD) ──────────→ Pin 2 (RX in)     ← Data dari pembaca
Pin 2 (RXD) ←────────── Pin 3 (TX out)    ← Data ke pembaca (opsional untuk Mode Aktif)
Pin 5 (GND) ──────────── Pin 5 (GND)      ← Ground bersama
```

### 11.4 Metode Koneksi Fisik

**Penting:** Konektor jumper Dupont female standar tidak dapat mencengkeram pin DB9 male dengan baik (pin DB9 berdiameter ~1mm bulat, Dupont dirancang untuk pin header 2.54mm persegi). Solusi yang berfungsi menggunakan konverter DB9 female-to-female straight-through sebagai adapter:

```
Reader DB9 male ──→ Konverter Female-to-Female ──→ Kabel jumper male ──→ MAX3232 DB9 female
                   (kontak yang baik              (masukkan ke lubang        (masukkan ke
                    dengan pin DB9 male)           socket konverter)         socket)
```

**Langkah-langkah perakitan:**

1. **Pasang** konverter DB9 female-to-female ke konektor DB9 male pembaca
2. **Masukkan** 3 kabel jumper male-to-male ke lubang socket konverter:
   - Kabel A: ke lubang **Pin 3** konverter → ujung lainnya ke lubang **Pin 2** MAX3232
   - Kabel B: ke lubang **Pin 2** konverter → ujung lainnya ke lubang **Pin 3** MAX3232
   - Kabel C: ke lubang **Pin 5** konverter → ujung lainnya ke lubang **Pin 5** MAX3232
3. **Hubungkan** sisi TTL MAX3232 ke ESP32:
   - MAX3232 **TX** → ESP32 **GPIO 16** (UART2 RX)
   - MAX3232 **RX** → ESP32 **GPIO 17** (UART2 TX)
   - MAX3232 **VCC** → ESP32 **3V3** (atau 5V/VIN, tergantung modul)
   - MAX3232 **GND** → ESP32 **GND**

**Alternatif (permanen):** Ganti konverter female-to-female + kabel jumper dengan **adapter DB9 null modem** (female-to-female, disilangkan secara internal). Ini memungkinkan koneksi langsung:

```
Reader DB9 male ──→ Adapter null modem ──→ MAX3232 DB9 female
```

### 11.5 Detail Modul MAX3232

| Spesifikasi | Nilai |
|---|---|
| Fungsi | Level shifter bidireksional RS232 (±12V) ↔ TTL (3.3V) |
| Chip | MAX3232 (atau kompatibel) |
| Daya | 3.3V atau 5V dari ESP32 |
| Konektor DB9 | Female, sisi RS232 |
| Header TTL | 4-pin (TX, RX, VCC, GND) |

> **PERINGATAN:** HW-VX6330K menggunakan level tegangan RS232 (±12V). Koneksi langsung ke GPIO ESP32 akan **merusak** chip ESP32. Modul MAX3232 wajib digunakan.

### 11.6 Pengkabelan Aktuator

| Komponen | GPIO ESP32 | Koneksi |
|---|---|---|
| LED Hijau | GPIO 25 | Melalui resistor 330Ω ke GND |
| LED Merah | GPIO 26 | Melalui resistor 330Ω ke GND |
| Buzzer KY-12 | GPIO 4 | Langsung ke GND (active buzzer) |
| LED Built-in | GPIO 2 | Onboard (tanpa pengkabelan) |

### 11.7 Panduan Troubleshooting

| Gejala | Penyebab | Solusi |
|---|---|---|
| Tidak ada data di Serial Monitor | Dupont female longgar pada pin DB9 male | Gunakan konverter female-to-female sebagai adapter (lihat 11.4) |
| Tidak ada data di Serial Monitor | TX/RX tidak disilangkan | Verifikasi Pin 3(reader) → Pin 2(MAX3232) |
| Data sampah | Baud rate salah | Coba 9600, 19200, 38400, 57600, 115200 |
| Loopback test gagal | Pengkabelan MAX3232 ke ESP32 | Periksa TTL TX→GPIO16, RX→GPIO17 |
| Pembaca tidak mengirim | Tidak ada daya eksternal | Pembaca membutuhkan catu daya 12V terpisah |

### 11.8 Loopback Test (Verifikasi MAX3232 + ESP32)

Untuk memverifikasi pengkabelan MAX3232 dan ESP32 secara independen dari pembaca:

1. Putuskan pembaca dari MAX3232
2. Hubungkan singkat Pin 2 dan Pin 3 pada konektor DB9 female MAX3232 dengan kabel jumper
3. Flash loopback test: ESP32 mengirim data via Serial2, memeriksa apakah menerima data yang sama kembali
4. Jika data kembali → pengkabelan MAX3232 + ESP32 benar
5. Jika tidak ada data → Periksa daya MAX3232, pengkabelan TTL, atau tukar GPIO 16/17

---

## 12. Cara Menjalankan (Pengembangan)

```bash
# 1. Konfigurasi rahasia
cp infrastructure/.env.example infrastructure/.env
# Isi semua nilai di .env

cp apps/firmware/src/secrets.h.example apps/firmware/src/secrets.h
# Isi SSID, password Wi-Fi, IP broker MQTT, kredensial MQTT

# 2. Jalankan infrastruktur (PostgreSQL + Mosquitto)
docker-compose --env-file infrastructure/.env up -d

# 3. Buat pengguna MQTT di Mosquitto
docker exec -it rfid_mosquitto mosquitto_passwd -c /mosquitto/config/passwd esp32_front_door
docker exec -it rfid_mosquitto mosquitto_passwd /mosquitto/config/passwd go_backend
docker exec -it rfid_mosquitto mosquitto_passwd /mosquitto/config/passwd nextjs_dashboard

# 4. Jalankan backend Go
cd apps/backend
go mod tidy
go run cmd/server/main.go
# Expected: "Connected to PostgreSQL" + "Connected to Mosquitto Broker"

# 5. Jalankan dashboard Next.js
cd apps/frontend
npm install
npm run dev
# Dashboard: http://localhost:3000

# 6. Flash firmware ESP32
# Buka apps/firmware/ di VS Code + PlatformIO
# Pastikan secrets.h sudah diisi
# Klik Upload and Monitor
```

---

## 13. Keterbatasan & Persyaratan Pra-Produksi

Item berikut perlu ditambahkan sebelum deploy ke produksi:

- [ ] Upgrade MQTT ke TLS (port 8883) — gunakan `WiFiClientSecure` di ESP32
- [ ] Autentikasi JWT pada semua route API backend
- [ ] NextAuth.js di dashboard — override dibatasi hanya untuk role `admin`
- [ ] Agregasi log (Loki + Grafana atau Datadog)
- [ ] Kebijakan retensi `access_logs` (cron/pg_cron)
- [ ] Broker MQTT terkluster (HiveMQ/EMQX) untuk ketersediaan tinggi
- [ ] Update firmware OTA melalui `ArduinoOTA`
