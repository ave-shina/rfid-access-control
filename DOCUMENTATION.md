# RFID Access Control System — Dokumentasi Lengkap

---

## 1. Tentang Proyek

**RFID Access Control System** adalah sistem kendali akses berbasis IoT (Pervasive Computing) yang memisahkan perangkat fisik (ESP32 + pembaca RFID) dari logika bisnis (Go backend) menggunakan protokol MQTT. Prinsip utama: **perangkat edge hanya membaca kartu dan merespons perintah** — semua keputusan otorisasi ada di backend.

Sistem ini mengontrol akses pintu melalui kartu RFID UHF. Ketika seseorang menempelkan kartu, ESP32 membaca UID dari modul HW-VX6330K via UART, meng-hash-nya, lalu mengirim ke backend via MQTT. Backend memvalidasi terhadap database PostgreSQL dan mengirim perintah kembali ke ESP32 untuk membuka (LED hijau + 1 beep) atau menolak (LED merah + 3 beep).

---

## 2. Tech Stack

| Lapisan | Teknologi |
|---|---|
| **Firmware (Edge)** | C++ (Arduino framework), PlatformIO, ESP32 |
| **Protokol Komunikasi** | MQTT (Eclipse Mosquitto 2), Wi-Fi (IEEE 802.11) |
| **Backend** | Go 1.22, `paho.mqtt.golang`, `database/sql`, `gorilla/mux`, `gorilla/websocket`, `zerolog` |
| **Database** | PostgreSQL 15 (Docker) |
| **Frontend** | Next.js 14 (App Router), React, TypeScript, Tailwind CSS, shadcn/ui |
| **Infrastruktur** | Docker, Docker Compose |

---

## 3. Arsitektur 5-Lapisan (Pervasive Computing)

```
┌─────────────────────────────────────────────────────────┐
│                  LAYER 5: APPLICATION                    │
│              Next.js Dashboard (port 3000)               │
│         LiveLogTable │ DoorControl │ Users Page          │
│         WebSocket ←─── Backend (:8080/api/ws)            │
├─────────────────────────────────────────────────────────┤
│                   LAYER 4: DATA                          │
│              PostgreSQL 15 (port 5434)                    │
│           users table │ access_logs table                 │
├─────────────────────────────────────────────────────────┤
│                 LAYER 3: MIDDLEWARE                       │
│       Go Backend (:8080) + Mosquitto Broker (:1883)      │
│   MQTT Handler │ Rate Limiter │ Anti-Replay │ WS Hub     │
├─────────────────────────────────────────────────────────┤
│                  LAYER 2: NETWORK                        │
│          Wi-Fi (ESP32) + MQTT over TCP                    │
│           Topics: door/scan, door/command, door/status    │
├─────────────────────────────────────────────────────────┤
│                LAYER 1: PERCEPTION (UPDATED)            │
│       ESP32 + HW-VX6330K (UHF) + MAX3232 + LEDs         │
│       UART2: RX=16, TX=17                               │
│       Green LED=25, Red LED=26, Buzzer=4, Built-in=2    │
└─────────────────────────────────────────────────────────┘
```

---

## 4. Struktur Direktori Monorepo

```
rfid-esp32/
├── apps/
│   ├── firmware/                          # ESP32 Edge Device
│   │   ├── platformio.ini                 # Board config + dependencies
│   │   └── src/
│   │       ├── main.cpp                   # Firmware utama (~296 baris)
│   │       ├── secrets.h                  # gitignored — kredensial Wi-Fi/MQTT
│   │       └── secrets.h.example          # Template (di-commit)
│   ├── backend/                           # Go Backend Service
│   │   ├── cmd/server/
│   │   │   └── main.go                    # Entry point, HTTP router, API endpoints
│   │   ├── internal/
│   │   │   ├── db/
│   │   │   │   └── postgres.go            # Connection pool, CRUD queries
│   │   │   ├── mqtt/
│   │   │   │   ├── handler.go             # MQTT subscription, scan processing
│   │   │   │   └── nonce.go              # Anti-replay nonce cache (TTL 60s)
│   │   │   ├── models/
│   │   │   │   └── schema.go             # Struct definitions
│   │   │   └── ws/
│   │   │       └── hub.go                # WebSocket client management + broadcast
│   │   ├── go.mod
│   │   └── go.sum
│   └── frontend/                          # Next.js Dashboard
│       └── src/
│           ├── app/
│           │   ├── layout.tsx             # Root layout + sidebar navigation
│           │   ├── page.tsx               # Dashboard (live log + door control)
│           │   ├── users/page.tsx         # User management page
│           │   ├── globals.css            # Theme variables, retro utilities
│           │   └── api/                   # BFF proxy routes
│           │       ├── logs/route.ts
│           │       ├── users/route.ts
│           │       ├── users/[id]/route.ts
│           │       └── door/override/route.ts
│           ├── components/
│           │   ├── LiveLogTable.tsx       # Real-time access log table
│           │   ├── DoorControl.tsx        # Manual override (GRANT/DENY)
│           │   └── ui/                    # shadcn/ui components (retro-themed)
│           └── lib/
│               ├── websocket.ts           # WebSocket hook + fetchAPI utility
│               └── utils.ts              # cn() utility
└── infrastructure/
    ├── docker-compose.yml                 # PostgreSQL + Mosquitto containers
    ├── mosquitto.conf                     # Broker config (auth required)
    ├── init.sql                           # Database schema seed
    ├── .env                               # gitignored — secrets
    └── .env.example                       # Template (di-commit)
```

