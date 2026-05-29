package handler

import (
	"encoding/json"
	"net/http"
	"strings"

	"edgevib/ota-server/internal/db"
	"edgevib/ota-server/internal/filestore"

	"github.com/go-chi/chi/v5"
)

type VersionHandler struct {
	db    *db.Client
	store *filestore.Store
}

func NewVersionHandler(dbClient *db.Client, store *filestore.Store) *VersionHandler {
	return &VersionHandler{db: dbClient, store: store}
}

func (h *VersionHandler) Register(r chi.Router) {
	r.Get("/firmware/version.json", h.ServeVersionJSON)
	r.Get("/firmware/{platform}/*", h.ServeFirmwareFile)
}

type versionEntry struct {
	LatestVersion  string `json:"latest_version"`
	BuildDate      string `json:"build_date,omitempty"`
	File           string `json:"file"`
	Size           int64  `json:"size"`
	SHA256         string `json:"sha256"`
	MinHardwareRev string `json:"min_hardware_rev,omitempty"`
	ReleaseNotes   string `json:"release_notes,omitempty"`
}

type modelEntry struct {
	LatestVersion string `json:"latest_version"`
	File          string `json:"file"`
	Size          int64  `json:"size"`
	SHA256        string `json:"sha256"`
}

func (h *VersionHandler) ServeVersionJSON(w http.ResponseWriter, r *http.Request) {
	// Firmware versions (existing)
	latest, err := h.db.GetLatestAllPlatforms(r.Context())
	if err != nil {
		writeError(w, http.StatusInternalServerError, "failed to query firmware versions")
		return
	}

	result := make(map[string]interface{})
	for platform, fw := range latest {
		result[platform] = versionEntry{
			LatestVersion:  fw.Version,
			BuildDate:      fw.BuildDate,
			File:           fw.FileName,
			Size:           fw.FileSize,
			SHA256:         fw.SHA256,
			MinHardwareRev: fw.MinHardwareRev,
			ReleaseNotes:   fw.ReleaseNotes,
		}
	}

	// Model versions — deployed models on esp32 platform
	models, err := h.db.GetLatestDeployedModels(r.Context(), "esp32")
	if err != nil {
		writeError(w, http.StatusInternalServerError, "failed to query model versions")
		return
	}

	if len(models) > 0 {
		modelMap := make(map[string]modelEntry)
		for _, m := range models {
			modelMap[m.ModelName] = modelEntry{
				LatestVersion: m.Version,
				File:          m.FileName,
				Size:          m.FileSize,
				SHA256:        m.SHA256,
			}
		}
		result["models"] = modelMap
	}

	w.Header().Set("Content-Type", "application/json")
	w.Header().Set("Cache-Control", "no-cache")
	json.NewEncoder(w).Encode(result)
}

func (h *VersionHandler) ServeFirmwareFile(w http.ResponseWriter, r *http.Request) {
	platform := chi.URLParam(r, "platform")
	if platform != "esp32" && platform != "f407" {
		writeError(w, http.StatusBadRequest, "platform must be 'esp32' or 'f407'")
		return
	}

	filePath := chi.URLParam(r, "*")
	filePath = strings.TrimPrefix(filePath, "/")

	// Allow .bin (firmware) and .tflite (models) extensions
	if strings.Contains(filePath, "..") {
		writeError(w, http.StatusBadRequest, "invalid file path")
		return
	}
	if !strings.HasSuffix(filePath, ".bin") && !strings.HasSuffix(filePath, ".tflite") {
		writeError(w, http.StatusBadRequest, "file must be .bin or .tflite")
		return
	}

	fullPath, err := h.store.GetFirmwarePath(platform, filePath)
	if err != nil {
		writeError(w, http.StatusBadRequest, "invalid file path")
		return
	}

	w.Header().Set("Content-Type", "application/octet-stream")
	http.ServeFile(w, r, fullPath)
}
