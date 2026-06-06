package router

import (
	"encoding/json"
	"fmt"
	"log/slog"
	"strings"
	"sync/atomic"
	"time"

	"edgevib/edge-router/internal/db"
	"edgevib/edge-router/internal/dedup"
	"edgevib/edge-router/internal/health"
	"edgevib/edge-router/internal/mqtt"
)

// IncomingAlert represents a parsed alert from ESP32 or inference-engine.
type IncomingAlert struct {
	SourceSite   string
	SourceDevice string
	AlertSource  string // "esp32" or "inference"
	Topic        string
	Payload      json.RawMessage
	IngestedAt   time.Time
}

// AlertPayload is the published cross-workshop alert message.
type AlertPayload struct {
	SourceSite     string      `json:"source_site"`
	SourceDevice   string      `json:"source_device"`
	AlertLevel     string      `json:"alert_level"`
	AlertSource    string      `json:"alert_source"`
	AIClass        string      `json:"ai_class"`
	Confidence     float64     `json:"confidence"`
	RMSCurrent     float64     `json:"rms_current"`
	RMSBaseline    float64     `json:"rms_baseline"`
	TemperatureC   float64     `json:"temperature_c"`
	Timestamp      string      `json:"timestamp"`
	ContextSnapshot ContextData `json:"context_snapshot"`
	TriggerReason  string      `json:"trigger_reason,omitempty"`
	AnomalyScore   *float64    `json:"anomaly_score,omitempty"`
	HealthScore    *float64    `json:"health_score,omitempty"`
	Severity       string      `json:"severity,omitempty"`
	Summary        string      `json:"summary,omitempty"`
}

// ContextData holds the TimescaleDB-derived vibration context snapshot.
type ContextData struct {
	RMSTrend    []float64 `json:"rms_trend"`
	FreqDriftHz float64   `json:"freq_drift_hz"`
}

// Router processes incoming alerts and routes them to target sites.
type Router struct {
	sites   []string
	cache   *dedup.Cache
	querier *db.Querier
	publish func(topic string, payload []byte) error
	logger  *slog.Logger
}

// New creates a new Router.
func New(
	sites []string,
	cache *dedup.Cache,
	querier *db.Querier,
	publish func(topic string, payload []byte) error,
	logger *slog.Logger,
) *Router {
	return &Router{
		sites:   sites,
		cache:   cache,
		querier: querier,
		publish: publish,
		logger:  logger,
	}
}

// Handle processes a raw MQTT message through the routing pipeline.
func (r *Router) Handle(rawMsg *mqtt.RawMessage, metrics *health.Metrics) {
	atomic.AddInt64(&metrics.MsgReceived, 1)

	// Step 1: Parse topic to extract source site, device, and alert source
	alert, err := r.parseIncoming(rawMsg.Topic, rawMsg.Payload)
	if err != nil {
		atomic.AddInt64(&metrics.ParseErrors, 1)
		r.logger.Warn("parse failed", "topic", rawMsg.Topic, "error", err)
		return
	}

	// Step 2: Filter — only route actual anomalies
	if !r.shouldAlert(alert) {
		return
	}

	// Step 3: Dedup check
	dedupKey := dedup.MakeKey(alert.SourceSite, alert.SourceDevice, alert.AlertSource)
	if !r.cache.CheckAndAdd(dedupKey) {
		atomic.AddInt64(&metrics.AlertsDeduped, 1)
		r.logger.Debug("deduped alert", "key", dedupKey)
		return
	}

	// Step 4: Extract alert fields from JSON
	esp32Data, infData, err := r.extractAlertData(alert)
	if err != nil {
		atomic.AddInt64(&metrics.ParseErrors, 1)
		r.logger.Warn("payload unmarshal failed", "topic", alert.Topic, "error", err)
		return
	}

	// Step 5: Query TimescaleDB for context snapshot
	atomic.AddInt64(&metrics.DBQueries, 1)
	rows, err := r.querier.QueryVibrationView(
		alert.SourceSite, alert.SourceDevice,
		5*time.Minute, 30,
	)
	if err != nil {
		atomic.AddInt64(&metrics.DBErrors, 1)
		r.logger.Warn("db query failed, routing without context",
			"site", alert.SourceSite, "device", alert.SourceDevice, "error", err)
		rows = nil
	}

	// Step 6: Build output payload
	outPayload := r.buildPayload(alert, esp32Data, infData, rows)

	// Step 7: Marshal and publish to target sites
	payloadJSON, err := json.Marshal(outPayload)
	if err != nil {
		r.logger.Error("marshal output payload failed", "error", err)
		return
	}

	targets := r.targetSites(alert.SourceSite)
	for _, targetSite := range targets {
		topic := fmt.Sprintf("EdgeVib/%s/router/%s/alert", targetSite, alert.SourceSite)
		if err := r.publish(topic, payloadJSON); err != nil {
			r.logger.Warn("publish failed", "topic", topic, "error", err)
		} else {
			r.logger.Info("alert routed",
				"source_site", alert.SourceSite,
				"target_site", targetSite,
				"source_device", alert.SourceDevice,
				"alert_level", outPayload.AlertLevel,
				"alert_source", alert.AlertSource,
			)
		}
	}

	atomic.AddInt64(&metrics.AlertsRouted, 1)
}

