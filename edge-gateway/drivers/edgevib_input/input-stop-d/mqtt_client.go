/*
 * mqtt_client.go — MQTT subscriber for F407 E-Stop state
 *
 * Subscribes to rs232-gateway MQTT topics, parses the JSON payload
 * to extract e_stop_state, and translates to KEY_STOP/KEY_WAKEUP events
 * via the Injector.
 *
 * Data flow:
 *   MQTT EdgeVib/+/motor/+/status/health  → parse JSON["data"]["e_stop_state"]
 *   MQTT EdgeVib/+/motor/+/status/emergency → immediate KEY_STOP=1
 *   → Injector.InjectEStopState(state)
 *
 * Fail-safe: MQTT timeout 10s → KEY_STOP=1
 *   Uses atomic.Int64 for lock-free sharing of last-rx timestamp between
 *   the MQTT handler goroutine and the fail-safe goroutine.
 *
 * Pattern: D3 gpio-d/health.go (MQTT subscription + OnConnect handler)
 */

package main

import (
	"context"
	"encoding/json"
	"fmt"
	"log/slog"
	"sync"
	"sync/atomic"
	"time"

	mqtt "github.com/eclipse/paho.mqtt.golang"
)

/* ---- MQTT message types ---- */

// systemStatusPayload matches rs232-gateway's JSON output for CMD 0x07.
type systemStatusData struct {
	SystemState   uint8 `json:"system_state"`
	OperationMode uint8 `json:"operation_mode"`
	EStopState    uint8 `json:"e_stop_state"`
	HealthLevel   uint8 `json:"health_level"`
	EventSource   uint8 `json:"event_source"`
}

type systemStatusMsg struct {
	Source string           `json:"source"`
	DevID  string           `json:"dev_id"`
	Cmd    string           `json:"cmd"`
	Data   systemStatusData `json:"data"`
}

// emergencyMsg matches rs232-gateway's JSON output for CMD 0x10.
type emergencyData struct {
	EventCode uint32 `json:"event_code"`
	Severity  uint32 `json:"severity"`
}

type emergencyMsg struct {
	Source string        `json:"source"`
	DevID  string        `json:"dev_id"`
	Cmd    string        `json:"cmd"`
	Data   emergencyData `json:"data"`
}

/* ---- MQTT client stats ---- */

type MQTTStats struct {
	MessagesRx     int64
	EmergencyRx    int64
	HealthRx       int64
	ParseErrors    int64
	InjectErrors   int64
	FailSafeEvents int64
	LastRxTime     time.Time
}

/* ---- MQTT subscriber ---- */

func runMQTTSubscriber(ctx context.Context, brokerURL string, cfg *Config,
	inj *Injector, statsCh chan<- MQTTStats, lastRxAtomic *atomic.Int64,
	logger *slog.Logger) {

	stats := MQTTStats{}
	var mu sync.Mutex

	healthClientID := cfg.MQTT.ClientID

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
				topicLocal := topic
				token := c.Subscribe(topicLocal, 1, func(_ mqtt.Client, msg mqtt.Message) {
					mu.Lock()
					stats.MessagesRx++
					stats.LastRxTime = time.Now()
					mu.Unlock()

					// Update shared atomic for fail-safe goroutine
					lastRxAtomic.Store(time.Now().UnixNano())

					handleMessage(msg.Topic(), msg.Payload(), inj,
						&stats, &mu, logger)
				})
				if token.Wait() && token.Error() != nil {
					logger.Error("mqtt subscribe failed",
						"topic", topic,
						"err", token.Error())
				}
			}
			logger.Info("mqtt connected",
				"broker", brokerURL,
				"topics", cfg.MQTT.SubscribeTopics)
		})

		opts.OnConnectionLost = func(c mqtt.Client, err error) {
			logger.Warn("mqtt connection lost", "err", err)
		}

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

