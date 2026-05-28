package config

import (
	"fmt"
	"os"
	"strings"

	"gopkg.in/yaml.v3"
)

type ServerConfig struct {
	Host         string `yaml:"host"`
	Port         int    `yaml:"port"`
	ReadTimeout  string `yaml:"read_timeout"`
	WriteTimeout string `yaml:"write_timeout"`
}

type ModelsConfig struct {
	StoragePath string `yaml:"storage_path"`
	SiteID      string `yaml:"site_id"`
	MaxVersions int    `yaml:"max_versions"`
}

type TimescaleDBConfig struct {
	Host         string `yaml:"host"`
	Port         int    `yaml:"port"`
	User         string `yaml:"user"`
	Password     string `yaml:"password"`
	Database     string `yaml:"database"`
	PoolMaxConns int    `yaml:"pool_max_conns"`
}

func (c *TimescaleDBConfig) DSN() string {
	return fmt.Sprintf("postgres://%s:%s@%s:%d/%s?sslmode=disable",
		c.User, c.Password, c.Host, c.Port, c.Database)
}

type MQTTConfig struct {
	Broker           string   `yaml:"broker"`
	ClientID         string   `yaml:"client_id"`
	PublishTopic     string   `yaml:"publish_topic"`
	SubscribeTopics  []string `yaml:"subscribe_topics"`
}

type LogConfig struct {
	Level  string `yaml:"level"`
	Format string `yaml:"format"`
}

type Config struct {
	Server      ServerConfig      `yaml:"server"`
	Models      ModelsConfig      `yaml:"models"`
	TimescaleDB TimescaleDBConfig `yaml:"timescaledb"`
	MQTT        MQTTConfig        `yaml:"mqtt"`
	Log         LogConfig         `yaml:"log"`
}

func expandEnv(data []byte) []byte {
	result := string(data)
	for _, line := range strings.Split(result, "\n") {
		if strings.Contains(line, "${") {
			start := strings.Index(line, "${")
			end := strings.Index(line[start:], "}") + start
			if start >= 0 && end > start {
				key := line[start+2 : end]
				val := ""
				defaultVal := ""
				if idx := strings.Index(key, ":-"); idx > 0 {
					defaultVal = key[idx+2:]
					key = key[:idx]
				}
				val = os.Getenv(key)
				if val == "" {
					val = defaultVal
				}
				result = strings.Replace(result, "${"+key, val, 1)
				if defaultVal != "" {
					result = strings.Replace(result, ":-"+defaultVal+"}", "", 1)
				} else {
					result = strings.Replace(result, "}", "", 1)
				}
				return expandEnv([]byte(result))
			}
		}
	}
	return []byte(result)
}

func Load(path string) (*Config, error) {
	data, err := os.ReadFile(path)
	if err != nil {
		return nil, fmt.Errorf("read config: %w", err)
	}

	expanded := expandEnv(data)

	var cfg Config
	if err := yaml.Unmarshal(expanded, &cfg); err != nil {
		return nil, fmt.Errorf("parse config: %w", err)
	}

	if cfg.TimescaleDB.PoolMaxConns == 0 {
		cfg.TimescaleDB.PoolMaxConns = 5
	}
	if cfg.Models.MaxVersions == 0 {
		cfg.Models.MaxVersions = 3
	}

	return &cfg, nil
}
