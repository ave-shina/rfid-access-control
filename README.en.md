# RFID Access Control System

> **Languages:** English | [Bahasa Indonesia](./README.md)

> An enterprise-grade, event-driven **RFID Access Control System** built on Pervasive Computing (IoT) principles. A dumb ESP32 edge sensor reads UHF tags; all business logic lives in a Go backend; a Next.js dashboard gives real-time monitoring and remote control.

The physical world (card taps, doors, LEDs) is decoupled from credential validation through the **MQTT pub/sub protocol** — so the edge device stays simple, secure, and replaceable.

---

## ✨ Highlights

- **UHF, not legacy** — Reads UHF tags via the **HW-VX6330K** reader over **RS485 / MAX485** (long range, fast, industrial).
- **Privacy by design** — Raw UIDs **never leave the ESP32**. Only a SHA-256 hash is transmitted; the DB stores a *peppered* hash, so a breach can't be cloned onto blank cards.
- **Fail-secure** — No server response = door stays locked. The ESP32 never self-authorizes.
- **Replay-proof** — Per-scan `nonce` + 30-second timestamp window validated on every message.
- **Real-time dashboard** — Live access log over WebSocket, manual door override, user management.
- **Production patterns** — Per-device rate limiting, DB connection pooling, watchdog timer, LWT offline detection, structured JSON logging.
- **Auto baud detect** — The ESP32 probes common baud rates on boot so reader swap-out is plug-and-play.

---

## 🏗 Architecture

### 5-Layer Pervasive Computing Model

| Layer | Role | Components |
|---|---|---|
| **Perception** | Sense & actuate | ESP32, HW-VX6330K UHF reader (MAX485/RS485), LEDs, KY-12 buzzer |
| **Network** | Transport | Wi-Fi (802.11), MQTT over TCP |
| **Middleware** | Routing & processing | Eclipse Mosquitto, Go backend |
| **Data** | Persistent storage | PostgreSQL 15 |
| **Application** | Human interface | Next.js 14 dashboard |

### Data Flow

```
 ┌──────────┐   RS485    ┌─────────┐  MQTT door/scan   ┌───────────┐
 │ UHF Tag  │───────────▶│  ESP32  │──────────────────▶│ Mosquitto │
 └──────────┘  /MAX485   │ (edge)  │  (SHA-256 hash)   │  broker   │
                          └────┬────┘                   └─────┬─────┘
        ┌──────────────────────│                              │
        │  MQTT door/command   │                              │
        │   {status:1|0}       │                              ▼
        │  ┌───────────────────┘                     ┌─────────────────┐
        │  ▼                                         │   Go Backend    │
 Green/Red LED + buzzer                              │  rate-limit +   │
 (grant / deny)                                      │  anti-replay +  │
                                                     │  DB validation  │
                                                     └────────┬────────┘
                                          PostgreSQL │  (users / access_logs)
                                                     │        │ WebSocket
                                                     ▼        ▼
                                              ┌─────────────────────┐
                                              │  Next.js Dashboard  │
                                              │  (live log + admin) │
                                              └─────────────────────┘
```

---

## 🧰 Tech Stack

| Layer | Technology |
|---|---|
| Edge firmware | C++ (Arduino), PlatformIO — ESP32 (`lolin32`) |
| Reader | HW-VX6330K UHF over RS485 (MAX485 transceiver) |
| Backend | Go 1.25, `paho.mqtt.golang`, `gorilla/mux` + `websocket`, `database/sql`, `zerolog`, `golang.org/x/time` |
| Database | PostgreSQL 15 (Docker) |
| MQTT broker | Eclipse Mosquitto 2 (Docker) |
| Frontend | Next.js 14, React 18, TypeScript, Tailwind CSS, shadcn/ui (Radix) |
| Infrastructure | Docker & Docker Compose |

---

## 📂 Repository Structure

