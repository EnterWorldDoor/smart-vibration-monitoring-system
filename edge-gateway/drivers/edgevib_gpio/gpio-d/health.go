/*
 * health.go — MQTT subscriber + health evaluation + health reporting
 *
 * Subscribes to system alert and device health MQTT topics,
 * evaluates system health status, outputs GPIO actions.
 *
 * Design: health evaluator runs in its own goroutine, outputting
 * GPIOAction messages to a channel consumed by the main loop.
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

/* ---- MQTT message types ---- */

type prometheusAlert struct {
	Status string `json:"status"` // "firing" | "resolved"
	Alerts []struct {
		Labels      map[string]string `json:"labels"`
		Annotations map[string]string `json:"annotations"`
	} `json:"alerts"`
}

type inferenceReport struct {
	SiteID     string `json:"site_id"`
	DeviceID   string `json:"device_id"`
	Severity   string `json:"severity"` // "WARNING" | "CRITICAL"
	DeviceType string `json:"device_type"`
}

type deviceHealth struct {
	DeviceID  string `json:"device_id"`
	Status    string `json:"status"` // "online" | "offline"
	Timestamp int64  `json:"timestamp_ms"`
}

/* ---- Rule engine state ---- */

type alertState struct {
	hasCritical bool
	hasWarning  bool
	deviceLastSeen map[string]time.Time // device_id → last heartbeat time
}

func newAlertState() *alertState {
	return &alertState{
		deviceLastSeen: make(map[string]time.Time),
	}
}

// evaluateHealth determines GPIO output states from current alert state.
// Rules (in priority order):
//   1. Any CRITICAL → SYSTEM_OK=LOW, GATEWAY_ALERT=HIGH
//   2. Any WARNING or device timeout → SYSTEM_OK=HIGH, GATEWAY_ALERT=HIGH
//   3. All clear → SYSTEM_OK=HIGH, GATEWAY_ALERT=LOW
func (s *alertState) evaluateHealth(timeoutS int) []GPIOAction {
	var actions []GPIOAction

	// Check device heartbeat timeouts
	now := time.Now()
	hasOffline := false
	for id, lastSeen := range s.deviceLastSeen {
		if now.Sub(lastSeen) > time.Duration(timeoutS)*time.Second {
			hasOffline = true
			slog.Debug("device heartbeat timeout", "device", id, "last_seen", lastSeen)
		}
	}

	if s.hasCritical {
		// Priority 1: CRITICAL overrides everything
		actions = append(actions,
			GPIOAction{Line: 0, Value: 0, Label: "SYSTEM_OK=0 (CRITICAL)"},
			GPIOAction{Line: 1, Value: 1, Label: "GATEWAY_ALERT=1 (CRITICAL)"},
		)
	} else if s.hasWarning || hasOffline {
		// Priority 2: WARNING or device offline — green still on, yellow on
		actions = append(actions,
			GPIOAction{Line: 0, Value: 1, Label: "SYSTEM_OK=1 (warning present)"},
			GPIOAction{Line: 1, Value: 1, Label: "GATEWAY_ALERT=1 (WARNING/offline)"},
		)
	} else {
		// Priority 3: All clear
		actions = append(actions,
			GPIOAction{Line: 0, Value: 1, Label: "SYSTEM_OK=1 (all clear)"},
			GPIOAction{Line: 1, Value: 0, Label: "GATEWAY_ALERT=0 (all clear)"},
		)
	}

	// Dedup: don't re-apply same state (compare with previous)
	// Simple approach: always emit — gpioset is idempotent

	return actions
}

/* ---- MQTT subscriber ---- */

func runMQTTSubscriber(ctx context.Context, brokerURL string, cfg *Config,
	actionCh chan<- GPIOAction, logger *slog.Logger) {

	state := newAlertState()
	healthClientID := cfg.MQTT.ClientID + "-health"

	for {
		select {
		case <-ctx.Done():
			return
		default:
		}

		opts := mqtt.NewClientOptions().
			AddBroker(brokerURL).
			SetClientID(healthClientID).
			SetAutoReconnect(true).
			SetConnectRetry(true).
			SetConnectRetryInterval(5 * time.Second).
			SetMaxReconnectInterval(30 * time.Second).
			SetCleanSession(true).
			SetKeepAlive(30 * time.Second).
			SetPingTimeout(10 * time.Second)

		opts.SetOnConnectHandler(func(c mqtt.Client) {
			for _, topic := range cfg.MQTT.SubscribeTopics {
				token := c.Subscribe(topic, 1, func(_ mqtt.Client, msg mqtt.Message) {
					handleAlertMessage(msg.Topic(), msg.Payload(), state, actionCh, logger)
				})
				if token.Wait() && token.Error() != nil {
					logger.Error("mqtt subscribe failed", "topic", topic, "err", token.Error())
				}
			}
			logger.Info("mqtt connected", "broker", brokerURL, "topics", cfg.MQTT.SubscribeTopics)
		})

		opts.SetOnConnectionLost(func(c mqtt.Client, err error) {
			logger.Warn("mqtt connection lost", "err", err)
		})

		client := mqtt.NewClient(opts)
		token := client.Connect()
		if token.WaitTimeout(10*time.Second) && token.Error() != nil {
			logger.Error("mqtt connect failed", "err", token.Error())
			select {
			case <-ctx.Done():
				return
			case <-time.After(5 * time.Second):
			}
			continue
		}

		// Block until context cancelled
		<-ctx.Done()
		client.Disconnect(250)
		return
	}
}

