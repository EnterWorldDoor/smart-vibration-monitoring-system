package config

import (
	"os"
	"path/filepath"
	"testing"
)

func TestLoadValidConfig(t *testing.T) {
	content := `
server:
  host: "0.0.0.0"
  port: 8080
  read_timeout: 10s
  write_timeout: 30s

timescaledb:
  host: "localhost"
  port: 5432
  user: "testuser"
  password: "testpass"
  database: "testdb"
  pool_max_conns: 5

mqtt:
  broker: "tcp://localhost:1883"
  client_id: "test-api-server"
  topics:
    - "test/topic"

auth:
  enabled: false
  api_key: ""

log:
  level: "info"
  format: "json"
`
	tmpDir := t.TempDir()
	configPath := filepath.Join(tmpDir, "test-config.yaml")
	if err := os.WriteFile(configPath, []byte(content), 0644); err != nil {
		t.Fatal(err)
	}

	cfg, err := Load(configPath)
	if err != nil {
		t.Fatalf("Load failed: %v", err)
	}

	if cfg.Server.Port != 8080 {
		t.Errorf("Port = %d, want 8080", cfg.Server.Port)
	}
	if cfg.TimescaleDB.Host != "localhost" {
		t.Errorf("DB Host = %s, want localhost", cfg.TimescaleDB.Host)
	}
	if cfg.TimescaleDB.PoolMaxConns != 5 {
		t.Errorf("PoolMaxConns = %d, want 5", cfg.TimescaleDB.PoolMaxConns)
	}
	if len(cfg.MQTT.Topics) != 1 {
		t.Errorf("Topics count = %d, want 1", len(cfg.MQTT.Topics))
	}
}

func TestLoadConfigEnvVarExpansion(t *testing.T) {
	os.Setenv("DB_PASSWORD", "secret123")
	defer os.Unsetenv("DB_PASSWORD")

	content := `
server:
  host: "0.0.0.0"
  port: 8080
  read_timeout: 10s
  write_timeout: 30s

timescaledb:
  host: "localhost"
  port: 5432
  user: "edgevib"
  password: "${DB_PASSWORD:-edgevib123}"
  database: "edgevib_ts"
  pool_max_conns: 5

mqtt:
  broker: "tcp://localhost:1883"
  client_id: "test"

auth:
  enabled: false
  api_key: ""

log:
  level: "info"
  format: "json"
`
	tmpDir := t.TempDir()
	configPath := filepath.Join(tmpDir, "test-config.yaml")
	if err := os.WriteFile(configPath, []byte(content), 0644); err != nil {
		t.Fatal(err)
	}

	cfg, err := Load(configPath)
	if err != nil {
		t.Fatalf("Load failed: %v", err)
	}

	if cfg.TimescaleDB.Password != "secret123" {
		t.Errorf("Password = %q, want %q (env var)", cfg.TimescaleDB.Password, "secret123")
	}
}

func TestLoadConfigDefaultValue(t *testing.T) {
	content := `
server:
  host: "0.0.0.0"
  port: 8080
  read_timeout: 10s
  write_timeout: 30s

timescaledb:
  host: "localhost"
  port: 5432
  user: "edgevib"
  password: "${NONEXISTENT:-fallback123}"
  database: "edgevib_ts"
  pool_max_conns: 5

mqtt:
  broker: "tcp://localhost:1883"
  client_id: "test"

auth:
  enabled: false
  api_key: ""

log:
  level: "info"
  format: "json"
`
	tmpDir := t.TempDir()
	configPath := filepath.Join(tmpDir, "test-config.yaml")
	if err := os.WriteFile(configPath, []byte(content), 0644); err != nil {
		t.Fatal(err)
	}

	cfg, err := Load(configPath)
	if err != nil {
		t.Fatalf("Load failed: %v", err)
	}

	if cfg.TimescaleDB.Password != "fallback123" {
		t.Errorf("Password = %q, want %q (default)", cfg.TimescaleDB.Password, "fallback123")
	}
}

func TestLoadConfigMissingFile(t *testing.T) {
	_, err := Load("/nonexistent/path/config.yaml")
	if err == nil {
		t.Error("Expected error for missing file")
	}
}