---

## 5. Alur Kerja Sistem (End-to-End Flow)

### 5.1 Alur Utama: Tap Kartu RFID UHF

```
[User tap kartu UHF]
      │
      ▼
┌──────────────────────────────────────┐
│  ESP32 (main.cpp:loop)              │
│  1. HW-VX6330K baca UHF tag        │
│     via UART2 (RX=16, TX=17)        │
│  2. Parse EPC/UID dari respons      │
│  3. Debounce: cooldown 2 detik      │
│  4. Hash UID → SHA-256 (64 hex)     │
│  5. Generate nonce (4-byte random)  │
│  6. Ambil timestamp dari NTP        │
│  7. Publish ke MQTT "door/scan"     │
│     {uid_hash, device_id, nonce,    │
│      timestamp}                      │
└──────────────┬───────────────────────┘
               │ MQTT QoS 1
               ▼
┌──────────────────────────────────────┐
│  Mosquitto Broker (:1883)           │
│  Route: door/scan → Go Backend      │
└──────────────┬───────────────────────┘
               │
               ▼
┌──────────────────────────────────────┐
│  Go Backend (mqtt/handler.go)       │
│  1. Parse JSON payload              │
│  2. Rate limit check (1 msg/2s)     │
│  3. Validasi: uid_hash 64 char,     │
│     device_id != "", timestamp <30s │
│  4. Anti-replay: nonce unik?        │
│  5. Apply pepper: sha256(hash+pep)  │
│  6. Query DB: SELECT dari users     │
│  7. Evaluasi:                       │
│     - Found + active → AUTHORIZED   │
│     - Found + inactive → DENIED     │
│     - Not found → DENIED            │
│  8. INSERT ke access_logs           │
│  9. Broadcast via WebSocket         │
│  10. Publish command ke "door/cmd"  │
│      {status:1/0, message, by}      │
└──────────────┬───────────────────────┘
               │ MQTT QoS 1
               ▼
┌──────────────────────────────────────┐
│  ESP32 (callback)                   │
│  Parse "door/command":              │
│  - status=1 → LED hijau + 1 beep   │
│  - status=0 → LED merah + 3 beep   │
└──────────────────────────────────────┘
```

### 5.2 Alur Dashboard Real-Time

```
┌──────────────────────────────────────┐
│  Go Backend (ws/hub.go)             │
│  - Setiap access_log → Broadcast    │
│    {type:"access_log", data:log}    │
│  - Device status → Broadcast        │
│    {type:"device_status", data}     │
└──────────────┬───────────────────────┘
               │ WebSocket (:8080/api/ws)
               ▼
┌──────────────────────────────────────┐
│  Next.js Dashboard                  │
│  useWebSocket() hook menerima msg   │
│  - access_log → prepend ke tabel   │
│  - device_status → update indikator│
│  Tabel auto-update tanpa refresh    │
└──────────────────────────────────────┘
```

### 5.3 Alur Manual Override dari Dashboard

```
Dashboard ──POST──▶ Next.js BFF ──POST──▶ Go Backend (:8080/api/door/override)
                                                    │
                                                    ├── Publish ke MQTT "door/command"
                                                    ├── INSERT ke access_logs
                                                    ├── Broadcast via WebSocket
                                                    ▼
                                              ESP32 menerima command
```

### 5.4 Alur Device Status (LWT)

