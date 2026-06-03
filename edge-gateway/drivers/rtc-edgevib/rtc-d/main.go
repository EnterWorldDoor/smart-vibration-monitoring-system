/*
 * main.go — EdgeVib Software RTC Daemon entry point
 *
 * Manages time persistence for the EdgeVib Software RTC kernel module:
 *   1. Boot: read /var/lib/edgevib/last_time → ioctl(RTC_SET_TIME)
 *   2. Runtime: ioctl(RTC_READ_TIME) → write file every 30s
 *   3. Shutdown: SIGTERM → final ioctl(RTC_READ_TIME) → write file
 *
 * Reference:
 *   gpio-d/main.go    — config loading + signal handling pattern
 *   iio-d/main.go     — periodic ticker + health goroutine pattern
 *
 * Usage:
 *   edgevib-rtc-d -config /opt/edge-gateway/config/rtc-edgevib.yaml
 */

package main

import (
	"context"
	"flag"
	"fmt"
	"log/slog"
	"os"
	"os/signal"
	"path/filepath"
	"syscall"
	"time"

	"gopkg.in/yaml.v3"
)

/* ---- Config ---- */

type Config struct {
	RTC struct {
		Device        string `yaml:"device"`
		PersistPath   string `yaml:"persist_path"`
		SaveIntervalS int    `yaml:"save_interval_s"`
	} `yaml:"rtc"`

	MQTT struct {
		Broker   string `yaml:"broker"`
		Port     int    `yaml:"port"`
		ClientID string `yaml:"client_id"`
	} `yaml:"mqtt"`

	Health struct {
		IntervalS int    `yaml:"interval_s"`
		Topic     string `yaml:"topic"`
	} `yaml:"health"`

	Logging struct {
		Level string `yaml:"level"`
	} `yaml:"logging"`
}

func loadConfig(path string) (*Config, error) {
	data, err := os.ReadFile(path)
	if err != nil {
		return nil, fmt.Errorf("read config: %w", err)
	}

	cfg := &Config{}

	/* Default values — merged with YAML via Unmarshal */
	cfg.RTC.Device = "/dev/rtc0"
	cfg.RTC.PersistPath = "/var/lib/edgevib/last_time"
	cfg.RTC.SaveIntervalS = 30
	cfg.MQTT.Broker = "localhost"
	cfg.MQTT.Port = 1883
	cfg.MQTT.ClientID = "edgevib-rtc-d"
	cfg.Health.IntervalS = 30
	cfg.Health.Topic = "EdgeVib/system/health/rtc-d"
	cfg.Logging.Level = "info"

	if err := yaml.Unmarshal(data, cfg); err != nil {
		return nil, fmt.Errorf("parse config: %w", err)
	}

	return cfg, nil
}

