/*
 * health.go — MQTT health reporter
 *
 * Publishes periodic health metrics to MQTT.
 * Uses persistent MQTT connection (pattern B: open once, publish many).
 *
 * Reference: iio-d/health.go for persistent MQTT connection pattern
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

/* ---- Health stats ---- */

type HealthStats struct {
	SaveCount    int64
	SaveErrors   int64
	LoadCount    int64
	LoadErrors   int64
	LastSaveTime time.Time
}

/* ---- MQTT health payload ---- */

type healthPayload struct {
	Service      string `json:"service"`
	Status       string `json:"status"`
	RTCDevice    string `json:"rtc_device"`
	SaveCount    int64  `json:"save_count"`
	SaveErrors   int64  `json:"save_errors"`
	LoadCount    int64  `json:"load_count"`
	LoadErrors   int64  `json:"load_errors"`
	LastSaveTime string `json:"last_save_time"`
	UptimeSec    int64  `json:"uptime_seconds"`
}

/* ---- RunHealthReporter ---- */

func RunHealthReporter(
	ctx context.Context,
	cfg *Config,
	rtcDevice string,
	statsCh <-chan HealthStats,
	logger *slog.Logger,
) {
	startTime := time.Now()

	brokerURL := fmt.Sprintf("tcp://%s:%d", cfg.MQTT.Broker, cfg.MQTT.Port)

	opts := mqtt.NewClientOptions().
		AddBroker(brokerURL).
		SetClientID(cfg.MQTT.ClientID + "-health").
		SetCleanSession(true).
		SetKeepAlive(30 * time.Second).
		SetConnectRetry(true).
		SetConnectRetryInterval(5 * time.Second)

	client := mqtt.NewClient(opts)
	token := client.Connect()
	if !token.WaitTimeout(5 * time.Second) {
		logger.Warn("health reporter: MQTT connect timeout during startup")
	}
	if err := token.Error(); err != nil {
		logger.Warn("health reporter: initial MQTT connect failed", "err", err)
	}

	defer func() {
		// Final health update before exit
		publishHealth(client, cfg.Health.Topic, rtcDevice, nil, startTime, logger)
		client.Disconnect(250)
	}()

	ticker := time.NewTicker(time.Duration(cfg.Health.IntervalS) * time.Second)
	defer ticker.Stop()

	var latestStats HealthStats
	hasStats := false

	for {
		select {
		case <-ctx.Done():
			return

		case s, ok := <-statsCh:
			if ok {
				latestStats = s
				hasStats = true
			}

		case <-ticker.C:
			var ps *HealthStats
			if hasStats {
				ps = &latestStats
			}
			publishHealth(client, cfg.Health.Topic, rtcDevice, ps, startTime, logger)
		}
	}
}

func publishHealth(
	client mqtt.Client,
	topic string,
	rtcDevice string,
	stats *HealthStats,
	startTime time.Time,
	logger *slog.Logger,
) {
	h := healthPayload{
		Service:   "edgevib-rtc-d",
		Status:    "running",
		RTCDevice: rtcDevice,
		UptimeSec: int64(time.Since(startTime).Seconds()),
	}

	if stats != nil {
		h.SaveCount = stats.SaveCount
		h.SaveErrors = stats.SaveErrors
		h.LoadCount = stats.LoadCount
		h.LoadErrors = stats.LoadErrors
		if !stats.LastSaveTime.IsZero() {
			h.LastSaveTime = stats.LastSaveTime.Format(time.RFC3339)
		}
	}

	payload, err := json.Marshal(h)
	if err != nil {
		logger.Warn("health: json marshal failed", "err", err)
		return
	}

	token := client.Publish(topic, 0, false, string(payload))
	if !token.WaitTimeout(3 * time.Second) {
		logger.Warn("health: publish timeout", "topic", topic)
		return
	}
	if err := token.Error(); err != nil {
		logger.Warn("health: publish failed", "topic", topic, "err", err)
	}
}
