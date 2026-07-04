# Go Backend — Scan Processing & API Reference

This document covers the backend side of the RFID system: how an incoming MQTT scan is validated, how the peppered hash comparison works against PostgreSQL, and the complete HTTP API reference.

---

## Backend Architecture

```
┌─────────────────────────────────────────────────────────────────────┐
│                        Go Backend (:8080)                           │
│                                                                     │
│  ┌──────────────┐    ┌──────────────┐    ┌──────────────────────┐  │
│  │ MQTT Handler │───▶│  PostgreSQL  │───▶│  WebSocket Hub       │  │
│  │ (handler.go) │    │  (postgres.go)│    │  (hub.go)            │  │
│  │              │    │              │    │                      │  │
│  │ Subscribe:   │    │ LookupUser   │    │ Broadcast to all     │  │
│  │  door/scan   │    │ InsertLog    │    │ connected dashboard  │  │
│  │  door/status │    │ GetRecentLogs│    │ clients in real-time │  │
│  │              │    │ CRUD users   │    │                      │  │
│  │ Publish:     │    │              │    │ Types:               │  │
│  │  door/command│    │              │    │  access_log          │  │
│  └──────────────┘    └──────────────┘    │  device_status       │  │
│         │                               └──────────────────────┘  │
│         │                                                         │
│  ┌──────▼──────┐                                                   │
│  │ NonceCache  │  Anti-replay: rejects duplicate nonces            │
│  │ (nonce.go)  │  within 60-second TTL window                      │
│  └─────────────┘                                                   │
│                                                                    │
│  ┌──────────────────────────────────────────────────────────────┐  │
│  │ HTTP Router (gorilla/mux)                                    │  │
│  │                                                              │  │
│  │  GET    /healthz           → DB ping                         │  │
│  │  GET    /api/ws            → WebSocket upgrade               │  │
│  │  GET    /api/logs          → Recent access logs              │  │
│  │  DELETE /api/logs          → Clear all logs                  │  │
│  │  GET    /api/users         → List all users                  │  │
│  │  POST   /api/users         → Register new user               │  │
│  │  PATCH  /api/users/{id}    → Toggle active/inactive          │  │
│  │  DELETE /api/users/{id}    → Delete user                     │  │
│  │  POST   /api/door/override → Manual door control             │  │
│  └──────────────────────────────────────────────────────────────┘  │
└─────────────────────────────────────────────────────────────────────┘
```

---

## Scan Processing Flow

When the ESP32 publishes a scan to `door/scan`, the Mosquitto broker delivers it to the Go backend. Here is the exact processing pipeline:

### Step 1: MQTT Message Reception

**File:** `internal/mqtt/handler.go:106-108`

The `handleScan` callback fires when a message arrives on `door/scan`. It immediately dispatches the work to a new goroutine so the MQTT library's internal thread is never blocked:

```go
func (h *Handler) handleScan(client pahomqtt.Client, msg pahomqtt.Message) {
    go h.processScan(msg.Payload())
}
```

### Step 2: JSON Deserialization

**File:** `internal/mqtt/handler.go:110-115`

The raw byte payload is parsed into a `ScanPayload` struct. If the JSON is malformed, the scan is silently dropped:

```go
var payload models.ScanPayload
if err := json.Unmarshal(raw, &payload); err != nil {
    log.Warn().Err(err).Msg("invalid scan payload")
    return
}
```

**Incoming payload shape:**

```json
{
    "uid_hash": "3f7c3e0a1b8d9c4e5f6a2b8c9d0e1f2a3b4c5d6e7f8a9b0c1d2e3f4a5b6c7d8e",
    "device_id": "esp32_front_door",
    "nonce": "a3f9c12b",
    "timestamp": 1718000000
}
```

### Step 3: Rate Limiting

**File:** `internal/mqtt/handler.go:117-121`

Each `device_id` gets its own rate limiter: **maximum 1 message per 2 seconds**. This prevents a malfunctioning or compromised ESP32 from flooding the backend:

```go
if !h.getLimiter(payload.DeviceID).Allow() {
    log.Warn().Str("device", payload.DeviceID).Msg("rate limit exceeded, dropping scan")
    return
}
```

Rate limiters are stored in a `sync.Map` (thread-safe) keyed by `device_id`, created on first use.

### Step 4: Input Validation

**File:** `internal/mqtt/handler.go:123-135`

Three checks run before any database query:

