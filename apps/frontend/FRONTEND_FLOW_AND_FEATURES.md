# Frontend UI Flow, Features & Connection Architecture

This document covers the Next.js 14 dashboard: its pages, features, how components connect to the Go backend via HTTP API and WebSocket, and the BFF (Backend-For-Frontend) proxy pattern.

---

## Frontend Architecture

```
┌─────────────────────────────────────────────────────────────────────────┐
│                    Next.js 14 Dashboard (:3000)                         │
│                                                                         │
│  ┌──────────────────────────────────────────────────────────────────┐  │
│  │ Layout (layout.tsx)                                             │  │
│  │  ┌────────────┐  ┌───────────────────────────────────────────┐  │  │
│  │  │  Sidebar   │  │  Main Content                              │  │  │
│  │  │  Nav       │  │                                             │  │  │
│  │  │            │  │  Page: /  → Dashboard                      │  │  │
│  │  │ • Dashboard│  │    ├─ LiveLogTable (WebSocket + API)       │  │  │
│  │  │ • Users   │  │    ├─ DoorControl (API)                     │  │  │
│  │  │            │  │    └─ ClearButton (API)                    │  │  │
│  │  │            │  │                                             │  │  │
│  │  │ SYS v0.1.0│  │  Page: /users → Users Management           │  │  │
│  │  └────────────┘  │    ├─ Add User Form (API)                  │  │  │
│  │                  │    └─ Users Table (API)                     │  │  │
│  │                  └───────────────────────────────────────────┘  │  │
│  └──────────────────────────────────────────────────────────────────┘  │
│                                                                         │
│  ┌─────────────────────────────┐  ┌────────────────────────────────┐  │
│  │  BFF Route Handlers         │  │  WebSocket Hook                │  │
│  │  (Next.js API Routes)       │  │  (lib/websocket.ts)            │  │
│  │                             │  │                                │  │
│  │  /api/logs    → Go :8080    │  │  useWebSocket()                │  │
│  │  /api/users   → Go :8080    │  │  fetchAPI()                    │  │
│  │  /api/door/…  → Go :8080    │  │  ws://localhost:8080/api/ws   │  │
│  └─────────────────────────────┘  └────────────────────────────────┘  │
└─────────────────────────────────────────────────────────────────────────┘
          │ HTTP                                     │ WebSocket
          ▼                                          ▼
┌─────────────────────────────────────────────────────────────────────────┐
│                     Go Backend (:8080)                                   │
│            HTTP API + WebSocket Hub                                     │
└─────────────────────────────────────────────────────────────────────────┘
```

---

## Pages & Routes

| Route | Page Component | Description |
|-------|---------------|-------------|
| `/` | `app/page.tsx` | **Dashboard** — real-time access log monitor with door override controls |
| `/users` | `app/users/page.tsx` | **Users** — CRUD management for RFID credentials |

Navigation is handled by a persistent sidebar in `app/layout.tsx`.

---

## Feature Breakdown

### 1. Live Access Log Table (`LiveLogTable.tsx`)

**File:** `components/LiveLogTable.tsx`

The core feature of the dashboard. Displays a real-time table of all access attempts, auto-updating via WebSocket.

**Data sources:**

| Source | Method | When |
|--------|--------|------|
| `GET /api/logs?limit=50` | HTTP (on mount) | Initial page load — fetches last 50 logs |
| `access_log` WS message | WebSocket (live) | Every new scan — prepended to table |

**Table columns:**

| Column | Data | Format |
|--------|------|--------|
| Time | `log.timestamp` | Local time (`HH:MM:SS`) |
| Status | `log.status` | Badge: `AUTHORIZED` (filled) or `DENIED` (destructive) |
| User | `log.action_by` | Bold text — user name or `"Unknown"` |
| Device | `log.device_id` | Monospace — e.g. `esp32_front_door` |
| UID Hash | `log.rfid_uid_hash` | Truncated `a8b9c0d1e2f3...` (first 16 chars) |

**WebSocket behavior:**

```
                         Go Backend
                             │
                    broadcast WS message
                             │
                             ▼
                   ┌─────────────────┐
                   │  useWebSocket()  │
                   │  onmessage()     │
                   └────────┬────────┘
                            │
                   msg.type === "access_log"
                            │
                            ▼
                   setLogs([newLog, ...prev].slice(0, 100))
                            │
                            ▼
                   Table re-renders with new row at top
```

