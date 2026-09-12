package models

import "time"

// ScanPayload is the inbound MQTT message from ESP32 on door/scan.
type ScanPayload struct {
	UIDHash   string `json:"uid_hash"`
	DeviceID  string `json:"device_id"`
	Nonce     string `json:"nonce"`
	Timestamp int64  `json:"timestamp"`
}

// CommandPayload is the outbound MQTT message to ESP32 on door/command.
type CommandPayload struct {
	Status   int    `json:"status"`
	Message  string `json:"message"`
	ActionBy string `json:"action_by"`
}

// User represents a row in the users table.
type User struct {
	ID          int       `json:"id"`
	Name        string    `json:"name"`
	RFIDUIDHash string    `json:"rfid_uid_hash"`
	Role        string    `json:"role"`
	IsActive    bool      `json:"is_active"`
	CreatedAt   time.Time `json:"created_at"`
}

// AccessLog represents a row in the access_logs table.
type AccessLog struct {
	ID          int       `json:"id"`
	RFIDUIDHash string    `json:"rfid_uid_hash"`
	Status      string    `json:"status"`
	ActionBy    string    `json:"action_by"`
	DeviceID    string    `json:"device_id"`
	Timestamp   time.Time `json:"timestamp"`
}

// CreateUserRequest is the JSON body for POST /api/users.
type CreateUserRequest struct {
	Name    string `json:"name"`
	RFIDUID string `json:"rfid_uid"` // raw UID from card (will be hashed + peppered)
	Role    string `json:"role"`
}

// OverrideRequest is the JSON body for POST /api/door/override.
type OverrideRequest struct {
	Status   int    `json:"status"` // 1 = grant, 0 = deny
	Message  string `json:"message"`
	ActionBy string `json:"action_by"`
}

// WSMessage is a message broadcast to WebSocket clients.
type WSMessage struct {
	Type string      `json:"type"` // "access_log" or "device_status"
	Data interface{} `json:"data"`
}