| Check | Rule | Reject Reason |
|-------|------|---------------|
| `uid_hash` length | Must be exactly 64 hex characters (SHA-256) | `invalid uid_hash length` |
| `device_id` | Must not be empty | `empty device_id` |
| `timestamp` | Must not be older than 30 seconds | `stale timestamp` |

```go
if len(payload.UIDHash) != 64 {
    log.Warn().Str("uid_hash", payload.UIDHash).Msg("invalid uid_hash length")
    return
}
if payload.DeviceID == "" {
    log.Warn().Msg("empty device_id")
    return
}
if time.Since(time.Unix(payload.Timestamp, 0)) > 30*time.Second {
    log.Warn().Int64("timestamp", payload.Timestamp).Msg("stale timestamp")
    return
}
```

### Step 5: Anti-Replay (Nonce Check)

**File:** `internal/mqtt/handler.go:137-141`, `internal/mqtt/nonce.go:27-36`

The backend maintains an in-memory `NonceCache` that stores every nonce seen in the last 60 seconds. If a nonce is reused within that window, the message is rejected as a replay attack:

```go
if !h.nonce.CheckAndStore(payload.Nonce) {
    log.Warn().Str("nonce", payload.Nonce).Msg("replay detected, duplicate nonce")
    return
}
```

The `NonceCache` is a `map[string]time.Time` protected by `sync.RWMutex`. A background goroutine cleans up expired entries every 30 seconds:

```go
func (nc *NonceCache) cleanup() {
    ticker := time.NewTicker(30 * time.Second)
    for range ticker.C {
        nc.mu.Lock()
        now := time.Now()
        for k, t := range nc.entries {
            if now.Sub(t) > nc.ttl {
                delete(nc.entries, k)
            }
        }
        nc.mu.Unlock()
    }
}
```

### Step 6: Apply Server-Side Pepper

**File:** `internal/mqtt/handler.go:98-103`

The ESP32 sends `sha256(rawUID)`. The backend applies a second hash with the server-side pepper before comparing against the database:

```go
func (h *Handler) pepperHash(uidHash string) string {
    combined := uidHash + h.pepper
    hash := sha256.Sum256([]byte(combined))
    return hex.EncodeToString(hash[:])
}
```

**Hashing chain:**

```
ESP32:   sha256("E20040D4")           → "3f7c3e0a..."   (sent over network)
Backend: sha256("3f7c3e0a..." + PEPPER) → "a8b9c0d1..."   (compared against DB)
DB:      stores "a8b9c0d1..."                           (peppered hash)
```

The pepper (`UID_PEPPER` from env) means that even if both the network traffic and the database are compromised, an attacker still cannot map a raw UID to a database row.

### Step 7: Database Lookup

**File:** `internal/db/postgres.go:45-58`

The peppered hash is used to query the `users` table:

```sql
SELECT id, name, rfid_uid_hash, role, is_active, created_at
FROM users
WHERE rfid_uid_hash = $1
```

- If no row is found → returns `nil, nil` (unknown card)
- If a row is found → returns `*User` with `IsActive` flag

The connection pool is configured to handle concurrent lookups:

```go
db.SetMaxOpenConns(25)
db.SetMaxIdleConns(10)
db.SetConnMaxLifetime(5 * time.Minute)
```

### Step 8: Access Evaluation

**File:** `internal/mqtt/handler.go:151-172`

Three possible outcomes:

| Condition | Status | Message | `cmdStatus` |
|-----------|--------|---------|-------------|
| User found AND `is_active = true` | `AUTHORIZED` | `Access Granted` | `1` |
| User found AND `is_active = false` | `DENIED` | `Access Denied — User Inactive` | `0` |
| User NOT found (hash has no match) | `DENIED` | `Access Denied — Unknown Card` | `0` |

### Step 9: Log the Access Attempt

**File:** `internal/mqtt/handler.go:180-197`

Every access attempt is recorded in `access_logs` **before** the door is actuated. If the database insert fails, the error is logged but the door still actuates — logging failure never blocks physical access:

```go
accessLog := &models.AccessLog{
    RFIDUIDHash: pepperedHash,
    Status:      status,
    ActionBy:    actionBy,
    DeviceID:    payload.DeviceID,
    Timestamp:   time.Now(),
}
if logErr := h.db.InsertAccessLog(accessLog); logErr != nil {
    log.Error().Err(logErr).Msg("failed to log access attempt (still actuating door)")
} else {
    h.hub.Broadcast(models.WSMessage{
        Type: "access_log",
        Data: accessLog,
    })
}
```