```
.
├── apps/
│   ├── firmware/               # ESP32 edge device (Arduino / PlatformIO)
│   │   ├── platformio.ini
│   │   └── src/
│   │       ├── main.cpp            # Entry point — setup() + loop()
│   │       ├── config.h            # GPIO pins, timeouts, constants
│   │       ├── uhf_reader.h        # RS485 + HW-VX6330K frame parsing
│   │       ├── crypto.h            # hashUID(), generateNonce()
│   │       ├── mqtt_handler.h      # connect / publish / callback
│   │       ├── actuators.h         # grantAccess() / denyAccess()
│   │       └── secrets.h.example   # copy → secrets.h (gitignored)
│   ├── backend/                # Go — MQTT handler, DB, WebSocket
│   │   ├── cmd/server/main.go      # Routes + wiring
│   │   └── internal/{db,mqtt,ws,models}/
│   └── frontend/               # Next.js 14 dashboard
│       └── src/{app,components,lib}/
├── infrastructure/
│   ├── docker-compose.yml          # Postgres + Mosquitto (dev)
│   ├── docker-compose.prod.yml     # Full stack incl. backend + frontend
│   ├── mosquitto.conf
│   ├── init.sql                    # Schema seed
│   └── .env.example                # Template for all secrets
└── package.json                    # Root orchestrator (concurrently)
```

---

## 🚀 Quick Start

### Prerequisites

- **Docker Desktop**
- **Go 1.25+**
- **Node.js 20+**
- **PlatformIO** (VS Code extension or CLI) — for flashing the ESP32

### 1 · Configure secrets

```bash
# Infrastructure
cp infrastructure/.env.example infrastructure/.env
# Fill POSTGRES_PASSWORD, JWT_SECRET, UID_PEPPER, MQTT credentials…

# Firmware
cp apps/firmware/src/secrets.h.example apps/firmware/src/secrets.h
# Fill WIFI_SSID, WIFI_PASSWORD, MQTT_SERVER, MQTT_USER, MQTT_PASS
```

Also create the broker credential file `infrastructure/passwd` (**not committed** — hashes only). It must exist before `infra:up`, and the passwords must match the `MQTT_PASSWORD_*` values in `.env`:

```bash
cd infrastructure
docker run --rm -it -v "$(pwd)/passwd:/mosquitto/config/passwd" eclipse-mosquitto:2 \
  mosquitto_passwd -c /mosquitto/config/passwd go_backend
docker run --rm -it -v "$(pwd)/passwd:/mosquitto/config/passwd" eclipse-mosquitto:2 \
  mosquitto_passwd /mosquitto/config/passwd esp32_front_door
docker run --rm -it -v "$(pwd)/passwd:/mosquitto/config/passwd" eclipse-mosquitto:2 \
  mosquitto_passwd /mosquitto/config/passwd nextjs_dashboard
cd ..
```

### 2 · Start infrastructure (Postgres + Mosquitto)

From the repo root — the root `package.json` wires the env file for you:

```bash
npm run infra:up      # = docker compose up -d
npm run infra:logs    # tail logs (optional)
```

Postgres on `:5434`, Mosquitto on `:1883` / `:9001`.

### 3 · Run backend + frontend together

```bash
npm run dev           # launches Go backend (air) + Next.js in parallel
```

- Backend → `http://localhost:8080` (health on `/healthz`)
- Dashboard → `http://localhost:3000`

Or run them individually:

```bash
cd apps/backend  && go run cmd/server/main.go
cd apps/frontend && npm install && npm run dev
```

### 4 · Flash the ESP32

Open `apps/firmware/` in VS Code with the **PlatformIO** extension, confirm `secrets.h` is filled in, then **Upload and Monitor**. Hold a UHF tag near the reader — the serial monitor prints the auto-detected baud rate and each parsed EPC.

> The firmware supports **broker auto-discovery** (gateway probe, then a subnet scan for port `1883`). Convenient for development, but discovery trusts any host on the subnet — for anything serious, set `MQTT_SERVER` in `secrets.h` to the broker IP statically.

---

## 🔌 MQTT Topics

