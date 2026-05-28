package config

import (
	"fmt"
	"os"
	"strings"
	"time"

	"gopkg.in/yaml.v3"
)

type Config struct {
	Server      ServerConfig      `yaml:"server"`
	Firmware    FirmwareConfig    `yaml:"firmware"`
	TimescaleDB TimescaleDBConfig `yaml:"timescaledb"`
	MQTT        MQTTConfig        `yaml:"mqtt"`
	Log         LogConfig         `yaml:"log"`
}

type ServerConfig struct {
	Host         string        `yaml:"host"`
	Port         int           `yaml:"port"`
	ReadTimeout  time.Duration `yaml:"read_timeout"`
	WriteTimeout time.Duration `yaml:"write_timeout"`
}

type FirmwareConfig struct {
	StoragePath string `yaml:"storage_path"`
	SiteID      string `yaml:"site_id"`
}

type TimescaleDBConfig struct {
	Host         string `yaml:"host"`
	Port         int    `yaml:"port"`
	User         string `yaml:"user"`
	Password     string `yaml:"password"`
	Database     string `yaml:"database"`
	PoolMaxConns int    `yaml:"pool_max_conns"`
}

type MQTTConfig struct {
	Broker   string   `yaml:"broker"`
	ClientID string   `yaml:"client_id"`
	Topics   []string `yaml:"topics"`
}

type LogConfig struct {
	Level  string `yaml:"level"`
	Format string `yaml:"format"`
}

func (c *TimescaleDBConfig) DSN() string {
	return fmt.Sprintf("postgres://%s:%s@%s:%d/%s?sslmode=disable",
		c.User, c.Password, c.Host, c.Port, c.Database)
}

func Load(path string) (*Config, error) {
	data, err := os.ReadFile(path)
	if err != nil {
		return nil, fmt.Errorf("read config: %w", err)
	}

	expanded := os.Expand(string(data), func(key string) string {
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
	if err := yaml.Unmarshal([]byte(expanded), &cfg); err != nil {
		return nil, fmt.Errorf("parse config: %w", err)
	}

	if cfg.TimescaleDB.PoolMaxConns <= 0 {
		cfg.TimescaleDB.PoolMaxConns = 5
	}

	return &cfg, nil
}
