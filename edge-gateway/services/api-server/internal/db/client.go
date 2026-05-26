package db

import (
	"context"
	"fmt"
	"strings"
	"time"

	"github.com/jackc/pgx/v5"
	"github.com/jackc/pgx/v5/pgxpool"
)

type Client struct {
	pool *pgxpool.Pool
}

func NewClient(pool *pgxpool.Pool) *Client {
	return &Client{pool: pool}
}

// --- Data types ---

type DeviceInfo struct {
	DeviceType string `json:"device_type"`
	DeviceID   string `json:"device_id"`
}

type DeviceStatus struct {
	LastSeen         time.Time `json:"last_seen"`
	SiteID           string    `json:"site_id"`
	DeviceType       string    `json:"device_type"`
	DeviceID         string    `json:"device_id"`
	ServiceState     *string   `json:"service_state,omitempty"`
	DataQuality      *int      `json:"data_quality,omitempty"`
	LastRMS          *float64  `json:"last_rms,omitempty"`
	LastTemperature  *float64  `json:"last_temperature,omitempty"`
	LastAIClass      *string   `json:"last_ai_class,omitempty"`
	LastAIConfidence *float64  `json:"last_ai_confidence,omitempty"`
	SamplesAnalyzed  *int      `json:"samples_analyzed,omitempty"`
	TotalAnalyses    *int      `json:"total_analyses,omitempty"`
	ESP32DevID       *int      `json:"esp32_dev_id,omitempty"`
}

type VibrationRow struct {
	Time            time.Time              `json:"time"`
	SiteID          string                 `json:"site_id"`
	DeviceType      string                 `json:"device_type"`
	DeviceID        string                 `json:"device_id"`
	RMSX            *float64               `json:"rms_x,omitempty"`
	RMSY            *float64               `json:"rms_y,omitempty"`
	RMSZ            *float64               `json:"rms_z,omitempty"`
	OverallRMS      *float64               `json:"overall_rms,omitempty"`
	PeakFrequencyHz *float64               `json:"peak_frequency_hz,omitempty"`
	PeakAmplitudeG  *float64               `json:"peak_amplitude_g,omitempty"`
	FFTPeaks        interface{} `json:"fft_peaks,omitempty"`
}

type EnvironmentRow struct {
	Time               time.Time `json:"time"`
	SiteID             string    `json:"site_id"`
	DeviceType         string    `json:"device_type"`
	DeviceID           string    `json:"device_id"`
	TemperatureC       *float64  `json:"temperature_c,omitempty"`
	HumidityRH         *float64  `json:"humidity_rh,omitempty"`
	CompensationActive *bool     `json:"compensation_active,omitempty"`
}

type AIReport struct {
	Time            time.Time              `json:"time"`
	SiteID          string                 `json:"site_id"`
	ReportType      string                 `json:"report_type"`
	DeviceID        *string                `json:"device_id,omitempty"`
	Severity        *string                `json:"severity,omitempty"`
	Payload         map[string]interface{} `json:"payload"`
	ModelName       *string                `json:"model_name,omitempty"`
	ModelVersion    *string                `json:"model_version,omitempty"`
	AnomalyScore    *float64               `json:"anomaly_score,omitempty"`
	HealthScore     *float64               `json:"health_score,omitempty"`
	InferenceTimeMS *float64               `json:"inference_time_ms,omitempty"`
	Details         map[string]interface{} `json:"details,omitempty"`
	CreatedAt       time.Time              `json:"created_at"`
}

type LLMReport struct {
	Time             time.Time `json:"time"`
	SiteID           string    `json:"site_id"`
	DeviceID         *string   `json:"device_id,omitempty"`
	ReportType       string    `json:"report_type"`
	Severity         *string   `json:"severity,omitempty"`
	Title            *string   `json:"title,omitempty"`
	Summary          *string   `json:"summary,omitempty"`
	Analysis         *string   `json:"analysis,omitempty"`
	Advice           *string   `json:"advice,omitempty"`
	RawOutput        *string   `json:"raw_output,omitempty"`
	ModelName        *string   `json:"model_name,omitempty"`
	ModelVersion     *string   `json:"model_version,omitempty"`
	TokensUsed       *int      `json:"tokens_used,omitempty"`
	GenerationTimeMS *float64  `json:"generation_time_ms,omitempty"`
	TriggerReason    *string   `json:"trigger_reason,omitempty"`
	CreatedAt        time.Time `json:"created_at"`
}

// --- Query methods ---

func (c *Client) Ping(ctx context.Context) error {
	return c.pool.Ping(ctx)
}

func (c *Client) ListSites(ctx context.Context) ([]string, error) {
	rows, err := c.pool.Query(ctx,
		`SELECT DISTINCT site_id FROM sensor_data ORDER BY site_id`)
	if err != nil {
		return nil, fmt.Errorf("list sites: %w", err)
	}
	defer rows.Close()

	var sites []string
	for rows.Next() {
		var s string
		if err := rows.Scan(&s); err != nil {
			return nil, fmt.Errorf("scan site: %w", err)
		}
		sites = append(sites, s)
	}
	return sites, rows.Err()
}

