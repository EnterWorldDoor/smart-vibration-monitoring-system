package main

import (
	"context"
	"flag"
	"fmt"
	"log/slog"
	"os"
	"os/signal"
	"sync/atomic"
	"syscall"
	"time"

	"github.com/jackc/pgx/v5/pgxpool"

	"edgevib/edge-router/internal/config"
	"edgevib/edge-router/internal/db"
	"edgevib/edge-router/internal/dedup"
	"edgevib/edge-router/internal/health"
	"edgevib/edge-router/internal/mqtt"
	"edgevib/edge-router/internal/router"
)

func main() {
	// 1. Parse flags
	configPath := flag.String("config", "config.yaml", "path to config YAML")
	flag.Parse()

	// 2. Init structured JSON logger
	logger := slog.New(slog.NewJSONHandler(os.Stdout, &slog.HandlerOptions{Level: slog.LevelInfo}))

	// 3. Load config
	cfg, err := config.Load(*configPath)
	if err != nil {
		logger.Error("failed to load config", "error", err)
		os.Exit(1)
	}
	logger.Info("config loaded", "path", *configPath)

	// 4. Create dedup cache
	cache := dedup.New(time.Duration(cfg.Dedup.WindowSeconds) * time.Second)
	logger.Info("dedup cache created", "ttl_seconds", cfg.Dedup.WindowSeconds)

	// 5. Create pgxpool
	pool, err := pgxpool.New(context.Background(), cfg.Database.DSN())
	if err != nil {
		logger.Error("failed to create db pool", "error", err)
		os.Exit(1)
	}
	defer pool.Close()

	if err := pool.Ping(context.Background()); err != nil {
		logger.Error("db ping failed", "error", err)
		os.Exit(1)
	}
	logger.Info("db connected", "host", cfg.Database.Host, "port", cfg.Database.Port)

	// 6. Create DB querier
	querier := db.NewQuerier(pool, logger)

	// 7. Create MQTT client
	mqttClient := mqtt.NewClient(logger)
	if err := mqttClient.Connect(cfg.MQTT.BrokerURL, cfg.MQTT.ClientID); err != nil {
		logger.Error("mqtt connect failed", "error", err, "broker", cfg.MQTT.BrokerURL)
		os.Exit(1)
	}
	if err := mqttClient.Subscribe(cfg.MQTT.Topics, cfg.MQTT.QoS); err != nil {
		logger.Error("mqtt subscribe failed", "error", err)
		os.Exit(1)
	}

	// 8. Create router
	r := router.New(
		cfg.Router.Sites,
		cache,
		querier,
		func(topic string, payload []byte) error {
			return mqttClient.Publish(topic, cfg.MQTT.QoS, payload)
		},
		logger,
	)

	// 9. Create health reporter (use first site as "self" site)
	selfSite := cfg.Router.Sites[0]
	if selfSite == "" {
		selfSite = "factory1"
	}
	reporter := health.NewReporter(
		selfSite,
		func(topic string, payload []byte) error {
			return mqttClient.Publish(topic, 1, payload)
		},
		logger,
	)

	// 10. Setup graceful shutdown
	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()

	// 11. Background goroutines
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

	go func() {
		ticker := time.NewTicker(5 * time.Second)
		defer ticker.Stop()
		for {
			select {
			case <-ticker.C:
				if mqttClient.IsConnected() {
					atomic.StoreInt32(&reporter.Metrics().MQTTConnected, 1)
				} else {
					atomic.StoreInt32(&reporter.Metrics().MQTTConnected, 0)
				}
				if err := pool.Ping(context.Background()); err == nil {
					atomic.StoreInt32(&reporter.Metrics().DBConnected, 1)
				} else {
					atomic.StoreInt32(&reporter.Metrics().DBConnected, 0)
					logger.Warn("db health check failed", "error", err)
				}
			case <-ctx.Done():
				return
			}
		}
	}()

	// 12. Signal handling
	sigChan := make(chan os.Signal, 1)
	signal.Notify(sigChan, syscall.SIGINT, syscall.SIGTERM)

	logger.Info("edge-router started",
		"topics", cfg.MQTT.Topics,
		"sites", cfg.Router.Sites,
	)

	// 13. Main event loop
	for {
		select {
		case rawMsg, ok := <-mqttClient.Messages():
			if !ok {
				logger.Info("message channel closed, exiting")
				return
			}
			r.Handle(rawMsg, reporter.Metrics())

		case sig := <-sigChan:
			logger.Info(fmt.Sprintf("received signal %v, shutting down", sig))
			cancel()
			mqttClient.Close()
			logger.Info("shutdown complete")
			return
		}
	}
}