func handleMessage(topic string, payload []byte, inj *Injector,
	stats *MQTTStats, mu *sync.Mutex, logger *slog.Logger) {

	logger.Debug("mqtt message", "topic", topic, "len", len(payload))

	// Try to parse as system_status (CMD 0x07) — primary channel
	var statusMsg systemStatusMsg
	if err := json.Unmarshal(payload, &statusMsg); err == nil && statusMsg.Cmd == "system_status" {
		mu.Lock()
		stats.HealthRx++
		mu.Unlock()

		eStopState := int(statusMsg.Data.EStopState)
		logger.Info("e_stop_state received",
			"dev_id", statusMsg.DevID,
			"system_state", statusMsg.Data.SystemState,
			"e_stop_state", eStopState,
			"health_level", statusMsg.Data.HealthLevel,
		)

		if err := inj.InjectEStopState(eStopState); err != nil {
			mu.Lock()
			stats.InjectErrors++
			mu.Unlock()
			logger.Warn("inject failed", "err", err, "e_stop_state", eStopState)
		}
		return
	}

	// Try to parse as emergency event (CMD 0x10) — fast path
	var emMsg emergencyMsg
	if err := json.Unmarshal(payload, &emMsg); err == nil && emMsg.Cmd == "emergency" {
		mu.Lock()
		stats.EmergencyRx++
		mu.Unlock()

		logger.Warn("EMERGENCY event received",
			"dev_id", emMsg.DevID,
			"event_code", emMsg.Data.EventCode,
			"severity", emMsg.Data.Severity,
		)

		if err := inj.InjectEStopState(1); err != nil {
			mu.Lock()
			stats.InjectErrors++
			mu.Unlock()
			logger.Warn("emergency inject failed", "err", err)
		}
		return
	}

	// Try to parse as generic JSON with e_stop_state at top level
	var simpleMsg struct {
		Data struct {
			EStopState uint8 `json:"e_stop_state"`
		} `json:"data"`
	}
	if err := json.Unmarshal(payload, &simpleMsg); err == nil && statusMsg.Cmd == "" {
		eStopState := int(simpleMsg.Data.EStopState)
		logger.Info("e_stop_state received (simple format)",
			"e_stop_state", eStopState,
		)
		if err := inj.InjectEStopState(eStopState); err != nil {
			mu.Lock()
			stats.InjectErrors++
			mu.Unlock()
			logger.Warn("inject failed", "err", err, "e_stop_state", eStopState)
		}
		mu.Lock()
		stats.HealthRx++
		mu.Unlock()
		return
	}

	// Unknown message format
	mu.Lock()
	stats.ParseErrors++
	mu.Unlock()
	logger.Debug("unrecognized MQTT message",
		"topic", topic,
		"payload_preview", fmt.Sprintf("%.100s", string(payload)),
	)
}

/* ---- Fail-safe goroutine ---- */

// runFailSafe monitors the last MQTT message timestamp via an atomic.
// If no message is received within fail_safe_timeout_s seconds, it
// injects KEY_STOP=1 (safety default). On recovery, the next MQTT
// message will restore the correct state.
func runFailSafe(ctx context.Context, cfg *Config, inj *Injector,
	lastRxAtomic *atomic.Int64, logger *slog.Logger) {

	timeout := time.Duration(cfg.MQTT.FailSafeTimeoutS) * time.Second
	if timeout <= 0 {
		return // fail-safe disabled
	}

	ticker := time.NewTicker(1 * time.Second)
	defer ticker.Stop()

	// Track fail-safe event count locally (only log, don't double-count)
	failSafeCount := int64(0)
	failSafeActive := false

	for {
		select {
		case <-ctx.Done():
			return
		case <-ticker.C:
			lastRxNano := lastRxAtomic.Load()

			// If never received a message, skip (boot grace period)
			if lastRxNano == 0 {
				continue
			}

			lastRx := time.Unix(0, lastRxNano)
			if time.Since(lastRx) > timeout {
				if !failSafeActive {
					failSafeActive = true
					failSafeCount++
					logger.Warn("MQTT timeout — fail-safe: KEY_STOP=1",
						"last_rx", lastRx.Format(time.RFC3339),
						"timeout_s", cfg.MQTT.FailSafeTimeoutS,
						"elapsed_s", int(time.Since(lastRx).Seconds()),
						"fail_safe_count", failSafeCount,
					)

					if err := inj.InjectEStopState(1); err != nil {
						logger.Warn("fail-safe inject failed", "err", err)
					}
				}
			} else {
				if failSafeActive {
					logger.Info("MQTT recovered — fail-safe cleared")
					failSafeActive = false
				}
			}
		}
	}
}
