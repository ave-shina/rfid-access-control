# RFID Access Control System — Complete Documentation

---

## 1. About the Project

**RFID Access Control System** is an IoT-based (Pervasive Computing) access control system that decouples the physical device (ESP32 + RFID reader) from the business logic (Go backend) using the MQTT protocol. Core principle: **the edge device only reads cards and responds to commands** — all authorization decisions are made by the backend.

The system controls door access via UHF RFID cards. When someone taps a card, the ESP32 reads the UID from the HW-VX6330K module via UART, hashes it, and sends it to the backend via MQTT. The backend validates it against a PostgreSQL database and sends a command back to the ESP32 to grant access (green LED + 1 beep) or deny (red LED + 3 beeps).

---

## 2. Tech Stack

| Layer | Technology |
|---|---|
| **Firmware (Edge)** | C++ (Arduino framework), PlatformIO, ESP32 |
| **Communication Protocol** | MQTT (Eclipse Mosquitto 2), Wi-Fi (IEEE 802.11) |
| **Backend** | Go 1.22, `paho.mqtt.golang`, `database/sql`, `gorilla/mux`, `gorilla/websocket`, `zerolog` |
| **Database** | PostgreSQL 15 (Docker) |
| **Frontend** | Next.js 14 (App Router), React, TypeScript, Tailwind CSS, shadcn/ui |
| **Infrastructure** | Docker, Docker Compose |

---

## 3. 5-Layer Architecture (Pervasive Computing)

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
│       ESP32 + HW-VX6330K (UHF) + MAX485 + LEDs          │
│       UART2: RX=16, TX=17, DE/RE=27 (RS485)             │
│       Green LED=25, Red LED=26, Buzzer=4, Built-in=2    │
└─────────────────────────────────────────────────────────┘
```

---

## 4. Monorepo Directory Structure

```
rfid-esp32/
├── apps/
│   ├── firmware/                          # ESP32 Edge Device
│   │   ├── platformio.ini                 # Board config + dependencies
│   │   └── src/
│   │       ├── main.cpp                   # Main firmware (~305 lines)
│   │       ├── config.h                   # Constants and pin definitions
│   │       ├── crypto.h                   # hashUID(), generateNonce()
│   │       ├── uhf_reader.h               # readUHFTag(), autoDetectBaud(), RS485 control
│   │       ├── mqtt_handler.h             # reconnect(), publishScan(), broker auto-discovery
│   │       ├── actuators.h                # grantAccess(), denyAccess(), updateStatusLEDs()
│   │       ├── secrets.h                  # gitignored — Wi-Fi/MQTT credentials
│   │       └── secrets.h.example          # Template (committed)
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
    └── .env.example                       # Template (committed)
```

---

## 5. System Workflow (End-to-End Flow)

### 5.1 Main Flow: UHF RFID Card Tap

```
[User taps UHF card]
      │
      ▼
┌──────────────────────────────────────┐
│  ESP32 (main.cpp:loop)              │
│  1. HW-VX6330K reads UHF tag       │
│     via UART2 (RX=16, TX=17)        │
│  2. Parse EPC/UID from response     │
│  3. Debounce: 2-second cooldown     │
│  4. Hash UID → SHA-256 (64 hex)     │
│  5. Generate nonce (4-byte random)  │
│  6. Get timestamp from NTP          │
│  7. Publish to MQTT "door/scan"     │
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
│  3. Validate: uid_hash 64 chars,    │
│     device_id != "", timestamp <30s │
│  4. Anti-replay: nonce unique?      │
│  5. Apply pepper: sha256(hash+pep)  │
│  6. Query DB: SELECT from users     │
│  7. Evaluate:                       │
│     - Found + active → AUTHORIZED   │
│     - Found + inactive → DENIED     │
│     - Not found → DENIED            │
│  8. INSERT into access_logs         │
│  9. Broadcast via WebSocket         │
│  10. Publish command to "door/cmd"  │
│      {status:1/0, message, by}      │
└──────────────┬───────────────────────┘
               │ MQTT QoS 1
               ▼
