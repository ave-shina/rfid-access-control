# CLAUDE.md — RFID Access Control System

This file provides Claude Code with the context, architecture, and conventions needed to work effectively on this codebase.

---

## Project Overview

An enterprise-grade, event-driven **RFID Access Control System** built on Pervasive Computing (IoT) principles. The system decouples physical RFID scanning (ESP32 edge device) from credential validation (Go backend) using the MQTT pub/sub protocol, with a Next.js dashboard for real-time monitoring and remote control.

**Core philosophy:** The edge device is a "dumb sensor" — it only reads cards and reacts to commands. All business logic lives in the backend.

---

## Monorepo Structure

```
/
├── apps/
│   ├── firmware/               # C++ (Arduino/PlatformIO) — ESP32 edge device
│   │   ├── platformio.ini
│   │   └── src/
│   │       ├── main.cpp
│   │       ├── secrets.h           # gitignored — copy from secrets.h.example
│   │       └── secrets.h.example   # committed — empty values only
│   ├── backend/                # Go — MQTT handler, DB validation, WebSocket broadcast
│   │   ├── cmd/server/
│   │   │   └── main.go         # Entry point
│   │   ├── internal/
│   │   │   ├── db/
│   │   │   │   └── postgres.go # Connection pool & SQL queries
│   │   │   ├── mqtt/
│   │   │   │   └── handler.go  # Pub/Sub subscriptions & publishing
│   │   │   └── models/
│   │   │       └── schema.go   # Structs mapped to PostgreSQL tables
│   │   ├── go.mod
│   │   └── go.sum
│   └── frontend/               # Next.js 14 (App Router) — Admin dashboard
│       └── src/
│           ├── app/
│           │   ├── page.tsx            # Main dashboard (real-time logs)
│           │   ├── users/page.tsx      # User credential management
│           │   ├── api/                # Next.js Route Handlers (BFF)
│           │   └── layout.tsx
│           ├── components/
│           │   ├── LiveLogTable.tsx    # Real-time access log table
│           │   └── DoorControl.tsx     # Manual override button
│           └── lib/
│               └── websocket.ts        # WebSocket connection utility
└── infrastructure/
    ├── docker-compose.yml
    ├── mosquitto.conf
    ├── .env.example            # committed — template for all secrets
    ├── .env                    # gitignored — fill from .env.example
    └── init.sql                # PostgreSQL schema seed
```

---

## Architecture: 5-Layer Pervasive Computing Model

| Layer | Role | Components |
|---|---|---|
| **Perception** | Sense & actuate the physical world | ESP32, HW-VX6330K UHF RFID reader (via MAX3232), LEDs, KY-12 buzzer |
| **Network** | Message transport | Wi-Fi (IEEE 802.11), MQTT over TCP |
| **Middleware** | Routing & processing | Eclipse Mosquitto broker, Go backend service |
| **Data** | Persistent storage | PostgreSQL (users + access_logs tables) |
| **Application** | Human interface | Next.js 14 dashboard |

---

## Tech Stack

| Layer | Technology |
|---|---|
| Edge Firmware | C++ (Arduino framework), PlatformIO |
| Backend | Go (Golang), `paho.mqtt.golang`, `database/sql`, `zerolog` |
| Database | PostgreSQL 15 (Docker) |
| MQTT Broker | Eclipse Mosquitto 2 (Docker) |
| Frontend | Next.js 14, React, TypeScript, Tailwind CSS, shadcn/ui |
| Infrastructure | Docker & Docker Compose |

---

## Frontend Design

### UI Framework

- **shadcn/ui** — headless components built on Radix UI primitives, styled with Tailwind CSS
- Components live in `apps/frontend/src/components/ui/` (Button, Card, Table, Badge, Input, Separator)
- Utility function `cn()` from `@/lib/utils` merges Tailwind classes via `clsx` + `tailwind-merge`

### Theme: Retro Black & White

The dashboard uses a **dark-first, black-and-white retro terminal aesthetic**:

- **Colors:** Pure grayscale palette — no accent colors. Background is near-black (`hsl(0 0% 3%)`), foreground is near-white (`hsl(0 0% 93%)`). All UI elements use shades of gray between.
- **Borders:** All borders are 2px solid (thick, Brutalist). No rounded corners (`--radius: 0px`).
- **Shadows:** Offset box-shadow (`shadow-[4px_4px_0px_0px_hsl(var(--foreground))]`) for a retro card effect.
- **Typography:** All text uses `uppercase` + `tracking-wider/widest` for a terminal/teletype feel. Monospace preferred.
- **Scanlines:** A CSS `repeating-linear-gradient` overlay (`.scanlines` utility class) simulates a CRT scanline effect.
- **CRT glow:** The `.crt-glow` utility adds a subtle `text-shadow` for headings.
- **Buttons:** Styled with `border-2`, uppercase `[ BRACKET ]` labels, and invert-on-hover (`bg-foreground text-background`).
- **Badges:** Thick bordered, uppercase, high-contrast — no color coding (status shown by fill vs outline).
- **Tables:** Header rows use `bg-foreground/10` with `uppercase tracking-wider` column headers.

### Theme Files

| File | Purpose |
|---|---|
| `tailwind.config.ts` | shadcn/ui CSS variable colors, `tailwindcss-animate` plugin, retro keyframes |
| `src/app/globals.css` | CSS variables for light/dark themes, `.scanlines`, `.crt-glow`, `.terminal-border` utilities |
| `components.json` | shadcn/ui CLI config (used when adding new shadcn components) |

### Adding New shadcn/ui Components

```bash
cd apps/frontend
npx shadcn-ui@latest add [component-name]
```

After adding, restyle the component to match the retro theme:
- Replace `border` with `border-2 border-foreground`
- Replace `rounded-md` with no rounding (remove or set to `rounded-none`)
- Add uppercase + tracking-wider to text
- Use the offset shadow pattern for cards/containers

---

## Secrets Management

> **Never hardcode credentials.** All secrets are loaded from environment variables. `.env` and `secrets.h` are gitignored — only their `.example` counterparts are committed.

### `infrastructure/.env.example` (commit this, never `.env`)

```env
# PostgreSQL
POSTGRES_USER=admin
POSTGRES_PASSWORD=
POSTGRES_DB=iot_access_db

# Go backend
DATABASE_URL=postgres://admin:PASSWORD@localhost:5432/iot_access_db
JWT_SECRET=
UID_PEPPER=        # server-side secret mixed into UID hashes

# MQTT broker credentials
MQTT_USERNAME_BACKEND=go_backend
MQTT_PASSWORD_BACKEND=
MQTT_USERNAME_DASHBOARD=nextjs_dashboard
MQTT_PASSWORD_DASHBOARD=
```

### `apps/firmware/src/secrets.h.example` (commit this, never `secrets.h`)

```cpp
#define WIFI_SSID     ""
#define WIFI_PASSWORD ""
#define MQTT_SERVER   ""
#define MQTT_USER     "esp32_front_door"
#define MQTT_PASS     ""
```

### Loading secrets in each layer

**Go backend** — read from environment at startup, never from source:
```go
dbURL     := os.Getenv("DATABASE_URL")    // required — panic if empty
jwtSecret := os.Getenv("JWT_SECRET")      // required
uidPepper := os.Getenv("UID_PEPPER")      // required
mqttUser  := os.Getenv("MQTT_USERNAME_BACKEND")
mqttPass  := os.Getenv("MQTT_PASSWORD_BACKEND")
```

**Docker Compose** — reads `.env` automatically (same directory as `docker-compose.yml`):
```yaml
environment:
  POSTGRES_USER:     ${POSTGRES_USER}
  POSTGRES_PASSWORD: ${POSTGRES_PASSWORD}
```

---

## MQTT Configuration

### Topic Registry

| Topic | Publisher | Subscriber | QoS | Purpose |
|---|---|---|---|---|
| `door/scan` | ESP32 | Go Backend | **1** | Edge reports a newly scanned RFID UID |
| `door/command` | Go Backend, Next.js UI | ESP32 | **1** | Server instructs hardware to grant/deny access |
| `door/status` | ESP32 (+ LWT) | Next.js UI | 0 | Heartbeat / offline detection |

> **QoS levels matter.** `door/scan` and `door/command` use **QoS 1** (at-least-once) so a dropped packet never silently fails to grant or deny access. The heartbeat uses QoS 0 because the LWT handles disconnections automatically.

### Broker Authentication

Anonymous connections must be disabled in `mosquitto.conf`:

```conf
allow_anonymous false
password_file /mosquitto/config/passwd
listener 1883
```

