package config

import (
	"fmt"
	"os"
	"strings"

	"gopkg.in/yaml.v3"
)

// Config is the top-level edge-router configuration.
type Config struct {
	MQTT     MQTTConfig     `yaml:"mqtt"`
	Database DatabaseConfig `yaml:"database"`
	Dedup    DedupConfig    `yaml:"dedup"`
	Router   RouterConfig   `yaml:"router"`
}

// MQTTConfig holds MQTT broker connection and subscription settings.
type MQTTConfig struct {
	BrokerURL string   `yaml:"broker_url"`
	ClientID  string   `yaml:"client_id"`
	Topics    []string `yaml:"topics"`
	QoS       byte     `yaml:"qos"`
}

// DatabaseConfig holds TimescaleDB connection settings.
type DatabaseConfig struct {
	Host         string `yaml:"host"`
	Port         int    `yaml:"port"`
	User         string `yaml:"user"`
	Password     string `yaml:"password"`
	DBName       string `yaml:"dbname"`
	SSLMode      string `yaml:"sslmode"`
	PoolMaxConns int    `yaml:"pool_max_conns"`
}

// DSN returns a PostgreSQL connection string.
func (d *DatabaseConfig) DSN() string {
	return fmt.Sprintf("postgres://%s:%s@%s:%d/%s?sslmode=%s&pool_max_conns=%d",
		d.User, d.Password, d.Host, d.Port, d.DBName, d.SSLMode, d.PoolMaxConns)
}

// DedupConfig holds deduplication settings.
type DedupConfig struct {
	WindowSeconds int `yaml:"window_seconds"`
}

// RouterConfig holds routing settings.
type RouterConfig struct {
	Sites []string `yaml:"sites"`
}

// Load reads and parses a YAML config file with ${VAR:-default} env var expansion.
func Load(path string) (*Config, error) {
	data, err := os.ReadFile(path)
	if err != nil {
		return nil, fmt.Errorf("read config file: %w", err)
	}

	content := os.Expand(string(data), func(key string) string {
		if idx := strings.Index(key, ":-"); idx >= 0 {
			envVar := key[:idx]
			defaultVal := key[idx+2:]
			if val := os.Getenv(envVar); val != "" {
				return val
			}
			return defaultVal
		}
		return os.Getenv(key)
	})

	var cfg Config
	if err := yaml.Unmarshal([]byte(content), &cfg); err != nil {
		return nil, fmt.Errorf("parse config: %w", err)
	}

	// Apply defaults
	if cfg.MQTT.QoS == 0 {
		cfg.MQTT.QoS = 1
	}
	if cfg.Database.PoolMaxConns == 0 {
		cfg.Database.PoolMaxConns = 5
	}
	if cfg.Database.Port == 0 {
		cfg.Database.Port = 5432
	}
	if cfg.Database.SSLMode == "" {
		cfg.Database.SSLMode = "disable"
	}
	if cfg.Dedup.WindowSeconds == 0 {
		cfg.Dedup.WindowSeconds = 30
	}

	return &cfg, nil
}