If the log insert succeeds, the result is also broadcast to all connected WebSocket clients (Next.js dashboard) in real-time.

### Step 10: Publish Door Command

**File:** `internal/mqtt/handler.go:199-207`

The backend publishes the access decision to `door/command` at QoS 1:

```go
cmd := &models.CommandPayload{
    Status:   cmdStatus,
    Message:  message,
    ActionBy: actionBy,
}
h.PublishCommand(cmd)
```

**Example published payload:**

```json
{
    "status": 1,
    "message": "Access Granted",
    "action_by": "John Doe"
}
```

The ESP32 receives this on its `door/command` subscription, then triggers the green LED + 1 beep (`status=1`) or red LED + 3 beeps (`status=0`).

### Step 11: WebSocket Broadcast

**File:** `internal/ws/hub.go:63-85`

When a scan is logged, the `Hub.Broadcast()` method sends the result to all connected dashboard clients:

```json
{
    "type": "access_log",
    "data": {
        "id": 0,
        "rfid_uid_hash": "a8b9c0d1...",
        "status": "AUTHORIZED",
        "action_by": "John Doe",
        "device_id": "esp32_front_door",
        "timestamp": "2024-06-10T12:00:00Z"
    }
}
```

Device status changes (online/offline via LWT) are also broadcast:

```json
{
    "type": "device_status",
    "data": {
        "status": "online"
    }
}
```

The hub caches the last `device_status` message so new WebSocket clients receive the current device status immediately upon connection.

---

## Complete Scan Processing Diagram

```
ESP32 publishes to "door/scan"
            │
            ▼
    ┌───────────────┐
    │ Mosquitto     │  QoS 1 delivery
    │ Broker        │
    └───────┬───────┘
            │
            ▼
    ┌───────────────────────────────────────────────────┐
    │ handleScan() → go processScan()                   │  New goroutine
    └───────────────────────┬───────────────────────────┘
                            │
                            ▼
    ┌───────────────────────────────────────────────────┐
    │ 1. JSON Unmarshal into ScanPayload                │  Fail → drop
    └───────────────────────┬───────────────────────────┘
                            │
                            ▼
    ┌───────────────────────────────────────────────────┐
    │ 2. Rate Limit: 1 msg / 2 sec per device_id        │  Fail → drop
    │    (golang.org/x/time/rate, sync.Map)             │
    └───────────────────────┬───────────────────────────┘
                            │
                            ▼
    ┌───────────────────────────────────────────────────┐
    │ 3. Input Validation                               │
    │    • uid_hash must be 64 hex chars                │  Fail → drop
    │    • device_id must not be empty                  │
    │    • timestamp must be within 30 seconds          │
    └───────────────────────┬───────────────────────────┘
                            │
                            ▼
    ┌───────────────────────────────────────────────────┐
    │ 4. Anti-Replay: NonceCache (60-sec TTL)           │  Duplicate → drop
    │    Background cleanup every 30 seconds            │
    └───────────────────────┬───────────────────────────┘
                            │
                            ▼
    ┌───────────────────────────────────────────────────┐
    │ 5. Pepper Hash: sha256(uid_hash + PEPPER)         │
    │    uid_hash from ESP32 + UID_PEPPER from env      │
    └───────────────────────┬───────────────────────────┘
                            │
                            ▼
    ┌───────────────────────────────────────────────────┐
    │ 6. PostgreSQL Lookup                              │
    │    SELECT ... FROM users WHERE rfid_uid_hash = $1 │
    │    Connection pool: 25 max open, 10 idle          │
    └───────────────────────┬───────────────────────────┘
                            │
               ┌────────────┼────────────┐
               │            │            │
               ▼            ▼            ▼
        ┌────────────┐ ┌────────────┐ ┌────────────┐
        │ Found +    │ │ Found +    │ │ Not Found  │
        │ active     │ │ inactive   │ │            │
        │            │ │            │ │            │
        │ AUTHORIZED │ │ DENIED     │ │ DENIED     │
        │ status=1   │ │ status=0   │ │ status=0   │
        └──────┬─────┘ └──────┬─────┘ └──────┬─────┘
               │              │              │
               └──────────────┼──────────────┘
                              │
                              ▼
    ┌───────────────────────────────────────────────────┐
    │ 7. INSERT INTO access_logs                        │
    │    If insert fails → log error, continue anyway   │
    │    If insert succeeds → broadcast via WebSocket   │
    └───────────────────────┬───────────────────────────┘
                            │
                            ▼
    ┌───────────────────────────────────────────────────┐
    │ 8. PUBLISH to "door/command" (QoS 1)              │
    │    {"status":1,"message":"Access Granted",...}    │
    └───────────────────────┬───────────────────────────┘
                            │
                            ▼
    ┌───────────────────────────────────────────────────┐
    │ 9. WebSocket Broadcast to Next.js dashboard       │
    │    {type: "access_log", data: {...}}              │
    └───────────────────────────────────────────────────┘
```

