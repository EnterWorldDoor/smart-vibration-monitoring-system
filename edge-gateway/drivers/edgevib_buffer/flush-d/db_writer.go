/*
 * db_writer.go — TimescaleDB batch writer for edgevib-flush-d
 *
 * Uses pgxpool + CopyFrom for batch INSERT into sensor_data.
 */

package main

import (
	"context"
	"fmt"
	"log/slog"
	"time"

	"github.com/jackc/pgx/v5"
	"github.com/jackc/pgx/v5/pgxpool"
)

/* ---- Sensor row matching sensor_data schema ---- */

type SensorRow struct {
	Time       time.Time
	SiteID     string
	DeviceType string
	DeviceID   string
	DataType   string
	Payload    []byte
	SourcePath string
}

/* ---- DB Writer ---- */

type DBWriter struct {
	pool *pgxpool.Pool
}

func NewDBWriter(ctx context.Context, dsn string) (*DBWriter, error) {
	pool, err := pgxpool.New(ctx, dsn)
	if err != nil {
		return nil, fmt.Errorf("pgxpool: %w", err)
	}

	if err := pool.Ping(ctx); err != nil {
		pool.Close()
		return nil, fmt.Errorf("ping: %w", err)
	}

	return &DBWriter{pool: pool}, nil
}

func (db *DBWriter) Close() {
	db.pool.Close()
}

func flushToDB(db *DBWriter, rows []*SensorRow, stats *DaemonStats,
	logger *slog.Logger) {

	if len(rows) == 0 {
		return
	}

	ctx, cancel := context.WithTimeout(context.Background(), 10*time.Second)
	defer cancel()

	// Use CopyFrom for efficient batch insert
	columns := []string{"time", "site_id", "device_type", "device_id",
		"data_type", "payload", "source_path"}

	inputRows := make([][]interface{}, len(rows))
	for i, row := range rows {
		inputRows[i] = []interface{}{
			row.Time,
			row.SiteID,
			row.DeviceType,
			row.DeviceID,
			row.DataType,
			row.Payload,
			"mqtt", // source_path
		}
	}

	conn, err := db.pool.Acquire(ctx)
	if err != nil {
		stats.DBErrors++
		logger.Warn("db acquire failed", "err", err)
		return
	}
	defer conn.Release()

	_, err = conn.CopyFrom(
		ctx,
		pgx.Identifier{"sensor_data"},
		columns,
		pgx.CopyFromRows(inputRows),
	)
	if err != nil {
		stats.DBErrors++
		logger.Warn("db CopyFrom failed", "err", err, "rows", len(rows))
		return
	}

	stats.Flushes++
	stats.RowsFlushed += int64(len(rows))
	stats.LastFlushTime = time.Now()

	logger.Debug("flush complete", "rows", len(rows), "total", stats.RowsFlushed)
}
