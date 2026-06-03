/*
 * main.go — EdgeVib RAM Buffer Flush Daemon entry point
 *
 * Dual-role daemon:
 *   Producer: MQTT subscribe → batch_entry → write(/dev/edgevib-buffer)
 *   Consumer: read(/dev/edgevib-buffer) → batch INSERT → TimescaleDB
 *
 * The block device acts as a process-lifetime-decoupled ring buffer.
 * If flush-d crashes, data remains in the kernel ring buffer.
 *
 * Usage:
 *   edgevib-flush-d -config /opt/edge-gateway/config/edgevib-buffer.yaml
 */

package main

import (
	"context"
	"flag"
	"fmt"
	"log/slog"
	"os"
	"os/signal"
	"syscall"
	"time"

	"gopkg.in/yaml.v3"
)

/* ---- Config ---- */

type Config struct {
	Buffer struct {
		DevicePath   string `yaml:"device_path"`
		SectorSize   int    `yaml:"sector_size"`
		TotalSectors int    `yaml:"total_sectors"`
	} `yaml:"buffer"`

	MQTT struct {
		Broker         string `yaml:"broker"`
		Port           int    `yaml:"port"`
		ClientID       string `yaml:"client_id"`
		SubscribeTopic string `yaml:"subscribe_topic"`
		QoS            byte   `yaml:"qos"`
	} `yaml:"mqtt"`

	TimescaleDB struct {
		Host           string `yaml:"host"`
		Port           int    `yaml:"port"`
		DBName         string `yaml:"dbname"`
		User           string `yaml:"user"`
		Password       string `yaml:"password"`
		SSLMode        string `yaml:"sslmode"`
		BatchSize      int    `yaml:"batch_size"`
		FlushIntervalMS int   `yaml:"flush_interval_ms"`
	} `yaml:"timescaledb"`

	Health struct {
		IntervalS int    `yaml:"interval_s"`
		Topic     string `yaml:"topic"`
	} `yaml:"health"`

	Logging struct {
		Level string `yaml:"level"`
	} `yaml:"logging"`
}

func loadConfig(path string) (*Config, error) {
	data, err := os.ReadFile(path)
	if err != nil {
		return nil, fmt.Errorf("read config: %w", err)
	}
	cfg := &Config{}
	cfg.Buffer.DevicePath = "/dev/edgevib-buffer"
	cfg.Buffer.SectorSize = 4096
	cfg.Buffer.TotalSectors = 256
	cfg.MQTT.Port = 1883
	cfg.MQTT.ClientID = "edgevib-flush-d"
	cfg.MQTT.SubscribeTopic = "EdgeVib/+/+/+/data/#"
	cfg.MQTT.QoS = 1
	cfg.TimescaleDB.SSLMode = "disable"
	cfg.TimescaleDB.BatchSize = 100
	cfg.TimescaleDB.FlushIntervalMS = 5000
	cfg.Health.IntervalS = 30
	cfg.Logging.Level = "info"

	if err := yaml.Unmarshal(data, cfg); err != nil {
		return nil, fmt.Errorf("parse config: %w", err)
	}
	return cfg, nil
}

func main() {
	configPath := flag.String("config", "/opt/edge-gateway/config/edgevib-buffer.yaml",
		"path to YAML config file")
	flag.Parse()

	cfg, err := loadConfig(*configPath)
	if err != nil {
		fmt.Fprintf(os.Stderr, "FATAL: load config: %v\n", err)
		os.Exit(1)
	}

	// Setup structured logging
	var level slog.Level
	switch cfg.Logging.Level {
	case "debug":
		level = slog.LevelDebug
	case "warn":
		level = slog.LevelWarn
	case "error":
		level = slog.LevelError
	default:
		level = slog.LevelInfo
	}
	logger := slog.New(slog.NewJSONHandler(os.Stdout, &slog.HandlerOptions{Level: level}))
	slog.SetDefault(logger)

	logger.Info("edgevib-flush-d starting",
		"buffer_path", cfg.Buffer.DevicePath,
		"mqtt_topic", cfg.MQTT.SubscribeTopic,
		"db_host", cfg.TimescaleDB.Host,
		"batch_size", cfg.TimescaleDB.BatchSize,
	)

	ctx, cancel := signal.NotifyContext(context.Background(),
		syscall.SIGINT, syscall.SIGTERM)
	defer cancel()

	// Run the daemon
	stats := runDaemon(ctx, cfg, logger)

	logger.Info("edgevib-flush-d shutting down",
		"writes", stats.Writes,
		"flushes", stats.Flushes,
		"rows_flushed", stats.RowsFlushed,
	)
}

func runDaemon(ctx context.Context, cfg *Config, logger *slog.Logger) DaemonStats {
	var stats DaemonStats

	dsn := fmt.Sprintf("postgres://%s:%s@%s:%d/%s?sslmode=%s",
		cfg.TimescaleDB.User, cfg.TimescaleDB.Password,
		cfg.TimescaleDB.Host, cfg.TimescaleDB.Port,
		cfg.TimescaleDB.DBName, cfg.TimescaleDB.SSLMode,
	)

	// Open block device for both read and write
	buffer, err := NewBufferIO(cfg.Buffer.DevicePath)
	if err != nil {
		logger.Error("failed to open block device", "err", err)
		return stats
	}
	defer buffer.Close()

	// Connect to TimescaleDB
	db, err := NewDBWriter(ctx, dsn)
	if err != nil {
		logger.Error("failed to connect to TimescaleDB", "err", err)
		return stats
	}
	defer db.Close()

	// Start MQTT producer (MQTT → block device write)
	mqttCh := make(chan *MQTTSensorMessage, 256)
	go runMQTTProducer(ctx, cfg, mqttCh, logger)

	// Start health reporter
	healthCh := make(chan DaemonStats, 8)
	go runHealthReporter(ctx, cfg, healthCh, logger)

	// Main loop: MQTT ingress → write block device + periodic flush
	ticker := time.NewTicker(time.Duration(cfg.TimescaleDB.FlushIntervalMS) * time.Millisecond)
	defer ticker.Stop()

	var pendingRows []*SensorRow

	for {
		select {
		case <-ctx.Done():
			// Final flush
			if len(pendingRows) > 0 {
				flushToDB(db, pendingRows, &stats, logger)
			}
			return stats

		case msg := <-mqttCh:
			// Incoming MQTT message — write to block device
			entry := buildBatchEntry(msg)
			if err := buffer.Write(entry); err != nil {
				stats.WriteErrors++
				logger.Warn("block device write failed", "err", err)
				continue
			}
			stats.Writes++

			// Also read and queue for DB flush
			rows, err := buffer.ReadNew(stats.LastReadSeq)
			if err == nil && len(rows) > 0 {
				pendingRows = append(pendingRows, rows...)
				if len(pendingRows) >= cfg.TimescaleDB.BatchSize {
					flushToDB(db, pendingRows, &stats, logger)
					pendingRows = nil
				}
			}

		case <-ticker.C:
			// Periodic read & flush
			rows, err := buffer.ReadNew(stats.LastReadSeq)
			if err == nil && len(rows) > 0 {
				pendingRows = append(pendingRows, rows...)
			}
			if len(pendingRows) > 0 {
				flushToDB(db, pendingRows, &stats, logger)
				pendingRows = nil
			}
		}

		// Send health stats periodically
		_ = healthSend(healthCh, stats)
	}
}