/* ---- Message handlers ---- */

func handleAlertMessage(topic string, payload []byte, state *alertState,
	actionCh chan<- GPIOAction, logger *slog.Logger) {

	logger.Debug("mqtt message", "topic", topic, "len", len(payload))

	// Try to parse as prometheus alert
	var promAlert prometheusAlert
	if json.Unmarshal(payload, &promAlert) == nil && len(promAlert.Alerts) > 0 {
		prevCritical := state.hasCritical
		prevWarning := state.hasWarning

		for _, a := range promAlert.Alerts {
			severity := a.Labels["severity"]
			if severity == "critical" {
				state.hasCritical = true
			} else if severity == "warning" {
				state.hasWarning = true
			}
		}
		if promAlert.Status == "resolved" {
			// Re-evaluate: remove critical/warning flags, device timeouts may remain
			state.hasCritical = false
			state.hasWarning = false
		}

		if prevCritical != state.hasCritical || prevWarning != state.hasWarning {
			actions := state.evaluateHealth(120)
			for _, a := range actions {
				actionCh <- a
			}
		}
		return
	}

	// Try to parse as inference report
	var infReport inferenceReport
	if json.Unmarshal(payload, &infReport) == nil && infReport.Severity != "" {
		prevCritical := state.hasCritical
		prevWarning := state.hasWarning

		switch infReport.Severity {
		case "CRITICAL":
			state.hasCritical = true
		case "WARNING":
			state.hasWarning = true
		}

		if prevCritical != state.hasCritical || prevWarning != state.hasWarning {
			actions := state.evaluateHealth(120)
			for _, a := range actions {
				actionCh <- a
			}
		}
		return
	}

	// Try to parse as device health heartbeat
	var devHealth deviceHealth
	if json.Unmarshal(payload, &devHealth) == nil && devHealth.DeviceID != "" {
		state.deviceLastSeen[devHealth.DeviceID] = time.Now()
	}
}

/* ---- Health reporter ---- */

func runHealthReporter(ctx context.Context, cfg *Config, statsCh <-chan DaemonStats, logger *slog.Logger) {
	healthClientID := cfg.MQTT.ClientID + "-health-reporter"
	var latest DaemonStats

	// goroutine: accumulate stats
	go func() {
		for {
			select {
			case <-ctx.Done():
				return
			case s := <-statsCh:
				latest = s
			}
		}
	}()

	ticker := time.NewTicker(time.Duration(cfg.Health.IntervalS) * time.Second)
	defer ticker.Stop()

	for {
		select {
		case <-ctx.Done():
			return
		case <-ticker.C:
			broker := fmt.Sprintf("tcp://%s:%d", cfg.MQTT.Broker, cfg.MQTT.Port)
			payload := map[string]interface{}{
				"service":    "edgevib-gpio-d",
				"status":     "running",
				"actions":    latest.Actions,
				"errors":     latest.Errors,
				"irq_events": latest.IRQEvents,
				"uptime_s":   int(time.Since(latest.StartTime).Seconds()),
			}

			opts := mqtt.NewClientOptions().
				AddBroker(broker).
				SetClientID(healthClientID).
				SetConnectTimeout(5 * time.Second)

			client := mqtt.NewClient(opts)
			if token := client.Connect(); token.WaitTimeout(5*time.Second) && token.Error() == nil {
				body, _ := json.Marshal(payload)
				client.Publish(cfg.Health.Topic, 1, false, body)
				client.Disconnect(250)
			}
		}
	}
}

// publishEvent sends a one-shot MQTT emergency message
func publishEvent(broker, topic string, payload map[string]interface{}) {
	body, _ := json.Marshal(payload)

	opts := mqtt.NewClientOptions().
		AddBroker(broker).
		SetClientID("edgevib-gpio-d-emergency").
		SetConnectTimeout(3 * time.Second)

	client := mqtt.NewClient(opts)
	if token := client.Connect(); token.WaitTimeout(3*time.Second) && token.Error() == nil {
		client.Publish(topic, 1, false, body)
		client.Disconnect(250)
	}
}
