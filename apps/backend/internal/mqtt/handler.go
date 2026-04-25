package mqtt

import (
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"fmt"
	"sync"
	"time"

	pahomqtt "github.com/eclipse/paho.mqtt.golang"
	"github.com/rs/zerolog/log"
	"golang.org/x/time/rate"

	"github.com/rfid-access-control/backend/internal/db"
	"github.com/rfid-access-control/backend/internal/models"
	"github.com/rfid-access-control/backend/internal/ws"
)

// Handler manages MQTT subscriptions and scan processing.
type Handler struct {
	client    pahomqtt.Client
	db        *db.PostgresDB
	hub       *ws.Hub
	pepper    string
	nonce     *NonceCache
	limiters  sync.Map
}

// NewHandler creates a new MQTT handler.
func NewHandler(broker, clientID, username, password string, database *db.PostgresDB, hub *ws.Hub, pepper string) *Handler {
	h := &Handler{
		db:     database,
		hub:    hub,
		pepper: pepper,
		nonce:  NewNonceCache(60 * time.Second),
	}

	opts := pahomqtt.NewClientOptions().
		AddBroker(broker).
		SetClientID(clientID).
		SetAutoReconnect(true).
		SetConnectRetry(true).
		SetOnConnectHandler(func(c pahomqtt.Client) {
			log.Info().Msg("Connected to Mosquitto Broker")
			c.Subscribe("door/scan", 1, h.handleScan)
			c.Subscribe("door/status", 0, h.handleStatus)
			log.Info().Msg("Subscribed to door/scan and door/status")
		}).
		SetConnectionLostHandler(func(c pahomqtt.Client, err error) {
			log.Error().Err(err).Msg("MQTT connection lost")
		})

	if username != "" {
		opts.SetUsername(username)
		opts.SetPassword(password)
	}

	h.client = pahomqtt.NewClient(opts)
	return h
}

// Connect initiates the MQTT connection (blocking until connected or retries exhaust).
func (h *Handler) Connect() error {
	token := h.client.Connect()
	token.Wait()
	if token.Error() != nil {
		return fmt.Errorf("mqtt connect: %w", token.Error())
	}
	return nil
}

// Disconnect cleanly closes the MQTT connection.
func (h *Handler) Disconnect() {
	h.client.Disconnect(250)
}

// PublishCommand sends a command to the ESP32 on door/command.
func (h *Handler) PublishCommand(cmd *models.CommandPayload) error {
	payload, err := json.Marshal(cmd)
	if err != nil {
		return fmt.Errorf("marshal command: %w", err)
	}
	token := h.client.Publish("door/command", 1, false, payload)
	token.Wait()
	if token.Error() != nil {
		return fmt.Errorf("publish command: %w", token.Error())
	}
	return nil
}

// getLimiter returns a rate limiter for the given device ID (1 message per 2 seconds).
func (h *Handler) getLimiter(deviceID string) *rate.Limiter {
	lim, _ := h.limiters.LoadOrStore(deviceID, rate.NewLimiter(rate.Every(2*time.Second), 1))
	return lim.(*rate.Limiter)
}

// pepperHash applies the server-side pepper to the incoming UID hash.
func (h *Handler) pepperHash(uidHash string) string {
	combined := uidHash + h.pepper
	hash := sha256.Sum256([]byte(combined))
	return hex.EncodeToString(hash[:])
}

// handleScan processes an incoming RFID scan from the ESP32.
func (h *Handler) handleScan(client pahomqtt.Client, msg pahomqtt.Message) {
	go h.processScan(msg.Payload())
}

func (h *Handler) processScan(raw []byte) {
	var payload models.ScanPayload
	if err := json.Unmarshal(raw, &payload); err != nil {
		log.Warn().Err(err).Msg("invalid scan payload")
		return
	}

	// Rate limit check
	if !h.getLimiter(payload.DeviceID).Allow() {
		log.Warn().Str("device", payload.DeviceID).Msg("rate limit exceeded, dropping scan")
		return
	}

	// Input validation
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

	// Anti-replay check
	if !h.nonce.CheckAndStore(payload.Nonce) {
		log.Warn().Str("nonce", payload.Nonce).Msg("replay detected, duplicate nonce")
		return
	}

	// Apply pepper and look up user
	pepperedHash := h.pepperHash(payload.UIDHash)
	user, err := h.db.LookupUser(pepperedHash)
	if err != nil {
		log.Error().Err(err).Msg("database lookup failed")
		return
	}

	// Evaluate access
	var status string
	var message string
	var actionBy string
	var cmdStatus int

	if user != nil && user.IsActive {
		status = "AUTHORIZED"
		message = "Access Granted"
		actionBy = user.Name
		cmdStatus = 1
	} else if user != nil && !user.IsActive {
		status = "DENIED"
		message = "Access Denied — User Inactive"
		actionBy = user.Name
		cmdStatus = 0
	} else {
		status = "DENIED"
		message = "Access Denied — Unknown Card"
		actionBy = "Unknown"
		cmdStatus = 0
	}

	log.Info().
		Str("device_id", payload.DeviceID).
		Str("status", status).
		Str("user", actionBy).
		Msg("access evaluated")

	// Log the access attempt (failure to log must not block door actuation)
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
		accessLog.ID = 0 // will be set by broadcast consumers if needed
		// Broadcast to WebSocket clients
		h.hub.Broadcast(models.WSMessage{
			Type: "access_log",
			Data: accessLog,
		})
	}

	// Actuate the door
	cmd := &models.CommandPayload{
		Status:   cmdStatus,
		Message:  message,
		ActionBy: actionBy,
	}
	if pubErr := h.PublishCommand(cmd); pubErr != nil {
		log.Error().Err(pubErr).Msg("failed to publish command")
	}
}

// handleStatus processes device status messages (online/offline via LWT).
func (h *Handler) handleStatus(client pahomqtt.Client, msg pahomqtt.Message) {
	log.Info().Str("payload", string(msg.Payload())).Msg("device status update")
	h.hub.Broadcast(models.WSMessage{
		Type: "device_status",
		Data: json.RawMessage(msg.Payload()),
	})
}