- New logs are **prepended** to the array (newest first)
- Table caps at **100 rows** (`slice(0, 100)`) to prevent memory growth
- No polling — updates are push-only via WebSocket

**Device status indicator:**

Above the table, a status dot shows the ESP32's online/offline state:

| Status | Visual |
|--------|--------|
| `online` | Filled square (bg-foreground) |
| `offline` | Empty square (bg-transparent) |
| `unknown` | Gray square (bg-muted-foreground) |

Status updates come from `device_status` WebSocket messages (triggered by ESP32 LWT).

---

### 2. Clear Logs Button (`ClearButton`)

**File:** `components/LiveLogTable.tsx:146-169`

Button in the log card header that deletes all access logs.

**Flow:**

```
User clicks [ CLEAR LOGS ]
        │
        ▼
DELETE /api/logs          →  BFF proxy  →  DELETE http://localhost:8080/api/logs
        │                                      │
        ▼                                      ▼
setLogs([])               ←  204 No Content  ←  PostgreSQL DELETE FROM access_logs
        │
        ▼
Table shows "// No access logs yet"
```

**Implementation detail:** The `ClearButton` component calls a module-level `clearFn` reference set by `LiveLogTable`, so both the table state and the database are cleared in one action.

---

### 3. Door Override Control (`DoorControl.tsx`)

**File:** `components/DoorControl.tsx`

Two buttons in the dashboard header for manual door control — bypasses the normal RFID scan flow.

**Buttons:**

| Button | Sends | Result on ESP32 |
|--------|-------|-----------------|
| `[ GRANT ]` | `{"status":1}` | Green LED + 1 beep |
| `[ DENY ]` | `{"status":0}` | Red LED + 3 beeps |

**Flow:**

```
User clicks [ GRANT ]
        │
        ▼
confirm("Are you sure you want to GRANT access?")
        │ yes
        ▼
POST /api/door/override                        →  BFF proxy
  body: {                                            │
    status: 1,                                       ▼
    message: "Manual Override — Access Granted",
    action_by: "Dashboard"                Go backend publishes to MQTT
  }                                            "door/command"
        │                                           │
        ▼                                           ▼
Response received                      ESP32 receives command → LED + buzzer
        │
        │                               Backend also logs override:
        ▼                                INSERT INTO access_logs
Override logged in table via            rfid_uid_hash = "override"
WebSocket broadcast                     device_id = "dashboard_override"
        │
        ▼
New row appears in LiveLogTable
```

---

### 4. Users Management (`users/page.tsx`)

**File:** `app/users/page.tsx`

Full CRUD for managing RFID credentials. All operations go through the BFF proxy.

**4a. List Users**

On page mount, fetches all users:

```
GET /api/users  →  BFF  →  GET http://localhost:8080/api/users
                              │
                              ▼
                         Returns all users ordered by created_at DESC
```

**Table columns:**

| Column | Data | Format |
|--------|------|--------|
| Name | `user.name` | Bold |
| Role | `user.role` | Uppercase badge (EMPLOYEE / ADMIN / CONTRACTOR) |
| Status | `user.is_active` | Badge: `ACTIVE` (filled) or `INACTIVE` (outline) |
| UID Hash | `user.rfid_uid_hash` | Truncated `a8b9c0d1...` (first 16 chars) |
| Created | `user.created_at` | Localized date |
| Actions | Buttons | DEACTIVATE/ACTIVATE + DEL |

**4b. Add User**

Form with three fields:

| Field | Input | Required |
|-------|-------|----------|
| NAME | Text input | Yes |
| RFID UID (hex) | Text input (monospace) | Yes |
| Role | Dropdown: EMPLOYEE / ADMIN / CONTRACTOR | No (default: EMPLOYEE) |

**Flow:**

```
User fills form, clicks [ ADD ]
        │
        ▼
POST /api/users                                →  BFF proxy
  body: {                                            │
    name: "John Doe",                                ▼
    rfid_uid: "E20040D4",
    role: "employee"                   Go backend:
  }                                     1. sha256("E20040D4") → "3f7c3e0a..."
        │                              2. sha256("3f7c3e0a..." + PEPPER) → "a8b9c0d1..."
        ▼                              3. INSERT INTO users (name, rfid_uid_hash, role)
Success → form reset + loadUsers()      4. Return new user object
        │
        ▼
Table refreshes with new user row
```

