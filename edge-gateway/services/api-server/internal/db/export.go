package db

import (
	"context"
	"fmt"
	"time"

	"github.com/jackc/pgx/v5"
)

// ExportParams holds optional filters for the data export query.
type ExportParams struct {
	Sites   []string
	Devices []string
	From    time.Time
	To      time.Time
}

// ExportRow represents a single row of exported training data.
type ExportRow struct {
	Time         time.Time
	SiteID       string
	DeviceType   string
	DeviceID     string
	RMSX         *float64
	RMSY         *float64
	RMSZ         *float64
	OverallRMS   *float64
	PeakFreq     *float64
	PeakAmp      *float64
	TemperatureC *float64
	HumidityRH   *float64
}

// QueryExportData retrieves training data from TimescaleDB for the given time range.
// Sites and Devices slices may be empty (meaning all).
func (c *Client) QueryExportData(ctx context.Context, params ExportParams) ([]ExportRow, error) {
	sql := `SELECT v.time, v.site_id, v.device_type, v.device_id,
		v.rms_x, v.rms_y, v.rms_z, v.overall_rms,
		v.peak_frequency_hz AS peak_freq,
		v.peak_amplitude_g AS peak_amp,
		env.temperature_c, env.humidity_rh
	FROM vibration_view v
	LEFT JOIN LATERAL (
		SELECT e.temperature_c, e.humidity_rh
		FROM environment_view e
		WHERE e.site_id = v.site_id
		  AND e.device_type = v.device_type
		  AND e.device_id = v.device_id
		  AND e.time BETWEEN v.time - INTERVAL '30 seconds' AND v.time + INTERVAL '30 seconds'
		ORDER BY ABS(EXTRACT(EPOCH FROM (e.time - v.time)))
		LIMIT 1
	) env ON true
	WHERE v.time >= $1 AND v.time <= $2`

	args := []interface{}{params.From, params.To}
	argIdx := 3

	// Optional site filter
	if len(params.Sites) > 0 {
		sql += fmt.Sprintf(" AND v.site_id = ANY($%d)", argIdx)
		args = append(args, params.Sites)
		argIdx++
	}

	// Optional device filter
	if len(params.Devices) > 0 {
		sql += fmt.Sprintf(" AND v.device_id = ANY($%d)", argIdx)
		args = append(args, params.Devices)
		argIdx++
	}

	sql += " ORDER BY v.time ASC"

	rows, err := c.pool.Query(ctx, sql, args...)
	if err != nil {
		return nil, fmt.Errorf("query export data: %w", err)
	}
	defer rows.Close()

	var results []ExportRow
	for rows.Next() {
		var r ExportRow
		if err := rows.Scan(
			&r.Time, &r.SiteID, &r.DeviceType, &r.DeviceID,
			&r.RMSX, &r.RMSY, &r.RMSZ, &r.OverallRMS,
			&r.PeakFreq, &r.PeakAmp,
			&r.TemperatureC, &r.HumidityRH,
		); err != nil {
			return nil, fmt.Errorf("scan export row: %w", err)
		}
		results = append(results, r)
	}
	if err := rows.Err(); err != nil {
		return nil, fmt.Errorf("export rows iteration: %w", err)
	}

	// Return empty slice instead of nil for JSON serialization
	if results == nil {
		results = []ExportRow{}
	}
	return results, nil
}

// QueryExportDataCount returns the count of rows matching the export params.
func (c *Client) QueryExportDataCount(ctx context.Context, params ExportParams) (int64, error) {
	sql := `SELECT COUNT(*) FROM vibration_view v
	WHERE v.time >= $1 AND v.time <= $2`

	args := []interface{}{params.From, params.To}
	argIdx := 3

	if len(params.Sites) > 0 {
		sql += fmt.Sprintf(" AND v.site_id = ANY($%d)", argIdx)
		args = append(args, params.Sites)
		argIdx++
	}
	if len(params.Devices) > 0 {
		sql += fmt.Sprintf(" AND v.device_id = ANY($%d)", argIdx)
		args = append(args, params.Devices)
		argIdx++
	}

	var count int64
	err := c.pool.QueryRow(ctx, sql, args...).Scan(&count)
	if err != nil {
		if err == pgx.ErrNoRows {
			return 0, nil
		}
		return 0, fmt.Errorf("count export data: %w", err)
	}
	return count, nil
}
