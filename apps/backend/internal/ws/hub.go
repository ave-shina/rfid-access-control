package ws

import (
	"encoding/json"
	"net/http"
	"sync"
	"time"

	"github.com/gorilla/websocket"
	"github.com/rs/zerolog/log"

	"github.com/rfid-access-control/backend/internal/models"
)

var upgrader = websocket.Upgrader{
	CheckOrigin: func(r *http.Request) bool {
		return true // Allow all origins in development
	},
}

// Hub maintains the set of active WebSocket clients and broadcasts messages.
type Hub struct {
	mu          sync.RWMutex
	clients     map[*websocket.Conn]bool
	lastStatus  []byte
}

// NewHub creates a new WebSocket hub.
func NewHub() *Hub {
	return &Hub{
		clients: make(map[*websocket.Conn]bool),
	}
}

// Register adds a WebSocket client.
func (h *Hub) Register(conn *websocket.Conn) {
	h.mu.Lock()
	h.clients[conn] = true
	h.mu.Unlock()
	log.Info().Int("total_clients", len(h.clients)).Msg("WebSocket client connected")

	// Send cached device status to the new client immediately
	h.mu.RLock()
	cached := h.lastStatus
	h.mu.RUnlock()
	if cached != nil {
		if err := conn.WriteMessage(websocket.TextMessage, cached); err != nil {
			log.Warn().Err(err).Msg("failed to send cached status to new client")
		}
	}
}

// Unregister removes a WebSocket client.
func (h *Hub) Unregister(conn *websocket.Conn) {
	h.mu.Lock()
	delete(h.clients, conn)
	h.mu.Unlock()
	conn.Close()
	log.Info().Int("total_clients", len(h.clients)).Msg("WebSocket client disconnected")
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

	h.mu.RLock()
	defer h.mu.RUnlock()

	for conn := range h.clients {
		if err := conn.WriteMessage(websocket.TextMessage, data); err != nil {
			go h.Unregister(conn)
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
	h.Register(conn)

	// Read loop to detect disconnections
	go func() {
		defer h.Unregister(conn)
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
		defer h.Unregister(conn)
		ticker := time.NewTicker(30 * time.Second)
		defer ticker.Stop()
		for range ticker.C {
			if err := conn.WriteMessage(websocket.PingMessage, nil); err != nil {
				break
			}
		}
	}()
}