func main() {
	configPath := flag.String("config",
		"/opt/edge-gateway/config/rtc-edgevib.yaml",
		"path to YAML config file")
	flag.Parse()

	/* ---- Load config ---- */
	cfg, err := loadConfig(*configPath)
	if err != nil {
		fmt.Fprintf(os.Stderr, "FATAL: load config: %v\n", err)
		os.Exit(1)
	}

	/* ---- Setup structured logging ---- */
	var level slog.Level
	switch cfg.Logging.Level {
	case "debug":
		level = slog.LevelDebug
	case "warn":
		level = slog.LevelWarn
	case "error":
		level = slog.LevelError
	default:
		level = slog.LevelInfo
	}
	logger := slog.New(slog.NewJSONHandler(os.Stdout, &slog.HandlerOptions{Level: level}))
	slog.SetDefault(logger)

	logger.Info("edgevib-rtc-d starting",
		"rtc_device", cfg.RTC.Device,
		"persist_path", cfg.RTC.PersistPath,
		"save_interval_s", cfg.RTC.SaveIntervalS,
		"mqtt_broker", cfg.MQTT.Broker,
	)

	/* ---- Open RTC device ---- */
	rtc, err := OpenRTC(cfg.RTC.Device)
	if err != nil {
		logger.Error("Failed to open RTC device", "path", cfg.RTC.Device, "err", err)
		os.Exit(1)
	}
	defer rtc.Close()
	logger.Info("RTC device opened", "path", rtc.Name())

	/* ---- Initialise time persistence ---- */
	persistDir := filepath.Dir(cfg.RTC.PersistPath)
	if err := os.MkdirAll(persistDir, 0755); err != nil {
		logger.Error("Failed to create persist directory", "dir", persistDir, "err", err)
		os.Exit(1)
	}

	timeFile := NewTimeFile(cfg.RTC.PersistPath)

	/* ---- Stats ---- */
	stats := HealthStats{}

	/* ---- Restore time from file ---- */
	savedTime, readErr := timeFile.Read()
	if readErr != nil {
		logger.Warn("No saved time to restore — using system clock as initial RTC value",
			"path", cfg.RTC.PersistPath,
			"reason", readErr.Error(),
		)
		stats.LoadErrors++
	} else {
		stats.LoadCount++
		logger.Info("Restoring saved time to RTC",
			"epoch", savedTime.Unix(),
			"time", savedTime.Format(time.RFC3339),
		)

		if err := rtc.SetTime(savedTime); err != nil {
			logger.Error("Failed to restore RTC time from file — falling back to system clock",
				"err", err,
			)
			stats.LoadErrors++
		} else {
			logger.Info("RTC time restored successfully",
				"time", savedTime.Format(time.RFC3339),
			)
		}
	}

	/* ---- Persist immediately (bootstraps last_time file if missing) ---- */
	currentTime, err := rtc.ReadTime()
	if err != nil {
		logger.Error("Failed to read RTC for initial persist", "err", err)
	} else {
		if err := timeFile.Write(currentTime); err != nil {
			logger.Warn("Initial persist failed", "err", err)
			stats.SaveErrors++
		} else {
			stats.SaveCount++
			stats.LastSaveTime = currentTime
			logger.Info("Initial time persisted",
				"epoch", currentTime.Unix(),
				"path", cfg.RTC.PersistPath,
			)
		}
	}

	/* ---- Signal handling ---- */
	ctx, cancel := signal.NotifyContext(context.Background(),
		syscall.SIGINT, syscall.SIGTERM)
	defer cancel()

	/* ---- Start health reporter ---- */
	statsCh := make(chan HealthStats, 8)
	go RunHealthReporter(ctx, cfg, rtc.Name(), statsCh, logger)

	/* ---- Periodic persist loop ---- */
	saveInterval := time.Duration(cfg.RTC.SaveIntervalS) * time.Second
	ticker := time.NewTicker(saveInterval)
	defer ticker.Stop()

	logger.Info("edgevib-rtc-d running",
		"save_interval", saveInterval.String(),
	)

	for {
		select {
		case <-ctx.Done():
			/* ---- Final flush on shutdown ---- */
			logger.Info("Shutting down — performing final RTC time persist...")
			finalTime, err := rtc.ReadTime()
			if err != nil {
				logger.Error("Final RTC read failed during shutdown", "err", err)
			} else {
				if err := timeFile.Write(finalTime); err != nil {
					logger.Error("Final persist failed during shutdown", "err", err)
					stats.SaveErrors++
				} else {
					stats.SaveCount++
					stats.LastSaveTime = finalTime
					logger.Info("Final time persisted",
						"epoch", finalTime.Unix(),
						"time", finalTime.Format(time.RFC3339),
					)
				}
			}

			/* Flush health stats */
			select {
			case statsCh <- stats:
			default:
			}

			logger.Info("edgevib-rtc-d stopped",
				"save_count", stats.SaveCount,
				"save_errors", stats.SaveErrors,
				"load_count", stats.LoadCount,
				"load_errors", stats.LoadErrors,
			)
			return

		case <-ticker.C:
			now, err := rtc.ReadTime()
			if err != nil {
				logger.Warn("RTC read failed — skipping persist cycle", "err", err)
				stats.SaveErrors++
				continue
			}

			if err := timeFile.Write(now); err != nil {
				logger.Warn("Persist write failed", "err", err)
				stats.SaveErrors++
			} else {
				stats.SaveCount++
				stats.LastSaveTime = now
			}

			/* Non-blocking stats update for health reporter */
			select {
			case statsCh <- stats:
			default:
			}
		}
	}
}
