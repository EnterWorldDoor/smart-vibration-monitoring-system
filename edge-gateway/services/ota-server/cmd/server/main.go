package main

import (
	"context"
	"flag"
	"fmt"
	"log/slog"
	"net/http"
	"os"
	"os/signal"
	"syscall"
	"time"

	"edgevib/ota-server/internal/config"
	"edgevib/ota-server/internal/db"
	"edgevib/ota-server/internal/filestore"
	"edgevib/ota-server/internal/handler"
	"edgevib/ota-server/internal/middleware"
	"edgevib/ota-server/internal/mqtt"

	"github.com/go-chi/chi/v5"
	"github.com/jackc/pgx/v5/pgxpool"
)

func main() {
	configPath := flag.String("config", "config.yaml", "path to YAML configuration file")
	flag.Parse()

	logger := slog.New(slog.NewJSONHandler(os.Stdout, &slog.HandlerOptions{Level: slog.LevelInfo}))
	slog.SetDefault(logger)

	cfg, err := config.Load(*configPath)
	if err != nil {
		logger.Error("failed to load config", "error", err)
		os.Exit(1)
	}
	logger.Info("config loaded", "path", *configPath)

	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()

	// --- TimescaleDB ---
	pool, err := pgxpool.New(ctx, cfg.TimescaleDB.DSN())
	if err != nil {
		logger.Error("failed to create connection pool", "error", err)
		os.Exit(1)
	}
	defer pool.Close()

	if err := pool.Ping(ctx); err != nil {
		logger.Error("failed to ping database", "error", err)
		os.Exit(1)
	}
	logger.Info("timescaledb connected", "host", cfg.TimescaleDB.Host)

	dbClient := db.NewClient(pool)

	// --- File Store ---
	store := filestore.NewStore(cfg.Firmware.StoragePath, logger)

	// --- MQTT ---
	mqttSub := mqtt.NewSubscriber(logger)
	if err := mqttSub.Connect(cfg.MQTT.Broker, cfg.MQTT.ClientID); err != nil {
		logger.Warn("mqtt connection failed, REST endpoints available, upgrade triggers unavailable", "error", err)
	} else {
		if err := mqttSub.Subscribe(cfg.MQTT.Topics); err != nil {
			logger.Warn("mqtt subscribe failed", "error", err)
		}
		mqttSub.ProcessEvents(ctx, dbClient, cfg.Firmware.SiteID)
	}

	// MQTT publish helper for trigger-upgrade
	mqttPublish := func(topic string, payload []byte) error {
		return mqttSub.Publish(topic, 1, false, payload)
	}

	// --- HTTP Router ---
	r := chi.NewRouter()

	r.Use(middleware.Recovery(logger))
	r.Use(middleware.Logging(logger))

	// Health
	healthH := handler.NewHealthHandler(dbClient, mqttSub.IsConnected)
	healthH.Register(r)

	// Firmware management REST API
	firmwareH := handler.NewFirmwareHandler(dbClient, store, &cfg.Firmware, mqttPublish)
	firmwareH.Register(r)

	// Firmware file download + version.json
	versionH := handler.NewVersionHandler(dbClient, store)
	versionH.Register(r)

	// --- HTTP Server ---
	addr := fmt.Sprintf("%s:%d", cfg.Server.Host, cfg.Server.Port)
	server := &http.Server{
		Addr:         addr,
		Handler:      r,
		ReadTimeout:  cfg.Server.ReadTimeout,
		WriteTimeout: cfg.Server.WriteTimeout,
		IdleTimeout:  120 * time.Second,
	}

	errCh := make(chan error, 1)
	go func() {
		logger.Info("ota-server starting", "addr", addr)
		if err := server.ListenAndServe(); err != nil && err != http.ErrServerClosed {
			errCh <- err
		}
	}()

	sigCh := make(chan os.Signal, 1)
	signal.Notify(sigCh, syscall.SIGINT, syscall.SIGTERM)

	select {
	case err := <-errCh:
		logger.Error("server error", "error", err)
		os.Exit(1)
	case sig := <-sigCh:
		logger.Info("shutting down", "signal", sig.String())
		cancel()
		mqttSub.Close()
		shutdownCtx, shutdownCancel := context.WithTimeout(context.Background(), 10*time.Second)
		defer shutdownCancel()
		if err := server.Shutdown(shutdownCtx); err != nil {
			logger.Error("shutdown error", "error", err)
		}
	}
}