```
ESP32 connect MQTT:
  ├── Publish "door/status" → {"status":"online"} (retained)
  └── Register LWT → {"status":"offline"} (otomatis jika disconnect)

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
  "uid_hash": "64-char-sha256-hex",
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

- `status: 1` → LED hijau + 1 beep
- `status: 0` → LED merah + 3 beep

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
  action_by     VARCHAR(100),                  -- Nama user atau 'Unknown'
  device_id     VARCHAR(50),                   -- ID perangkat (multi-door)
  timestamp     TIMESTAMP DEFAULT CURRENT_TIMESTAMP
);

-- Index untuk performa query
CREATE INDEX idx_access_logs_device_time ON access_logs(device_id, timestamp DESC);
CREATE INDEX idx_access_logs_time        ON access_logs(timestamp DESC);
CREATE INDEX idx_access_logs_status      ON access_logs(status);
```

### Proses Hashing UID (Double Hash + Pepper)

```
Raw UID (dari kartu): "A1B2C3D4"
         │
         ▼ sha256()
Firmware hash: "8f7a3b...64char"
         │ (dikirim via MQTT, raw UID TIDAK PERNAH dikirim)
         ▼
Go Backend: sha256(firmware_hash + pepper)
         │
         ▼
Peppered hash: "e2d9f1...64char"  ← disimpan di DB, dipakai untuk lookup
```

---

## 8. API Endpoints (Go Backend)

| Method | Endpoint | Deskripsi |
|---|---|---|
| `GET` | `/healthz` | Health check (ping DB) |
| `GET` | `/api/ws` | WebSocket upgrade untuk real-time events |
| `GET` | `/api/logs?limit=N` | Ambil N log terbaru (default 50) |
| `DELETE` | `/api/logs` | Hapus semua access logs |
| `GET` | `/api/users` | List semua user |
| `POST` | `/api/users` | Registrasi user baru (body: `{name, rfid_uid, role}`) |
| `PATCH` | `/api/users/{id}` | Toggle is_active (body: `{is_active: bool}`) |
| `DELETE` | `/api/users/{id}` | Hapus user |
| `POST` | `/api/door/override` | Manual override (body: `{status: 0/1, message, action_by}`) |

### BFF Layer (Next.js Route Handlers)

Frontend berkomunikasi ke backend melalui route handlers di `src/app/api/` yang mem-proxy request ke `http://localhost:8080`. Pola ini memisahkan frontend dari backend URL dan memungkinkan penambahan autentikasi di masa depan.

---

## 9. Pseudocode

### 9.1 Firmware ESP32 (`main.cpp`)

```
// SETUP
function setup():
    init_serial(115200)
    init_GPIO(green_led=25 OUTPUT, red_led=26 OUTPUT, buzzer=4 OUTPUT, built_in_led=2 OUTPUT)
    init_UART2(rx=16, tx=17, baud=9600)   // HW-VX6330K UHF reader via MAX3232

    connect_wifi(SSID, PASSWORD)       // blocking sampai terkoneksi
    sync_NTP_time()                     // untuk timestamp akurat

    mqtt.set_server(BROKER, 1883)
    mqtt.set_callback(on_message)

    init_watchdog(timeout=10s, panic=true)

// MAIN LOOP
function loop():
    reset_watchdog()
    ensure_mqtt_connected()

    if no_uhf_response():
        mqtt.loop()
        return

    uid = parse_epc_from_uart()        // parse EPC dari respons HW-VX6330K

    if uid == last_uid AND (now - last_scan_time) < 2000ms:
        mqtt.loop()                     // debounce: kartu sama dalam 2 detik diabaikan
        return

    last_uid = uid
    last_scan_time = now

    hashed = sha256(uid)                // hash raw UID
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

// MQTT MESSAGE CALLBACK
function on_message(topic, payload):
    if topic == "door/command":
        status = parse_json(payload).status

        if status == 1:
            grant_access()              // LED hijau ON, 1 beep, delay 3s, LED OFF
        else if status == 0:
            deny_access()               // LED merah ON, 3 beep, delay 3s, LED OFF

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

        // connect dengan LWT (Last Will and Testament)
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
// DIPANGGIL SETIAP KALI ADA PESAN di "door/scan"
function handle_scan(raw_message):
    go process_scan(raw_message)        // jalankan di goroutine terpisah

function process_scan(raw_message):
    payload = parse_json(raw_message)

    // 1. RATE LIMIT CHECK
    if not rate_limiter.allow(payload.device_id, rate=1_per_2s):
        log.warn("rate limit exceeded")
        return

    // 2. INPUT VALIDATION
    if len(payload.uid_hash) != 64:
        return                          // invalid hash length
    if payload.device_id == "":
        return                          // missing device_id
    if now() - payload.timestamp > 30s:
        return                          // stale timestamp

    // 3. ANTI-REPLAY CHECK
    if not nonce_cache.is_fresh(payload.nonce):
        log.warn("replay detected")
        return                          // nonce sudah pernah dilihat dalam 60 detik

    // 4. APPLY PEPPER + DATABASE LOOKUP
    peppered_hash = sha256(payload.uid_hash + PEPPER)
    user = db.query("SELECT * FROM users WHERE rfid_uid_hash = ?", peppered_hash)

    // 5. EVALUATE ACCESS
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

    // 6. LOG TO DATABASE (kegagalan logging TIDAK boleh memblokir actuation)
    access_log = {peppered_hash, status, action_by, device_id, now()}
    try:
        db.insert("access_logs", access_log)
        ws_hub.broadcast({type: "access_log", data: access_log})
    catch error:
        log.error("failed to log", error)
        // TETAP LANJUT ke actuation

    // 7. ACTUATE DOOR
    command = {status: cmd_status, message, action_by}
    mqtt.publish("door/command", command, QoS=1)
```