┌──────────────────────────────────────┐
│  ESP32 (callback)                   │
│  Parse "door/command":              │
│  - status=1 → Green LED + 1 beep   │
│  - status=0 → Red LED + 3 beeps    │
└──────────────────────────────────────┘
```

### 5.2 Real-Time Dashboard Flow

```
┌──────────────────────────────────────┐
│  Go Backend (ws/hub.go)             │
│  - Each access_log → Broadcast      │
│    {type:"access_log", data:log}    │
│  - Device status → Broadcast        │
│    {type:"device_status", data}     │
└──────────────┬───────────────────────┘
               │ WebSocket (:8080/api/ws)
               ▼
┌──────────────────────────────────────┐
│  Next.js Dashboard                  │
│  useWebSocket() hook receives msg   │
│  - access_log → prepend to table   │
│  - device_status → update indicator│
│  Table auto-updates without refresh │
└──────────────────────────────────────┘
```

### 5.3 Manual Override Flow from Dashboard

```
Dashboard ──POST──▶ Next.js BFF ──POST──▶ Go Backend (:8080/api/door/override)
                                                    │
                                                    ├── Publish to MQTT "door/command"
                                                    ├── INSERT into access_logs
                                                    ├── Broadcast via WebSocket
                                                    ▼
                                              ESP32 receives command
```

### 5.4 Device Status Flow (LWT)

```
ESP32 connects to MQTT:
  ├── Publish "door/status" → {"status":"online"} (retained)
  └── Register LWT → {"status":"offline"} (automatic on disconnect)

Mosquitto ──door/status──▶ Go Backend ──WebSocket──▶ Dashboard
```

---

## 6. MQTT Topics and Payload Schemas

| Topic | Publisher | Subscriber | QoS | Payload |
|---|---|---|---|---|
| `door/scan` | ESP32 | Go Backend | **1** | `{uid_hash, device_id, nonce, timestamp}` |
| `door/command` | Go Backend | ESP32 | **1** | `{status, message, action_by}` |
| `door/status` | ESP32 (LWT) | Go Backend | **0** | `{status: "online"/"offline"}` |

### `door/scan` Schema (ESP32 → Backend)

```json
{
  "uid_hash": "64-char-sha256-hex",
  "device_id": "esp32_front_door",
  "nonce": "a3f9c12b",
  "timestamp": 1718000000
}
```

### `door/command` Schema (Backend → ESP32)

```json
{
  "status": 1,
  "message": "Access Granted",
  "action_by": "John Doe"
}
```

- `status: 1` → Green LED + 1 beep
- `status: 0` → Red LED + 3 beeps

---

## 7. Database Schema

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
  status        VARCHAR(20) NOT NULL,          -- 'AUTHORIZED' or 'DENIED'
  action_by     VARCHAR(100),                  -- User name or 'Unknown'
  device_id     VARCHAR(50),                   -- Device ID (multi-door)
  timestamp     TIMESTAMP DEFAULT CURRENT_TIMESTAMP
);

-- Indexes for query performance
CREATE INDEX idx_access_logs_device_time ON access_logs(device_id, timestamp DESC);
CREATE INDEX idx_access_logs_time        ON access_logs(timestamp DESC);
CREATE INDEX idx_access_logs_status      ON access_logs(status);
```

### UID Hashing Process (Double Hash + Pepper)

```
Raw UID (from card): "A1B2C3D4"
         │
         ▼ sha256()
Firmware hash: "8f7a3b...64char"
         │ (sent via MQTT, raw UID is NEVER transmitted)
         ▼
Go Backend: sha256(firmware_hash + pepper)
         │
         ▼
Peppered hash: "e2d9f1...64char"  ← stored in DB, used for lookup
```

---

## 8. API Endpoints (Go Backend)

| Method | Endpoint | Description |
|---|---|---|
| `GET` | `/healthz` | Health check (ping DB) |
| `GET` | `/api/ws` | WebSocket upgrade for real-time events |
| `GET` | `/api/logs?limit=N` | Fetch N most recent logs (default 50) |
| `DELETE` | `/api/logs` | Delete all access logs |
| `GET` | `/api/users` | List all users |
| `POST` | `/api/users` | Register a new user (body: `{name, rfid_uid, role}`) |
| `PATCH` | `/api/users/{id}` | Toggle is_active (body: `{is_active: bool}`) |
| `DELETE` | `/api/users/{id}` | Delete a user |
| `POST` | `/api/door/override` | Manual override (body: `{status: 0/1, message, action_by}`) |