---

## Hash Chain Explained

Understanding how the UID is transformed at each layer is critical for debugging and security:

```
Layer         Operation                              Result
─────────     ──────────────────────────────────     ─────────────────────────────────
ESP32         Raw EPC bytes from reader              E2 00 40 D4 (binary)
ESP32         Convert to hex string                  "E20040D4"
ESP32         sha256("E20040D4")                     "3f7c3e0a1b8d..." (64 hex chars)
              │
              │  MQTT (network) — only the hash is transmitted
              ▼
Backend       Receive uid_hash                       "3f7c3e0a1b8d..."
Backend       sha256("3f7c3e0a1b8d..." + UID_PEPPER) "a8b9c0d1e2f3..." (64 hex chars)
              │
              │  PostgreSQL query
              ▼
Database      Compare against users.rfid_uid_hash    "a8b9c0d1e2f3..."
```

When registering a user via `POST /api/users`, the same double-hash is applied:

```go
// Step 1: hash raw UID (same as what ESP32 would produce)
rawHash := sha256.Sum256([]byte(req.RFIDUID))
uidHash := hex.EncodeToString(rawHash[:])

// Step 2: apply pepper (same as backend does on scan)
peppered := sha256.Sum256([]byte(uidHash + uidPepper))
pepperedHash := hex.EncodeToString(peppered[:])

// Step 3: store in database
database.CreateUser(req.Name, pepperedHash, req.Role)
```

---

## HTTP API Reference

All endpoints are served on `http://localhost:8080`.

---

### `GET /healthz`

Health check endpoint for Docker, load balancers, and uptime monitors.

**Request:**

```bash
curl http://localhost:8080/healthz
```

**Response — healthy (200):**

```text
(no body)
```

**Response — unhealthy (503):** (database unreachable)

```text
(no body)
```

**Source:** `cmd/server/main.go:64-70`

---

### `GET /api/ws`

WebSocket endpoint for real-time dashboard updates. The Next.js frontend connects here to receive live access logs and device status changes.

**Connection:**

```javascript
const ws = new WebSocket("ws://localhost:8080/api/ws");
```

**Messages received (server → client):**

**Access log event:**

```json
{
    "type": "access_log",
    "data": {
        "id": 0,
        "rfid_uid_hash": "a8b9c0d1e2f3...",
        "status": "AUTHORIZED",
        "action_by": "John Doe",
        "device_id": "esp32_front_door",
        "timestamp": "2024-06-10T12:00:00Z"
    }
}
```

**Device status event (from ESP32 LWT):**

```json
{
    "type": "device_status",
    "data": {
        "status": "online"
    }
}
```

**Protocol details:**

| Detail | Value |
|--------|-------|
| Ping interval | 30 seconds (server → client) |
| Pong timeout | 60 seconds (client must respond) |
| Read limit | 512 bytes per message |
| Origin check | Allow all (development only) |
| Cached status | New clients receive last `device_status` immediately |

**Source:** `internal/ws/hub.go:88-124`

---

### `GET /api/logs?limit={n}`

Retrieve recent access logs, ordered newest first.

**Request:**

```bash
curl http://localhost:8080/api/logs?limit=10
```

**Query parameters:**

| Parameter | Type | Default | Max | Description |
|-----------|------|---------|-----|-------------|
| `limit` | integer | 50 | 500 | Number of logs to return |

**Response (200):**

```json
[
    {
        "id": 42,
        "rfid_uid_hash": "a8b9c0d1e2f3...",
        "status": "AUTHORIZED",
        "action_by": "John Doe",
        "device_id": "esp32_front_door",
        "timestamp": "2024-06-10T12:00:00Z"
    },
    {
        "id": 41,
        "rfid_uid_hash": "b9c0d1e2f3a4...",
        "status": "DENIED",
        "action_by": "Unknown",
        "device_id": "esp32_front_door",
        "timestamp": "2024-06-10T11:58:30Z"
    }
]
```

