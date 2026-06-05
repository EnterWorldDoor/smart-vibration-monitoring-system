/*
 * health.go — MQTT health heartbeat reporter
 *
 * Publishes periodic health telemetry to MQTT. Receives MQTTStats
 * from the subscriber goroutine via a channel.
 *
 * Pattern: D5 hwmon-d/health.go (separate MQTT connection for health)
 */

package main

import (
	"context"
	"encoding/json"
	"fmt"
	"log/slog"
	"time"

	mqtt "github.com/eclipse/paho.mqtt.golang"
)

type healthReport struct {
	Service       string `json:"service"`
	Status        string `json:"status"`
	MessagesRx    int64  `json:"messages_rx"`
	EmergencyRx   int64  `json:"emergency_rx"`
	HealthRx      int64  `json:"health_rx"`
	ParseErrors   int64  `json:"parse_errors"`
	InjectErrors  int64  `json:"inject_errors"`
	FailSafeEvents int64 `json:"fail_safe_events"`
	UptimeS       int64  `json:"uptime_s"`
}

func runHealthReporter(ctx context.Context, cfg *Config,
	ch <-chan MQTTStats, logger *slog.Logger) {

	var latest MQTTStats
	startTime := time.Now()

	brokerURL := fmt.Sprintf("tcp://%s:%d", cfg.Health.Broker, cfg.Health.Port)
	opts := mqtt.NewClientOptions().
		AddBroker(brokerURL).
		SetClientID("edgevib-input-stop-d-health").
		SetCleanSession(true).
		SetAutoReconnect(true).
		SetConnectRetry(true).
		SetConnectRetryInterval(5 * time.Second).
		SetMaxReconnectInterval(30 * time.Second).
		SetKeepAlive(30 * time.Second).
		SetPingTimeout(10 * time.Second)

	client := mqtt.NewClient(opts)
	token := client.Connect()
	if !token.WaitTimeout(10*time.Second) || token.Error() != nil {
		logger.Warn("health MQTT connect failed, health reporting disabled",
			"err", func() error {
				if token.Error() != nil {
					return token.Error()
				}
				return fmt.Errorf("timeout")
			}(),
		)
		return
	}
	defer client.Disconnect(250)
	logger.Info("health MQTT connected",
		"broker", brokerURL,
		"topic", cfg.Health.Topic,
	)

	// Goroutine: collect latest stats (non-blocking)
	go func() {
		for {
			select {
			case <-ctx.Done():
				return
			case s := <-ch:
				latest = s
			}
		}
	}()

	ticker := time.NewTicker(time.Duration(cfg.Health.IntervalS) * time.Second)
	defer ticker.Stop()

	for {
		select {
		case <-ctx.Done():
			publishHealth(client, cfg.Health.Topic, latest, startTime, logger)
			return
		case <-ticker.C:
			publishHealth(client, cfg.Health.Topic, latest, startTime, logger)
		}
	}
}

func publishHealth(client mqtt.Client, topic string,
	stats MQTTStats, startTime time.Time, logger *slog.Logger) {

	report := healthReport{
		Service:        "edgevib-input-stop-d",
		Status:         "ok",
		MessagesRx:     stats.MessagesRx,
		EmergencyRx:    stats.EmergencyRx,
		HealthRx:       stats.HealthRx,
		ParseErrors:    stats.ParseErrors,
		InjectErrors:   stats.InjectErrors,
		FailSafeEvents: stats.FailSafeEvents,
		UptimeS:        int64(time.Since(startTime).Seconds()),
	}

	payload, err := json.Marshal(report)
	if err != nil {
		logger.Warn("health marshal failed", "err", err)
		return
	}

	token := client.Publish(topic, 1, false, payload)
	if !token.WaitTimeout(5*time.Second) || token.Error() != nil {
		logger.Warn("health publish failed",
			"err", func() error {
				if token.Error() != nil {
					return token.Error()
				}
				return fmt.Errorf("timeout")
			}(),
		)
	}
}