### 9.3 Go Backend — HTTP API (`main.go`)

```
// POST /api/users — Registrasi user baru
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

// POST /api/door/override — Manual override dari dashboard
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
            conn.send(cached_device_status)   // kirim status langsung ke client baru

    function unregister(conn):
        delete clients[conn]
        conn.close()

    function broadcast(message):
        data = json.encode(message)

        if message.type == "device_status":
            cached_device_status = data        // cache untuk client baru

        for each conn in clients:
            try:
                conn.send(data)
            catch:
                go unregister(conn)           // koneksi mati, hapus

    function serve_websocket(http_request):
        conn = upgrade_to_websocket(http_request)
        register(conn)

        // Read loop — deteksi disconnect
        go function():
            while true:
                try: conn.read()
                catch: break                  // client disconnect
            unregister(conn)

        // Ping loop — keep-alive
        go function():
            every 30 seconds:
                try: conn.ping()
                catch: break                  // koneksi mati
            unregister(conn)
```

### 9.5 Go Backend — Anti-Replay Nonce Cache (`nonce.go`)

```
class NonceCache:
    entries: map[nonce_string → timestamp]
    ttl: 60 seconds
    mutex: lock

    function check_and_store(nonce):
        lock(mutex)
        if nonce in entries AND (now - entries[nonce]) < ttl:
            unlock(mutex)
            return false                   // REPLAY DETECTED
        entries[nonce] = now()
        unlock(mutex)
        return true                        // nonce fresh, disimpan

    // Background cleanup setiap 30 detik
    function cleanup_loop():
        every 30 seconds:
            lock(mutex)
            for nonce, timestamp in entries:
                if now - timestamp > ttl:
                    delete entries[nonce]
            unlock(mutex)
```

### 9.6 Frontend — WebSocket Hook (`websocket.ts`)

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
            if ws is still active:
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

### 9.7 Frontend — LiveLogTable Component

```
component LiveLogTable():
    state logs = []
    state device_status = "unknown"

    // Load initial data
    on_mount:
        logs = fetchAPI("/api/logs?limit=50")

    // Subscribe real-time updates
    useWebSocket(on_message):
        if message.type == "access_log":
            logs = [message.data, ...logs].slice(0, 100)    // prepend, max 100
        if message.type == "device_status":
            device_status = message.data.status

    render:
        // Device status indicator (kotak hitam/putih berdasarkan status)
        <status_indicator status={device_status} />

        // Tabel log
        <table columns=[Time, Status, User, Device, UID Hash]>
            for each log in logs:
                <row>
                    <cell>{log.timestamp.toTimeString()}</cell>
                    <cell><badge>{log.status}</badge></cell>
                    <cell bold>{log.action_by}</cell>
                    <cell mono>{log.device_id}</cell>
                    <cell mono truncated>{log.uid_hash[:16]}...</cell>
                </row>
```

### 9.8 Frontend — DoorControl Component

```
component DoorControl():
    state loading = false

    function override(status):
        action = status == 1 ? "GRANT" : "DENY"
        if not confirm("Are you sure you want to {action} access?"):
            return

        loading = true
        try:
            fetchAPI("/api/door/override", {
                method: POST,
                body: {status, message: "Manual Override — ...", action_by: "Dashboard"}
            })
        catch error:
            alert("Override failed: " + error)
        finally:
            loading = false

    render:
        <button onClick={() => override(1)>[ GRANT ]</button>
        <button onClick={() => override(0)>[ DENY ]</button>
```