**Error response (500):**

```text
Internal Server Error
```

**Source:** `cmd/server/main.go:78-94`

---

### `DELETE /api/logs`

Delete all access logs.

**Request:**

```bash
curl -X DELETE http://localhost:8080/api/logs
```

**Response (204):** (no body)

**Source:** `cmd/server/main.go:96-105`

---

### `GET /api/users`

List all registered users, ordered by creation date (newest first).

**Request:**

```bash
curl http://localhost:8080/api/users
```

**Response (200):**

```json
[
    {
        "id": 1,
        "name": "John Doe",
        "rfid_uid_hash": "a8b9c0d1e2f3...",
        "role": "admin",
        "is_active": true,
        "created_at": "2024-06-01T10:00:00Z"
    },
    {
        "id": 2,
        "name": "Jane Smith",
        "rfid_uid_hash": "d2e3f4a5b6c7...",
        "role": "employee",
        "is_active": true,
        "created_at": "2024-06-05T14:30:00Z"
    }
]
```

**Source:** `cmd/server/main.go:107-116`

---

### `POST /api/users`

Register a new user. The raw RFID UID is hashed and peppered on the backend before storage.

**Request:**

```bash
curl -X POST http://localhost:8080/api/users \
  -H "Content-Type: application/json" \
  -d '{
    "name": "John Doe",
    "rfid_uid": "E20040D4",
    "role": "employee"
  }'
```

**Request body:**

| Field | Type | Required | Default | Description |
|-------|------|----------|---------|-------------|
| `name` | string | Yes | — | User display name |
| `rfid_uid` | string | Yes | — | Raw UID from the RFID card (hex string) |
| `role` | string | No | `"employee"` | User role |

**What happens server-side:**

1. `sha256("E20040D4")` → `"3f7c3e0a..."` (same as ESP32 would produce)
2. `sha256("3f7c3e0a..." + UID_PEPPER)` → `"a8b9c0d1..."` (peppered hash)
3. `INSERT INTO users (name, rfid_uid_hash, role) VALUES (...)` with the peppered hash

**Response (200):**

```json
{
    "id": 3,
    "name": "John Doe",
    "rfid_uid_hash": "a8b9c0d1e2f3...",
    "role": "employee",
    "is_active": true,
    "created_at": "2024-06-10T12:00:00Z"
}
```

**Error responses:**

| Status | When |
|--------|------|
| 400 | Missing `name` or `rfid_uid`, or invalid JSON |
| 409 | RFID UID already registered (duplicate `rfid_uid_hash`) |
| 500 | Database error |

**Source:** `cmd/server/main.go:118-152`

---

### `PATCH /api/users/{id}`

Toggle a user's active status. Inactive users are denied access even if their card hash matches.

**Request:**

```bash
curl -X PATCH http://localhost:8080/api/users/2 \
  -H "Content-Type: application/json" \
  -d '{"is_active": false}'
```

**Request body:**

| Field | Type | Required | Description |
|-------|------|----------|-------------|
| `is_active` | boolean | Yes | `true` to enable, `false` to disable |

**Response (200):**

```json
{
    "id": 2,
    "name": "Jane Smith",
    "rfid_uid_hash": "d2e3f4a5b6c7...",
    "role": "employee",
    "is_active": false,
    "created_at": "2024-06-05T14:30:00Z"
}
```

**Error responses:**

| Status | When |
|--------|------|
| 400 | Invalid user ID, invalid JSON, or missing `is_active` |
| 404 | User not found |
| 500 | Database error |

**Source:** `cmd/server/main.go:154-183`

---

### `DELETE /api/users/{id}`

Permanently delete a user. Their RFID card will no longer match any database row (access denied).

**Request:**

```bash
curl -X DELETE http://localhost:8080/api/users/2
```

**Response (204):** (no body)

**Error responses:**

| Status | When |
|--------|------|
| 400 | Invalid user ID (not a number) |
| 404 | User not found |

**Source:** `cmd/server/main.go:185-197`

---

### `POST /api/door/override`

Manually control the door from the dashboard. Publishes a command directly to `door/command` and logs the override action.

**Request:**

```bash
curl -X POST http://localhost:8080/api/door/override \
  -H "Content-Type: application/json" \
  -d '{
    "status": 1,
    "message": "Manual Override — Security Check",
    "action_by": "Admin"
  }'
```

**Request body:**