Create per-client credentials (run once, store passwords in `.env`):
```bash
docker exec -it mosquitto mosquitto_passwd -c /mosquitto/config/passwd esp32_front_door
docker exec -it mosquitto mosquitto_passwd    /mosquitto/config/passwd go_backend
docker exec -it mosquitto mosquitto_passwd    /mosquitto/config/passwd nextjs_dashboard
```

Each client authenticates with its own username/password — loaded from environment variables, never hardcoded.

### Last Will and Testament (LWT)

The ESP32 registers an LWT on connect so the broker automatically publishes an offline event if the device disconnects unexpectedly:

```cpp
// Set before mqttClient.connect()
mqttClient.setWill("door/status", "{\"status\":\"offline\"}", false, 0);
```

The Go backend subscribes to `door/status` and forwards offline alerts to the Next.js dashboard via WebSocket.

### Payload Schemas

**Inbound** (`door/scan` — ESP32 → Server):
```json
{
  "uid_hash": "64-char-sha256-hex",
  "device_id": "esp32_front_door",
  "nonce": "a3f9c12b",
  "timestamp": 1718000000
}
```

> The ESP32 sends a SHA-256 hash of the raw UID — never the raw UID itself. The `nonce` (random 4-byte hex, generated per scan) and `timestamp` are validated by the backend to reject replayed packets.

**Outbound** (`door/command` — Server → ESP32):
```json
{
  "status": 1,
  "message": "Access Granted",
  "action_by": "System"
}
```
- `status: 1` → Green LED + 1 beep (Authorized)
- `status: 0` → Red LED + 3 beeps (Denied)

---

## Database Schema

```sql
-- Authorized users mapped to RFID tags
-- rfid_uid_hash stores SHA-256(rawUID + PEPPER) — never the raw UID
CREATE TABLE users (
  id            SERIAL PRIMARY KEY,
  name          VARCHAR(100) NOT NULL,
  rfid_uid_hash VARCHAR(64) UNIQUE NOT NULL,
  role          VARCHAR(50) DEFAULT 'employee',
  is_active     BOOLEAN DEFAULT TRUE,
  created_at    TIMESTAMP DEFAULT CURRENT_TIMESTAMP
);

-- Immutable audit trail of all access attempts
CREATE TABLE access_logs (
  id            SERIAL PRIMARY KEY,
  rfid_uid_hash VARCHAR(64) NOT NULL,
  status        VARCHAR(20) NOT NULL,   -- 'AUTHORIZED' or 'DENIED'
  action_by     VARCHAR(100),           -- User name or 'Unknown'
  device_id     VARCHAR(50),            -- Which door was accessed
  timestamp     TIMESTAMP DEFAULT CURRENT_TIMESTAMP
);

-- Required indexes — access_logs grows unbounded; queries will time out without these
CREATE INDEX idx_access_logs_device_time ON access_logs(device_id, timestamp DESC);
CREATE INDEX idx_access_logs_time        ON access_logs(timestamp DESC);
CREATE INDEX idx_access_logs_status      ON access_logs(status);
```

> **RFID UIDs are stored as peppered SHA-256 hashes.** If the database is ever breached, raw UIDs cannot be recovered and cloned onto blank cards. When registering a user, hash on the backend: `hex(sha256(rawUID + os.Getenv("UID_PEPPER")))`.

### Data Retention

`access_logs` will grow indefinitely. Schedule a periodic cleanup matching your compliance requirements:

```sql
-- Example: delete logs older than 90 days (run via cron or pg_cron)
DELETE FROM access_logs WHERE timestamp < NOW() - INTERVAL '90 days';
```

---

## Go Backend

### Validation Flow

Each incoming MQTT message on `door/scan` is handled inside an independent Goroutine:

1. **Rate-limit check** — reject if this `device_id` has exceeded 1 message per 2 seconds
2. **Input validation** — reject if `uid_hash` is not exactly 64 hex chars, `device_id` is empty, or `timestamp` is older than 30 seconds
3. **Anti-replay check** — reject if `nonce` has been seen in the last 60 seconds (use a TTL map or Redis)
4. **Query** — `SELECT name, is_active FROM users WHERE rfid_uid_hash = $1`
5. **Evaluate**:
   - Hash found AND `is_active = true` → Authorized
   - Hash not found OR `is_active = false` → Denied