func (c *Client) GetSiteOverview(ctx context.Context, siteID string) ([]DeviceStatus, error) {
	rows, err := c.pool.Query(ctx,
		`SELECT last_seen, site_id, device_type, device_id,
		        service_state, data_quality, last_rms, last_temperature,
		        last_ai_class, last_ai_confidence, samples_analyzed,
		        total_analyses, esp32_dev_id
		 FROM device_status_view
		 WHERE site_id = $1
		 ORDER BY device_type, device_id`, siteID)
	if err != nil {
		return nil, fmt.Errorf("get site overview: %w", err)
	}
	defer rows.Close()

	var devices []DeviceStatus
	for rows.Next() {
		var d DeviceStatus
		if err := rows.Scan(&d.LastSeen, &d.SiteID, &d.DeviceType, &d.DeviceID,
			&d.ServiceState, &d.DataQuality, &d.LastRMS, &d.LastTemperature,
			&d.LastAIClass, &d.LastAIConfidence, &d.SamplesAnalyzed,
			&d.TotalAnalyses, &d.ESP32DevID); err != nil {
			return nil, fmt.Errorf("scan device status: %w", err)
		}
		devices = append(devices, d)
	}
	return devices, rows.Err()
}

func (c *Client) ListDevices(ctx context.Context, siteID string) ([]DeviceInfo, error) {
	rows, err := c.pool.Query(ctx,
		`SELECT DISTINCT device_type, device_id
		 FROM sensor_data
		 WHERE site_id = $1 AND source_path = 'mqtt'
		 ORDER BY device_type, device_id`, siteID)
	if err != nil {
		return nil, fmt.Errorf("list devices: %w", err)
	}
	defer rows.Close()

	var devices []DeviceInfo
	for rows.Next() {
		var d DeviceInfo
		if err := rows.Scan(&d.DeviceType, &d.DeviceID); err != nil {
			return nil, fmt.Errorf("scan device info: %w", err)
		}
		devices = append(devices, d)
	}
	return devices, rows.Err()
}

func (c *Client) GetDeviceStatus(ctx context.Context, siteID, deviceType, deviceID string) (*DeviceStatus, error) {
	var d DeviceStatus
	err := c.pool.QueryRow(ctx,
		`SELECT last_seen, site_id, device_type, device_id,
		        service_state, data_quality, last_rms, last_temperature,
		        last_ai_class, last_ai_confidence, samples_analyzed,
		        total_analyses, esp32_dev_id
		 FROM device_status_view
		 WHERE site_id = $1 AND device_type = $2 AND device_id = $3`,
		siteID, deviceType, deviceID,
	).Scan(&d.LastSeen, &d.SiteID, &d.DeviceType, &d.DeviceID,
		&d.ServiceState, &d.DataQuality, &d.LastRMS, &d.LastTemperature,
		&d.LastAIClass, &d.LastAIConfidence, &d.SamplesAnalyzed,
		&d.TotalAnalyses, &d.ESP32DevID)
	if err == pgx.ErrNoRows {
		return nil, nil
	}
	if err != nil {
		return nil, fmt.Errorf("get device status: %w", err)
	}
	return &d, nil
}

func (c *Client) QuerySensorData(ctx context.Context, siteID, deviceType, deviceID string, from, to time.Time, limit, offset int) ([]VibrationRow, error) {
	rows, err := c.pool.Query(ctx,
		`SELECT time, site_id, device_type, device_id,
		        rms_x, rms_y, rms_z, overall_rms,
		        peak_frequency_hz, peak_amplitude_g, fft_peaks
		 FROM vibration_view
		 WHERE site_id = $1 AND device_type = $2 AND device_id = $3
		   AND time >= $4 AND time <= $5
		 ORDER BY time DESC
		 LIMIT $6 OFFSET $7`,
		siteID, deviceType, deviceID, from, to, limit, offset)
	if err != nil {
		return nil, fmt.Errorf("query sensor data: %w", err)
	}
	defer rows.Close()

	var results []VibrationRow
	for rows.Next() {
		var r VibrationRow
		if err := rows.Scan(&r.Time, &r.SiteID, &r.DeviceType, &r.DeviceID,
			&r.RMSX, &r.RMSY, &r.RMSZ, &r.OverallRMS,
			&r.PeakFrequencyHz, &r.PeakAmplitudeG, &r.FFTPeaks); err != nil {
			return nil, fmt.Errorf("scan vibration row: %w", err)
		}
		results = append(results, r)
	}
	return results, rows.Err()
}

