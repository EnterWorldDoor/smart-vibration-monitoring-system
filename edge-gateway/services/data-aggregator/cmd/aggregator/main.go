package main

import (
	"context"
	"flag"
	"fmt"
	"log/slog"
	"os"
	"os/signal"
	"strings"
	"sync/atomic"
	"syscall"
	"time"

	"github.com/jackc/pgx/v5/pgxpool"
	"gopkg.in/yaml.v3"

	"edgevib/data-aggregator/internal/dedup"
	"edgevib/data-aggregator/internal/health"
	"edgevib/data-aggregator/internal/model"
	"edgevib/data-aggregator/internal/mqtt"
	"edgevib/data-aggregator/internal/store"
	"edgevib/data-aggregator/internal/topic"
)

func main() {
	configPath := flag.String("config", "config.yaml", "path to aggregator config YAML")
	flag.Parse()

	logger := slog.New(slog.NewJSONHandler(os.Stdout, &slog.HandlerOptions{Level: slog.LevelInfo}))

	cfg, err := loadConfig(*configPath)
	if err != nil {
		logger.Error("failed to load config", "error", err)
		os.Exit(1)
	}

	registry, err := model.LoadDeviceRegistry(cfg.DevicesRegistryPath, cfg.SiteID)
	if err != nil {
		logger.Error("failed to load device registry", "error", err)
		os.Exit(1)
	}
	logger.Info("device registry loaded", "path", cfg.DevicesRegistryPath)

	cache := dedup.New(time.Duration(cfg.Dedup.WindowSeconds) * time.Second)

	dsn := fmt.Sprintf("postgres://%s:%s@%s:%d/%s?sslmode=%s",
		cfg.Database.User, cfg.Database.Password,
		cfg.Database.Host, cfg.Database.Port,
		cfg.Database.DBName, cfg.Database.SSLMode)

	pool, err := pgxpool.New(context.Background(), dsn)
	if err != nil {
		logger.Error("failed to create db pool", "error", err)
		os.Exit(1)
	}
	defer pool.Close()

	if err := pool.Ping(context.Background()); err != nil {
		logger.Error("database ping failed", "error", err)
		os.Exit(1)
	}
	logger.Info("database connected", "host", cfg.Database.Host, "db", cfg.Database.DBName)

	writer := store.NewBatchWriter(pool, 100, 5*time.Second, logger)

	sub := mqtt.NewSubscriber(logger)
	if err := sub.Connect(cfg.MQTT.BrokerURL, cfg.MQTT.ClientID); err != nil {
		logger.Error("mqtt connect failed", "error", err)
		os.Exit(1)
	}
	defer sub.Close()

	if err := sub.Subscribe(cfg.MQTT.Topics, cfg.MQTT.QoS); err != nil {
		logger.Error("mqtt subscribe failed", "error", err)
		os.Exit(1)
	}

	reporter := health.NewReporter(cfg.SiteID, func(topic string, payload []byte) error {
		return sub.Publish(topic, 1, payload)
	}, logger)

	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()

	go writer.TimerFlush(ctx)
	go reporter.Run(ctx)
	go func() {
		ticker := time.NewTicker(time.Duration(cfg.Dedup.WindowSeconds) * time.Second)
		defer ticker.Stop()
		for {
			select {
			case <-ticker.C:
				cache.PurgeExpired()
			case <-ctx.Done():
				return
			}
		}
	}()

	// Track MQTT connection state for health reporting
	go func() {
		ticker := time.NewTicker(5 * time.Second)
		defer ticker.Stop()
		for {
			select {
			case <-ticker.C:
				if sub.IsConnected() {
					atomic.StoreInt32(&reporter.Metrics().MQTTConnected, 1)
				} else {
					atomic.StoreInt32(&reporter.Metrics().MQTTConnected, 0)
				}
				if err := pool.Ping(context.Background()); err == nil {
					atomic.StoreInt32(&reporter.Metrics().DBConnected, 1)
				} else {
					atomic.StoreInt32(&reporter.Metrics().DBConnected, 0)
				}
				rows, errs := writer.Stats()
				atomic.StoreInt64(&reporter.Metrics().RowsWritten, rows)
				atomic.StoreInt64(&reporter.Metrics().DBErrors, errs)
			case <-ctx.Done():
				return
			}
		}
	}()

	sigChan := make(chan os.Signal, 1)
	signal.Notify(sigChan, syscall.SIGINT, syscall.SIGTERM)

	logger.Info("data-aggregator started", "topics", cfg.MQTT.Topics, "site_id", cfg.SiteID)

	for {
		select {
		case msg, ok := <-sub.Messages():
			if !ok {
				logger.Info("message channel closed, exiting")
				return
			}
			processMessage(msg, registry, cache, writer, reporter.Metrics(), logger)

		case <-sigChan:
			logger.Info("shutting down")
			cancel()
			sub.Close()
			writer.Close()
			logger.Info("shutdown complete")
			return
		}
	}
}

func processMessage(
	msg *model.SensorMessage,
	registry *model.DeviceRegistry,
	cache *dedup.Cache,
	writer *store.BatchWriter,
	metrics *health.Metrics,
	logger *slog.Logger,
) {
	atomic.AddInt64(&metrics.MsgReceived, 1)

	pr, err := topic.ParseTopic(msg.Topic)
	if err != nil {
		atomic.AddInt64(&metrics.ParseErrors, 1)
		logger.Warn("topic parse failed", "topic", msg.Topic, "error", err)
		return
	}

	switch pr.Pattern {
	case "A":
		siteID, deviceType, deviceID := registry.Lookup(pr.DevID)
		msg.SiteID = siteID
		msg.DeviceType = deviceType
		msg.DeviceID = deviceID
		if pr.DataType != "" {
			msg.DataType = pr.DataType
		}
	case "B":
		msg.SiteID = pr.SiteID
		msg.DeviceType = pr.DeviceType
		msg.DeviceID = pr.DeviceID
		msg.DataType = pr.DataType
	}

	if msg.TimestampMS >= 0 {
		key := dedup.MakeKey(msg.DeviceID, msg.TimestampMS, msg.Topic)
		if !cache.CheckAndAdd(key) {
			atomic.AddInt64(&metrics.MsgDeduped, 1)
			return
		}
	}

	writer.Add(msg)
}

func loadConfig(path string) (*model.Config, error) {
	data, err := os.ReadFile(path)
	if err != nil {
		return nil, fmt.Errorf("read config: %w", err)
	}

	content := os.Expand(string(data), func(key string) string {
		if idx := strings.Index(key, ":-"); idx >= 0 {
			envVar := key[:idx]
			defaultVal := key[idx+2:]
			if val := os.Getenv(envVar); val != "" {
				return val
			}
			return defaultVal
		}
		return os.Getenv(key)
	})

	var cfg model.Config
	if err := yaml.Unmarshal([]byte(content), &cfg); err != nil {
		return nil, fmt.Errorf("parse config: %w", err)
	}

	// Defaults
	if cfg.DevicesRegistryPath == "" {
		cfg.DevicesRegistryPath = "devices.yaml"
	}
	if cfg.MQTT.QoS == 0 {
		cfg.MQTT.QoS = 1
	}

	return &cfg, nil
}