### BFF Layer (Next.js Route Handlers)

The frontend communicates with the backend through route handlers in `src/app/api/` that proxy requests to `http://localhost:8080`. This pattern decouples the frontend from the backend URL and allows adding authentication in the future.

---

## 9. Pseudocode

### 9.1 ESP32 Firmware (`main.cpp`)

```
// SETUP
function setup():
    init_serial(115200)
    init_GPIO(green_led=25 OUTPUT, red_led=26 OUTPUT, buzzer=4 OUTPUT,
              built_in_led=2 OUTPUT, rs485_de=27 OUTPUT)
    rs485_de = LOW                      // MAX485 default: receive mode

    baud = auto_detect_baud()           // try popular baud rates (9600-115200)
    init_UART2(rx=16, tx=17, baud=baud) // HW-VX6330K UHF via MAX485/RS485

    connect_wifi(SSID, PASSWORD)       // blocking until connected
    sync_NTP_time()                     // for accurate timestamps

    mqtt.set_callback(on_message)       // server is set by reconnect() via auto-discovery

    init_watchdog(timeout=30s, panic=true)

// MAIN LOOP
function loop():
    reset_watchdog()
    reconnect()                         // non-blocking, check MQTT connection each iteration

    // Non-blocking actuators: turn off LEDs when duration expires
    if actuator_active AND millis() >= actuator_end:
        turn_off_leds()
        actuator_active = false

    // Debounce: flush UART data during cooldown
    if in_cooldown OR actuator_active:
        flush_uart_buffer()

    // Read tag only if not in cooldown and no actuator running
    if not actuator_active AND not in_cooldown:
        uid = read_uhf_tag()            // parse EPC from inventory frame via RS485

        if uid != "":
            last_uid = uid
            last_scan_time = now
            publish_scan(uid)           // hash, compose JSON, publish to "door/scan"

    // Health check: no data for 30 seconds → reader offline
    check_reader_health()

    update_status_leds()                // Wi-Fi + MQTT indicator LEDs
    mqtt.loop()                         // process incoming messages + keepalive

// MQTT MESSAGE CALLBACK
function on_message(topic, payload):
    if topic == "door/command":
        status = parse_json(payload).status

        if status == 1:
            grant_access()              // Green LED ON, 1 beep, 3s duration (non-blocking)
        else if status == 0:
            deny_access()               // Red LED ON, 3 beeps, 3s duration (non-blocking)

// RECONNECT NON-BLOCKING WITH EXPONENTIAL BACKOFF + AUTO-DISCOVERY
function reconnect():
    if mqtt.connected(): return
    if not enough_time_since_last_attempt(): return  // respect backoff delay

    if wifi_disconnected():
        reconnect_wifi()
        reset_discovery_cache()         // clear cached broker when network changes
        return                          // wait for next iteration

    // ── Broker Auto-Discovery ──
    // If broker not yet found, scan the subnet:
    //   1. Try gateway IP
    //   2. Scan x.x.x.1 through x.x.x.254 for port 1883
    broker_ip = get_broker_ip()         // use cache if previously discovered
    if broker_ip == "":
        fail_count++
        if fail_count >= 10:
            reboot_esp32()              // full recovery
        return

    mqtt.set_server(broker_ip, 1883)

    // connect with LWT (Last Will and Testament)
    success = mqtt.connect(
        client_id="esp32_front_door",
        auth=(user, pass),
        lwt_topic="door/status",        // QoS 0, retained=true
        lwt_payload={"status":"offline"}
    )

    if success:
        fail_count = 0
        mqtt.subscribe("door/command", QoS=1)
        mqtt.publish("door/status", {"status":"online"}, retained=true)
        backoff = 1000ms               // reset backoff
    else:
        fail_count++
        if fail_count >= 10:
            reboot_esp32()              // full recovery
        backoff = min(backoff * 2, 5000ms)  // 1s→2s→4s→5s (max)
```

### 9.2 Go Backend — MQTT Handler (`handler.go`)