// parseIncoming extracts alert metadata from the MQTT topic.
func (r *Router) parseIncoming(topic string, payload []byte) (*IncomingAlert, error) {
	parts := strings.Split(topic, "/")
	if len(parts) < 5 || parts[0] != "EdgeVib" {
		return nil, fmt.Errorf("invalid topic: %s", topic)
	}

	siteID := parts[1]
	category := parts[2] // "motor" or "inference"
	deviceID := parts[3] // e.g. "motor01"

	var alertSource string
	switch category {
	case "motor":
		alertSource = "esp32"
	case "inference":
		alertSource = "inference"
	default:
		return nil, fmt.Errorf("unsupported category: %s", category)
	}

	return &IncomingAlert{
		SourceSite:   siteID,
		SourceDevice: deviceID,
		AlertSource:  alertSource,
		Topic:        topic,
		Payload:      payload,
		IngestedAt:   time.Now(),
	}, nil
}

// esp32SensorData represents the relevant fields from an ESP32 sensor message.
type esp32SensorData struct {
	Data struct {
		AI struct {
			ClassName  string  `json:"class_name"`
			Confidence float64 `json:"confidence"`
		} `json:"ai"`
		Vibration struct {
			OverallRMS float64 `json:"overall_rms"`
		} `json:"vibration"`
		Environment struct {
			TemperatureC float64 `json:"temperature_c"`
		} `json:"environment"`
	} `json:"data"`
}

// inferenceReportData represents the relevant fields from an inference-engine report.
type inferenceReportData struct {
	AnomalyDetected bool     `json:"anomaly_detected"`
	HealthScore     *float64 `json:"health_score"`
	AnomalyScore    *float64 `json:"anomaly_score"`
	Severity        string   `json:"severity"`
	TriggerReason   string   `json:"trigger_reason"`
	Summary         string   `json:"summary"`
}

// shouldAlert checks if the incoming message represents an actual anomaly.
func (r *Router) shouldAlert(alert *IncomingAlert) bool {
	if alert.AlertSource == "esp32" {
		var data esp32SensorData
		if err := json.Unmarshal(alert.Payload, &data); err != nil {
			return false
		}
		className := data.Data.AI.ClassName
		if className == "" || className == "normal" {
			r.logger.Debug("skipping normal sensor message",
				"site", alert.SourceSite, "device", alert.SourceDevice, "class", className)
			return false
		}
		return true
	}

	// Inference-engine: every message IS an anomaly (pre-filtered by inference-engine)
	return true
}

