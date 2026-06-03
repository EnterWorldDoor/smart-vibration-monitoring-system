/*
 * main.go — EdgeVib GPIO Daemon entry point
 *
 * Subscribes to MQTT alert/health topics, controls GPIO output lines
 * via gpioset (subprocess exec), and monitors IRQ input lines via gpiomon.
 *
 * Usage:
 *   edgevib-gpio-d -config /opt/edge-gateway/config/edgevib-gpio.yaml
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
	GPIO struct {
		ChipPath              string `yaml:"chip_path"`
		Lines                 struct {
			SystemOK     int `yaml:"system_ok"`
			GatewayAlert int `yaml:"gateway_alert"`
			Heartbeat    int `yaml:"heartbeat"`
			Reserved     int `yaml:"reserved"`
			EstopMaster  int `yaml:"estop_master"`
			PSUFailIn    int `yaml:"psu_fail_in"`
		} `yaml:"lines"`
		HeartbeatIntervalMS int `yaml:"heartbeat_interval_ms"`
	} `yaml:"gpio"`

	MQTT struct {
		Broker             string   `yaml:"broker"`
		Port               int      `yaml:"port"`
		ClientID           string   `yaml:"client_id"`
		SubscribeTopics    []string `yaml:"subscribe_topics"`
		PublishEstopTopic  string   `yaml:"publish_estop_topic"`
		PublishPSUTopic    string   `yaml:"publish_psu_topic"`
	} `yaml:"mqtt"`

	Health struct {
		IntervalS        int    `yaml:"interval_s"`
		Topic            string `yaml:"topic"`
		ServiceTimeoutS  int    `yaml:"service_timeout_s"`
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
	/* Defaults */
	cfg.GPIO.ChipPath = "/dev/gpiochip2"
	cfg.GPIO.Lines.SystemOK = 0
	cfg.GPIO.Lines.GatewayAlert = 1
	cfg.GPIO.Lines.Heartbeat = 2
	cfg.GPIO.Lines.Reserved = 3
	cfg.GPIO.Lines.EstopMaster = 4
	cfg.GPIO.Lines.PSUFailIn = 5
	cfg.GPIO.HeartbeatIntervalMS = 500
	cfg.MQTT.Broker = "localhost"
	cfg.MQTT.Port = 1883
	cfg.MQTT.ClientID = "edgevib-gpio-d"
	cfg.Health.IntervalS = 30
	cfg.Logging.Level = "info"

	if err := yaml.Unmarshal(data, cfg); err != nil {
		return nil, fmt.Errorf("parse config: %w", err)
	}
	return cfg, nil
}

func main() {
	configPath := flag.String("config", "/opt/edge-gateway/config/edgevib-gpio.yaml",
		"path to YAML config file")
	flag.Parse()

	cfg, err := loadConfig(*configPath)
	if err != nil {
		fmt.Fprintf(os.Stderr, "FATAL: load config: %v\n", err)
		os.Exit(1)
	}

	/* Setup structured logging */
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

	logger.Info("edgevib-gpio-d starting",
		"chip", cfg.GPIO.ChipPath,
		"mqtt_broker", cfg.MQTT.Broker,
		"health_topic", cfg.Health.Topic,
	)

	ctx, cancel := signal.NotifyContext(context.Background(),
		syscall.SIGINT, syscall.SIGTERM)
	defer cancel()

	/* ---- Start sub-components ---- */

	// MQTT subscriber + health evaluator
	healthCh := make(chan GPIOAction, 8)
	mqttBroker := fmt.Sprintf("tcp://%s:%d", cfg.MQTT.Broker, cfg.MQTT.Port)
	go runMQTTSubscriber(ctx, mqttBroker, cfg, healthCh, logger)

	// Heartbeat goroutine
	go runHeartbeat(ctx, cfg.GPIO.ChipPath, cfg.GPIO.Lines.Heartbeat,
		cfg.GPIO.HeartbeatIntervalMS, logger)

	// IRQ monitor (estop + psu_fail)
	irqLines := []int{cfg.GPIO.Lines.EstopMaster, cfg.GPIO.Lines.PSUFailIn}
	go runIRQMonitor(ctx, cfg.GPIO.ChipPath, irqLines, cfg, logger)

	// Health reporter
	statsCh := make(chan DaemonStats, 8)
	go runHealthReporter(ctx, cfg, statsCh, logger)

	/* ---- Main loop: consume GPIO actions from health evaluator ---- */
	var stats DaemonStats
	stats.StartTime = time.Now()
	chip := cfg.GPIO.ChipPath

	for {
		select {
		case <-ctx.Done():
			logger.Info("edgevib-gpio-d shutting down",
				"actions", stats.Actions,
				"errors", stats.Errors,
			)
			return

		case action := <-healthCh:
			stats.Actions++
			err := applyAction(chip, action)
			if err != nil {
				stats.Errors++
				logger.Warn("gpio action failed", "err", err, "action", action)
			}
		}

		// Non-blocking stats push
		select {
		case statsCh <- stats:
		default:
		}
	}
}
