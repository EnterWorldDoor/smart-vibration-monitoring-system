package handler

import (
	"fmt"
	"log/slog"
	"net/http"
	"time"

	"github.com/go-chi/chi/v5"

	"edgevib/model-deploy/internal/config"
	"edgevib/model-deploy/internal/db"
)

type RollbackHandler struct {
	db          *db.Client
	cfg         *config.ModelsConfig
	mqttPublish func(topic, modelName, version, filePath string) error
	logger      *slog.Logger
}

func NewRollbackHandler(dbClient *db.Client, cfg *config.ModelsConfig, mqttPublish func(string, string, string, string) error, logger *slog.Logger) *RollbackHandler {
	return &RollbackHandler{
		db:          dbClient,
		cfg:         cfg,
		mqttPublish: mqttPublish,
		logger:      logger,
	}
}

func (h *RollbackHandler) Register(r chi.Router) {
	r.Post("/api/v1/models/{name}/rollback", h.Rollback)
}

func (h *RollbackHandler) Rollback(w http.ResponseWriter, r *http.Request) {
	modelName := chi.URLParam(r, "name")
	if modelName == "" {
		writeError(w, http.StatusBadRequest, "model name is required")
		return
	}

	var req struct {
		Version string `json:"version"`
	}
	if err := decodeJSON(r, &req); err != nil {
		writeError(w, http.StatusBadRequest, "invalid request body")
		return
	}

	if req.Version == "" {
		writeError(w, http.StatusBadRequest, "version is required")
		return
	}

	// Verify version exists
	mv, err := h.db.GetModelVersion(r.Context(), modelName, req.Version)
	if err != nil {
		h.logger.Error("get model version failed", "err", err)
		writeError(w, http.StatusInternalServerError, "failed to find version")
		return
	}
	if mv == nil {
		writeError(w, http.StatusNotFound, "version not found")
		return
	}

	// Mark as deployed
	if err := h.db.MarkDeployed(r.Context(), modelName, req.Version, "rollback"); err != nil {
		h.logger.Error("mark deployed failed", "err", err)
		writeError(w, http.StatusInternalServerError, "failed to rollback")
		return
	}

	// Publish reload via MQTT
	if h.mqttPublish != nil {
		reloadTopic := fmt.Sprintf("EdgeVib/%s/inference/%s/model/reload", h.cfg.SiteID, modelName)
		if err := h.mqttPublish(reloadTopic, modelName, req.Version, mv.FilePath); err != nil {
			h.logger.Warn("mqtt reload publish failed", "err", err)
		}
	}

	h.logger.Info("model rolled back",
		"model", modelName,
		"version", req.Version,
	)

	writeJSON(w, http.StatusOK, map[string]interface{}{
		"model_name":  modelName,
		"version":     req.Version,
		"status":      "rolled_back",
		"rolled_back_at": time.Now().UTC().Format(time.RFC3339),
	})
}
