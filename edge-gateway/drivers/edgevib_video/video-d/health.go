package main

import (
	"context"
	"encoding/json"
	"fmt"
	"log/slog"
	"time"

	mqtt "github.com/eclipse/paho.mqtt.golang"
)

// HealthReport is the JSON payload published to MQTT health topic
type HealthReport struct {
	Service      string `json:"service"`
	Status       string `json:"status"`
	UptimeS      int64  `json:"uptime_s"`
	Injections   int64  `json:"injections"`
	DBErrors     int64  `json:"db_errors"`
	InjectErrors int64  `json:"inject_errors"`
	SkipNoFile   int64  `json:"skip_no_file"`
	SkipDup      int64  `json:"skip_duplicate"`
	LastPollMS   int64  `json:"last_poll_ms"`
	NumDevices   int    `json:"num_devices"`
}

func runHealthReporter(ctx context.Context, cfg *Config,
	statsCh <-chan InjectionStats, logger *slog.Logger) {

	// MQTT connect
	brokerURL := fmt.Sprintf("tcp://%s:%d", cfg.Health.Broker, cfg.Health.Port)
	opts := mqtt.NewClientOptions().
		AddBroker(brokerURL).
		SetClientID("edgevib-video-d-health").
		SetConnectRetry(true).
		SetConnectRetryInterval(5 * time.Second).
		SetAutoReconnect(true).
		SetKeepAlive(30 * time.Second).
		SetPingTimeout(10 * time.Second)

	client := mqtt.NewClient(opts)
	token := client.Connect()
	if !token.WaitTimeout(10 * time.Second) {
		logger.Error("MQTT health connect timeout", "broker", brokerURL)
		return
	}
	if err := token.Error(); err != nil {
		logger.Error("MQTT health connect failed", "err", err)
		return
	}
	defer client.Disconnect(250)

	logger.Info("MQTT health connected", "topic", cfg.Health.Topic)

	var latestStats InjectionStats
	startTime := time.Now()
	ticker := time.NewTicker(time.Duration(cfg.Health.IntervalS) * time.Second)
	defer ticker.Stop()

	for {
		select {
		case <-ctx.Done():
			// Publish final "stopped" status
			publishHealth(client, cfg.Health.Topic, latestStats,
				startTime, len(cfg.Devices), "stopped", logger)
			return

		case s := <-statsCh:
			latestStats = s

		case <-ticker.C:
			publishHealth(client, cfg.Health.Topic, latestStats,
				startTime, len(cfg.Devices), "running", logger)
		}
	}
}

func publishHealth(client mqtt.Client, topic string,
	stats InjectionStats, startTime time.Time,
	numDevices int, status string, logger *slog.Logger) {

	report := HealthReport{
		Service:      "edgevib-video-d",
		Status:       status,
		UptimeS:      int64(time.Since(startTime).Seconds()),
		Injections:   stats.Injections,
		DBErrors:     stats.DBErrors,
		InjectErrors: stats.InjectErrors,
		SkipNoFile:   stats.SkipNoFile,
		SkipDup:      stats.SkipDuplicate,
		LastPollMS:   stats.LastPollMS,
		NumDevices:   numDevices,
	}

	payload, err := json.Marshal(report)
	if err != nil {
		logger.Error("health marshal failed", "err", err)
		return
	}

	token := client.Publish(topic, 1, false, payload)
	if token.WaitTimeout(5*time.Second) && token.Error() != nil {
		logger.Error("health publish failed", "err", token.Error())
	}
}