Error cases displayed inline:

| Error | Cause |
|-------|-------|
| `RFID UID already registered` | Duplicate `rfid_uid_hash` (status 409) |
| `name and rfid_uid are required` | Missing fields (status 400) |

**4c. Toggle Active/Inactive**

Toggles a user's access without deleting the credential:

```
User clicks DEACTIVATE on "John Doe"
        │
        ▼
PATCH /api/users/1                    →  BFF  →  PATCH http://localhost:8080/api/users/1
  body: { is_active: false }                           │
                                                       ▼
                                            UPDATE users SET is_active = false
                                            WHERE id = 1
        │
        ▼
loadUsers() → table refreshes → John Doe shows INACTIVE badge
```

When `is_active = false`, the user's card will be **DENIED** on the next scan ("Access Denied — User Inactive").

**4d. Delete User**

```
User clicks DEL on "John Doe"
        │
        ▼
confirm('Delete user "John Doe"?')
        │ yes
        ▼
DELETE /api/users/1                   →  BFF  →  DELETE http://localhost:8080/api/users/1
                                                       │
                                                       ▼
                                            DELETE FROM users WHERE id = 1
        │
        ▼
loadUsers() → table refreshes → John Doe removed
```

After deletion, the RFID card becomes unknown — scans will return "Access Denied — Unknown Card".

---

## Connection Architecture: BFF Proxy Pattern

The frontend does **not** call the Go backend directly from browser components. Instead, it uses Next.js Route Handlers as a BFF (Backend-For-Frontend) proxy:

```
┌──────────────┐     fetch()      ┌──────────────────┐     fetch()      ┌──────────┐
│  React       │ ──────────────▶ │  Next.js BFF     │ ──────────────▶ │  Go      │
│  Component   │  /api/users     │  Route Handler    │  localhost:8080  │  Backend │
│  (Browser)   │  /api/logs      │  (Server-side)    │  /api/users     │  (:8080) │
│              │  /api/door/…    │                   │  /api/logs      │          │
└──────────────┘                  └──────────────────┘                  └──────────┘

┌──────────────┐   new WebSocket()  ┌──────────┐
│  React       │ ───────────────── │  Go      │
│  Component   │  ws://localhost:   │  Backend │
│  (Browser)   │  8080/api/ws      │  WS Hub  │
└──────────────┘                    └──────────┘
```

**Why BFF?**

- Avoids CORS issues — browser only talks to Next.js (`localhost:3000`)
- Hides the backend URL from the browser
- Backend URL is a server-side env var (`BACKEND_URL`), not exposed to client

### BFF Route Handlers

| Next.js Route | Method | Proxies To | File |
|---------------|--------|------------|------|
| `/api/logs` | `GET` | `GET http://localhost:8080/api/logs?limit=N` | `app/api/logs/route.ts` |
| `/api/logs` | `DELETE` | `DELETE http://localhost:8080/api/logs` | `app/api/logs/route.ts` |
| `/api/users` | `GET` | `GET http://localhost:8080/api/users` | `app/api/users/route.ts` |
| `/api/users` | `POST` | `POST http://localhost:8080/api/users` | `app/api/users/route.ts` |
| `/api/users/[id]` | `PATCH` | `PATCH http://localhost:8080/api/users/{id}` | `app/api/users/[id]/route.ts` |
| `/api/users/[id]` | `DELETE` | `DELETE http://localhost:8080/api/users/{id}` | `app/api/users/[id]/route.ts` |
| `/api/door/override` | `POST` | `POST http://localhost:8080/api/door/override` | `app/api/door/override/route.ts` |

Each BFF handler simply forwards the request body and returns the backend response — no transformation.

### WebSocket (Direct Connection)

The WebSocket connection goes **directly** from the browser to the Go backend — it does not go through the BFF proxy:

```
Browser:  new WebSocket("ws://localhost:8080/api/ws")
               │
               ▼
          Go Backend WS Hub
```

**File:** `lib/websocket.ts`

```typescript
const wsUrl = BACKEND_URL.replace(/^http/, "ws") + "/api/ws";
const ws = new WebSocket(wsUrl);
```

The `BACKEND_URL` is read from `NEXT_PUBLIC_BACKEND_URL` (client-side env var, defaults to `http://localhost:8080`).

---

## WebSocket Hook (`useWebSocket`)

**File:** `lib/websocket.ts:12-55`

