/*
 * inject.go — TimescaleDB polling + IIO injection logic
 *
 * Every 2s, queries vibration_view for the latest feature vector,
 * constructs a 96-byte binary struct (24×float32), and writes it
 * to /dev/edgevib-iio-inject.
 */

package main

import (
	"context"
	"encoding/binary"
	"fmt"
	"log/slog"
	"os"
	"time"

	"github.com/jackc/pgx/v5/pgxpool"
)

/* ---- 24-dim feature vector struct (96 bytes, 1:1 with ESP32) ---- */

const IIOInjectionSize = 96 // 24 × float32

type IIOInjection struct {
	RMS_X         float32 // chan 0
	RMS_Y         float32 // chan 1
	RMS_Z         float32 // chan 2
	OverallRMS    float32 // chan 3
	PeakFreqX     float32 // chan 4
	PeakAmpX      float32 // chan 5
	SkewnessX     float32 // chan 6
	KurtosisX     float32 // chan 7
	CrestFactorX  float32 // chan 8
	BandEnergyX0  float32 // chan 9
	BandEnergyX1  float32 // chan 10
	BandEnergyX2  float32 // chan 11
	BandEnergyX3  float32 // chan 12
	BandEnergyX4  float32 // chan 13
	BandEnergyX5  float32 // chan 14
	BandEnergyX6  float32 // chan 15
	BandEnergyX7  float32 // chan 16
	PeakFreqY     float32 // chan 17
	PeakAmpY      float32 // chan 18
	CrestFactorY  float32 // chan 19
	PeakFreqZ     float32 // chan 20
	PeakAmpZ      float32 // chan 21
	CrestFactorZ  float32 // chan 22
	TemperatureC  float32 // chan 23
}

/* ---- Stats ---- */

type InjectionStats struct {
	Injections   int64
	DBErrors     int64
	InjectErrors int64
	LastPollMS   int64
}

/* ---- DB polling and injection loop ---- */