```
// CALLED EVERY TIME A MESSAGE ARRIVES ON "door/scan"
function handle_scan(raw_message):
    go process_scan(raw_message)        // run in a separate goroutine

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
        return                          // nonce already seen within 60 seconds

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

    // 6. LOG TO DATABASE (logging failure MUST NOT block actuation)
    access_log = {peppered_hash, status, action_by, device_id, now()}
    try:
        db.insert("access_logs", access_log)
        ws_hub.broadcast({type: "access_log", data: access_log})
    catch error:
        log.error("failed to log", error)
        // STILL PROCEED to actuation

    // 7. ACTUATE DOOR
    command = {status: cmd_status, message, action_by}
    mqtt.publish("door/command", command, QoS=1)
```

### 9.3 Go Backend — HTTP API (`main.go`)

```
// POST /api/users — Register a new user
function create_user(request):
    body = parse_json(request)
    validate(body.name != "" AND body.rfid_uid != "")

    // Double hash: sha256(rawUID) then sha256(hash + pepper)
    firmware_hash = sha256(body.rfid_uid)
    peppered_hash = sha256(firmware_hash + PEPPER)

    user = db.insert("users", {
        name: body.name,
        rfid_uid_hash: peppered_hash,
        role: body.role || "employee"
    })

    return json(user)

// POST /api/door/override — Manual override from dashboard
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
    cached_device_status: bytes          // cache last status

    function register(conn):
        clients[conn] = true
        if cached_device_status != null:
            conn.send(cached_device_status)   // send status immediately to new client

    function unregister(conn):
        delete clients[conn]
        conn.close()

    function broadcast(message):
        data = json.encode(message)

        if message.type == "device_status":
            cached_device_status = data        // cache for new clients

        for each conn in clients:
            try:
                conn.send(data)
            catch:
                go unregister(conn)           // dead connection, remove

    function serve_websocket(http_request):
        conn = upgrade_to_websocket(http_request)
        register(conn)

        // Read loop — detect disconnect
        go function():
            while true:
                try: conn.read()
                catch: break                  // client disconnected
            unregister(conn)

        // Ping loop — keep-alive
        go function():
            every 30 seconds:
                try: conn.ping()
                catch: break                  // dead connection
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
        return true                        // nonce is fresh, stored

    // Background cleanup every 30 seconds
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

    // Subscribe to real-time updates
    useWebSocket(on_message):
        if message.type == "access_log":
            logs = [message.data, ...logs].slice(0, 100)    // prepend, max 100
        if message.type == "device_status":
            device_status = message.data.status

    render:
        // Device status indicator (black/white box based on status)
        <status_indicator status={device_status} />

        // Log table
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

## 10. Implemented Security Features

| Feature | Implementation | Location |
|---|---|---|
| **UID Hashing** | SHA-256 on firmware, double-hash + pepper on backend | `crypto.h:hashUID()` (UHF EPC hashed), `handler.go:pepperHash()` |
| **Anti-Replay** | 4-byte random nonce + cache TTL 60 seconds | `nonce.go:CheckAndStore()` |
| **Timestamp Validation** | Reject if >30 seconds from server time | `handler.go:processScan()` |
| **Rate Limiting** | 1 message per 2 seconds per device_id | `handler.go:getLimiter()` using `golang.org/x/time/rate` |
| **MQTT Auth** | Username/password required, anonymous disabled | `mosquitto.conf` |
| **LWT (Last Will)** | Automatically publishes offline if ESP32 disconnects | `mqtt_handler.h:reconnect()` |
| **Watchdog Timer** | ESP32 auto-reboots if hung (30s timeout) | `main.cpp:setup()` |
| **Input Validation** | uid_hash must be 64 char hex, device_id must not be empty | `handler.go:processScan()` |
| **Connection Pool** | Max 25 open, 10 idle, 5 min lifetime | `postgres.go:New()` |
| **Scan Debounce** | Same card within 2 seconds is ignored | `main.cpp:loop()` — debounce on UART UHF parsing |

---

## 11. Hardware Wiring Diagram

### 11.1 Complete System Wiring

```
                         ┌─────────────────────────────────────────────────────┐
                         │              ESP32 (LoLin32)                        │
                         │                                                     │
   ┌──────────────┐      │  GPIO 16 (RX2) ←──── MAX485 RO       ┐            │
   │ HW-VX6330K   │      │  GPIO 17 (TX2) ────→ MAX485 DI       │            │
   │ UHF Reader   │      │  GPIO 27        ────→ MAX485 DE+RE    │            │
   │ (Active Mode)│      │  3V3            ────→ MAX485 VCC      │            │
   │              │      │  GND            ────→ MAX485 GND      │            │
   │  Ext. Power  │      │                                     │            │
   │  (12V DC)    │      │  GPIO 25 ──── 330Ω ──── Green LED ── GND         │
   └──────┬───────┘      │  GPIO 26 ──── 330Ω ──── Red LED ──── GND         │
          │              │  GPIO 5  ──── 330Ω ──── MQTT LED ─── GND         │
          │              │  GPIO 4  ──────────── KY-12 Buzzer ── GND         │
          │              │  GPIO 2  ──────────── Built-in LED                │
          │              └─────────────────────────────────────────────────────┘
          │
          ▼
   ┌──────────────┐
   │ MAX485       │        ┌──────────────────────────────┐
   │ Transceiver  │        │ HW-VX6330K UHF Reader        │
   │              │        │                              │
   │ TTL Side:    │        │ RS485 Terminals:             │
   │  DI ← GPIO17 │        │  A+ (DATA+) ─── A ────────→ │
   │  RO → GPIO16 │   ──── │  B- (DATA-) ─── B ────────→ │
   │  DE+RE←GPIO27│        │  GND         ─── GND ─────→ │
   │  VCC → 3V3   │        │                              │
   │  GND → GND   │        │  Power: 12V DC external     │
   │              │        └──────────────────────────────┘
   │ RS485 Side:  │
   │  A ─────────→ │   Reader terminal A
   │  B ─────────→ │   Reader terminal B
   └──────────────┘

   Termination: 120Ω resistor between A and B at both ends of the bus
