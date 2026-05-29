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

	"edgevib/api-server/internal/config"
	"edgevib/api-server/internal/db"
	"edgevib/api-server/internal/handler"
	"edgevib/api-server/internal/middleware"
	"edgevib/api-server/internal/mqtt"
	"edgevib/api-server/internal/ws"

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

	// --- MQTT ---
	mqttSub := mqtt.NewSubscriber(logger)
	if err := mqttSub.Connect(cfg.MQTT.Broker, cfg.MQTT.ClientID); err != nil {
		logger.Warn("mqtt connection failed, REST endpoints available, WS unavailable", "error", err)
	} else {
		if err := mqttSub.Subscribe(cfg.MQTT.Topics); err != nil {
			logger.Warn("mqtt subscribe failed", "error", err)
		}
	}

	// --- WebSocket Hub ---
	hub := ws.NewHub()
	go hub.Run()

	// Bridge MQTT events to WebSocket hub
	if mqttSub.IsConnected() {
		mqttSub.StartBridge(hub.Broadcast)
	}

	// --- HTTP Router ---
	r := chi.NewRouter()

	r.Use(middleware.Recovery(logger))
	r.Use(middleware.Logging(logger))
	r.Use(middleware.Auth(cfg.Auth.Enabled, cfg.Auth.APIKey))

	// Health
	r.Get("/api/v1/health", handler.Health(dbClient, mqttSub.IsConnected))

	// Sites & devices
	sitesH := handler.NewSitesHandler(dbClient)
	sitesH.Register(r)

	devicesH := handler.NewDevicesHandler(dbClient)
	devicesH.Register(r)

	// Reports
	reportsH := handler.NewReportsHandler(dbClient)
	reportsH.Register(r)

	// Data export (training data sync)
	exportH := handler.NewExportHandler(dbClient)
	exportH.Register(r)

	// WebSocket
	wsH := handler.NewWSHandler(hub)
	wsH.Register(r)

	// --- HTTP Server ---
	addr := fmt.Sprintf("%s:%d", cfg.Server.Host, cfg.Server.Port)
	server := &http.Server{
		Addr:         addr,
		Handler:      r,
		ReadTimeout:  cfg.Server.ReadTimeout,
		WriteTimeout: cfg.Server.WriteTimeout,
		IdleTimeout:  120 * time.Second,
	}

	// Graceful shutdown
	errCh := make(chan error, 1)
	go func() {
		logger.Info("api-server starting", "addr", addr)
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
