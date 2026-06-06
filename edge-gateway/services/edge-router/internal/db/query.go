package db

import (
	"context"
	"fmt"
	"log/slog"
	"time"

	"github.com/jackc/pgx/v5/pgxpool"
)

// VibrationRow holds a single row from the vibration_view.
type VibrationRow struct {
	Time            time.Time
	SiteID          string
	DeviceType      string
	DeviceID        string
	RMSX            *float64
	RMSY            *float64
	RMSZ            *float64
	OverallRMS      *float64
	PeakFrequencyHz *float64
	PeakAmplitudeG  *float64
	FFTPeaks        interface{}
}

// Querier performs TimescaleDB read queries.
type Querier struct {
	pool   *pgxpool.Pool
	logger *slog.Logger
}

// NewQuerier creates a new database querier.
func NewQuerier(pool *pgxpool.Pool, logger *slog.Logger) *Querier {
	return &Querier{pool: pool, logger: logger}
}

// QueryVibrationView queries the vibration_view for recent context data.
func (q *Querier) QueryVibrationView(siteID, deviceID string, window time.Duration, limit int) ([]VibrationRow, error) {
	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()

	intervalStr := fmt.Sprintf("%d minutes", int(window.Minutes()))
	if int(window.Minutes()) == 0 {
		intervalStr = fmt.Sprintf("%d seconds", int(window.Seconds()))
	}

	rows, err := q.pool.Query(ctx,
		`SELECT time, site_id, device_type, device_id,
		        rms_x, rms_y, rms_z, overall_rms,
		        peak_frequency_hz, peak_amplitude_g, fft_peaks
		 FROM vibration_view
		 WHERE site_id = $1 AND device_id = $2
		   AND time > NOW() - $3::interval
		 ORDER BY time DESC
		 LIMIT $4`,
		siteID, deviceID, intervalStr, limit,
	)
	if err != nil {
		return nil, fmt.Errorf("query vibration_view: %w", err)
	}
	defer rows.Close()

	var results []VibrationRow
	for rows.Next() {
		var r VibrationRow
		if err := rows.Scan(
			&r.Time, &r.SiteID, &r.DeviceType, &r.DeviceID,
			&r.RMSX, &r.RMSY, &r.RMSZ, &r.OverallRMS,
			&r.PeakFrequencyHz, &r.PeakAmplitudeG, &r.FFTPeaks,
		); err != nil {
			return nil, fmt.Errorf("scan vibration_view row: %w", err)
		}
		results = append(results, r)
	}

	if err := rows.Err(); err != nil {
		return nil, fmt.Errorf("iterate vibration_view rows: %w", err)
	}

	return results, nil
}
