/*
 * health.go — 30s MQTT health telemetry for edgevib-iio-d
 *
 * Follows the same pattern as D1 can-d/health.go:
 *   - Separate MQTT connection (client_id + "-health")
 *   - 30s ticker
 *   - JSON health report
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

/* ---- Health Report ---- */

type HealthStats struct {
	Injections   int64
	DBErrors     int64
	InjectErrors int64
	LastPollMS   int64
}

type healthReport struct {
	Service       string `json:"service"`
	Status        string `json:"status"`
	Injections    int64  `json:"injections"`
	DBErrors      int64  `json:"db_errors"`
	InjectErrors  int64  `json:"inject_errors"`
	LastPollMS    int64  `json:"last_poll_ms"`
	UptimeS       int64  `json:"uptime_s"`
}

func healthSend(ch chan<- HealthStats, stats InjectionStats) error {
	select {
	case ch <- HealthStats{
		Injections:   stats.Injections,
		DBErrors:     stats.DBErrors,
		InjectErrors: stats.InjectErrors,
		LastPollMS:   stats.LastPollMS,
	}:
		return nil
	default:
		return fmt.Errorf("health channel full")
	}
}

func runHealthReporter(ctx context.Context, cfg *Config,
	ch <-chan HealthStats, logger *slog.Logger) {

	var latest HealthStats
	startTime := time.Now()

	// Connect to MQTT
	brokerURL := fmt.Sprintf("tcp://%s:%d", "localhost", 1883)
	opts := mqtt.NewClientOptions().
		AddBroker(brokerURL).
		SetClientID("edgevib-iio-d-health").
		SetCleanSession(true).
		SetAutoReconnect(true).
		SetConnectRetry(true).
		SetConnectRetryInterval(5 * time.Second).
		SetMaxReconnectInterval(30 * time.Second).
		SetKeepAlive(30 * time.Second).
		SetPingTimeout(10 * time.Second)

	client := mqtt.NewClient(opts)
	token := client.Connect()
	if !token.WaitTimeout(10 * time.Second) {
		logger.Warn("health MQTT connect timeout")
		return
	}
	if token.Error() != nil {
		logger.Warn("health MQTT connect failed", "err", token.Error())
		return
	}
	defer client.Disconnect(250)
	logger.Info("health MQTT connected")

	// Collect stats from channel
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

	// Publish every 30s
	ticker := time.NewTicker(time.Duration(cfg.Health.IntervalS) * time.Second)
	defer ticker.Stop()

	for {
		select {
		case <-ctx.Done():
			// Final report
			publishHealth(client, cfg.Health.Topic, latest, startTime, logger)
			return

		case <-ticker.C:
			publishHealth(client, cfg.Health.Topic, latest, startTime, logger)
		}
	}
}

func publishHealth(client mqtt.Client, topic string,
	stats HealthStats, startTime time.Time, logger *slog.Logger) {

	report := healthReport{
		Service:      "edgevib-iio-d",
		Status:       "ok",
		Injections:   stats.Injections,
		DBErrors:     stats.DBErrors,
		InjectErrors: stats.InjectErrors,
		LastPollMS:   stats.LastPollMS,
		UptimeS:      int64(time.Since(startTime).Seconds()),
	}

	payload, err := json.Marshal(report)
	if err != nil {
		logger.Warn("health marshal failed", "err", err)
		return
	}

	token := client.Publish(topic, 1, false, payload)
	if token.WaitTimeout(5*time.Second) && token.Error() != nil {
		logger.Warn("health publish failed", "err", token.Error())
		return
	}

	logger.Debug("health published", "topic", topic, "injections", stats.Injections)
}