| Field | Type | Required | Default | Description |
|-------|------|----------|---------|-------------|
| `status` | integer | Yes | — | `1` = grant access, `0` = deny access |
| `message` | string | No | `"Manual Override — Access Granted"` or `"Manual Override — Access Denied"` | Custom message |
| `action_by` | string | No | `"Dashboard"` | Who triggered the override |

**What happens server-side:**

1. Publishes `{"status":1,"message":"...","action_by":"..."}` to `door/command` (QoS 1)
2. Inserts an override log into `access_logs` with `rfid_uid_hash="override"` and `device_id="dashboard_override"`
3. Broadcasts the log to WebSocket clients

**Response (200):**

```json
{
    "status": 1,
    "message": "Manual Override — Security Check",
    "action_by": "Admin"
}
```

**Error responses:**

| Status | When |
|--------|------|
| 400 | Invalid JSON or `status` not 0 or 1 |
| 500 | Failed to publish MQTT command |

**Source:** `cmd/server/main.go:199-249`

---

## API Summary Table

| Method | Path | Description | Auth |
|--------|------|-------------|------|
| `GET` | `/healthz` | Database health check | No |
| `GET` | `/api/ws` | WebSocket for real-time events | No |
| `GET` | `/api/logs?limit=N` | Get recent access logs | No |
| `DELETE` | `/api/logs` | Clear all access logs | No |
| `GET` | `/api/users` | List all registered users | No |
| `POST` | `/api/users` | Register new user (raw UID) | No |
| `PATCH` | `/api/users/{id}` | Toggle user active/inactive | No |
| `DELETE` | `/api/users/{id}` | Delete user permanently | No |
| `POST` | `/api/door/override` | Manual door grant/deny | No |

> **Note:** No authentication is enforced yet. JWT middleware will be added for production. Currently all endpoints are open for development convenience.

---

## Database Schema

```sql
CREATE TABLE IF NOT EXISTS users (
    id            SERIAL PRIMARY KEY,
    name          VARCHAR(100) NOT NULL,
    rfid_uid_hash VARCHAR(64) UNIQUE NOT NULL,  -- sha256(sha256(rawUID) + PEPPER)
    role          VARCHAR(50) DEFAULT 'employee',
    is_active     BOOLEAN DEFAULT TRUE,
    created_at    TIMESTAMP DEFAULT CURRENT_TIMESTAMP
);

CREATE TABLE IF NOT EXISTS access_logs (
    id            SERIAL PRIMARY KEY,
    rfid_uid_hash VARCHAR(64) NOT NULL,          -- peppered hash or "override"
    status        VARCHAR(20) NOT NULL,           -- 'AUTHORIZED' or 'DENIED'
    action_by     VARCHAR(100),                   -- User name or 'Unknown'
    device_id     VARCHAR(50),                    -- e.g. 'esp32_front_door'
    timestamp     TIMESTAMP DEFAULT CURRENT_TIMESTAMP
);

CREATE INDEX IF NOT EXISTS idx_access_logs_device_time ON access_logs(device_id, timestamp DESC);
CREATE INDEX IF NOT EXISTS idx_access_logs_time        ON access_logs(timestamp DESC);
CREATE INDEX IF NOT EXISTS idx_access_logs_status      ON access_logs(status);
```

**Source:** `infrastructure/init.sql`

---

## Environment Variables

| Variable | Required | Default | Description |
|----------|----------|---------|-------------|
| `DATABASE_URL` | Yes | — | PostgreSQL connection string |
| `JWT_SECRET` | Yes | — | Secret for future JWT auth |
| `UID_PEPPER` | Yes | — | Server-side pepper mixed into UID hashes |
| `MQTT_BROKER` | No | `tcp://localhost:1883` | MQTT broker address |
| `MQTT_USERNAME_BACKEND` | No | — | MQTT broker username |
| `MQTT_PASSWORD_BACKEND` | No | — | MQTT broker password |

---

## Source File Map

| File | Purpose |
|------|---------|
| `cmd/server/main.go` | Entry point, HTTP routes, environment loading |
| `internal/mqtt/handler.go` | MQTT subscribe, scan processing, command publish |
| `internal/mqtt/nonce.go` | Anti-replay nonce cache with TTL cleanup |
| `internal/db/postgres.go` | Connection pool, all SQL queries |
| `internal/models/schema.go` | Go structs for payloads, users, logs |
| `internal/ws/hub.go` | WebSocket hub: broadcast, ping/pong, client management |
