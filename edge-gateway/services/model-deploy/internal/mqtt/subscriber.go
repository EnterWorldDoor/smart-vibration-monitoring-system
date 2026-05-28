package mqtt

import (
	"context"
	"encoding/json"
	"fmt"
	"log/slog"
	"strings"
	"time"

	paho "github.com/eclipse/paho.mqtt.golang"

	"edgevib/model-deploy/internal/db"
)

type ReloadStatus struct {
	ModelName string `json:"model_name"`
	Version   string `json:"version"`
	Status    string `json:"status"`
	Error     string `json:"error,omitempty"`
	Timestamp string `json:"timestamp_utc"`
}

type Subscriber struct {
	client   paho.Client
	logger   *slog.Logger
	dbClient *db.Client
	statusCh chan ReloadStatus
	closeCh  chan struct{}
}

func NewSubscriber(logger *slog.Logger) *Subscriber {
	if logger == nil {
		logger = slog.New(slog.DiscardHandler)
	}
	return &Subscriber{
		logger:   logger,
		statusCh: make(chan ReloadStatus, 64),
		closeCh:  make(chan struct{}),
	}
}

func (s *Subscriber) Connect(brokerURL, clientID string, timeout time.Duration) error {
	opts := paho.NewClientOptions().
		AddBroker(brokerURL).
		SetClientID(clientID + "-sub").
		SetAutoReconnect(true).
		SetConnectRetry(true).
		SetConnectRetryInterval(5 * time.Second).
		SetMaxReconnectInterval(30 * time.Second).
		SetKeepAlive(30 * time.Second).
		SetPingTimeout(10 * time.Second).
		SetDefaultPublishHandler(s.onMessage)

	opts.OnConnect = func(c paho.Client) {
		s.logger.Info("mqtt subscriber connected", "broker", brokerURL)
	}
	opts.OnConnectionLost = func(c paho.Client, err error) {
		s.logger.Warn("mqtt subscriber connection lost", "err", err)
	}

	s.client = paho.NewClient(opts)
	token := s.client.Connect()
	if !token.WaitTimeout(timeout) {
		return fmt.Errorf("mqtt subscriber connect timeout")
	}
	return token.Error()
}

func (s *Subscriber) Subscribe(topics []string, timeout time.Duration) error {
	for _, topic := range topics {
		token := s.client.Subscribe(topic, 1, nil)
		if !token.WaitTimeout(timeout) {
			return fmt.Errorf("subscribe timeout for topic %s", topic)
		}
		if err := token.Error(); err != nil {
			s.logger.Error("subscribe failed", "topic", topic, "err", err)
			continue
		}
		s.logger.Info("subscribed to topic", "topic", topic)
	}
	return nil
}

func (s *Subscriber) IsConnected() bool {
	return s.client != nil && s.client.IsConnected()
}

func (s *Subscriber) SetDBClient(dbClient *db.Client) {
	s.dbClient = dbClient
}

func (s *Subscriber) onMessage(_ paho.Client, msg paho.Message) {
	topic := msg.Topic()
	s.logger.Debug("mqtt message received", "topic", topic)

	parts := strings.Split(topic, "/")
	if len(parts) < 6 {
		s.logger.Warn("unexpected topic format", "topic", topic)
		return
	}

	var status ReloadStatus
	if err := json.Unmarshal(msg.Payload(), &status); err != nil {
		s.logger.Warn("failed to parse status payload", "err", err)
		return
	}

	if status.ModelName == "" || status.Status == "" {
		s.logger.Warn("incomplete status message", "topic", topic)
		return
	}

	select {
	case s.statusCh <- status:
	default:
		s.logger.Warn("status channel full, dropping message", "topic", topic)
	}
}

func (s *Subscriber) ProcessEvents(ctx context.Context) {
	s.logger.Info("starting event processor")
	for {
		select {
		case <-ctx.Done():
			s.logger.Info("event processor stopped")
			return
		case status := <-s.statusCh:
			s.handleStatus(ctx, status)
		}
	}
}

func (s *Subscriber) handleStatus(ctx context.Context, status ReloadStatus) {
	s.logger.Info("processing reload status",
		"model", status.ModelName,
		"version", status.Version,
		"status", status.Status,
	)

	if s.dbClient == nil {
		s.logger.Warn("no db client set, skipping status persistence")
		return
	}

	if status.Status == "success" {
		if err := s.dbClient.MarkDeployed(ctx, status.ModelName, status.Version, "mqtt-ack"); err != nil {
			s.logger.Error("failed to mark deployed", "err", err)
		}
	} else {
		s.logger.Warn("reload reported failure",
			"model", status.ModelName,
			"version", status.Version,
			"error", status.Error,
		)
	}
}

func (s *Subscriber) Close() {
	if s.client != nil && s.client.IsConnected() {
		s.client.Disconnect(250)
	}
}