A custom React hook that manages the WebSocket lifecycle:

| Feature | Implementation |
|---------|---------------|
| Auto-connect | Opens WebSocket on component mount |
| Auto-reconnect | Retries connection 3 seconds after disconnect |
| Message parsing | Parses JSON, calls `onMessage` callback |
| Cleanup | Closes WebSocket on component unmount |
| Stale detection | Only reconnects if the closed socket is still the active reference |

**Usage in components:**

```typescript
useWebSocket((msg: WSMessage) => {
    if (msg.type === "access_log") {
        setLogs((prev) => [msg.data as AccessLog, ...prev].slice(0, 100));
    } else if (msg.type === "device_status") {
        setDeviceStatus((msg.data as { status?: string }).status || "unknown");
    }
});
```

**Message types:**

| `msg.type` | `msg.data` | Trigger |
|------------|-----------|---------|
| `access_log` | `AccessLog` object | ESP32 scans a card → backend logs → broadcasts |
| `device_status` | `{ status: "online" }` or `{ status: "offline" }` | ESP32 connects (LWT online) or disconnects (LWT offline) |

---

## fetchAPI Utility

**File:** `lib/websocket.ts:57-65`

A typed fetch wrapper used by all components for HTTP requests:

```typescript
export async function fetchAPI<T>(path: string, options?: RequestInit): Promise<T> {
    const res = await fetch(path, options);
    if (!res.ok) {
        const text = await res.text();
        throw new Error(text || res.statusText);
    }
    if (res.status === 204) return undefined as T;
    return res.json();
}
```

- Throws on non-2xx responses (error text from backend is included)
- Returns `undefined` for 204 No Content
- Paths are relative (`/api/users`) — browser sends to Next.js BFF

---

## Complete Data Flow: Full Scan Lifecycle (Frontend Perspective)

```
ESP32 scans card
        │
        ▼
MQTT "door/scan" ──▶ Mosquitto ──▶ Go Backend
                                         │
                                    processScan()
                                         │
                              ┌──────────┼──────────┐
                              │          │          │
                              ▼          ▼          ▼
                         DB Lookup   Insert Log   Publish
                                         │       "door/command"
                                         │          │
                                         ▼          ▼
                              hub.Broadcast()    ESP32 LED+Buzzer
                                         │
                                         ▼
                              WebSocket to browser
                                         │
                                         ▼
                              useWebSocket() callback
                                         │
                                    msg.type === "access_log"
                                         │
                                         ▼
                              setLogs([newLog, ...prev])
                                         │
                                         ▼
                              LiveLogTable re-renders
                              New row appears at top
```

---

## Complete Data Flow: Manual Override (Frontend Perspective)

```
User clicks [ GRANT ] button in DoorControl.tsx
        │
        ▼
confirm("Are you sure...")
        │ yes
        ▼
fetchAPI("/api/door/override", {        ← browser calls Next.js BFF
    method: "POST",
    body: { status: 1, message: "...", action_by: "Dashboard" }
})
        │
        ▼
Next.js BFF (app/api/door/override/route.ts)
        │
        ▼
POST http://localhost:8080/api/door/override
        │
        ▼
Go backend:
  1. Publish {"status":1} to MQTT "door/command"
     └──▶ ESP32 receives → Green LED + 1 beep
  2. INSERT INTO access_logs (rfid_uid_hash="override", device_id="dashboard_override")
  3. hub.Broadcast({ type: "access_log", data: overrideLog })
        │
        ▼
WebSocket delivers to browser
        │
        ▼
LiveLogTable shows new row:
  "Manual Override — Access Granted" by Dashboard
```

---

## UI Layout Structure