---

## 10. Fitur Keamanan yang Diimplementasi

| Fitur | Implementasi | Lokasi |
|---|---|---|
| **UID Hashing** | SHA-256 di firmware, double-hash + pepper di backend | `main.cpp:hashUID()` (UHF EPC di-hash), `handler.go:pepperHash()` |
| **Anti-Replay** | Nonce 4-byte random + cache TTL 60 detik | `nonce.go:CheckAndStore()` |
| **Timestamp Validation** | Reject jika >30 detik dari waktu server | `handler.go:processScan()` |
| **Rate Limiting** | 1 pesan per 2 detik per device_id | `handler.go:getLimiter()` menggunakan `golang.org/x/time/rate` |
| **MQTT Auth** | Username/password wajib, anonymous disabled | `mosquitto.conf` |
| **LWT (Last Will)** | Otomatis publish offline jika ESP32 disconnect | `main.cpp:reconnect()` |
| **Watchdog Timer** | ESP32 reboot otomatis jika hang (10s timeout) | `main.cpp:setup()` |
| **Input Validation** | uid_hash harus 64 char hex, device_id tidak boleh kosong | `handler.go:processScan()` |
| **Connection Pool** | Max 25 open, 10 idle, 5 min lifetime | `postgres.go:New()` |
| **Scan Debounce** | Kartu sama dalam 2 detik diabaikan | `main.cpp:loop()` — debounce pada parsing UART UHF |

---

## 11. Wiring Diagram Hardware

```
ESP32 (LoLin32)
┌──────────────────┐
│                  │     HW-VX6330K UHF RFID Reader
│  GPIO 16 (RX2)───┼────── TXD ──── MAX3232 ──── RS232 TX
│  GPIO 17 (TX2)───┼────── RXD ──── MAX3232 ──── RS232 RX
│  3V3 ───────────┼────── VCC  (via MAX3232 level shifter)
│  GND ───────────┼────── GND
│                  │
│  GPIO 25 ────330Ω──── 🟢 Green LED ──── GND
│  GPIO 26 ────330Ω──── 🔴 Red LED ────── GND
│  GPIO 4  ──────────── 🔊 KY-12 Buzzer ─ GND
│  GPIO 2  ──────────── 🔵 Built-in LED   │
└──────────────────┘

Catatan:
- HW-VX6330K menggunakan RS232 (±12V). MAX3232 diperlukan sebagai
  level shifter untuk mengkonversi sinyal RS232 ke TTL 3.3V yang
  kompatibel dengan ESP32.
- UART2 (Serial2) pada ESP32: RX=GPIO16, TX=GPIO17, default baud 9600.
- Baud rate bisa berbeda tergantung konfigurasi HW-VX6330K
  (umumnya 9600 atau 115200).
```

---

## 12. Cara Menjalankan (Development)

```bash
# 1. Konfigurasi secrets
cp infrastructure/.env.example infrastructure/.env
# Isi semua nilai di .env

cp apps/firmware/src/secrets.h.example apps/firmware/src/secrets.h
# Isi SSID, password Wi-Fi, IP broker MQTT, kredensial MQTT

# 2. Jalankan infrastruktur (PostgreSQL + Mosquitto)
docker-compose --env-file infrastructure/.env up -d

# 3. Buat user MQTT di Mosquitto
docker exec -it rfid_mosquitto mosquitto_passwd -c /mosquitto/config/passwd esp32_front_door
docker exec -it rfid_mosquitto mosquitto_passwd /mosquitto/config/passwd go_backend
docker exec -it rfid_mosquitto mosquitto_passwd /mosquitto/config/passwd nextjs_dashboard

# 4. Jalankan Go backend
cd apps/backend
go mod tidy
go run cmd/server/main.go
# Expected: "Connected to PostgreSQL" + "Connected to Mosquitto Broker"

# 5. Jalankan Next.js dashboard
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

## 13. Batasan & Prasyarat Pre-Production

Sebelum deploy ke produksi, hal-hal berikut perlu ditambahkan:

- [ ] Upgrade MQTT ke TLS (port 8883) — gunakan `WiFiClientSecure` di ESP32
- [ ] JWT authentication di semua API route backend
- [ ] NextAuth.js di dashboard — override hanya untuk role `admin`
- [ ] Log aggregation (Loki + Grafana atau Datadog)
- [ ] Kebijakan retensi `access_logs` (cron/pg_cron)
- [ ] MQTT broker terkluster (HiveMQ/EMQX) untuk high availability
- [ ] OTA firmware update via `ArduinoOTA`