func (c *Client) QueryEnvironment(ctx context.Context, siteID, deviceType, deviceID string, from, to time.Time, limit, offset int) ([]EnvironmentRow, error) {
	rows, err := c.pool.Query(ctx,
		`SELECT time, site_id, device_type, device_id,
		        temperature_c, humidity_rh, compensation_active
		 FROM environment_view
		 WHERE site_id = $1 AND device_type = $2 AND device_id = $3
		   AND time >= $4 AND time <= $5
		 ORDER BY time DESC
		 LIMIT $6 OFFSET $7`,
		siteID, deviceType, deviceID, from, to, limit, offset)
	if err != nil {
		return nil, fmt.Errorf("query environment: %w", err)
	}
	defer rows.Close()

	var results []EnvironmentRow
	for rows.Next() {
		var r EnvironmentRow
		if err := rows.Scan(&r.Time, &r.SiteID, &r.DeviceType, &r.DeviceID,
			&r.TemperatureC, &r.HumidityRH, &r.CompensationActive); err != nil {
			return nil, fmt.Errorf("scan environment row: %w", err)
		}
		results = append(results, r)
	}
	return results, rows.Err()
}

func (c *Client) QueryAIReports(ctx context.Context, siteID, deviceID string, from, to time.Time, severity string, limit int) ([]AIReport, error) {
	var (
		rows pgx.Rows
		err  error
	)

	if severity != "" {
		rows, err = c.pool.Query(ctx,
			`SELECT time, site_id, report_type, device_id, severity,
			        payload, model_name, model_version,
			        anomaly_score, health_score, inference_time_ms,
			        details, created_at
			 FROM ai_reports
			 WHERE site_id = $1 AND device_id = $2
			   AND time >= $3 AND time <= $4
			   AND severity = $5
			 ORDER BY time DESC
			 LIMIT $6`,
			siteID, deviceID, from, to, severity, limit)
	} else {
		rows, err = c.pool.Query(ctx,
			`SELECT time, site_id, report_type, device_id, severity,
			        payload, model_name, model_version,
			        anomaly_score, health_score, inference_time_ms,
			        details, created_at
			 FROM ai_reports
			 WHERE site_id = $1 AND device_id = $2
			   AND time >= $3 AND time <= $4
			 ORDER BY time DESC
			 LIMIT $5`,
			siteID, deviceID, from, to, limit)
	}
	if err != nil {
		return nil, fmt.Errorf("query ai reports: %w", err)
	}
	defer rows.Close()

	var reports []AIReport
	for rows.Next() {
		var r AIReport
		if err := rows.Scan(&r.Time, &r.SiteID, &r.ReportType, &r.DeviceID, &r.Severity,
			&r.Payload, &r.ModelName, &r.ModelVersion,
			&r.AnomalyScore, &r.HealthScore, &r.InferenceTimeMS,
			&r.Details, &r.CreatedAt); err != nil {
			return nil, fmt.Errorf("scan ai report: %w", err)
		}
		reports = append(reports, r)
	}
	return reports, rows.Err()
}

func (c *Client) QueryLLMReports(ctx context.Context, siteID, deviceID string, from, to time.Time, severity string, limit int) ([]LLMReport, error) {
	var (
		rows pgx.Rows
		err  error
	)

	columns := `time, site_id, device_id, report_type, severity,
	            title, summary, analysis, advice, raw_output,
	            model_name, model_version, tokens_used,
	            generation_time_ms, trigger_reason, created_at`

	if severity != "" {
		rows, err = c.pool.Query(ctx,
			`SELECT `+columns+`
			 FROM llm_reports
			 WHERE site_id = $1 AND device_id = $2
			   AND time >= $3 AND time <= $4
			   AND severity = $5
			 ORDER BY time DESC
			 LIMIT $6`,
			siteID, deviceID, from, to, severity, limit)
	} else {
		rows, err = c.pool.Query(ctx,
			`SELECT `+columns+`
			 FROM llm_reports
			 WHERE site_id = $1 AND device_id = $2
			   AND time >= $3 AND time <= $4
			 ORDER BY time DESC
			 LIMIT $5`,
			siteID, deviceID, from, to, limit)
	}
	if err != nil {
		return nil, fmt.Errorf("query llm reports: %w", err)
	}
	defer rows.Close()

	var reports []LLMReport
	for rows.Next() {
		var r LLMReport
		if err := rows.Scan(&r.Time, &r.SiteID, &r.DeviceID, &r.ReportType, &r.Severity,
			&r.Title, &r.Summary, &r.Analysis, &r.Advice, &r.RawOutput,
			&r.ModelName, &r.ModelVersion, &r.TokensUsed,
			&r.GenerationTimeMS, &r.TriggerReason, &r.CreatedAt); err != nil {
			return nil, fmt.Errorf("scan llm report: %w", err)
		}
		reports = append(reports, r)
	}
	return reports, rows.Err()
}

func BuildSeverityFilter(severity string) string {
	severity = strings.ToUpper(strings.TrimSpace(severity))
	switch severity {
	case "NORMAL", "WARNING", "CRITICAL":
		return severity
	default:
		return ""
	}
}
