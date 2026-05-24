package mqtt

import (
	"encoding/json"
	"fmt"
	"log/slog"
	"time"

	mqtt "github.com/eclipse/paho.mqtt.golang"

	"edgevib/data-aggregator/internal/model"
)

// Subscriber receives MQTT messages and pushes them to a channel.
type Subscriber struct {
	client  mqtt.Client
	msgChan chan *model.SensorMessage
	logger  *slog.Logger
}

// NewSubscriber creates a new MQTT subscriber.
func NewSubscriber(logger *slog.Logger) *Subscriber {
	return &Subscriber{
		msgChan: make(chan *model.SensorMessage, 1024),
		logger:  logger,
	}
}

// Connect establishes the MQTT connection with persistent session.
func (s *Subscriber) Connect(brokerURL, clientID string) error {
	opts := mqtt.NewClientOptions()
	opts.AddBroker(brokerURL)
	opts.SetClientID(clientID)
	opts.SetCleanSession(false)
	opts.SetAutoReconnect(true)
	opts.SetConnectRetry(true)
	opts.SetConnectRetryInterval(5 * time.Second)
	opts.SetMaxReconnectInterval(30 * time.Second)
	opts.SetKeepAlive(30 * time.Second)
	opts.SetPingTimeout(10 * time.Second)

	s.client = mqtt.NewClient(opts)
	token := s.client.Connect()
	if !token.WaitTimeout(10 * time.Second) {
		return fmt.Errorf("mqtt connect timeout")
	}
	if err := token.Error(); err != nil {
		return fmt.Errorf("mqtt connect: %w", err)
	}
	s.logger.Info("mqtt connected", "broker", brokerURL, "client_id", clientID)
	return nil
}

// Subscribe subscribes to the given topics with the configured QoS.
func (s *Subscriber) Subscribe(topics []string, qos byte) error {
	for _, topic := range topics {
		token := s.client.Subscribe(topic, qos, s.onMessage)
		if !token.WaitTimeout(5 * time.Second) {
			return fmt.Errorf("subscribe timeout: %s", topic)
		}
		if err := token.Error(); err != nil {
			return fmt.Errorf("subscribe %s: %w", topic, err)
		}
		s.logger.Info("mqtt subscribed", "topic", topic, "qos", qos)
	}
	return nil
}

// Publish sends a message to the given MQTT topic.
func (s *Subscriber) Publish(topic string, qos byte, payload []byte) error {
	if s.client == nil || !s.client.IsConnected() {
		return fmt.Errorf("mqtt not connected")
	}
	token := s.client.Publish(topic, qos, false, payload)
	if !token.WaitTimeout(5 * time.Second) {
		return fmt.Errorf("publish timeout: %s", topic)
	}
	return token.Error()
}

// IsConnected returns the current MQTT connection state.
func (s *Subscriber) IsConnected() bool {
	return s.client != nil && s.client.IsConnected()
}

// Messages returns the channel to receive parsed sensor messages.
func (s *Subscriber) Messages() <-chan *model.SensorMessage {
	return s.msgChan
}

// Close disconnects from the MQTT broker.
func (s *Subscriber) Close() {
	if s.client != nil {
		s.client.Disconnect(250)
	}
}

func (s *Subscriber) onMessage(_ mqtt.Client, msg mqtt.Message) {
	ts := int64(-1)
	var raw struct {
		TimestampMS int64 `json:"timestamp_ms"`
	}
	if err := json.Unmarshal(msg.Payload(), &raw); err == nil {
		ts = raw.TimestampMS
	}

	sm := &model.SensorMessage{
		Topic:       msg.Topic(),
		TimestampMS: ts,
		Payload:     msg.Payload(),
		IngestedAt:  time.Now(),
	}

	select {
	case s.msgChan <- sm:
	default:
		s.logger.Warn("message channel full, dropping", "topic", msg.Topic())
	}
}
