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

	"github.com/go-chi/chi/v5"
	"github.com/jackc/pgx/v5/pgxpool"

	"edgevib/model-deploy/internal/config"
	"edgevib/model-deploy/internal/db"
	"edgevib/model-deploy/internal/filestore"
	"edgevib/model-deploy/internal/handler"
	"edgevib/model-deploy/internal/middleware"
	"edgevib/model-deploy/internal/mqtt"
)

func main() {
	configPath := flag.String("config", "config.yaml", "path to config YAML")
	flag.Parse()

	logger := slog.New(slog.NewJSONHandler(os.Stdout, &slog.HandlerOptions{
		Level: slog.LevelInfo,
	}))

	cfg, err := config.Load(*configPath)
	if err != nil {
		logger.Error("failed to load config", "err", err)
		os.Exit(1)
	}

	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()

	// Database
	poolCfg, err := pgxpool.ParseConfig(cfg.TimescaleDB.DSN())
	if err != nil {
		logger.Error("failed to parse DSN", "err", err)
		os.Exit(1)
	}
	poolCfg.MaxConns = int32(cfg.TimescaleDB.PoolMaxConns)

	pool, err := pgxpool.NewWithConfig(ctx, poolCfg)
	if err != nil {
		logger.Error("failed to create db pool", "err", err)
		os.Exit(1)
	}
	defer pool.Close()

	if err := pool.Ping(ctx); err != nil {
		logger.Error("db ping failed", "err", err)
		os.Exit(1)
	}
	logger.Info("database connected")

	dbClient := db.NewClient(pool)

	// File store
	if err := os.MkdirAll(cfg.Models.StoragePath, 0755); err != nil {
		logger.Error("failed to create storage dir", "path", cfg.Models.StoragePath, "err", err)
		os.Exit(1)
	}
	store := filestore.NewStore(cfg.Models.StoragePath, logger)

	var firmwareStore *filestore.Store
	if cfg.Models.FirmwareStoragePath != "" {
		if err := os.MkdirAll(cfg.Models.FirmwareStoragePath, 0755); err != nil {
			logger.Error("failed to create firmware storage dir", "path", cfg.Models.FirmwareStoragePath, "err", err)
			os.Exit(1)
		}
		firmwareStore = filestore.NewStore(cfg.Models.FirmwareStoragePath, logger)
	}

	// MQTT Publisher
	mqttPub := mqtt.NewPublisher(logger)
	if err := mqttPub.Connect(cfg.MQTT.Broker, cfg.MQTT.ClientID, 10*time.Second); err != nil {
		logger.Warn("mqtt publisher connect failed, service will run without MQTT", "err", err)
	}

	// MQTT Subscriber
	mqttSub := mqtt.NewSubscriber(logger)
	mqttSub.SetDBClient(dbClient)
	if err := mqttSub.Connect(cfg.MQTT.Broker, cfg.MQTT.ClientID, 10*time.Second); err != nil {
		logger.Warn("mqtt subscriber connect failed", "err", err)
	} else if len(cfg.MQTT.SubscribeTopics) > 0 {
		if err := mqttSub.Subscribe(cfg.MQTT.SubscribeTopics, 5*time.Second); err != nil {
			logger.Warn("mqtt subscribe failed", "err", err)
		} else {
			go mqttSub.ProcessEvents(ctx)
		}
	}

	// MQTT publish closure for handlers
	mqttPublish := func(topic, modelName, version, filePath string) error {
		if !mqttPub.IsConnected() {
			return fmt.Errorf("mqtt not connected")
		}
		return mqttPub.PublishReload(topic, modelName, version, filePath)
	}

	// Router
	r := chi.NewRouter()
	r.Use(middleware.Recovery(logger))
	r.Use(middleware.Logging(logger))

	// Handlers
	healthH := handler.NewHealthHandler(dbClient, mqttPub.IsConnected)
	healthH.Register(r)

	deployH := handler.NewDeployHandler(dbClient, store, firmwareStore, &cfg.Models, mqttPublish, logger)
	deployH.Register(r)

	rollbackH := handler.NewRollbackHandler(dbClient, &cfg.Models, mqttPublish, logger)
	rollbackH.Register(r)

	// HTTP Server
	addr := fmt.Sprintf("%s:%d", cfg.Server.Host, cfg.Server.Port)
	srv := &http.Server{
		Addr:         addr,
		Handler:      r,
		ReadTimeout:  10 * time.Second,
		WriteTimeout: 30 * time.Second,
		IdleTimeout:  120 * time.Second,
	}

	errCh := make(chan error, 1)
	go func() {
		logger.Info("server starting", "addr", addr)
		if err := srv.ListenAndServe(); err != nil && err != http.ErrServerClosed {
			errCh <- err
		}
	}()

	// Graceful shutdown
	sigCh := make(chan os.Signal, 1)
	signal.Notify(sigCh, syscall.SIGINT, syscall.SIGTERM)

	select {
	case sig := <-sigCh:
		logger.Info("received signal, shutting down", "signal", sig.String())
	case err := <-errCh:
		logger.Error("server error", "err", err)
	}

	cancel()

	shutdownCtx, shutdownCancel := context.WithTimeout(context.Background(), 10*time.Second)
	defer shutdownCancel()

	if err := srv.Shutdown(shutdownCtx); err != nil {
		logger.Error("shutdown error", "err", err)
	}

	mqttSub.Close()
	mqttPub.Close()

	logger.Info("server stopped")
}
