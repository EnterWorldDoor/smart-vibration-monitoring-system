package config

import (
	"os"
	"path/filepath"
	"testing"
)

func TestLoadValidConfig(t *testing.T) {
	path := filepath.Join("testdata", "valid.yaml")
	cfg, err := Load(path)
	if err != nil {
		t.Fatalf("Load failed: %v", err)
	}
	if cfg.Server.Port != 8090 {
		t.Errorf("expected port 8090, got %d", cfg.Server.Port)
	}
	if cfg.Firmware.StoragePath != "/app/firmware" {
		t.Errorf("expected /app/firmware, got %s", cfg.Firmware.StoragePath)
	}
	if cfg.Firmware.SiteID != "factory1" {
		t.Errorf("expected factory1, got %s", cfg.Firmware.SiteID)
	}
	if cfg.TimescaleDB.PoolMaxConns != 5 {
		t.Errorf("expected PoolMaxConns=5, got %d", cfg.TimescaleDB.PoolMaxConns)
	}
}

func TestLoadMissingFile(t *testing.T) {
	_, err := Load("nonexistent.yaml")
	if err == nil {
		t.Fatal("expected error for missing file")
	}
}

func TestDSN(t *testing.T) {
	ts := TimescaleDBConfig{
		Host:     "timescaledb",
		Port:     5432,
		User:     "edgevib",
		Password: "secret",
		Database: "edgevib_ts",
	}
	expected := "postgres://edgevib:secret@timescaledb:5432/edgevib_ts?sslmode=disable"
	if got := ts.DSN(); got != expected {
		t.Errorf("DSN mismatch\n  got:  %s\n  want: %s", got, expected)
	}
}

func TestEnvExpansion(t *testing.T) {
	os.Setenv("DB_PASSWORD", "testpass")
	defer os.Unsetenv("DB_PASSWORD")

	path := filepath.Join("testdata", "with_env.yaml")
	cfg, err := Load(path)
	if err != nil {
		t.Fatalf("Load failed: %v", err)
	}
	if cfg.TimescaleDB.Password != "testpass" {
		t.Errorf("expected env-expanded password 'testpass', got '%s'", cfg.TimescaleDB.Password)
	}
}

func TestDefaultPoolMaxConns(t *testing.T) {
	path := filepath.Join("testdata", "no_pool.yaml")
	cfg, err := Load(path)
	if err != nil {
		t.Fatalf("Load failed: %v", err)
	}
	if cfg.TimescaleDB.PoolMaxConns != 5 {
		t.Errorf("expected default PoolMaxConns=5, got %d", cfg.TimescaleDB.PoolMaxConns)
	}
}
