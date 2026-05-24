package health

import (
	"context"
	"encoding/json"
	"fmt"
	"log/slog"
	"sync/atomic"
	"time"
)

// Metrics holds atomic counters for aggregator health reporting.
type Metrics struct {
	MsgReceived    int64
	MsgDeduped     int64
	ParseErrors    int64
	BatchesFlushed int64
	RowsWritten    int64
	DBErrors       int64
	MQTTConnected  int32
	DBConnected    int32
}

// Reporter periodically publishes aggregator health metrics via MQTT.
type Reporter struct {
	metrics *Metrics
	siteID  string
	logger  *slog.Logger
	startAt time.Time
	publish func(topic string, payload []byte) error
}

// NewReporter creates a health reporter.
func NewReporter(siteID string, publish func(string, []byte) error, logger *slog.Logger) *Reporter {
	return &Reporter{
		metrics: &Metrics{},
		siteID:  siteID,
		logger:  logger,
		startAt: time.Now(),
		publish: publish,
	}
}

// Metrics returns the underlying Metrics struct for atomic counter access.
func (r *Reporter) Metrics() *Metrics {
	return r.metrics
}

// Run starts the periodic health report loop (every 30s).
func (r *Reporter) Run(ctx context.Context) {
	ticker := time.NewTicker(30 * time.Second)
	defer ticker.Stop()

	for {
		select {
		case <-ticker.C:
			r.report()
		case <-ctx.Done():
			return
		}
	}
}

func (r *Reporter) report() {
	topic := fmt.Sprintf("EdgeVib/%s/aggregator/orangepi/status/health", r.siteID)

	report := map[string]interface{}{
		"service":         "data-aggregator",
		"version":         "1.0.0",
		"site_id":         r.siteID,
		"uptime_seconds":  int64(time.Since(r.startAt).Seconds()),
		"mqtt_connected":  atomic.LoadInt32(&r.metrics.MQTTConnected) == 1,
		"db_connected":    atomic.LoadInt32(&r.metrics.DBConnected) == 1,
		"msg_received":    atomic.LoadInt64(&r.metrics.MsgReceived),
		"msg_deduped":     atomic.LoadInt64(&r.metrics.MsgDeduped),
		"parse_errors":    atomic.LoadInt64(&r.metrics.ParseErrors),
		"batches_flushed": atomic.LoadInt64(&r.metrics.BatchesFlushed),
		"rows_written":    atomic.LoadInt64(&r.metrics.RowsWritten),
		"db_errors":       atomic.LoadInt64(&r.metrics.DBErrors),
		"timestamp":       time.Now().UTC().Format(time.RFC3339),
	}

	payload, err := json.Marshal(report)
	if err != nil {
		r.logger.Error("health report marshal failed", "error", err)
		return
	}

	if err := r.publish(topic, payload); err != nil {
		r.logger.Warn("health report publish failed", "error", err)
	}
}