```
┌─────────────────────────────────────────────────────────────────────┐
│                          Browser Window                             │
├──────────────┬──────────────────────────────────────────────────────┤
│              │                                                      │
│   SIDEBAR    │                     MAIN CONTENT                     │
│              │                                                      │
│  ┌────────┐  │  ┌──────────────────────────────────────────────┐   │
│  │ RFID   │  │  │  Dashboard                                    │   │
│  │ //     │  │  │  Real-time access monitor                     │   │
│  │ ACCESS │  │  └──────────────────────────────────────────────┘   │
│  │ CTRL   │  │                                                      │
│  ├────────┤  │  ┌──────────────────────────────────────────────┐   │
│  │        │  │  │  Door Override                               │   │
│  │ Dash-  │  │  │  [ GRANT ]  [ DENY ]                        │   │
│  │ board  │  │  └──────────────────────────────────────────────┘   │
│  │        │  │                                                      │
│  │ Users  │  │  ┌──────────────────────────────────────────────┐   │
│  │        │  │  │  Live Access Log              [ CLEAR LOGS ] │   │
│  │        │  │  │  Recent scan events from all devices         │   │
│  ├────────┤  │  │                                              │   │
│  │        │  │  │  ● Device Status: online                     │   │
│  │ SYS    │  │  │                                              │   │
│  │ v0.1.0 │  │  │  ┌──────┬────────┬──────┬────────┬────────┐ │   │
│  └────────┘  │  │  │ Time │ Status │ User │ Device │ Hash   │ │   │
│              │  │  ├──────┼────────┼──────┼────────┼────────┤ │   │
│              │  │  │12:00 │AUTHORIZ│John  │esp32_..│a8b9c0..│ │   │
│              │  │  │11:58 │DENIED  │Unkno│esp32_..│d2e3f4..│ │   │
│              │  │  └──────┴────────┴──────┴────────┴────────┘ │   │
│              │  └──────────────────────────────────────────────┘   │
│              │                                                      │
└──────────────┴──────────────────────────────────────────────────────┘
```

**Users page layout:**

```
┌──────────────┬──────────────────────────────────────────────────────┐
│              │                                                      │
│   SIDEBAR    │                     MAIN CONTENT                     │
│              │                                                      │
│              │  ┌──────────────────────────────────────────────┐   │
│              │  │  Users                                        │   │
│              │  │  // Credential management                     │   │
│              │  └──────────────────────────────────────────────┘   │
│              │                                                      │
│              │  ┌──────────────────────────────────────────────┐   │
│              │  │  Add User                                     │   │
│              │  │  Register a new RFID credential               │   │
│              │  │                                              │   │
│              │  │  [ NAME ] [ RFID UID (hex) ] [ROLE ▼] [ ADD ]│   │
│              │  └──────────────────────────────────────────────┘   │
│              │                                                      │
│              │  ┌──────┬──────┬────────┬────────┬────────┬──────┐  │
│              │  │ Name │ Role │ Status │ Hash   │Created │Action│  │
│              │  ├──────┼──────┼────────┼────────┼────────┼──────┤  │
│              │  │John  │ADMIN │ACTIVE  │a8b9c0.│6/10/24 │DEACT │  │
│              │  │      │      │        │       │        │ DEL  │  │
│              │  ├──────┼──────┼────────┼────────┼────────┼──────┤  │
│              │  │Jane  │EMPLY │INACTIV │d2e3f4.│6/5/24  │ACTIV │  │
│              │  │      │      │        │       │        │ DEL  │  │
│              │  └──────┴──────┴────────┴────────┴────────┴──────┘  │
│              │                                                      │
└──────────────┴──────────────────────────────────────────────────────┘
```

---

## File Structure Reference

| File | Purpose |
|------|---------|
| `app/layout.tsx` | Root layout — sidebar navigation, font loading (JetBrains Mono) |
| `app/page.tsx` | Dashboard page — composes LiveLogTable + DoorControl |
| `app/users/page.tsx` | Users page — add form + CRUD table |
| `components/LiveLogTable.tsx` | Live log table + device status + clear button |
| `components/DoorControl.tsx` | Grant/Deny override buttons |
| `lib/websocket.ts` | `useWebSocket` hook + `fetchAPI` utility |
| `app/api/logs/route.ts` | BFF proxy: GET + DELETE logs |
| `app/api/users/route.ts` | BFF proxy: GET + POST users |
| `app/api/users/[id]/route.ts` | BFF proxy: PATCH + DELETE user by ID |
| `app/api/door/override/route.ts` | BFF proxy: POST door override |

---

## Environment Variables

| Variable | Side | Default | Description |
|----------|------|---------|-------------|
| `BACKEND_URL` | Server (BFF) | `http://localhost:8080` | Go backend URL — used by Next.js Route Handlers |
| `NEXT_PUBLIC_BACKEND_URL` | Client (browser) | `http://localhost:8080` | Go backend URL — used by WebSocket connection |

`NEXT_PUBLIC_` prefix makes the variable available in browser-side code. Without the prefix, it's only accessible in server-side Route Handlers.
