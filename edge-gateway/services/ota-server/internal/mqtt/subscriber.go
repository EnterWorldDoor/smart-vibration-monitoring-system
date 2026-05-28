package mqtt

import (
	"context"
	"encoding/json"
	"fmt"
	"log/slog"
	"strings"
	"time"

	"edgevib/ota-server/internal/db"

	paho "github.com/eclipse/paho.mqtt.golang"
)

type OTAStatusEvent struct {
	Platform   string `json:"platform"`
	DeviceID   string `json:"device_id"`
	SiteID     string `json:"site_id"`
	FromVer    string `json:"from_version,omitempty"`
	ToVer      string `json:"to_version,omitempty"`
	Status     string `json:"status,omitempty"`
	Progress   int    `json:"progress,omitempty"`
	Version    string `json:"version,omitempty"`
	ErrorMsg   string `json:"error_msg,omitempty"`
	Timestamp  string `json:"timestamp,omitempty"`
}

type Subscriber struct {
	client  paho.Client
	eventCh chan OTAStatusEvent
	logger  *slog.Logger
}

func NewSubscriber(logger *slog.Logger) *Subscriber {
	return &Subscriber{
		eventCh: make(chan OTAStatusEvent, 256),
		logger:  logger,
	}
}

func (s *Subscriber) Connect(brokerURL, clientID string) error {
	opts := paho.NewClientOptions().
		AddBroker(brokerURL).
		SetClientID(clientID).
		SetAutoReconnect(true).
		SetConnectRetry(true).
		SetConnectRetryInterval(5 * time.Second).
		SetMaxReconnectInterval(30 * time.Second).
		SetKeepAlive(30 * time.Second).
		SetPingTimeout(10 * time.Second).
		SetOnConnectHandler(func(c paho.Client) {
			s.logger.Info("mqtt connected", "broker", brokerURL)
		}).
		SetConnectionLostHandler(func(c paho.Client, err error) {
			s.logger.Warn("mqtt connection lost", "error", err)
		})

	s.client = paho.NewClient(opts)
	token := s.client.Connect()
	if token.WaitTimeout(10 * time.Second) {
		return token.Error()
	}
	return nil
}

func (s *Subscriber) Subscribe(topics []string) error {
	for _, topic := range topics {
		token := s.client.Subscribe(topic, 1, s.onMessage)
		if token.WaitTimeout(5 * time.Second) {
			if err := token.Error(); err != nil {
				s.logger.Warn("subscribe failed", "topic", topic, "error", err)
				continue
			}
		}
		s.logger.Info("subscribed", "topic", topic)
	}
	return nil
}

func (s *Subscriber) IsConnected() bool {
	return s.client != nil && s.client.IsConnected()
}

func (s *Subscriber) Publish(topic string, qos byte, retained bool, payload interface{}) error {
	if !s.IsConnected() {
		return fmt.Errorf("mqtt not connected")
	}
	token := s.client.Publish(topic, qos, retained, payload)
	if token.WaitTimeout(5*time.Second) && token.Error() != nil {
		return token.Error()
	}
	return nil
}

func (s *Subscriber) Close() {
	if s.client != nil && s.client.IsConnected() {
		s.client.Disconnect(250)
	}
}

func (s *Subscriber) onMessage(_ paho.Client, msg paho.Message) {
	parts := strings.Split(msg.Topic(), "/")
	if len(parts) < 5 {
		return
	}
	// EdgeVib/{site_id}/ota/{device_id}/status  → parts[0-5]
	// EdgeVib/{site_id}/ota/{device_id}/version → parts[0-5]
	siteID := parts[1]
	deviceID := parts[3]
	msgType := parts[4]

	var event OTAStatusEvent
	if err := json.Unmarshal(msg.Payload(), &event); err != nil {
		s.logger.Warn("mqtt message parse failed", "topic", msg.Topic(), "error", err)
		return
	}

	event.SiteID = siteID
	event.DeviceID = deviceID

	switch msgType {
	case "status":
		if event.Status == "" {
			return
		}
	case "version":
		if event.Version == "" {
			return
		}
	default:
		return
	}

	select {
	case s.eventCh <- event:
	default:
		s.logger.Warn("mqtt event channel full, dropping message", "topic", msg.Topic())
	}
}

func (s *Subscriber) ProcessEvents(ctx context.Context, dbClient *db.Client, siteID string) {
	go func() {
		for {
			select {
			case <-ctx.Done():
				return
			case event := <-s.eventCh:
				s.handleEvent(ctx, dbClient, siteID, event)
			}
		}
	}()
}

func (s *Subscriber) handleEvent(ctx context.Context, dbClient *db.Client, siteID string, event OTAStatusEvent) {
	if event.Platform == "" {
		return
	}

	switch {
	case event.Status != "":
		// OTA status update
		existing, err := dbClient.FindPendingUpgrade(ctx, event.Platform, event.DeviceID, event.SiteID)
		if err != nil {
			s.logger.Warn("find pending upgrade failed", "error", err)
			return
		}

		if existing != nil {
			if err := dbClient.UpdateUpgradeStatus(ctx, existing.ID, event.Status, event.Progress, strPtr(event.ErrorMsg)); err != nil {
				s.logger.Warn("update upgrade status failed", "error", err)
			}
		} else {
			// New upgrade initiated by device
			h := &db.UpgradeHistory{
				Platform:    event.Platform,
				DeviceID:    event.DeviceID,
				SiteID:      event.SiteID,
				FromVersion: event.FromVer,
				ToVersion:   event.ToVer,
				Status:      event.Status,
				Progress:    event.Progress,
				ErrorMsg:    strPtr(event.ErrorMsg),
			}
			if _, err := dbClient.InsertUpgradeHistory(ctx, h); err != nil {
				s.logger.Warn("insert upgrade history failed", "error", err)
			}
		}

	case event.Version != "":
		// Version report — record as success entry
		h := &db.UpgradeHistory{
			Platform:    event.Platform,
			DeviceID:    event.DeviceID,
			SiteID:      event.SiteID,
			ToVersion:   event.Version,
			Status:      "success",
			Progress:    100,
		}
		if _, err := dbClient.InsertUpgradeHistory(ctx, h); err != nil {
			s.logger.Warn("insert version report failed", "error", err)
		}
	}
}

func strPtr(s string) *string {
	if s == "" {
		return nil
	}
	return &s
}
