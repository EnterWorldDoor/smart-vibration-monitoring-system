package store

import (
	"context"
	"fmt"
	"log/slog"
	"sync"
	"time"

	"github.com/jackc/pgx/v5"
	"github.com/jackc/pgx/v5/pgxpool"

	"edgevib/data-aggregator/internal/model"
)

// BatchWriter buffers sensor messages and flushes them to TimescaleDB in batches.
type BatchWriter struct {
	pool      *pgxpool.Pool
	buffer    []*model.SensorMessage
	mu        sync.Mutex
	maxSize   int
	maxAge    time.Duration
	lastFlush time.Time
	logger    *slog.Logger

	rowsWritten int64
	dbErrors    int64
}

// NewBatchWriter creates a batch writer backed by a pgxpool connection pool.
func NewBatchWriter(pool *pgxpool.Pool, maxSize int, maxAge time.Duration, logger *slog.Logger) *BatchWriter {
	return &BatchWriter{
		pool:      pool,
		buffer:    make([]*model.SensorMessage, 0, maxSize),
		maxSize:   maxSize,
		maxAge:    maxAge,
		lastFlush: time.Now(),
		logger:    logger,
	}
}

// Add appends a message to the batch buffer. Flushes if batch is full.
func (w *BatchWriter) Add(msg *model.SensorMessage) {
	w.mu.Lock()
	w.buffer = append(w.buffer, msg)
	needFlush := len(w.buffer) >= w.maxSize
	w.mu.Unlock()

	if needFlush {
		w.Flush()
	}
}

// Flush writes all buffered messages to TimescaleDB.
func (w *BatchWriter) Flush() (int, error) {
	w.mu.Lock()
	if len(w.buffer) == 0 {
		w.mu.Unlock()
		return 0, nil
	}
	batch := w.buffer
	w.buffer = make([]*model.SensorMessage, 0, w.maxSize)
	w.lastFlush = time.Now()
	w.mu.Unlock()

	n, err := w.insertBatch(batch)
	if err != nil {
		w.mu.Lock()
		w.dbErrors++
		w.mu.Unlock()
		w.logger.Error("db write failed", "rows", len(batch), "error", err)
		return 0, err
	}

	w.mu.Lock()
	w.rowsWritten += int64(n)
	w.mu.Unlock()
	w.logger.Debug("batch flushed", "rows", n, "total", w.rowsWritten)
	return n, nil
}

// insertBatch uses PostgreSQL COPY protocol for efficient bulk insert.
func (w *BatchWriter) insertBatch(batch []*model.SensorMessage) (int, error) {
	ctx, cancel := context.WithTimeout(context.Background(), 10*time.Second)
	defer cancel()

	conn, err := w.pool.Acquire(ctx)
	if err != nil {
		return 0, fmt.Errorf("acquire conn: %w", err)
	}
	defer conn.Release()

	columns := []string{"time", "site_id", "device_type", "device_id", "data_type", "payload", "source_path"}
	inputRows := make([][]interface{}, len(batch))
	for i, msg := range batch {
		inputRows[i] = []interface{}{
			msg.IngestedAt,
			msg.SiteID,
			msg.DeviceType,
			msg.DeviceID,
			msg.DataType,
			msg.Payload,
			"mqtt",
		}
	}

	copied, err := conn.CopyFrom(
		ctx,
		pgx.Identifier{"sensor_data"},
		columns,
		pgx.CopyFromRows(inputRows),
	)
	return int(copied), err
}

// TimerFlush periodically flushes the buffer when maxAge is exceeded.
func (w *BatchWriter) TimerFlush(ctx context.Context) {
	ticker := time.NewTicker(time.Second)
	defer ticker.Stop()
	for {
		select {
		case <-ticker.C:
			w.mu.Lock()
			age := time.Since(w.lastFlush)
			hasData := len(w.buffer) > 0
			w.mu.Unlock()
			if hasData && age >= w.maxAge {
				w.Flush()
			}
		case <-ctx.Done():
			w.Flush()
			return
		}
	}
}

// Stats returns the cumulative write statistics.
func (w *BatchWriter) Stats() (rowsWritten, dbErrors int64) {
	w.mu.Lock()
	defer w.mu.Unlock()
	return w.rowsWritten, w.dbErrors
}

// Close flushes remaining data and closes the connection pool.
func (w *BatchWriter) Close() {
	w.Flush()
	w.pool.Close()
}
