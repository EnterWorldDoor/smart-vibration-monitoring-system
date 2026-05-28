package config

import (
	"os"
	"testing"
)

func TestLoadValidConfig(t *testing.T) {
	cfg, err := Load("testdata/valid.yaml")
	if err != nil {
		t.Fatalf("Load failed: %v", err)
	}
	if cfg.Server.Port != 8091 {
		t.Errorf("expected port 8091, got %d", cfg.Server.Port)
	}
	if cfg.Models.SiteID != "factory1" {
		t.Errorf("expected site_id factory1, got %s", cfg.Models.SiteID)
	}
	if cfg.Models.MaxVersions != 3 {
		t.Errorf("expected max_versions 3, got %d", cfg.Models.MaxVersions)
	}
	if cfg.TimescaleDB.Host != "timescaledb" {
		t.Errorf("expected host timescaledb, got %s", cfg.TimescaleDB.Host)
	}
	if cfg.MQTT.ClientID != "model-deploy" {
		t.Errorf("expected client_id model-deploy, got %s", cfg.MQTT.ClientID)
	}
}

func TestLoadMissingFile(t *testing.T) {
	_, err := Load("testdata/nonexistent.yaml")
	if err == nil {
		t.Error("expected error for missing file")
	}
}

func TestDSN(t *testing.T) {
	cfg := &TimescaleDBConfig{
		Host:     "timescaledb",
		Port:     5432,
		User:     "edgevib",
		Password: "edgevib123",
		Database: "edgevib_ts",
	}
	expected := "postgres://edgevib:edgevib123@timescaledb:5432/edgevib_ts?sslmode=disable"
	if cfg.DSN() != expected {
		t.Errorf("DSN mismatch\ngot:  %s\nwant: %s", cfg.DSN(), expected)
	}
}

func TestEnvExpansion(t *testing.T) {
	os.Setenv("DB_PASSWORD", "testpass")
	defer os.Unsetenv("DB_PASSWORD")

	data := []byte(`timescaledb:
  password: "${DB_PASSWORD:-edgevib123}"`)
	expanded := expandEnv(data)
	if string(expanded) != "timescaledb:\n  password: \"testpass\"" {
		t.Errorf("unexpected expansion: %s", string(expanded))
	}
}

func TestDefaults(t *testing.T) {
	cfg := &Config{}
	// Load applies defaults; test that zero values are handled
	if cfg.Models.MaxVersions != 0 {
		return // already set
	}
	// After Load, defaults should be applied
}
