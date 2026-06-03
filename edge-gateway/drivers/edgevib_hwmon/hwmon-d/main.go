package main

import (
	"context"
	"flag"
	"fmt"
	"log/slog"
	"os"
	"os/signal"
	"syscall"

	"gopkg.in/yaml.v3"
)

// Config mirrors edgevib-hwmon.yaml structure
type Config struct {
	Hwmon       HwmonConfig       `yaml:"hwmon"`
	TimescaleDB TimescaleDBConfig `yaml:"timescaledb"`
	Health      HealthConfig      `yaml:"health"`
	Logging     LoggingConfig     `yaml:"logging"`
}

type HwmonConfig struct {
	InjectPath   string   `yaml:"inject_path"`
	NumMotors    int      `yaml:"num_motors"`
	MotorDevices []string `yaml:"motor_devices"`
}

type TimescaleDBConfig struct {
	Host     string `yaml:"host"`
	Port     int    `yaml:"port"`
	DBName   string `yaml:"dbname"`
	User     string `yaml:"user"`
	Password string `yaml:"password"`
	SSLMode  string `yaml:"sslmode"`
	PollMS   int    `yaml:"poll_interval_ms"`
}

type HealthConfig struct {
	IntervalS int    `yaml:"interval_s"`
	Topic     string `yaml:"topic"`
	Broker    string `yaml:"broker"`
	Port      int    `yaml:"port"`
}

type LoggingConfig struct {
	Level string `yaml:"level"`
}

func loadConfig(path string) (*Config, error) {
	data, err := os.ReadFile(path)
	if err != nil {
		return nil, fmt.Errorf("read config: %w", err)
	}

	// Set defaults before unmarshalling (config file overrides)
	cfg := &Config{}
	cfg.Hwmon.InjectPath = "/dev/edgevib-hwmon-inject"
	cfg.Hwmon.NumMotors = 4
	cfg.Hwmon.MotorDevices = []string{"motor-01", "motor-02", "motor-03", "motor-04"}
	cfg.TimescaleDB.Host = "localhost"
	cfg.TimescaleDB.Port = 5432
	cfg.TimescaleDB.SSLMode = "disable"
	cfg.TimescaleDB.PollMS = 2000
	cfg.Health.IntervalS = 30
	cfg.Health.Broker = "localhost"
	cfg.Health.Port = 1883
	cfg.Logging.Level = "info"

	if err := yaml.Unmarshal(data, cfg); err != nil {
		return nil, fmt.Errorf("parse config: %w", err)
	}
	return cfg, nil
}

func main() {
	configPath := flag.String("config",
		"/opt/edge-gateway/config/edgevib-hwmon.yaml",
		"path to YAML config file")
	flag.Parse()

	cfg, err := loadConfig(*configPath)
	if err != nil {
		fmt.Fprintf(os.Stderr, "FATAL: load config: %v\n", err)
		os.Exit(1)
	}

	// Setup structured JSON logging
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
	logger := slog.New(slog.NewJSONHandler(os.Stdout,
		&slog.HandlerOptions{Level: level}))
	slog.SetDefault(logger)

	logger.Info("edgevib-hwmon-d starting",
		"inject_path", cfg.Hwmon.InjectPath,
		"num_motors", cfg.Hwmon.NumMotors,
		"motor_devices", cfg.Hwmon.MotorDevices,
		"poll_ms", cfg.TimescaleDB.PollMS,
	)

	// Build DSN
	dsn := fmt.Sprintf("postgres://%s:%s@%s:%d/%s?sslmode=%s",
		cfg.TimescaleDB.User, cfg.TimescaleDB.Password,
		cfg.TimescaleDB.Host, cfg.TimescaleDB.Port,
		cfg.TimescaleDB.DBName, cfg.TimescaleDB.SSLMode,
	)

	// Signal handling — graceful shutdown on SIGINT/SIGTERM
	ctx, cancel := signal.NotifyContext(context.Background(),
		syscall.SIGINT, syscall.SIGTERM)
	defer cancel()

	// Health reporting goroutine
	statsCh := make(chan InjectionStats, 8)
	go runHealthReporter(ctx, cfg, statsCh, logger)

	// Main injection loop
	stats := runInjectLoop(ctx, cfg, dsn, statsCh, logger)

	logger.Info("edgevib-hwmon-d stopped",
		"total_injections", stats.Injections,
		"db_errors", stats.DBErrors,
		"inject_errors", stats.InjectErrors,
	)
}
