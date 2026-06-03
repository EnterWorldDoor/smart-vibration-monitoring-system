/*
 * health.go — 30s MQTT health telemetry for edgevib-flush-d
 *
 * Follows the same pattern as D1 can-d/health.go and D2 iio-d/health.go.
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

/* ---- Daemon stats ---- */

type DaemonStats struct {
	Writes        int64
	WriteErrors   int64
	Flushes       int64
	RowsFlushed   int64
	DBErrors      int64
	LastReadSeq   uint32
	LastFlushTime time.Time
}

type flushHealthReport struct {
	Service      string `json:"service"`
	Status       string `json:"status"`
	Writes       int64  `json:"writes"`
	WriteErrors  int64  `json:"write_errors"`
	Flushes      int64  `json:"flushes"`
	RowsFlushed  int64  `json:"rows_flushed"`
	DBErrors     int64  `json:"db_errors"`
	UptimeS      int64  `json:"uptime_s"`
}

func healthSend(ch chan<- DaemonStats, stats DaemonStats) error {
	select {
	case ch <- stats:
		return nil
	default:
		return fmt.Errorf("health channel full")
	}
}

func runHealthReporter(ctx context.Context, cfg *Config,
	ch <-chan DaemonStats, logger *slog.Logger) {

	var latest DaemonStats
	startTime := time.Now()

	brokerURL := fmt.Sprintf("tcp://%s:%d", cfg.MQTT.Broker, cfg.MQTT.Port)
	opts := mqtt.NewClientOptions().
		AddBroker(brokerURL).
		SetClientID(cfg.MQTT.ClientID + "-health").
		SetCleanSession(true).
		SetAutoReconnect(true).
		SetConnectRetry(true).
		SetConnectRetryInterval(5 * time.Second).
		SetMaxReconnectInterval(30 * time.Second)

	client := mqtt.NewClient(opts)
	token := client.Connect()
	if !token.WaitTimeout(10*time.Second) || token.Error() != nil {
		logger.Warn("health MQTT connect failed")
		return
	}
	defer client.Disconnect(250)

	// Collect stats
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
			publishFlushHealth(client, cfg.Health.Topic, latest, startTime, logger)
			return
		case <-ticker.C:
			publishFlushHealth(client, cfg.Health.Topic, latest, startTime, logger)
		}
	}
}

func publishFlushHealth(client mqtt.Client, topic string,
	stats DaemonStats, startTime time.Time, logger *slog.Logger) {

	report := flushHealthReport{
		Service:     "edgevib-flush-d",
		Status:      "ok",
		Writes:      stats.Writes,
		WriteErrors: stats.WriteErrors,
		Flushes:     stats.Flushes,
		RowsFlushed: stats.RowsFlushed,
		DBErrors:    stats.DBErrors,
		UptimeS:     int64(time.Since(startTime).Seconds()),
	}

	payload, _ := json.Marshal(report)
	client.Publish(topic, 1, false, payload)
}
