package ws

import (
	"encoding/json"
	"net/http"
	"os"
	"strings"
	"sync"
	"time"

	"github.com/gorilla/websocket"
	"github.com/rs/zerolog/log"

	"github.com/rfid-access-control/backend/internal/models"
)

var upgrader = websocket.Upgrader{
	CheckOrigin: func(r *http.Request) bool {
		// Allow non-browser clients (no Origin header) and a configurable
		// allowlist of browser origins. Defaults to the Next.js dev server.
		origin := r.Header.Get("Origin")
		if origin == "" {
			return true
		}
		allowed := os.Getenv("WS_ALLOWED_ORIGINS")
		if allowed == "" {
			allowed = "http://localhost:3000"
		}
		for _, o := range strings.Split(allowed, ",") {
			if strings.TrimSpace(o) == origin {
				return true
			}
		}
		return false
	},
}

// client wraps a WebSocket connection with a write mutex. gorilla/websocket
// supports at most one concurrent writer per connection, and the hub writes
// from several goroutines (Broadcast, the ping loop, the cached-status send
// on Register) — so every write must be serialized through writeMessage.
type client struct {
	conn *websocket.Conn
	mu   sync.Mutex
}

// writeMessage serializes writes to the underlying connection.
func (c *client) writeMessage(messageType int, data []byte) error {
	c.mu.Lock()
	defer c.mu.Unlock()
	return c.conn.WriteMessage(messageType, data)
}

// Hub maintains the set of active WebSocket clients and broadcasts messages.
type Hub struct {
	mu         sync.RWMutex
	clients    map[*client]bool
	lastStatus []byte
}

// NewHub creates a new WebSocket hub.
func NewHub() *Hub {
	return &Hub{
		clients: make(map[*client]bool),
	}
}

// Register adds a WebSocket client and returns it so ServeWS can use the
// same handle in its read/ping loops.
func (h *Hub) Register(conn *websocket.Conn) *client {
	c := &client{conn: conn}
	h.mu.Lock()
	h.clients[c] = true
	total := len(h.clients)
	cached := h.lastStatus
	h.mu.Unlock()
	log.Info().Int("total_clients", total).Msg("WebSocket client connected")

	// Send cached device status to the new client immediately
	if cached != nil {
		if err := c.writeMessage(websocket.TextMessage, cached); err != nil {
			log.Warn().Err(err).Msg("failed to send cached status to new client")
		}
	}
	return c
}

// Unregister removes a WebSocket client. Safe to call multiple times — only
// the first call closes the connection and logs (the read loop and the ping
// loop both defer it).
func (h *Hub) Unregister(c *client) {
	h.mu.Lock()
	_, existed := h.clients[c]
	delete(h.clients, c)
	total := len(h.clients)
	h.mu.Unlock()
	if !existed {
		return
	}
	c.conn.Close()
	log.Info().Int("total_clients", total).Msg("WebSocket client disconnected")
}

// Broadcast sends a message to all connected WebSocket clients.
func (h *Hub) Broadcast(msg models.WSMessage) {
	data, err := json.Marshal(msg)
	if err != nil {
		log.Error().Err(err).Msg("failed to marshal WS message")
		return
	}

	// Cache device_status messages
	if msg.Type == "device_status" {
		h.mu.Lock()
		h.lastStatus = data
		h.mu.Unlock()
	}

	// Snapshot the client set under the read lock, then write outside the
	// lock so a slow client cannot block broadcasts or Register/Unregister.
	// Individual writes are serialized per connection by client.writeMessage.
	h.mu.RLock()
	clients := make([]*client, 0, len(h.clients))
	for c := range h.clients {
		clients = append(clients, c)
	}
	h.mu.RUnlock()

	for _, c := range clients {
		if err := c.writeMessage(websocket.TextMessage, data); err != nil {
			h.Unregister(c)
		}
	}
}

// ServeWS handles WebSocket upgrade requests.
func (h *Hub) ServeWS(w http.ResponseWriter, r *http.Request) {
	conn, err := upgrader.Upgrade(w, r, nil)
	if err != nil {
		log.Error().Err(err).Msg("WebSocket upgrade failed")
		return
	}
	c := h.Register(conn)

	// Read loop to detect disconnections
	go func() {
		defer h.Unregister(c)
		conn.SetReadLimit(512)
		conn.SetReadDeadline(time.Now().Add(60 * time.Second))
		conn.SetPongHandler(func(string) error {
			conn.SetReadDeadline(time.Now().Add(60 * time.Second))
			return nil
		})
		for {
			_, _, err := conn.ReadMessage()
			if err != nil {
				break
			}
		}
	}()

	// Ping loop to keep connection alive
	go func() {
		defer h.Unregister(c)
		ticker := time.NewTicker(30 * time.Second)
		defer ticker.Stop()
		for range ticker.C {
			if err := c.writeMessage(websocket.PingMessage, nil); err != nil {
				break
			}
		}
	}()
}