6. **Log** — `INSERT` into `access_logs`; if this fails, log the error but **still actuate the door** — never block physical access due to a logging failure
7. **Actuate** — publish `{"status": 1}` or `{"status": 0}` to `door/command` at QoS 1

### Database Connection Pool

Configure limits to prevent exhausting PostgreSQL connections under load:

```go
db, err := sql.Open("postgres", os.Getenv("DATABASE_URL"))
db.SetMaxOpenConns(25)
db.SetMaxIdleConns(10)
db.SetConnMaxLifetime(5 * time.Minute)
```

### Rate Limiting

Protect the backend from a malfunctioning or malicious edge device flooding `door/scan`:

```go
import "golang.org/x/time/rate"

var limiters sync.Map // map[string]*rate.Limiter keyed by device_id

func getLimiter(deviceID string) *rate.Limiter {
    lim, _ := limiters.LoadOrStore(deviceID, rate.NewLimiter(rate.Every(2*time.Second), 1))
    return lim.(*rate.Limiter)
}

// In the MQTT message handler — before any DB work:
if !getLimiter(payload.DeviceID).Allow() {
    log.Warn().Str("device", payload.DeviceID).Msg("rate limit exceeded, dropping scan")
    return
}
```

### Structured Logging

Use `zerolog` for structured JSON output compatible with log aggregators:

```go
import "github.com/rs/zerolog/log"

log.Info().
    Str("device_id", payload.DeviceID).
    Str("status", "AUTHORIZED").
    Str("user", userName).
    Msg("access granted")
```

Never use `fmt.Println` or `log.Printf` — unstructured logs are unsearchable in production.

### Health Check Endpoint

Expose `/healthz` so Docker health checks, load balancers, and uptime monitors can verify the backend:

```go
http.HandleFunc("/healthz", func(w http.ResponseWriter, r *http.Request) {
    if err := db.Ping(); err != nil {
        w.WriteHeader(http.StatusServiceUnavailable)
        return
    }
    w.WriteHeader(http.StatusOK)
})
```

---

## Firmware (ESP32)

### Offline Fail-Secure Policy

> **Decision: this system is fail-secure.** If the ESP32 loses connection to the MQTT broker, the door stays locked. No access is granted without an explicit server-validated `{"status": 1}` response.

The ESP32 must never self-authorize. Ensure physical key backup exists for emergency access during extended outages.

### Wi-Fi + MQTT Reconnection with Backoff

```cpp
void reconnect() {
  int delayMs = 1000;
  while (!mqttClient.connected()) {
    if (WiFi.status() != WL_CONNECTED) {
      WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
      delay(delayMs);
      delayMs = min(delayMs * 2, 30000);
      continue;
    }
    // LWT registered on every connect attempt
    if (mqttClient.connect("esp32_front_door", MQTT_USER, MQTT_PASS,
                           "door/status", 0, false, "{\"status\":\"offline\"}")) {
      mqttClient.subscribe("door/command", 1); // QoS 1
      mqttClient.publish("door/status", "{\"status\":\"online\"}", false);
      delayMs = 1000; // reset backoff
    } else {
      delay(delayMs);
      delayMs = min(delayMs * 2, 30000);
    }
  }
}
```

Call `reconnect()` at the top of `loop()` before any scan logic.

### Watchdog Timer

Enables automatic reboot if the firmware hangs:

```cpp
#include "esp_task_wdt.h"

void setup() {
  esp_task_wdt_init(10, true); // 10-second timeout, panic on trigger
  esp_task_wdt_add(NULL);
  // ... rest of setup
}

void loop() {
  esp_task_wdt_reset(); // must be called every iteration
  reconnect();
  // ... scan logic
}
```

### RFID Read Debounce

Without a cooldown, one card tap fires many duplicate publishes:

```cpp
String lastUID = "";
unsigned long lastScanTime = 0;
const unsigned long SCAN_COOLDOWN_MS = 2000;

void loop() {
  esp_task_wdt_reset();
  reconnect();

  // Read UHF tag via UART2 (Serial2)
  String uid = readUHFTag(); // parse EPC from HW-VX6330K response
  if (uid.length() == 0) {
    mqttClient.loop();
    return;
  }

  unsigned long now = millis();

  if (uid == lastUID && (now - lastScanTime) < SCAN_COOLDOWN_MS) return;

  lastUID = uid;
  lastScanTime = now;

  publishScan(uid); // hash, add nonce + timestamp, publish to door/scan at QoS 1
}
```

