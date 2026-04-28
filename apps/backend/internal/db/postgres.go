package db

import (
	"database/sql"
	"fmt"
	"time"

	_ "github.com/lib/pq"

	"github.com/rfid-access-control/backend/internal/models"
)

type PostgresDB struct {
	db *sql.DB
}

// New creates a connection pool and verifies connectivity.
func New(databaseURL string) (*PostgresDB, error) {
	db, err := sql.Open("postgres", databaseURL)
	if err != nil {
		return nil, fmt.Errorf("open db: %w", err)
	}
	db.SetMaxOpenConns(25)
	db.SetMaxIdleConns(10)
	db.SetConnMaxLifetime(5 * time.Minute)

	if err := db.Ping(); err != nil {
		return nil, fmt.Errorf("ping db: %w", err)
	}
	return &PostgresDB{db: db}, nil
}

// Ping verifies the database is reachable.
func (p *PostgresDB) Ping() error {
	return p.db.Ping()
}

// Close closes the connection pool.
func (p *PostgresDB) Close() error {
	return p.db.Close()
}

// LookupUser finds a user by their peppered RFID UID hash.
// The caller is responsible for applying the pepper before calling this.
func (p *PostgresDB) LookupUser(pepperedHash string) (*models.User, error) {
	var u models.User
	err := p.db.QueryRow(
		"SELECT id, name, rfid_uid_hash, role, is_active, created_at FROM users WHERE rfid_uid_hash = $1",
		pepperedHash,
	).Scan(&u.ID, &u.Name, &u.RFIDUIDHash, &u.Role, &u.IsActive, &u.CreatedAt)
	if err == sql.ErrNoRows {
		return nil, nil
	}
	if err != nil {
		return nil, fmt.Errorf("lookup user: %w", err)
	}
	return &u, nil
}

// InsertAccessLog records an access attempt. Logging failure must not block door actuation,
// so callers should handle errors but continue.
func (p *PostgresDB) InsertAccessLog(log *models.AccessLog) error {
	_, err := p.db.Exec(
		"INSERT INTO access_logs (rfid_uid_hash, status, action_by, device_id, timestamp) VALUES ($1, $2, $3, $4, $5)",
		log.RFIDUIDHash, log.Status, log.ActionBy, log.DeviceID, log.Timestamp,
	)
	return err
}

// GetRecentLogs returns the most recent access logs, ordered newest first.
func (p *PostgresDB) GetRecentLogs(limit int) ([]models.AccessLog, error) {
	if limit <= 0 || limit > 500 {
		limit = 50
	}
	rows, err := p.db.Query(
		"SELECT id, rfid_uid_hash, status, action_by, device_id, timestamp FROM access_logs ORDER BY timestamp DESC LIMIT $1",
		limit,
	)
	if err != nil {
		return nil, fmt.Errorf("get recent logs: %w", err)
	}
	defer rows.Close()

	var logs []models.AccessLog
	for rows.Next() {
		var l models.AccessLog
		if err := rows.Scan(&l.ID, &l.RFIDUIDHash, &l.Status, &l.ActionBy, &l.DeviceID, &l.Timestamp); err != nil {
			return nil, fmt.Errorf("scan log row: %w", err)
		}
		logs = append(logs, l)
	}
	return logs, rows.Err()
}

// ClearLogs deletes all access logs.
func (p *PostgresDB) ClearLogs() error {
	_, err := p.db.Exec("DELETE FROM access_logs")
	if err != nil {
		return fmt.Errorf("clear logs: %w", err)
	}
	return nil
}

// GetAllUsers returns all registered users.
func (p *PostgresDB) GetAllUsers() ([]models.User, error) {
	rows, err := p.db.Query(
		"SELECT id, name, rfid_uid_hash, role, is_active, created_at FROM users ORDER BY created_at DESC",
	)
	if err != nil {
		return nil, fmt.Errorf("get all users: %w", err)
	}
	defer rows.Close()

	var users []models.User
	for rows.Next() {
		var u models.User
		if err := rows.Scan(&u.ID, &u.Name, &u.RFIDUIDHash, &u.Role, &u.IsActive, &u.CreatedAt); err != nil {
			return nil, fmt.Errorf("scan user row: %w", err)
		}
		users = append(users, u)
	}
	return users, rows.Err()
}

// CreateUser inserts a new user. The rfidUIDHash should already be the peppered hash.
func (p *PostgresDB) CreateUser(name, pepperedHash, role string) (*models.User, error) {
	var u models.User
	err := p.db.QueryRow(
		"INSERT INTO users (name, rfid_uid_hash, role) VALUES ($1, $2, $3) RETURNING id, name, rfid_uid_hash, role, is_active, created_at",
		name, pepperedHash, role,
	).Scan(&u.ID, &u.Name, &u.RFIDUIDHash, &u.Role, &u.IsActive, &u.CreatedAt)
	if err != nil {
		return nil, fmt.Errorf("create user: %w", err)
	}
	return &u, nil
}

// ToggleUserActive flips the is_active flag for a user.
func (p *PostgresDB) ToggleUserActive(id int, active bool) (*models.User, error) {
	var u models.User
	err := p.db.QueryRow(
		"UPDATE users SET is_active = $1 WHERE id = $2 RETURNING id, name, rfid_uid_hash, role, is_active, created_at",
		active, id,
	).Scan(&u.ID, &u.Name, &u.RFIDUIDHash, &u.Role, &u.IsActive, &u.CreatedAt)
	if err == sql.ErrNoRows {
		return nil, nil
	}
	if err != nil {
		return nil, fmt.Errorf("toggle user active: %w", err)
	}
	return &u, nil
}

// DeleteUser removes a user by ID.
func (p *PostgresDB) DeleteUser(id int) error {
	res, err := p.db.Exec("DELETE FROM users WHERE id = $1", id)
	if err != nil {
		return fmt.Errorf("delete user: %w", err)
	}
	if n, _ := res.RowsAffected(); n == 0 {
		return sql.ErrNoRows
	}
	return nil
}