```

### 11.2 MAX485 → ESP32 Pinout

| MAX485 Pin | → | ESP32 GPIO | Notes |
|---|---|---|---|
| DI | ← | GPIO 17 (TX2) | ESP32 UART2 TX → MAX485 Data In |
| RO | → | GPIO 16 (RX2) | MAX485 Receiver Out → ESP32 UART2 RX |
| DE + RE | ← | GPIO 27 | Direction control (tied together): HIGH=TX, LOW=RX |
| VCC | → | 3V3 | MAX485 powered from ESP32 (3.3V compatible) |
| GND | → | GND | Common ground |

### 11.3 MAX485 → Reader (RS485 Side)

| MAX485 Pin | → | Reader Terminal | Notes |
|---|---|---|---|
| A | → | A+ (DATA+) | Differential pair (non-inverting) |
| B | → | B- (DATA-) | Differential pair (inverting) |

**Termination:** Place a **120Ω termination resistor** between A and B at each end of the RS485 bus (one at the MAX485, one at the reader) to prevent signal reflections.

> **MAX485 is required.** The HW-VX6330K uses RS485 differential signaling. A MAX485 transceiver converts between RS485 and 3.3V TTL. Direct connection to ESP32 GPIO will not work and may damage the chip.

> **DE and RE pins are tied together** to GPIO 27 for simple direction control. HIGH = transmit mode, LOW = receive mode (default).

> **Reader has external power** (12V DC). Power is NOT provided through the data connector.

### 11.4 MAX485 Module Details

| Spec | Value |
|---|---|
| Function | RS485 ↔ TTL (3.3V) half-duplex transceiver |
| Chip | MAX485 (or compatible like SP3485) |
| Power | 3.3V from ESP32 |
| TTL header | 4-pin (DI, RO, DE+RE, VCC, GND) |
| RS485 connection | Screw terminals or header (A, B) |
| Termination | 120Ω resistor between A and B (at each bus end) |

### 11.5 Actuators Wiring

| Component | ESP32 GPIO | Connection |
|---|---|---|
| Green LED | GPIO 25 | Via 330Ω resistor to GND (active-LOW) |
| Red LED | GPIO 26 | Via 330Ω resistor to GND (active-LOW) |
| MQTT LED | GPIO 5 | Via 330Ω resistor to GND (active-LOW) |
| KY-12 Buzzer | GPIO 4 | Direct to GND (active buzzer, active-HIGH) |
| Built-in LED | GPIO 2 | Onboard (active-HIGH) |

### 11.6 Troubleshooting Guide

| Symptom | Cause | Solution |
|---|---|---|
| No data on Serial Monitor | A/B swapped | Swap A and B wires on the RS485 side |
| No data on Serial Monitor | DE/RE not connected or stuck HIGH | Ensure GPIO 27 is connected to MAX485 DE+RE, defaults LOW |
| Garbage data | Wrong baud rate | Firmware auto-detects — make sure a tag is near the reader at boot |
| Garbage data | No 120Ω termination | Add 120Ω resistor between A and B |
| Invalid frames | UART TX/RX swapped | Check GPIO 16=RX (from MAX485 RO), GPIO 17=TX (to MAX485 DI) |
| Reader not sending | No external power | Reader needs separate 12V power supply |
| Baud auto-detect fails | Bad wiring or no tag | Check A/B connections, ensure a UHF tag is near the reader at boot |

### 11.7 Broker Auto-Discovery (MQTT)

The ESP32 no longer uses a hardcoded broker IP. Instead, the firmware performs **auto-discovery** to find the MQTT broker on the current subnet:

1. **Try gateway IP** — TCP probe to `<gateway>:1883`
2. **Scan subnet** — TCP probe to `x.x.x.1` through `x.x.x.254` on port 1883
3. **Cache result** — discovered broker IP is cached until WiFi disconnects

> **Note:** `MQTT_SERVER` in `secrets.h` is still defined but unused by the firmware. Auto-discovery completely replaces manual configuration.

### 11.8 Loopback Test (Verify MAX485 + ESP32)

To verify the MAX485 and ESP32 wiring independently of the reader:

1. Disconnect reader from the MAX485 RS485 side
2. Short terminals A and B on the MAX485 RS485 side together
3. Set GPIO 27 HIGH (transmit mode), send data via Serial2
4. Set GPIO 27 LOW (receive mode), check if the same data comes back
5. If data returns → MAX485 + ESP32 wiring is correct
6. If no data → Check MAX485 power, DI/RO wiring, or swap GPIO 16/17

---

## 12. How to Run (Development)

```bash
# 1. Configure secrets
cp infrastructure/.env.example infrastructure/.env
# Fill in all values in .env

