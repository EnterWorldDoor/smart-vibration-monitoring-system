package main

import (
	"encoding/hex"
	"encoding/json"
	"fmt"
	"log/slog"
	"time"

	mqtt "github.com/eclipse/paho.mqtt.golang"
)

/* canRawMsg matches the JSON published by ESP32 on_can_raw_data_received */
type canRawMsg struct {
	CanID uint32 `json:"id"`
	DLC   uint8  `json:"dlc"`
	Data  string `json:"data"`   /* hex string, e.g. "A1B2C3D4E5F6" */
	Flags uint8  `json:"flags"`
}

/* MQTTClient wraps a paho MQTT client subscribed to CAN raw topics */
type MQTTClient struct {
	client  mqtt.Client
	canSock *SocketCAN
	logger  *slog.Logger
	stats   struct {
		framesRx uint64
		framesTx uint64
		crcErrors uint64
	}
}

/* NewMQTTClient creates and connects an MQTT client */
func NewMQTTClient(brokerURL, clientID, topicPattern string,
	canSock *SocketCAN, logger *slog.Logger) (*MQTTClient, error) {

	mc := &MQTTClient{
		canSock: canSock,
		logger:  logger,
	}

	opts := mqtt.NewClientOptions().
		AddBroker(brokerURL).
		SetClientID(clientID).
		SetAutoReconnect(true).
		SetConnectRetry(true).
		SetConnectRetryInterval(5 * time.Second).
		SetMaxReconnectInterval(30 * time.Second).
		SetCleanSession(true).
		SetKeepAlive(30 * time.Second).
		SetPingTimeout(10 * time.Second)

	opts.OnConnect = func(c mqtt.Client) {
		logger.Info("MQTT connected", "broker", brokerURL)
		token := c.Subscribe(topicPattern, 1, mc.onMessage)
		if token.Wait() && token.Error() != nil {
			logger.Error("MQTT subscribe failed", "topic", topicPattern, "err", token.Error())
		} else {
			logger.Info("MQTT subscribed", "topic", topicPattern)
		}
	}

	opts.OnConnectionLost = func(c mqtt.Client, err error) {
		logger.Warn("MQTT connection lost", "err", err)
	}

	opts.OnReconnecting = func(c mqtt.Client, opts *mqtt.ClientOptions) {
		logger.Info("MQTT reconnecting")
	}

	mc.client = mqtt.NewClient(opts)
	token := mc.client.Connect()
	if !token.WaitTimeout(10 * time.Second) {
		return nil, fmt.Errorf("MQTT connect timeout")
	}
	if token.Error() != nil {
		return nil, fmt.Errorf("MQTT connect: %w", token.Error())
	}

	return mc, nil
}

/* onMessage is the MQTT message callback (runs in paho goroutine) */
func (mc *MQTTClient) onMessage(client mqtt.Client, msg mqtt.Message) {
	mc.stats.framesRx++

	var raw canRawMsg
	if err := json.Unmarshal(msg.Payload(), &raw); err != nil {
		mc.logger.Warn("JSON parse error", "topic", msg.Topic(), "err", err)
		return
	}

	/* Hex decode CAN data */
	data, err := hex.DecodeString(raw.Data)
	if err != nil {
		mc.logger.Warn("hex decode error", "data", raw.Data, "err", err)
		return
	}

	/* Inject into SocketCAN */
	if err := mc.canSock.WriteFrame(raw.CanID, data, raw.DLC, raw.Flags); err != nil {
		mc.logger.Error("SocketCAN write error", "err", err)
		return
	}

	mc.stats.framesTx++

	/* Track CRC errors from F407 side (flags bit0 = CRC pass/fail) */
	if raw.Flags&0x01 == 0 {
		mc.stats.crcErrors++
	}

	mc.logger.Debug("CAN frame injected",
		"id", fmt.Sprintf("0x%03X", raw.CanID),
		"dlc", raw.DLC,
		"flags", raw.Flags,
	)
}

/* Close disconnects the MQTT client */
func (mc *MQTTClient) Close() {
	mc.client.Disconnect(250)
}

/* Stats returns the current statistics */
func (mc *MQTTClient) Stats() (framesRx, framesTx, crcErrors uint64) {
	return mc.stats.framesRx, mc.stats.framesTx, mc.stats.crcErrors
}
