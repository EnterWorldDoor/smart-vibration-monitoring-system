package mqtt

import (
	"encoding/json"
	"fmt"
	"log/slog"
	"time"

	paho "github.com/eclipse/paho.mqtt.golang"
)

type Publisher struct {
	client     paho.Client
	logger     *slog.Logger
	clientID   string
}

func NewPublisher(logger *slog.Logger) *Publisher {
	if logger == nil {
		logger = discardLog
	}
	return &Publisher{logger: logger}
}

func (p *Publisher) Connect(brokerURL, clientID string, timeout time.Duration) error {
	p.clientID = clientID
	opts := paho.NewClientOptions().
		AddBroker(brokerURL).
		SetClientID(clientID).
		SetAutoReconnect(true).
		SetConnectRetry(true).
		SetConnectRetryInterval(5 * time.Second).
		SetMaxReconnectInterval(30 * time.Second).
		SetKeepAlive(30 * time.Second).
		SetPingTimeout(10 * time.Second)

	opts.OnConnect = func(c paho.Client) {
		p.logger.Info("mqtt publisher connected", "broker", brokerURL)
	}
	opts.OnConnectionLost = func(c paho.Client, err error) {
		p.logger.Warn("mqtt publisher connection lost", "err", err)
	}

	p.client = paho.NewClient(opts)
	token := p.client.Connect()
	if !token.WaitTimeout(timeout) {
		return fmt.Errorf("mqtt publisher connect timeout")
	}
	return token.Error()
}

func (p *Publisher) IsConnected() bool {
	return p.client != nil && p.client.IsConnected()
}

type ReloadPayload struct {
	ModelName   string `json:"model_name"`
	Version     string `json:"version"`
	FilePath    string `json:"file_path"`
	Timestamp   string `json:"timestamp_utc"`
}

func (p *Publisher) PublishReload(topic, modelName, version, filePath string) error {
	if p.client == nil || !p.client.IsConnected() {
		return fmt.Errorf("mqtt publisher not connected")
	}

	payload := ReloadPayload{
		ModelName: modelName,
		Version:   version,
		FilePath:  filePath,
		Timestamp: time.Now().UTC().Format(time.RFC3339),
	}

	data, err := json.Marshal(payload)
	if err != nil {
		return fmt.Errorf("marshal reload payload: %w", err)
	}

	token := p.client.Publish(topic, 1, false, data)
	if !token.WaitTimeout(5 * time.Second) {
		return fmt.Errorf("publish reload timeout")
	}
	if err := token.Error(); err != nil {
		return fmt.Errorf("publish reload: %w", err)
	}

	p.logger.Info("reload command published", "topic", topic, "model", modelName, "version", version)
	return nil
}

func (p *Publisher) Close() {
	if p.client != nil && p.client.IsConnected() {
		p.client.Disconnect(250)
	}
}
