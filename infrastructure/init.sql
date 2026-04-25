-- Authorized users mapped to RFID tags
CREATE TABLE IF NOT EXISTS users (
  id            SERIAL PRIMARY KEY,
  name          VARCHAR(100) NOT NULL,
  rfid_uid_hash VARCHAR(64) UNIQUE NOT NULL,
  role          VARCHAR(50) DEFAULT 'employee',
  is_active     BOOLEAN DEFAULT TRUE,
  created_at    TIMESTAMP DEFAULT CURRENT_TIMESTAMP
);

-- Immutable audit trail of all access attempts
CREATE TABLE IF NOT EXISTS access_logs (
  id            SERIAL PRIMARY KEY,
  rfid_uid_hash VARCHAR(64) NOT NULL,
  status        VARCHAR(20) NOT NULL,
  action_by     VARCHAR(100),
  device_id     VARCHAR(50),
  timestamp     TIMESTAMP DEFAULT CURRENT_TIMESTAMP
);

-- Required indexes
CREATE INDEX IF NOT EXISTS idx_access_logs_device_time ON access_logs(device_id, timestamp DESC);
CREATE INDEX IF NOT EXISTS idx_access_logs_time        ON access_logs(timestamp DESC);
CREATE INDEX IF NOT EXISTS idx_access_logs_status      ON access_logs(status);