### UID Hashing on the Firmware

The raw UID is hashed on the ESP32 before publishing — it never leaves the device in plaintext:

```cpp
#include <SHA256.h> // add to lib_deps: arduino-libraries/Arduino_CryptoLibrary

String hashUID(String rawUID) {
  SHA256 sha;
  sha.update((const uint8_t*)rawUID.c_str(), rawUID.length());
  uint8_t digest[32];
  sha.finalize(digest, 32);
  String hex = "";
  for (int i = 0; i < 32; i++) {
    if (digest[i] < 16) hex += "0";
    hex += String(digest[i], HEX);
  }
  return hex;
}
```

> Note: the pepper (server-side secret) is applied by the Go backend when comparing against the database — not on the firmware. The firmware hashes the raw UID with SHA-256 only. The backend compares `sha256(incomingHash + pepper)` against the stored peppered hash.

---

## Hardware Wiring Reference

### ESP32 ↔ HW-VX6330K UHF Reader (UART via MAX3232)

The reader has a DB9 male connector. Connection requires RS232 null-modem (cross) wiring through a MAX3232 level shifter:

**Reader Cable Pinout:**

| Wire Color | DB9 Male Pin | Signal |
|---|---|---|
| Pink | Pin 3 | TXD (data from reader) |
| White | Pin 2 | RXD (data to reader) |
| Brown | Pin 5 | GND |

**Null-Modem Cross Wiring (Reader DB9 → MAX3232 DB9 Female):**

| Reader DB9 Male | → | MAX3232 DB9 Female | Signal |
|---|---|---|---|
| Pin 3 (TXD) | → | **Pin 2** (RX in) | Data from reader |
| Pin 2 (RXD) | → | **Pin 3** (TX out) | Data to reader |
| Pin 5 (GND) | → | **Pin 5** (GND) | Common ground |

**MAX3232 TTL Side → ESP32:**

| MAX3232 TTL Pin | → | ESP32 GPIO | Notes |
|---|---|---|---|
| TX | → | GPIO 16 (RX2) | TTL → ESP32 UART2 RX |
| RX | ← | GPIO 17 (TX2) | ESP32 UART2 TX → TTL |
| VCC | → | 3V3 or 5V/VIN | MAX3232 powered from ESP32 |
| GND | → | GND | Common ground |

**Physical Connection Method:**
Standard Dupont female connectors cannot grip DB9 male pins reliably. Use a **DB9 female-to-female straight-through converter** as an adapter:

```
Reader DB9 male → Female-to-Female converter → Male jumper wires → MAX3232 DB9 female
                   (good contact with           (cross-wired:
                    DB9 male pins)               Pin 3→Pin 2, Pin 2→Pin 3, Pin 5→Pin 5)
```

> **MAX3232 is required.** The HW-VX6330K uses RS232 voltage levels (±12V). Direct connection to ESP32 GPIO will destroy the chip. MAX3232 converts RS232 ↔ 3.3V TTL.

> **Reader has external power** (12V DC). Power is NOT provided through the DB9 connector.

### Actuators

| Component | GPIO | Notes |
|---|---|---|
| Green LED | GPIO 25 | Via 330Ω resistor |
| Red LED | GPIO 26 | Via 330Ω resistor |
| KY-12 Buzzer | GPIO 4 | Active buzzer — digital HIGH only, no PWM needed |
| Built-in LED | GPIO 2 | Onboard LED (useful for debug) |

---

## Development Setup

### Prerequisites

- Docker Desktop
- Go 1.22+
- Node.js 20+
- VS Code + PlatformIO extension

### 1. Configure secrets

```bash
cp infrastructure/.env.example infrastructure/.env
# Fill in all values in infrastructure/.env

cp apps/firmware/src/secrets.h.example apps/firmware/src/secrets.h
# Fill in Wi-Fi SSID, password, MQTT broker IP and credentials in secrets.h
```

### 2. Start infrastructure

```bash
docker-compose --env-file infrastructure/.env up -d
```

Starts PostgreSQL on `5432` and Mosquitto on `1883` / `9001`.

### 3. Run Go backend

```bash
cd apps/backend
go mod tidy
go run cmd/server/main.go
```

