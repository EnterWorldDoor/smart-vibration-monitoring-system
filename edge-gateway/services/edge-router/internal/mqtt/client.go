package mqtt

import (
	"log/slog"
	"time"

	mqtt "github.com/eclipse/paho.mqtt.golang"
)

// RawMessage represents an incoming MQTT message with topic and payload.
type RawMessage struct {
	Topic   string
	Payload []byte
}

// Client wraps an MQTT connection with a buffered message channel.
type Client struct {
	client  mqtt.Client
	msgChan chan *RawMessage
	logger  *slog.Logger
}

// NewClient creates a new MQTT client wrapper.
func NewClient(logger *slog.Logger) *Client {
	return &Client{
		msgChan: make(chan *RawMessage, 1024),
		logger:  logger,
	}
}

// Connect establishes an MQTT connection with auto-reconnect.
func (c *Client) Connect(brokerURL, clientID string) error {
	opts := mqtt.NewClientOptions().
		AddBroker(brokerURL).
		SetClientID(clientID).
		SetCleanSession(false).
		SetAutoReconnect(true).
		SetConnectRetry(true).
		SetConnectRetryInterval(5 * time.Second).
		SetMaxReconnectInterval(30 * time.Second).
		SetKeepAlive(30 * time.Second).
		SetPingTimeout(10 * time.Second)

	c.client = mqtt.NewClient(opts)
	token := c.client.Connect()
	if !token.WaitTimeout(10 * time.Second) {
		return token.Error()
	}
	if err := token.Error(); err != nil {
		return err
	}

	c.logger.Info("mqtt connected", "broker", brokerURL, "client_id", clientID)
	return nil
}

// Subscribe subscribes to the given topic filters with the specified QoS.
func (c *Client) Subscribe(topics []string, qos byte) error {
	for _, topic := range topics {
		token := c.client.Subscribe(topic, qos, c.onMessage)
		if !token.WaitTimeout(5 * time.Second) {
			return token.Error()
		}
		if err := token.Error(); err != nil {
			return err
		}
		c.logger.Info("mqtt subscribed", "topic", topic, "qos", qos)
	}
	return nil
}

// onMessage is the MQTT message callback handler.
func (c *Client) onMessage(_ mqtt.Client, msg mqtt.Message) {
	raw := &RawMessage{
		Topic:   msg.Topic(),
		Payload: msg.Payload(),
	}

	select {
	case c.msgChan <- raw:
	default:
		c.logger.Warn("message channel full, dropping", "topic", msg.Topic())
	}
}

// Messages returns the read-only message channel.
func (c *Client) Messages() <-chan *RawMessage {
	return c.msgChan
}

// Publish sends a message to the given MQTT topic.
func (c *Client) Publish(topic string, qos byte, payload []byte) error {
	if c.client == nil || !c.client.IsConnected() {
		return nil // silently drop if not connected
	}
	token := c.client.Publish(topic, qos, false, payload)
	token.Wait()
	return token.Error()
}

// IsConnected reports whether the MQTT client is currently connected.
func (c *Client) IsConnected() bool {
	return c.client != nil && c.client.IsConnected()
}

// Close gracefully disconnects the MQTT client.
func (c *Client) Close() {
	if c.client != nil && c.client.IsConnected() {
		c.client.Disconnect(250)
		c.logger.Info("mqtt disconnected")
	}
}
