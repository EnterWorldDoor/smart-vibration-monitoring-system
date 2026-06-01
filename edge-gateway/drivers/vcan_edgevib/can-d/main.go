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

/* Config matches vcan-edgevib.yaml */
type Config struct {
	MQTT struct {
		BrokerURL    string `yaml:"broker_url"`
		ClientID     string `yaml:"client_id"`
		QoS          int    `yaml:"qos"`
		TopicPattern string `yaml:"topic_pattern"`
	} `yaml:"mqtt"`

	CANInterface      string `yaml:"can_interface"`
	HealthIntervalSec int    `yaml:"health_interval_sec"`
	HealthTopic       string `yaml:"health_topic"`
	LogLevel          string `yaml:"log_level"`
}

func loadConfig(path string) (*Config, error) {
	data, err := os.ReadFile(path)
	if err != nil {
		return nil, fmt.Errorf("read config: %w", err)
	}

	cfg := &Config{}
	/* Defaults */
	cfg.CANInterface = "vcan_edgevib"
	cfg.HealthIntervalSec = 30
	cfg.HealthTopic = "EdgeVib/system/health/vcan_edgevib"
	cfg.LogLevel = "info"

	if err := yaml.Unmarshal(data, cfg); err != nil {
		return nil, fmt.Errorf("parse config: %w", err)
	}
	return cfg, nil
}

func main() {
	configPath := flag.String("config", "/opt/edge-gateway/config/vcan-edgevib.yaml",
		"Path to YAML config file")
	flag.Parse()

	cfg, err := loadConfig(*configPath)
	if err != nil {
		fmt.Fprintf(os.Stderr, "Config error: %v\n", err)
		os.Exit(1)
	}

	/* Structured logging */
	var level slog.Level
	switch cfg.LogLevel {
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

	logger.Info("edgevib-can-d starting",
		"can_if", cfg.CANInterface,
		"mqtt_broker", cfg.MQTT.BrokerURL,
		"mqtt_topic", cfg.MQTT.TopicPattern,
	)

	/* Open AF_CAN socket */
	canSock, err := NewSocketCAN(cfg.CANInterface)
	if err != nil {
		logger.Error("Failed to open SocketCAN", "err", err)
		os.Exit(1)
	}
	defer canSock.Close()
	logger.Info("SocketCAN opened", "if", canSock.IfName())

	/* Connect MQTT */
	mc, err := NewMQTTClient(cfg.MQTT.BrokerURL, cfg.MQTT.ClientID,
		cfg.MQTT.TopicPattern, canSock, logger)
	if err != nil {
		logger.Error("Failed to connect MQTT", "err", err)
		os.Exit(1)
	}
	defer mc.Close()

	/* Start health loop in background */
	go RunHealthLoop(cfg.MQTT.BrokerURL, cfg.MQTT.ClientID,
		cfg.HealthTopic, mc, logger)

	logger.Info("edgevib-can-d running, waiting for CAN frames...")

	/* Wait for shutdown signal */
	ctx, cancel := signal.NotifyContext(context.Background(),
		syscall.SIGINT, syscall.SIGTERM)
	defer cancel()

	<-ctx.Done()
	logger.Info("Shutting down", "frames_rx", mc.stats.framesRx, "frames_tx", mc.stats.framesTx)
}