cp apps/firmware/src/secrets.h.example apps/firmware/src/secrets.h
# Fill in SSID, Wi-Fi password, MQTT broker IP, MQTT credentials

# 2. Start infrastructure (PostgreSQL + Mosquitto)
docker-compose --env-file infrastructure/.env up -d

# 3. Create MQTT users in Mosquitto
docker exec -it rfid_mosquitto mosquitto_passwd -c /mosquitto/config/passwd esp32_front_door
docker exec -it rfid_mosquitto mosquitto_passwd /mosquitto/config/passwd go_backend
docker exec -it rfid_mosquitto mosquitto_passwd /mosquitto/config/passwd nextjs_dashboard

# 4. Run Go backend
cd apps/backend
go mod tidy
go run cmd/server/main.go
# Expected: "Connected to PostgreSQL" + "Connected to Mosquitto Broker"

# 5. Run Next.js dashboard
cd apps/frontend
npm install
npm run dev
# Dashboard: http://localhost:3000

# 6. Flash ESP32 firmware
# Open apps/firmware/ in VS Code + PlatformIO
# Make sure secrets.h is filled in
# Click Upload and Monitor
```

---

## 13. Limitations & Pre-Production Requirements

The following items need to be added before deploying to production:

- [ ] Upgrade MQTT to TLS (port 8883) — use `WiFiClientSecure` on ESP32
- [ ] JWT authentication on all backend API routes
- [ ] NextAuth.js on the dashboard — override restricted to `admin` role only
- [ ] Log aggregation (Loki + Grafana or Datadog)
- [ ] `access_logs` retention policy (cron/pg_cron)
- [ ] Clustered MQTT broker (HiveMQ/EMQX) for high availability
- [ ] OTA firmware update via `ArduinoOTA`
