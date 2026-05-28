package handler

import (
	"encoding/json"
	"fmt"
	"log/slog"
	"net/http"
	"regexp"
	"time"

	"github.com/go-chi/chi/v5"

	"edgevib/model-deploy/internal/config"
	"edgevib/model-deploy/internal/db"
	"edgevib/model-deploy/internal/filestore"
)

var semverRegex = regexp.MustCompile(`^\d+\.\d+\.\d+$`)

type DeployHandler struct {
	db          *db.Client
	store       *filestore.Store
	cfg         *config.ModelsConfig
	mqttPublish func(topic, modelName, version, filePath string) error
	logger      *slog.Logger
}

func NewDeployHandler(dbClient *db.Client, store *filestore.Store, cfg *config.ModelsConfig, mqttPublish func(string, string, string, string) error, logger *slog.Logger) *DeployHandler {
	return &DeployHandler{
		db:          dbClient,
		store:       store,
		cfg:         cfg,
		mqttPublish: mqttPublish,
		logger:      logger,
	}
}

func (h *DeployHandler) Register(r chi.Router) {
	r.Post("/api/v1/models/deploy", h.Deploy)
	r.Get("/api/v1/models", h.ListAllModels)
	r.Get("/api/v1/models/{name}", h.GetModel)
	r.Get("/api/v1/models/{name}/versions", h.ListVersions)
}

func (h *DeployHandler) Deploy(w http.ResponseWriter, r *http.Request) {
	if err := r.ParseMultipartForm(32 << 20); err != nil {
		writeError(w, http.StatusBadRequest, "failed to parse multipart form")
		return
	}

	modelName := r.FormValue("model_name")
	version := r.FormValue("version")
	metricsStr := r.FormValue("metrics_json")

	if modelName == "" {
		writeError(w, http.StatusBadRequest, "model_name is required")
		return
	}
	if !semverRegex.MatchString(version) {
		writeError(w, http.StatusBadRequest, "version must be semver (e.g. 1.0.0)")
		return
	}

	file, header, err := r.FormFile("model_file")
	if err != nil {
		writeError(w, http.StatusBadRequest, "model_file is required")
		return
	}
	defer file.Close()

	if !isONNX(header.Filename) {
		writeError(w, http.StatusBadRequest, "model_file must be a .onnx file")
		return
	}

	var metrics json.RawMessage
	if metricsStr != "" {
		if !json.Valid([]byte(metricsStr)) {
			writeError(w, http.StatusBadRequest, "metrics_json is not valid JSON")
			return
		}
		metrics = json.RawMessage(metricsStr)
	} else {
		metrics = json.RawMessage("{}")
	}

	// Save file
	relPath, fileSize, sha256, err := h.store.SaveModel(file, modelName, version)
	if err != nil {
		h.logger.Error("save model failed", "err", err)
		writeError(w, http.StatusInternalServerError, "failed to save model file")
		return
	}

	// Insert DB record
	now := time.Now()
	mv := &db.ModelVersion{
		ModelName:   modelName,
		Version:     version,
		FilePath:    relPath,
		FileSize:    fileSize,
		SHA256:      sha256,
		MetricsJSON: metrics,
		DeployedAt:  &now,
		DeployedBy:  "pc-push",
	}

	id, err := h.db.InsertModelVersion(r.Context(), mv)
	if err != nil {
		h.logger.Error("insert model version failed", "err", err)
		writeError(w, http.StatusInternalServerError, "failed to record model version")
		return
	}
	mv.ID = id

	// Rotate old versions
	if err := h.store.RotateVersions(modelName, h.cfg.MaxVersions); err != nil {
		h.logger.Warn("rotate versions failed", "err", err)
	}

	// Publish reload via MQTT
	if h.mqttPublish != nil {
		reloadTopic := fmt.Sprintf("EdgeVib/%s/inference/%s/model/reload", h.cfg.SiteID, modelName)
		if err := h.mqttPublish(reloadTopic, modelName, version, relPath); err != nil {
			h.logger.Warn("mqtt reload publish failed", "err", err)
		}
	}

	h.logger.Info("model deployed",
		"model", modelName,
		"version", version,
		"size", fileSize,
	)

	writeJSON(w, http.StatusCreated, map[string]interface{}{
		"id":               mv.ID,
		"model_name":       modelName,
		"version":          version,
		"file_path":        relPath,
		"file_size":        fileSize,
		"sha256":           sha256,
		"status":           "deployed",
	})
}

func (h *DeployHandler) ListAllModels(w http.ResponseWriter, r *http.Request) {
	versions, err := h.db.ListAllModels(r.Context())
	if err != nil {
		h.logger.Error("list models failed", "err", err)
		writeError(w, http.StatusInternalServerError, "failed to list models")
		return
	}
	if versions == nil {
		versions = []db.ModelVersion{}
	}

	type modelSummary struct {
		ModelName     string     `json:"model_name"`
		LatestVersion string     `json:"latest_version"`
		DeployedAt    *time.Time `json:"deployed_at"`
		UploadedAt    time.Time  `json:"uploaded_at"`
	}

	var result []modelSummary
	for _, mv := range versions {
		result = append(result, modelSummary{
			ModelName:     mv.ModelName,
			LatestVersion: mv.Version,
			DeployedAt:    mv.DeployedAt,
			UploadedAt:    mv.UploadedAt,
		})
	}

	writeJSON(w, http.StatusOK, map[string]interface{}{"models": result})
}

func (h *DeployHandler) GetModel(w http.ResponseWriter, r *http.Request) {
	modelName := chi.URLParam(r, "name")
	if modelName == "" {
		writeError(w, http.StatusBadRequest, "model name is required")
		return
	}

	versions, err := h.db.ListModelVersions(r.Context(), modelName)
	if err != nil {
		h.logger.Error("get model versions failed", "err", err)
		writeError(w, http.StatusInternalServerError, "failed to get model")
		return
	}
	if versions == nil {
		versions = []db.ModelVersion{}
	}

	deployed, _ := h.db.GetDeployedVersion(r.Context(), modelName)

	writeJSON(w, http.StatusOK, map[string]interface{}{
		"model_name": modelName,
		"deployed":   deployed,
		"versions":   versions,
	})
}

func (h *DeployHandler) ListVersions(w http.ResponseWriter, r *http.Request) {
	modelName := chi.URLParam(r, "name")
	if modelName == "" {
		writeError(w, http.StatusBadRequest, "model name is required")
		return
	}

	versions, err := h.db.ListModelVersions(r.Context(), modelName)
	if err != nil {
		h.logger.Error("list versions failed", "err", err)
		writeError(w, http.StatusInternalServerError, "failed to list versions")
		return
	}
	if versions == nil {
		versions = []db.ModelVersion{}
	}

	writeJSON(w, http.StatusOK, map[string]interface{}{"versions": versions})
}

func isONNX(filename string) bool {
	return len(filename) >= 5 && filename[len(filename)-5:] == ".onnx"
}
