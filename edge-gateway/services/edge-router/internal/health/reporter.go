package health

import (
	"context"
	"encoding/json"
	"log/slog"
	"sync/atomic"
	"time"
)

// Metrics holds atomic counters for health reporting.
type Metrics struct {
	MsgReceived   int64
	AlertsRouted  int64
	AlertsDeduped int64
	DBQueries     int64
	DBErrors      int64
	ParseErrors   int64
	MQTTConnected int32
	DBConnected   int32
}

// Reporter periodically publishes health metrics via MQTT.
type Reporter struct {
	metrics *Metrics
	siteID  string
	logger  *slog.Logger
	startAt time.Time
	publish func(topic string, payload []byte) error
}

// NewReporter creates a new health reporter.
func NewReporter(
	siteID string,
	publish func(topic string, payload []byte) error,
	logger *slog.Logger,
) *Reporter {
	return &Reporter{
		metrics: &Metrics{},
		siteID:  siteID,
		logger:  logger,
		startAt: time.Now(),
		publish: publish,
	}
}

// Metrics returns a pointer to the shared Metrics struct.
func (r *Reporter) Metrics() *Metrics {
	return r.metrics
}

// Run starts the periodic health reporting loop.
func (r *Reporter) Run(ctx context.Context) {
	ticker := time.NewTicker(30 * time.Second)
	defer ticker.Stop()

	for {
		select {
		case <-ticker.C:
			r.report()
		case <-ctx.Done():
			r.logger.Info("health reporter stopping")
			return
		}
	}
}

// report builds and publishes the current health status.
func (r *Reporter) report() {
	report := map[string]interface{}{
		"service":        "edge-router",
		"version":        "1.0.0",
		"site_id":        r.siteID,
		"uptime_seconds": int(time.Since(r.startAt).Seconds()),
		"msg_received":   atomic.LoadInt64(&r.metrics.MsgReceived),
		"alerts_routed":  atomic.LoadInt64(&r.metrics.AlertsRouted),
		"alerts_deduped": atomic.LoadInt64(&r.metrics.AlertsDeduped),
		"db_queries":     atomic.LoadInt64(&r.metrics.DBQueries),
		"db_errors":      atomic.LoadInt64(&r.metrics.DBErrors),
		"parse_errors":   atomic.LoadInt64(&r.metrics.ParseErrors),
		"mqtt_connected": atomic.LoadInt32(&r.metrics.MQTTConnected) == 1,
		"db_connected":   atomic.LoadInt32(&r.metrics.DBConnected) == 1,
		"timestamp":      time.Now().UTC().Format(time.RFC3339),
	}

	payload, err := json.Marshal(report)
	if err != nil {
		r.logger.Error("health report marshal failed", "error", err)
		return
	}

	topic := "EdgeVib/" + r.siteID + "/router/orangepi/status/health"
	if err := r.publish(topic, payload); err != nil {
		r.logger.Warn("health report publish failed", "error", err)
	}
}
