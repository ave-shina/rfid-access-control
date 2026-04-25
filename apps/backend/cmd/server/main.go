package main

import (
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"net/http"
	"os"
	"strconv"
	"strings"
	"time"

	"github.com/gorilla/mux"
	"github.com/joho/godotenv"
	"github.com/rs/zerolog"
	"github.com/rs/zerolog/log"

	"github.com/rfid-access-control/backend/internal/db"
	"github.com/rfid-access-control/backend/internal/models"
	"github.com/rfid-access-control/backend/internal/mqtt"
	"github.com/rfid-access-control/backend/internal/ws"
)

func main() {
	// Load .env from infrastructure/ (works when run from apps/backend/)
	_ = godotenv.Load("../../infrastructure/.env")

	// Configure zerolog
	zerolog.TimeFieldFormat = zerolog.TimeFormatUnix
	log.Logger = log.Output(zerolog.ConsoleWriter{Out: os.Stderr})

	// Read required environment variables
	databaseURL := mustEnv("DATABASE_URL")
	jwtSecret := mustEnv("JWT_SECRET")
	uidPepper := mustEnv("UID_PEPPER")
	mqttBroker := envWithDefault("MQTT_BROKER", "tcp://localhost:1883")
	mqttUser := os.Getenv("MQTT_USERNAME_BACKEND")
	mqttPass := os.Getenv("MQTT_PASSWORD_BACKEND")
	_ = jwtSecret // used in future JWT auth middleware

	// Connect to PostgreSQL
	database, err := db.New(databaseURL)
	if err != nil {
		log.Fatal().Err(err).Msg("Failed to connect to PostgreSQL")
	}
	defer database.Close()
	log.Info().Msg("Connected to PostgreSQL")

	// Create WebSocket hub
	hub := ws.NewHub()

	// Create and connect MQTT handler
	handler := mqtt.NewHandler(mqttBroker, "go_backend", mqttUser, mqttPass, database, hub, uidPepper)
	if err := handler.Connect(); err != nil {
		log.Fatal().Err(err).Msg("Failed to connect to Mosquitto Broker")
	}
	defer handler.Disconnect()

	// Set up HTTP router
	r := mux.NewRouter()

	// Health check
	r.HandleFunc("/healthz", func(w http.ResponseWriter, r *http.Request) {
		if err := database.Ping(); err != nil {
			w.WriteHeader(http.StatusServiceUnavailable)
			return
		}
		w.WriteHeader(http.StatusOK)
	}).Methods("GET")

	// WebSocket
	r.HandleFunc("/api/ws", hub.ServeWS)

	// API routes
	api := r.PathPrefix("/api").Subrouter()

	// GET /api/logs
	api.HandleFunc("/logs", func(w http.ResponseWriter, r *http.Request) {
		limitStr := r.URL.Query().Get("limit")
		limit := 50
		if limitStr != "" {
			if l, err := strconv.Atoi(limitStr); err == nil {
				limit = l
			}
		}
		logs, err := database.GetRecentLogs(limit)
		if err != nil {
			log.Error().Err(err).Msg("Failed to get logs")
			http.Error(w, "Internal Server Error", http.StatusInternalServerError)
			return
		}
		writeJSON(w, logs)
	}).Methods("GET")

	// GET /api/users
	api.HandleFunc("/users", func(w http.ResponseWriter, r *http.Request) {
		users, err := database.GetAllUsers()
		if err != nil {
			log.Error().Err(err).Msg("Failed to get users")
			http.Error(w, "Internal Server Error", http.StatusInternalServerError)
			return
		}
		writeJSON(w, users)
	}).Methods("GET")

	// POST /api/users
	api.HandleFunc("/users", func(w http.ResponseWriter, r *http.Request) {
		var req models.CreateUserRequest
		if err := json.NewDecoder(r.Body).Decode(&req); err != nil {
			http.Error(w, "Invalid JSON", http.StatusBadRequest)
			return
		}
		if req.Name == "" || req.RFIDUID == "" {
			http.Error(w, "name and rfid_uid are required", http.StatusBadRequest)
			return
		}
		if req.Role == "" {
			req.Role = "employee"
		}

		// Step 1: hash the raw UID
		rawHash := sha256.Sum256([]byte(req.RFIDUID))
		uidHash := hex.EncodeToString(rawHash[:])

		// Step 2: apply pepper
		peppered := sha256.Sum256([]byte(uidHash + uidPepper))
		pepperedHash := hex.EncodeToString(peppered[:])

		user, err := database.CreateUser(req.Name, pepperedHash, req.Role)
		if err != nil {
			if strings.Contains(err.Error(), "duplicate key") || strings.Contains(err.Error(), "unique_constraint") {
				http.Error(w, "RFID UID already registered", http.StatusConflict)
				return
			}
			log.Error().Err(err).Msg("Failed to create user")
			http.Error(w, "Internal Server Error", http.StatusInternalServerError)
			return
		}
		writeJSON(w, user)
	}).Methods("POST")

	// PATCH /api/users/{id}
	api.HandleFunc("/users/{id}", func(w http.ResponseWriter, r *http.Request) {
		id, err := strconv.Atoi(mux.Vars(r)["id"])
		if err != nil {
			http.Error(w, "Invalid user ID", http.StatusBadRequest)
			return
		}
		var body struct {
			IsActive *bool `json:"is_active"`
		}
		if err := json.NewDecoder(r.Body).Decode(&body); err != nil {
			http.Error(w, "Invalid JSON", http.StatusBadRequest)
			return
		}
		if body.IsActive == nil {
			http.Error(w, "is_active is required", http.StatusBadRequest)
			return
		}
		user, err := database.ToggleUserActive(id, *body.IsActive)
		if err != nil {
			log.Error().Err(err).Msg("Failed to toggle user")
			http.Error(w, "Internal Server Error", http.StatusInternalServerError)
			return
		}
		if user == nil {
			http.Error(w, "User not found", http.StatusNotFound)
			return
		}
		writeJSON(w, user)
	}).Methods("PATCH")

	// DELETE /api/users/{id}
	api.HandleFunc("/users/{id}", func(w http.ResponseWriter, r *http.Request) {
		id, err := strconv.Atoi(mux.Vars(r)["id"])
		if err != nil {
			http.Error(w, "Invalid user ID", http.StatusBadRequest)
			return
		}
		if err := database.DeleteUser(id); err != nil {
			http.Error(w, "User not found", http.StatusNotFound)
			return
		}
		w.WriteHeader(http.StatusNoContent)
	}).Methods("DELETE")

	// POST /api/door/override
	api.HandleFunc("/door/override", func(w http.ResponseWriter, r *http.Request) {
		var req models.OverrideRequest
		if err := json.NewDecoder(r.Body).Decode(&req); err != nil {
			http.Error(w, "Invalid JSON", http.StatusBadRequest)
			return
		}
		if req.Status != 0 && req.Status != 1 {
			http.Error(w, "status must be 0 or 1", http.StatusBadRequest)
			return
		}
		if req.Message == "" {
			if req.Status == 1 {
				req.Message = "Manual Override — Access Granted"
			} else {
				req.Message = "Manual Override — Access Denied"
			}
		}
		if req.ActionBy == "" {
			req.ActionBy = "Dashboard"
		}
		cmd := &models.CommandPayload{
			Status:   req.Status,
			Message:  req.Message,
			ActionBy: req.ActionBy,
		}
		if err := handler.PublishCommand(cmd); err != nil {
			log.Error().Err(err).Msg("Failed to publish override command")
			http.Error(w, "Failed to send command", http.StatusInternalServerError)
			return
		}

		// Log the override
		overrideLog := &models.AccessLog{
			RFIDUIDHash: "override",
			Status:      map[int]string{1: "AUTHORIZED", 0: "DENIED"}[req.Status],
			ActionBy:    req.ActionBy,
			DeviceID:    "dashboard_override",
			Timestamp:   time.Now(),
		}
		if logErr := database.InsertAccessLog(overrideLog); logErr != nil {
			log.Error().Err(logErr).Msg("failed to log override")
		}

		writeJSON(w, cmd)
	}).Methods("POST")

	// Start HTTP server
	addr := ":8080"
	log.Info().Str("addr", addr).Msg("HTTP server starting")
	if err := http.ListenAndServe(addr, r); err != nil {
		log.Fatal().Err(err).Msg("HTTP server failed")
	}
}

func mustEnv(key string) string {
	val := os.Getenv(key)
	if val == "" {
		log.Fatal().Str("key", key).Msg("required environment variable not set")
	}
	return val
}

func envWithDefault(key, defaultVal string) string {
	val := os.Getenv(key)
	if val == "" {
		return defaultVal
	}
	return val
}

func writeJSON(w http.ResponseWriter, v interface{}) {
	w.Header().Set("Content-Type", "application/json")
	json.NewEncoder(w).Encode(v)
}