| Topic | Publisher → Subscriber | QoS | Purpose |
|---|---|---|---|
| `door/scan` | ESP32 → Backend | **1** | Edge reports a scanned UID (hashed) |
| `door/command` | Backend/UI → ESP32 | **1** | Grant/deny instruction |
| `door/status` | ESP32 (+ LWT) → UI | 0 | Heartbeat / offline detection |

`door/scan` payload — raw UID never transmitted:

```json
{
  "uid_hash": "64-char-sha256-hex",
  "device_id": "esp32_front_door",
  "nonce": "a3f9c12b",
  "timestamp": 1718000000
}
```

`door/command` payload:

```json
{ "status": 1, "message": "Access Granted", "action_by": "System" }
```

- `status: 1` → Green LED + 1 beep
- `status: 0` → Red LED + 3 beeps

---

## 🌐 Backend API

| Method | Endpoint | Description |
|---|---|---|
| `GET` | `/healthz` | Liveness probe (checks DB ping) |
| `GET` | `/api/ws` | WebSocket — real-time scan + status stream |
| `GET` | `/api/logs` | Recent access logs (`?limit=`, max 500) |
| `DELETE` | `/api/logs` | Clear all access logs |
| `GET` | `/api/users` | List registered users |
| `POST` | `/api/users` | Register a new user (hashes UID + pepper) |
| `PATCH` | `/api/users/{id}` | Activate/deactivate a user (`is_active`) |
| `DELETE` | `/api/users/{id}` | Remove user |
| `POST` | `/api/door/override` | Manually grant/deny from dashboard |

---

## 🗄 Database Schema

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

Indexed on `(device_id, timestamp DESC)`, `(timestamp DESC)`, and `(status)` — `access_logs` is write-heavy and grows unbounded; schedule periodic cleanup per your retention policy.

---

## 🔐 Security Model

- ✅ UIDs hashed on-device (SHA-256) before transmission — raw UID never hits the wire
- ✅ DB stores `sha256(rawUID + UID_PEPPER)` — a DB leak can't be reversed into cloneable cards
- ✅ MQTT broker requires per-client credentials — anonymous connections disabled
- ✅ Anti-replay: `nonce` (deduped 60s) + 30-second timestamp window
- ✅ Per-`device_id` rate limiting (max 1 scan / 2s) in the backend
- ✅ ESP32 watchdog timer + exponential Wi-Fi/MQTT backoff
- ✅ LWT publishes `offline` automatically if the device drops
- ✅ Fail-secure: no validated server response → door stays locked

**Before production**, also lock down: MQTTS/TLS (`:8883`) **plus per-device HMAC** — without message authentication, anti-replay only guards against accidental duplicates, not attackers — JWT auth on all routes, NextAuth role gating on the override, log aggregation (Loki/Grafana), and a log retention cron. See [`CLAUDE.md`](./CLAUDE.md) for the full pre-production checklist.

---

## 🔧 Hardware Wiring (ESP32 ↔ HW-VX6330K via MAX485)

| MAX485 | → | ESP32 | Notes |
|---|---|---|---|
| DI | ← | GPIO 17 (TX2) | ESP32 TX → MAX485 data-in |
| RO | → | GPIO 16 (RX2) | MAX485 receiver-out → ESP32 RX |
| DE + RE | ← | GPIO 27 | Direction (tied): HIGH=TX, LOW=RX |
| A / B | → | Reader A+/B- | RS485 differential pair, 120Ω termination at each end |
| VCC / GND | → | 3V3 / GND | Reader itself is externally powered (12V) |

** Actuators:** Green LED `GPIO 25`, Red LED `GPIO 26`, KY-12 buzzer `GPIO 4`, built-in LED `GPIO 2`.

> ⚠️ **MAX485 is mandatory.** The HW-VX6330K speaks RS485 — direct wiring to ESP32 GPIO won't work and may damage the chip.

---

## 📜 License

This project is part of a Magister (graduate) academic submission. All rights reserved unless otherwise stated.