func runInjectLoop(ctx context.Context, cfg *Config, dsn string,
	healthCh chan<- HealthStats, logger *slog.Logger) InjectionStats {

	var stats InjectionStats

	// Connect to TimescaleDB
	pool, err := pgxpool.New(ctx, dsn)
	if err != nil {
		logger.Error("failed to connect to TimescaleDB", "err", err)
		return stats
	}
	defer pool.Close()

	// Test connection
	if err := pool.Ping(ctx); err != nil {
		logger.Error("TimescaleDB ping failed", "err", err)
		return stats
	}
	logger.Info("TimescaleDB connected")

	// Open IIO injection device
	injectFd, err := os.OpenFile(cfg.IIO.InjectPath, os.O_WRONLY, 0)
	if err != nil {
		logger.Error("failed to open inject device", "path", cfg.IIO.InjectPath, "err", err)
		return stats
	}
	defer injectFd.Close()
	logger.Info("IIO inject device opened", "path", cfg.IIO.InjectPath)

	// Poll timer
	ticker := time.NewTicker(time.Duration(cfg.TimescaleDB.PollMS) * time.Millisecond)
	defer ticker.Stop()

	// Track last seen timestamp to avoid re-injecting stale data
	var lastSeenTime time.Time

	// SQL: get latest row from vibration_view
	query := fmt.Sprintf(
		`SELECT time, rms_x, rms_y, rms_z, overall_rms,
		        peak_frequency_hz, peak_amplitude_g,
		        fft_peaks,
		        temperature_c
		 FROM vibration_view
		 WHERE device_id = '%s'
		 ORDER BY time DESC LIMIT 1`,
		cfg.Device.DeviceID,
	)

	for {
		select {
		case <-ctx.Done():
			return stats

		case <-ticker.C:
			t0 := time.Now()

			// Query latest row
			var (
				rowTime       time.Time
				rmsX, rmsY, rmsZ, overallRMS float64
				peakFreq, peakAmp             float64
				fftPeaksJSON                  []byte
				tempC                         float64
			)

			err := pool.QueryRow(ctx, query).Scan(
				&rowTime, &rmsX, &rmsY, &rmsZ, &overallRMS,
				&peakFreq, &peakAmp,
				&fftPeaksJSON,
				&tempC,
			)
			if err != nil {
				stats.DBErrors++
				logger.Warn("DB query failed", "err", err)
				_ = healthSend(healthCh, stats)
				continue
			}

			// Skip if no new data
			if !rowTime.After(lastSeenTime) {
				stats.LastPollMS = time.Since(t0).Milliseconds()
				continue
			}
			lastSeenTime = rowTime

			// Build injection struct (simplified: use available VIEW columns,
			// fill missing features with ~0.0)
			inj := IIOInjection{
				RMS_X:      float32(rmsX),
				RMS_Y:      float32(rmsY),
				RMS_Z:      float32(rmsZ),
				OverallRMS: float32(overallRMS),
				PeakFreqX:  float32(peakFreq),
				PeakAmpX:   float32(peakAmp),
				// Remaining features not in vibration_view; set to 0
				TemperatureC: float32(tempC),
			}

			// Serialize to 96 bytes LE
			buf := make([]byte, IIOInjectionSize)
			putFloat32 := func(off int, v float32) {
				binary.LittleEndian.PutUint32(buf[off:off+4],
					uint32(int32(v*1000))) // scaled
			}
			// Write raw float32 values (module reads float32 directly)
			binary.LittleEndian.PutUint32(buf[0:4],  uint32(int32(inj.RMS_X*1000)))
			binary.LittleEndian.PutUint32(buf[4:8],  uint32(int32(inj.RMS_Y*1000)))
			binary.LittleEndian.PutUint32(buf[8:12],  uint32(int32(inj.RMS_Z*1000)))
			binary.LittleEndian.PutUint32(buf[12:16], uint32(int32(inj.OverallRMS*1000)))
			binary.LittleEndian.PutUint32(buf[16:20], uint32(int32(inj.PeakFreqX*1000)))
			binary.LittleEndian.PutUint32(buf[20:24], uint32(int32(inj.PeakAmpX*1000)))
			binary.LittleEndian.PutUint32(buf[24:28], uint32(int32(inj.SkewnessX*1000)))
			binary.LittleEndian.PutUint32(buf[28:32], uint32(int32(inj.KurtosisX*1000)))
			binary.LittleEndian.PutUint32(buf[32:36], uint32(int32(inj.CrestFactorX*1000)))
			// band_energy[0..7] = channels 9-16
			off := 36
			for i := 0; i < 8; i++ {
				binary.LittleEndian.PutUint32(buf[off:off+4], uint32(int32(inj.BandEnergyX0*1000)))
				off += 4
			}
			binary.LittleEndian.PutUint32(buf[68:72], uint32(int32(inj.PeakFreqY*1000)))
			binary.LittleEndian.PutUint32(buf[72:76], uint32(int32(inj.PeakAmpY*1000)))
			binary.LittleEndian.PutUint32(buf[76:80], uint32(int32(inj.CrestFactorY*1000)))
			binary.LittleEndian.PutUint32(buf[80:84], uint32(int32(inj.PeakFreqZ*1000)))
			binary.LittleEndian.PutUint32(buf[84:88], uint32(int32(inj.PeakAmpZ*1000)))
			binary.LittleEndian.PutUint32(buf[88:92], uint32(int32(inj.CrestFactorZ*1000)))
			binary.LittleEndian.PutUint32(buf[92:96], uint32(int32(inj.TemperatureC*1000)))
			_ = putFloat32

			// Write to injection device
			n, err := injectFd.Write(buf)
			if err != nil || n != IIOInjectionSize {
				stats.InjectErrors++
				logger.Warn("inject write failed", "err", err, "bytes", n)
				_ = healthSend(healthCh, stats)
				continue
			}

			stats.Injections++
			stats.LastPollMS = time.Since(t0).Milliseconds()

			logger.Debug("injected feature vector",
				"time", rowTime.Format(time.RFC3339),
				"rms_x", fmt.Sprintf("%.3f", rmsX),
				"poll_ms", stats.LastPollMS,
			)

			// Send health update
			_ = healthSend(healthCh, stats)
		}
	}
}
