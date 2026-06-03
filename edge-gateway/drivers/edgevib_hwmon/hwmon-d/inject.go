package main

import (
	"context"
	"encoding/binary"
	"log/slog"
	"os"
	"time"

	"github.com/jackc/pgx/v5/pgxpool"
)

const motorInjectionSize = 20 // 5 × int32

// MotorInjection is the binary struct written to /dev/edgevib-hwmon-inject
type MotorInjection struct {
	MotorID int32
	TempMC  int32
	CurrMA  int32
	VoltMV  int32
	PowerMW int32  // hwmon power unit is microwatt (µW)
}

// Marshal serializes to 20-byte little-endian binary
func (m *MotorInjection) Marshal() []byte {
	buf := make([]byte, motorInjectionSize)
	binary.LittleEndian.PutUint32(buf[0:4], uint32(m.MotorID))
	binary.LittleEndian.PutUint32(buf[4:8], uint32(m.TempMC))
	binary.LittleEndian.PutUint32(buf[8:12], uint32(m.CurrMA))
	binary.LittleEndian.PutUint32(buf[12:16], uint32(m.VoltMV))
	binary.LittleEndian.PutUint32(buf[16:20], uint32(m.PowerMW))
	return buf
}

// InjectionStats tracks daemon health metrics
type InjectionStats struct {
	Injections   int64
	DBErrors     int64
	InjectErrors int64
	LastPollMS   int64
}

func runInjectLoop(ctx context.Context, cfg *Config, dsn string,
	statsCh chan<- InjectionStats, logger *slog.Logger) InjectionStats {

	var stats InjectionStats

	pool, err := pgxpool.New(ctx, dsn)
	if err != nil {
		logger.Error("failed to create DB pool", "err", err)
		return stats
	}
	defer pool.Close()

	if err := pool.Ping(ctx); err != nil {
		logger.Error("TimescaleDB ping failed", "err", err)
		return stats
	}
	logger.Info("TimescaleDB connected")

	injectFd, err := os.OpenFile(cfg.Hwmon.InjectPath, os.O_WRONLY, 0)
	if err != nil {
		logger.Error("failed to open inject device",
			"path", cfg.Hwmon.InjectPath, "err", err)
		return stats
	}
	defer injectFd.Close()

	ticker := time.NewTicker(time.Duration(cfg.TimescaleDB.PollMS) * time.Millisecond)
	defer ticker.Stop()

	// SQL: extract motor electrical parameters from JSONB payload
	// JSONB path: {data,motor,temperature_c}, {data,motor,current_a}, etc.
	// These paths mirror the ESP32 MQTT JSON structure transmitted by CMD 0x06.
	const query = `SELECT
		(payload #>> '{data,motor,temperature_c}')::float AS temp_c,
		(payload #>> '{data,motor,current_a}')::float    AS current_a,
		(payload #>> '{data,motor,voltage_v}')::float    AS voltage_v,
		(payload #>> '{data,motor,power_w}')::float      AS power_w
	FROM sensor_data
	WHERE device_id = $1
	  AND payload #> '{data,motor}' IS NOT NULL
	  AND source_path = 'mqtt'
	ORDER BY time DESC
	LIMIT 1`

	for {
		select {
		case <-ctx.Done():
			return stats
		case <-ticker.C:
			t0 := time.Now()

			for i, devID := range cfg.Hwmon.MotorDevices {
				var tempC, currentA, voltageV, powerW float64

				err := pool.QueryRow(ctx, query, devID).Scan(
					&tempC, &currentA, &voltageV, &powerW)
				if err != nil {
					stats.DBErrors++
					logger.Warn("DB query failed",
						"motor_id", i,
						"device_id", devID,
						"err", err)
					continue
				}

				// Convert to hwmon standard units (s32)
				inj := MotorInjection{
					MotorID: int32(i),
					TempMC:  int32(tempC * 1000),    // °C → m°C
					CurrMA:  int32(currentA * 1000),  // A → mA
					VoltMV:  int32(voltageV * 1000),  // V → mV
					PowerMW: int32(powerW * 1000000), // W → µW
				}

				buf := inj.Marshal()
				n, writeErr := injectFd.Write(buf)
				if writeErr != nil || n != motorInjectionSize {
					stats.InjectErrors++
					logger.Warn("inject write failed",
						"motor_id", i,
						"err", writeErr,
						"bytes", n)
					continue
				}
				stats.Injections++
			}

			stats.LastPollMS = time.Since(t0).Milliseconds()

			// Non-blocking send to health reporter
			select {
			case statsCh <- stats:
			default:
			}
		}
	}
}