Expected: `{"level":"info","msg":"Connected to PostgreSQL"}` and `{"level":"info","msg":"Connected to Mosquitto Broker"}`

### 4. Run Next.js dashboard

```bash
cd apps/frontend
npm install
npm run dev
```

Dashboard at `http://localhost:3000`

### 5. Flash ESP32 firmware

- Open `apps/firmware/` in VS Code with PlatformIO
- Confirm `apps/firmware/src/secrets.h` exists and is filled in
- Click **Upload and Monitor** in the PlatformIO toolbar

---

## Key Conventions & Rules

- **Never store raw RFID UIDs anywhere** — store and transmit only SHA-256 hashes.
- **Never store RFID UIDs on the ESP32** — credentials live only in PostgreSQL.
- **Never hardcode secrets** — all credentials come from `.env` or `secrets.h` (both gitignored).
- **Never use HTTP polling** for device-server communication — all real-time events go through MQTT.
- **Every MQTT handler runs in its own Goroutine** — never block the main thread.
- **All MQTT payloads are JSON** — maintain strict schema compatibility between C++ and Go.
- **`device_id` is mandatory** in all `door/scan` payloads to support multi-door scaling.
- **The HW-VX6330K uses RS232 voltage levels** — always use MAX3232 level shifter between reader and ESP32 GPIO. Direct connection will destroy the ESP32.
- **Log every access attempt** (authorized and denied) before publishing the command. If logging fails, still actuate the door.
- **Use QoS 1** for `door/scan` and `door/command`. Use QoS 0 only for heartbeats.
- **Use structured logging** (`zerolog`) throughout the Go backend — no `fmt.Println`.
- **The door is fail-secure** — no server response means the door stays locked.

---

## Security Checklist

### Development hardening (implement from day one)

- [ ] RFID UIDs hashed (SHA-256 + server-side pepper) before storage
- [ ] MQTT broker requires username/password — anonymous connections disabled
- [ ] Secrets loaded from `.env` / `secrets.h` — never hardcoded in source
- [ ] Anti-replay: nonce + 30-second timestamp window validated on every `door/scan`
- [ ] RFID read debounce (2s cooldown) prevents duplicate publishes from firmware (UHF UART parsing)
- [ ] Rate limiting per `device_id` in Go backend (max 1 message per 2 seconds)
- [ ] Database connection pool limits set (`SetMaxOpenConns`, `SetMaxIdleConns`)
- [ ] Watchdog timer enabled on ESP32
- [ ] LWT configured on ESP32 for automatic offline detection
- [ ] `access_logs` indexes created for `device_id + timestamp`, `timestamp`, `status`
- [ ] `/healthz` endpoint on Go backend

### Pre-production (required before live deployment)

- [ ] Upgrade MQTT `1883` → `8883` (MQTTS/TLS), use `WiFiClientSecure` on ESP32
- [ ] JWT authentication on all Go backend API routes (especially `POST /api/door/override`)
- [ ] NextAuth.js on the dashboard; override restricted to `admin` role only
- [ ] Structured log aggregation pipeline (Loki + Grafana, or Datadog)
- [ ] `access_logs` retention/archival policy scheduled (cron or pg_cron)
- [ ] Clustered MQTT broker (HiveMQ or EMQX) replacing single Mosquitto instance
- [ ] OTA firmware updates via `ArduinoOTA` library

---

## Scaling Notes

- **Multiple doors:** Each ESP32 only needs a unique `device_id` in `apps/firmware/src/secrets.h` — no firmware logic changes required
- **High load:** Go's Goroutine model handles thousands of concurrent messages; the stateless binary can be replicated behind a load balancer
- **Broker HA:** Replace single Mosquitto with a clustered broker (HiveMQ or EMQX) for 99.99% uptime

---

## PlatformIO Dependencies (`platformio.ini`)

```ini
[env:lolin32]
platform = espressif32
board = lolin32
framework = arduino
monitor_speed = 115200
lib_deps =
  knolleary/PubSubClient @ ^2.8
  arduino-libraries/Arduino_CryptoLibrary @ ^1.0.0
```

> **Note:** `miguelbalboa/MFRC522` has been removed. The HW-VX6330K UHF reader communicates via UART (Serial2) using ESP32's built-in `HardwareSerial` — no external library needed.