// extractAlertData parses the JSON payload based on alert source.
func (r *Router) extractAlertData(alert *IncomingAlert) (*esp32SensorData, *inferenceReportData, error) {
	var esp32 *esp32SensorData
	var inf *inferenceReportData

	switch alert.AlertSource {
	case "esp32":
		var data esp32SensorData
		if err := json.Unmarshal(alert.Payload, &data); err != nil {
			return nil, nil, fmt.Errorf("unmarshal esp32: %w", err)
		}
		esp32 = &data
	case "inference":
		var data inferenceReportData
		if err := json.Unmarshal(alert.Payload, &data); err != nil {
			return nil, nil, fmt.Errorf("unmarshal inference: %w", err)
		}
		inf = &data
	}

	return esp32, inf, nil
}

// buildPayload constructs the output AlertPayload.
func (r *Router) buildPayload(
	alert *IncomingAlert,
	esp32 *esp32SensorData,
	inf *inferenceReportData,
	rows []db.VibrationRow,
) AlertPayload {
	// Determine alert level
	alertLevel := "info"
	aiClass := ""
	confidence := 0.0
	rmsCurrent := 0.0
	temperatureC := 0.0

	if esp32 != nil {
		aiClass = esp32.Data.AI.ClassName
		confidence = esp32.Data.AI.Confidence
		rmsCurrent = esp32.Data.Vibration.OverallRMS
		temperatureC = esp32.Data.Environment.TemperatureC
		if aiClass != "" && aiClass != "normal" {
			alertLevel = "warning"
		}
	}

	var triggerReason string
	var anomalyScore *float64
	var healthScore *float64
	var severity string
	var summary string

	if inf != nil {
		severity = inf.Severity
		summary = inf.Summary
		triggerReason = inf.TriggerReason
		anomalyScore = inf.AnomalyScore
		healthScore = inf.HealthScore
		if severity != "" {
			alertLevel = strings.ToLower(severity) // "warning" or "critical"
		}
	}

	// Build RMS trend from DB rows (reverse to chronological order)
	rmsTrend := make([]float64, 0, len(rows))
	if len(rows) > 0 {
		for i := len(rows) - 1; i >= 0; i-- {
			if rows[i].OverallRMS != nil {
				rmsTrend = append(rmsTrend, *rows[i].OverallRMS)
			}
		}
	}

	// Calculate frequency drift
	freqDriftHz := 0.0
	if len(rows) >= 2 {
		firstFreq := 0.0
		lastFreq := 0.0
		if rows[0].PeakFrequencyHz != nil {
			firstFreq = *rows[0].PeakFrequencyHz
		}
		if rows[len(rows)-1].PeakFrequencyHz != nil {
			lastFreq = *rows[len(rows)-1].PeakFrequencyHz
		}
		freqDriftHz = firstFreq - lastFreq
	}

	// Baseline RMS (earliest point in trend)
	rmsBaseline := 0.0
	if len(rmsTrend) > 0 {
		rmsBaseline = rmsTrend[0]
	}

	// Update rmsCurrent from DB if not from ESP32
	if esp32 == nil && len(rmsTrend) > 0 {
		rmsCurrent = rmsTrend[len(rmsTrend)-1]
	}

	return AlertPayload{
		SourceSite:   alert.SourceSite,
		SourceDevice: alert.SourceDevice,
		AlertLevel:   alertLevel,
		AlertSource:  alert.AlertSource,
		AIClass:      aiClass,
		Confidence:   confidence,
		RMSCurrent:   rmsCurrent,
		RMSBaseline:  rmsBaseline,
		TemperatureC: temperatureC,
		Timestamp:    time.Now().UTC().Format(time.RFC3339),
		ContextSnapshot: ContextData{
			RMSTrend:    rmsTrend,
			FreqDriftHz: freqDriftHz,
		},
		TriggerReason: triggerReason,
		AnomalyScore:  anomalyScore,
		HealthScore:   healthScore,
		Severity:      severity,
		Summary:       summary,
	}
}

// targetSites returns all sites except the source site.
func (r *Router) targetSites(sourceSite string) []string {
	targets := make([]string, 0, len(r.sites)-1)
	for _, s := range r.sites {
		if s != sourceSite {
			targets = append(targets, s)
		}
	}
	return targets
}
