/*
 * main.go — EdgeVib IIO Vibration Daemon entry point
 *
 * Polls TimescaleDB vibration_view every 2s, writes 24-dim feature
 * vectors (96 bytes) to /dev/edgevib-iio-inject for IIO consumption.
 *
 * Usage:
 *   edgevib-iio-d -config /opt/edge-gateway/config/iio-vibration.yaml
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
	Device struct {
		Mode     string `yaml:"mode"`     // "single" | "multi"
		DeviceID string `yaml:"device_id"` // single mode device
	} `yaml:"device"`

	TimescaleDB struct {
		Host     string `yaml:"host"`
		Port     int    `yaml:"port"`
		DBName   string `yaml:"dbname"`
		User     string `yaml:"user"`
		Password string `yaml:"password"`
		SSLMode  string `yaml:"sslmode"`
		PollMS   int    `yaml:"poll_interval_ms"`
	} `yaml:"timescaledb"`

	IIO struct {
		DeviceName string `yaml:"device_name"`
		InjectPath string `yaml:"inject_path"`
	} `yaml:"iio"`

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
	// Defaults
	cfg.Device.Mode = "single"
	cfg.Device.DeviceID = "de01"
	cfg.TimescaleDB.Host = "localhost"
	cfg.TimescaleDB.Port = 5432
	cfg.TimescaleDB.SSLMode = "disable"
	cfg.TimescaleDB.PollMS = 2000
	cfg.IIO.InjectPath = "/dev/edgevib-iio-inject"
	cfg.Health.IntervalS = 30
	cfg.Logging.Level = "info"

	if err := yaml.Unmarshal(data, cfg); err != nil {
		return nil, fmt.Errorf("parse config: %w", err)
	}
	return cfg, nil
}

func main() {
	configPath := flag.String("config", "/opt/edge-gateway/config/iio-vibration.yaml",
		"path to YAML config file")
	flag.Parse()

	// Load config
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

	logger.Info("edgevib-iio-d starting",
		"db_host", cfg.TimescaleDB.Host,
		"poll_ms", cfg.TimescaleDB.PollMS,
		"device_id", cfg.Device.DeviceID,
		"inject_path", cfg.IIO.InjectPath,
	)

	// Build DSN
	dsn := fmt.Sprintf("postgres://%s:%s@%s:%d/%s?sslmode=%s",
		cfg.TimescaleDB.User, cfg.TimescaleDB.Password,
		cfg.TimescaleDB.Host, cfg.TimescaleDB.Port,
		cfg.TimescaleDB.DBName, cfg.TimescaleDB.SSLMode,
	)

	// Create context with signal handling
	ctx, cancel := signal.NotifyContext(context.Background(),
		syscall.SIGINT, syscall.SIGTERM)
	defer cancel()

	// Start health reporter
	healthCh := make(chan HealthStats, 8)
	go runHealthReporter(ctx, cfg, healthCh, logger)

	// Run main injection loop
	stats := runInjectLoop(ctx, cfg, dsn, healthCh, logger)

	logger.Info("edgevib-iio-d shutting down",
		"total_injections", stats.Injections,
		"db_errors", stats.DBErrors,
		"inject_errors", stats.InjectErrors,
	)
}
