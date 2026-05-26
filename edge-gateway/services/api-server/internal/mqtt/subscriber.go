package mqtt

import (
	"encoding/json"
	"log/slog"
	"strings"
	"time"

	paho "github.com/eclipse/paho.mqtt.golang"
)

type Event struct {
	Type      string `json:"type"`
	SiteID    string `json:"site_id,omitempty"`
	DeviceID  string `json:"device_id,omitempty"`
	Severity  string `json:"severity,omitempty"`
	Title     string `json:"title,omitempty"`
	Timestamp string `json:"timestamp"`
}

type Subscriber struct {
	client  paho.Client
	eventCh chan Event
	logger  *slog.Logger
}

func NewSubscriber(logger *slog.Logger) *Subscriber {
	return &Subscriber{
		eventCh: make(chan Event, 256),
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
	if token.Wait() && token.Error() != nil {
		return token.Error()
	}
	return nil
}

func (s *Subscriber) Subscribe(topics []string) error {
	for _, topic := range topics {
		token := s.client.Subscribe(topic, 1, s.onMessage)
		if token.Wait() && token.Error() != nil {
			return token.Error()
		}
		s.logger.Info("mqtt subscribed", "topic", topic)
	}
	return nil
}

func (s *Subscriber) IsConnected() bool {
	if s.client == nil {
		return false
	}
	return s.client.IsConnected()
}

func (s *Subscriber) Close() {
	if s.client != nil && s.client.IsConnected() {
		s.client.Disconnect(250)
	}
}

func (s *Subscriber) onMessage(_ paho.Client, msg paho.Message) {
	event := Event{
		Timestamp: time.Now().UTC().Format(time.RFC3339),
	}

	topic := msg.Topic()
	parts := strings.Split(topic, "/")

	// Parse topic: EdgeVib/{site_id}/...
	if len(parts) >= 2 && parts[0] == "EdgeVib" {
		event.SiteID = parts[1]

		switch {
		case strings.Contains(topic, "/inference/"):
			event.Type = "ai_alert"
			// EdgeVib/{site}/inference/{device_id}/ai/report
			if len(parts) >= 4 {
				event.DeviceID = parts[3]
			}
			parseSeverity(msg.Payload(), &event)

		case strings.Contains(topic, "/llm/"):
			event.Type = "llm_report"
			// EdgeVib/{site}/llm/{device_id}/report
			if len(parts) >= 4 {
				event.DeviceID = parts[3]
			}
			parseLLMTitle(msg.Payload(), &event)

		case strings.Contains(topic, "/status/health"):
			event.Type = "device_status"
			if len(parts) >= 4 {
				event.DeviceID = parts[3]
			}
		}
	}

	select {
	case s.eventCh <- event:
	default:
		s.logger.Warn("mqtt event channel full, dropping event", "topic", topic)
	}
}

// StartBridge forwards MQTT events to the WebSocket hub broadcast channel.
func (s *Subscriber) StartBridge(hubBroadcast chan<- []byte) {
	go func() {
		for event := range s.eventCh {
			data, err := json.Marshal(event)
			if err != nil {
				s.logger.Warn("mqtt event marshal error", "error", err)
				continue
			}
			select {
			case hubBroadcast <- data:
			default:
				s.logger.Warn("ws broadcast channel full, dropping event")
			}
		}
	}()
}

func parseSeverity(payload []byte, event *Event) {
	var data struct {
		Severity string `json:"severity"`
	}
	if err := json.Unmarshal(payload, &data); err == nil {
		event.Severity = data.Severity
	}
}

func parseLLMTitle(payload []byte, event *Event) {
	var data struct {
		Title    string `json:"title"`
		Severity string `json:"severity"`
	}
	if err := json.Unmarshal(payload, &data); err == nil {
		event.Title = data.Title
		event.Severity = data.Severity
	}
}
