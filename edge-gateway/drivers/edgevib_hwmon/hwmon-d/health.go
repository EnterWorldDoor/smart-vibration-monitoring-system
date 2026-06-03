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
	Service      string `json:"service"`
	Status       string `json:"status"`
	Injections   int64  `json:"injections"`
	DBErrors     int64  `json:"db_errors"`
	InjectErrors int64  `json:"inject_errors"`
	LastPollMS   int64  `json:"last_poll_ms"`
	UptimeS      int64  `json:"uptime_s"`
}

func runHealthReporter(ctx context.Context, cfg *Config,
	ch <-chan InjectionStats, logger *slog.Logger) {

	var latest InjectionStats
	startTime := time.Now()

	brokerURL := fmt.Sprintf("tcp://%s:%d", cfg.Health.Broker, cfg.Health.Port)
	opts := mqtt.NewClientOptions().
		AddBroker(brokerURL).
		SetClientID("edgevib-hwmon-d-health").
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
			"err", token.Error())
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
	stats InjectionStats, startTime time.Time, logger *slog.Logger) {

	report := healthReport{
		Service:      "edgevib-hwmon-d",
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
	if !token.WaitTimeout(5*time.Second) || token.Error() != nil {
		logger.Warn("health publish failed", "err", token.Error())
	}
}
