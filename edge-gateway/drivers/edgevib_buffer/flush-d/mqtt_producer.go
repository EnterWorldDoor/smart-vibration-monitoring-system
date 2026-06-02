/*
 * mqtt_producer.go — MQTT subscriber for edgevib-flush-d
 *
 * Subscribes to EdgeVib/+/+/+/data/# (same as data-aggregator)
 * and forwards parsed messages for block device storage.
 */

package main

import (
	"context"
	"encoding/json"
	"fmt"
	"log/slog"
	"strings"
	"time"

	mqtt "github.com/eclipse/paho.mqtt.golang"
)

/* ---- MQTT types ---- */

type MQTTSensorMessage struct {
	Topic     string
	SiteID    string
	DeviceID  string
	Payload   json.RawMessage
	Timestamp int64
}

/* ---- MQTT Producer ---- */

func runMQTTProducer(ctx context.Context, cfg *Config,
	msgCh chan<- *MQTTSensorMessage, logger *slog.Logger) {

	brokerURL := fmt.Sprintf("tcp://%s:%d", cfg.MQTT.Broker, cfg.MQTT.Port)

	opts := mqtt.NewClientOptions().
		AddBroker(brokerURL).
		SetClientID(cfg.MQTT.ClientID).
		SetCleanSession(false).          // persistent session
		SetAutoReconnect(true).
		SetConnectRetry(true).
		SetConnectRetryInterval(5 * time.Second).
		SetMaxReconnectInterval(30 * time.Second).
		SetKeepAlive(30 * time.Second).
		SetPingTimeout(10 * time.Second)

	opts.SetOnConnectHandler(func(client mqtt.Client) {
		logger.Info("MQTT connected, subscribing",
			"topic", cfg.MQTT.SubscribeTopic)
		token := client.Subscribe(cfg.MQTT.SubscribeTopic,
			cfg.MQTT.QoS, mqttMessageHandler(msgCh, logger))
		if token.WaitTimeout(10*time.Second) && token.Error() != nil {
			logger.Error("MQTT subscribe failed", "err", token.Error())
		}
	})

	opts.SetConnectionLostHandler(func(client mqtt.Client, err error) {
		logger.Warn("MQTT connection lost", "err", err)
	})

	client := mqtt.NewClient(opts)
	token := client.Connect()
	if !token.WaitTimeout(10 * time.Second) {
		logger.Error("MQTT connect timeout")
		return
	}
	if token.Error() != nil {
		logger.Error("MQTT connect failed", "err", token.Error())
		return
	}

	// Block until shutdown
	<-ctx.Done()
	client.Disconnect(250)
	logger.Info("MQTT producer stopped")
}

func mqttMessageHandler(msgCh chan<- *MQTTSensorMessage,
	logger *slog.Logger) mqtt.MessageHandler {

	return func(client mqtt.Client, msg mqtt.Message) {
		topic := msg.Topic()

		// Parse topic: EdgeVib/{site_id}/{device_type}/{device_id}/data/{data_type}
		parts := strings.Split(topic, "/")
		if len(parts) < 5 {
			logger.Debug("cannot parse topic", "topic", topic)
			return
		}

		siteID := parts[1]
		deviceID := parts[3]

		// Parse JSON to extract timestamp
		var rawMsg map[string]interface{}
		if err := json.Unmarshal(msg.Payload(), &rawMsg); err != nil {
			logger.Debug("cannot parse MQTT payload", "err", err)
			return
		}

		sensorMsg := &MQTTSensorMessage{
			Topic:    topic,
			SiteID:   siteID,
			DeviceID: deviceID,
			Payload:  msg.Payload(),
		}

		if ts, ok := rawMsg["timestamp_ms"]; ok {
			switch v := ts.(type) {
			case float64:
				sensorMsg.Timestamp = int64(v)
			case int64:
				sensorMsg.Timestamp = v
			}
		}

		select {
		case msgCh <- sensorMsg:
		default:
			logger.Warn("MQTT message channel full, dropping message")
		}
	}
}
