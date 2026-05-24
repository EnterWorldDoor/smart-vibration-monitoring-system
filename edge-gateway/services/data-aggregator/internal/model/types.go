package model

import (
	"encoding/json"
	"time"
)

// SensorMessage is the parsed form of an MQTT sensor message.
type SensorMessage struct {
	SiteID      string
	DeviceType  string
	DeviceID    string
	DataType    string
	Topic       string          // full MQTT topic (used for dedup key)
	TimestampMS int64           // extracted from JSON "timestamp_ms" field, -1 if absent
	Payload     json.RawMessage // raw JSON, passed through to JSONB
	IngestedAt  time.Time       // Orange Pi local time
}

// DeviceEntry maps an ESP32 uint8 dev_id to schema metadata.
type DeviceEntry struct {
	DevID       uint8  `yaml:"dev_id"`
	SiteID      string `yaml:"site_id"`
	DeviceType  string `yaml:"device_type"`
	DeviceID    string `yaml:"device_id"`
	Description string `yaml:"description,omitempty"`
}

// MQTTConfig holds MQTT connection parameters.
type MQTTConfig struct {
	BrokerURL string   `yaml:"broker_url"`
	ClientID  string   `yaml:"client_id"`
	Topics    []string `yaml:"topics"`
	QoS       byte     `yaml:"qos"`
}

// DatabaseConfig holds TimescaleDB connection parameters.
type DatabaseConfig struct {
	Host     string `yaml:"host"`
	Port     int    `yaml:"port"`
	User     string `yaml:"user"`
	Password string `yaml:"password"`
	DBName   string `yaml:"dbname"`
	SSLMode  string `yaml:"sslmode"`
}

// DedupConfig holds deduplication parameters.
type DedupConfig struct {
	WindowSeconds int      `yaml:"window_seconds"`
	KeyFields     []string `yaml:"key_fields"`
}

// Config is the top-level configuration for the data-aggregator.
type Config struct {
	MQTT                MQTTConfig    `yaml:"mqtt"`
	Database            DatabaseConfig `yaml:"database"`
	Dedup               DedupConfig   `yaml:"dedup"`
	SiteID              string        `yaml:"site_id"`
	DevicesRegistryPath string        `yaml:"devices_registry_path"`
}

// HealthReport holds aggregator self-reporting metrics.
type HealthReport struct {
	Service       string `json:"service"`
	Version       string `json:"version"`
	SiteID        string `json:"site_id"`
	UptimeSeconds int64  `json:"uptime_seconds"`
	MQTTConnected bool   `json:"mqtt_connected"`
	MsgReceived   int64  `json:"msg_received"`
	MsgDeduped    int64  `json:"msg_deduped"`
	ParseErrors   int64  `json:"parse_errors"`
	BatchesFlushed int64 `json:"batches_flushed"`
	RowsWritten   int64  `json:"rows_written"`
	DBErrors      int64  `json:"db_errors"`
	DBConnected   bool   `json:"db_connected"`
	LastFlushAt   string `json:"last_flush_at,omitempty"`
}
