/*
 * main.go — EdgeVib Input Stop Daemon entry point
 *
 * Subscribes to MQTT for F407 emergency stop state (via rs232-gateway)
 * and translates e_stop_state 0/1/2 → KEY_STOP/KEY_WAKEUP evdev events
 * via /dev/edgevib-input-inject.
 *
 * Usage:
 *   edgevib-input-stop-d -config /opt/edge-gateway/config/edgevib-input.yaml
 *
 * Pattern: D5 hwmon-d/main.go (config + signal + goroutine dispatch)
 */

package main

import (
	"context"
	"flag"
	"fmt"
	"log/slog"
	"os"
	"os/signal"
	"sync/atomic"
	"syscall"
	"time"

	"gopkg.in/yaml.v3"
)

/* ---- Config ---- */

type Config struct {
	Input struct {
		InjectPath string `yaml:"inject_path"`
	} `yaml:"input"`

	MQTT struct {
		Broker           string   `yaml:"broker"`
		Port             int      `yaml:"port"`
		ClientID         string   `yaml:"client_id"`
		SubscribeTopics  []string `yaml:"subscribe_topics"`
		FailSafeTimeoutS int      `yaml:"fail_safe_timeout_s"`
	} `yaml:"mqtt"`

	Health struct {
		IntervalS int    `yaml:"interval_s"`
		Topic     string `yaml:"topic"`
		Broker    string `yaml:"broker"`
		Port      int    `yaml:"port"`
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

	// Set defaults BEFORE unmarshalling (config file overrides)
	cfg := &Config{}
	cfg.Input.InjectPath = "/dev/edgevib-input-inject"
	cfg.MQTT.Broker = "localhost"
	cfg.MQTT.Port = 1883
	cfg.MQTT.ClientID = "edgevib-input-stop-d"
	cfg.MQTT.FailSafeTimeoutS = 10
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
		"/opt/edge-gateway/config/edgevib-input.yaml",
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

	logger.Info("edgevib-input-stop-d starting",
		"inject_path", cfg.Input.InjectPath,
		"mqtt_broker", cfg.MQTT.Broker,
		"mqtt_topics", cfg.MQTT.SubscribeTopics,
		"fail_safe_timeout_s", cfg.MQTT.FailSafeTimeoutS,
		"health_topic", cfg.Health.Topic,
	)

	// Signal handling — graceful shutdown on SIGINT/SIGTERM
	ctx, cancel := signal.NotifyContext(context.Background(),
		syscall.SIGINT, syscall.SIGTERM)
	defer cancel()

	// Open injection cdev
	inj, err := OpenInjector(cfg.Input.InjectPath)
	if err != nil {
		logger.Error("failed to open inject device",
			"path", cfg.Input.InjectPath,
			"err", err,
		)
		os.Exit(1)
	}
	defer inj.Close()

	// Start MQTT subscriber (main data source)
	mqttBroker := fmt.Sprintf("tcp://%s:%d", cfg.MQTT.Broker, cfg.MQTT.Port)
	statsCh := make(chan MQTTStats, 8)

	// Shared atomic for fail-safe: last MQTT message receive time (Unix nano)
	var lastRxAtomic atomic.Int64

	go runMQTTSubscriber(ctx, mqttBroker, cfg, inj, statsCh, &lastRxAtomic, logger)

	// Start fail-safe goroutine (MQTT timeout → KEY_STOP=1)
	go runFailSafe(ctx, cfg, inj, &lastRxAtomic, logger)

	// Start health reporter
	go runHealthReporter(ctx, cfg, statsCh, logger)

	// Block until shutdown
	<-ctx.Done()

	// Graceful exit log
	logger.Info("edgevib-input-stop-d shutting down")

	// Allow cleanup goroutines a moment to publish final health
	shutdownCtx, shutdownCancel := context.WithTimeout(context.Background(), 3*time.Second)
	defer shutdownCancel()
	<-shutdownCtx.Done()
}
