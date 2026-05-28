package handler

import (
	"encoding/json"
	"net/http"
	"regexp"
	"strconv"

	"edgevib/ota-server/internal/config"
	"edgevib/ota-server/internal/db"
	"edgevib/ota-server/internal/filestore"

	"github.com/go-chi/chi/v5"
)

var semverRe = regexp.MustCompile(`^\d+\.\d+\.\d+$`)

type FirmwareHandler struct {
	db    *db.Client
	store *filestore.Store
	cfg   *config.FirmwareConfig
	// mqttPublish is a function to publish MQTT messages for upgrade triggers
	mqttPublish func(topic string, payload []byte) error
}

func NewFirmwareHandler(dbClient *db.Client, store *filestore.Store, cfg *config.FirmwareConfig, mqttPublish func(string, []byte) error) *FirmwareHandler {
	return &FirmwareHandler{
		db:          dbClient,
		store:       store,
		cfg:         cfg,
		mqttPublish: mqttPublish,
	}
}

func (h *FirmwareHandler) Register(r chi.Router) {
	r.Post("/api/v1/firmware/upload", h.UploadFirmware)
	r.Get("/api/v1/firmware/versions", h.ListVersions)
	r.Get("/api/v1/firmware/upgrade-history", h.ListUpgradeHistory)
	r.Post("/api/v1/firmware/trigger-upgrade", h.TriggerUpgrade)
}

func (h *FirmwareHandler) UploadFirmware(w http.ResponseWriter, r *http.Request) {
	if err := r.ParseMultipartForm(32 << 20); err != nil {
		writeError(w, http.StatusBadRequest, "failed to parse multipart form")
		return
	}

	platform := r.FormValue("platform")
	if platform != "esp32" && platform != "f407" {
		writeError(w, http.StatusBadRequest, "platform must be 'esp32' or 'f407'")
		return
	}

	version := r.FormValue("version")
	if !semverRe.MatchString(version) {
		writeError(w, http.StatusBadRequest, "version must be semver (e.g. 1.2.3)")
		return
	}

	file, header, err := r.FormFile("file")
	if err != nil {
		writeError(w, http.StatusBadRequest, "missing firmware file")
		return
	}
	defer file.Close()

	if header.Size > 16*1024*1024 {
		writeError(w, http.StatusBadRequest, "file too large (max 16MB)")
		return
	}

	buildDate := r.FormValue("build_date")
	if buildDate == "" {
		writeError(w, http.StatusBadRequest, "build_date is required")
		return
	}

	fileName := platform + "-gateway-" + version + ".bin"
	relPath, fileSize, sha256Hash, err := h.store.SaveFirmware(file, platform, fileName)
	if err != nil {
		writeError(w, http.StatusInternalServerError, "failed to save firmware")
		return
	}

	fw := &db.FirmwareVersion{
		Platform:       platform,
		Version:        version,
		BuildDate:      buildDate,
		FileName:       fileName,
		FileSize:       fileSize,
		SHA256:         sha256Hash,
		MinHardwareRev: r.FormValue("min_hardware_rev"),
		ReleaseNotes:   r.FormValue("release_notes"),
		FilePath:       relPath,
	}
	if fw.MinHardwareRev == "" {
		fw.MinHardwareRev = "v1.0"
	}

	id, err := h.db.InsertFirmwareVersion(r.Context(), fw)
	if err != nil {
		writeError(w, http.StatusInternalServerError, "failed to save version to database")
		return
	}
	fw.ID = id

	writeJSON(w, http.StatusCreated, fw)
}

func (h *FirmwareHandler) ListVersions(w http.ResponseWriter, r *http.Request) {
	platform := r.URL.Query().Get("platform")
	versions, err := h.db.ListVersions(r.Context(), platform)
	if err != nil {
		writeError(w, http.StatusInternalServerError, "failed to list versions")
		return
	}
	if versions == nil {
		versions = []db.FirmwareVersion{}
	}
	writeJSON(w, http.StatusOK, map[string]interface{}{"versions": versions})
}

func (h *FirmwareHandler) ListUpgradeHistory(w http.ResponseWriter, r *http.Request) {
	platform := r.URL.Query().Get("platform")
	deviceID := r.URL.Query().Get("device_id")

	limit, _ := strconv.Atoi(r.URL.Query().Get("limit"))
	if limit <= 0 || limit > 100 {
		limit = 20
	}
	offset, _ := strconv.Atoi(r.URL.Query().Get("offset"))
	if offset < 0 {
		offset = 0
	}

	history, err := h.db.ListUpgradeHistory(r.Context(), platform, deviceID, limit, offset)
	if err != nil {
		writeError(w, http.StatusInternalServerError, "failed to list upgrade history")
		return
	}
	if history == nil {
		history = []db.UpgradeHistory{}
	}
	writeJSON(w, http.StatusOK, map[string]interface{}{"history": history})
}

func (h *FirmwareHandler) TriggerUpgrade(w http.ResponseWriter, r *http.Request) {
	if h.mqttPublish == nil {
		writeError(w, http.StatusServiceUnavailable, "MQTT not available")
		return
	}

	var req struct {
		Platform      string `json:"platform"`
		DeviceID      string `json:"device_id"`
		TargetVersion string `json:"target_version"`
	}
	if err := decodeJSON(r, &req); err != nil {
		writeError(w, http.StatusBadRequest, "invalid JSON body")
		return
	}

	if req.Platform == "" || req.DeviceID == "" || req.TargetVersion == "" {
		writeError(w, http.StatusBadRequest, "platform, device_id, and target_version are required")
		return
	}

	fw, err := h.db.GetLatestVersion(r.Context(), req.Platform)
	if err != nil {
		writeError(w, http.StatusInternalServerError, "failed to check target version")
		return
	}
	if fw == nil || fw.Version != req.TargetVersion {
		writeError(w, http.StatusBadRequest, "target version not found")
		return
	}

	history := &db.UpgradeHistory{
		Platform:    req.Platform,
		DeviceID:    req.DeviceID,
		SiteID:      h.cfg.SiteID,
		ToVersion:   req.TargetVersion,
		Status:      "pending",
	}
	id, err := h.db.InsertUpgradeHistory(r.Context(), history)
	if err != nil {
		writeError(w, http.StatusInternalServerError, "failed to create upgrade record")
		return
	}

	topic := "EdgeVib/" + h.cfg.SiteID + "/ota/" + req.DeviceID + "/upgrade"
	payload := []byte(`{"target_version":"` + req.TargetVersion + `","file":"` + fw.FileName + `"}`)
	if err := h.mqttPublish(topic, payload); err != nil {
		writeError(w, http.StatusInternalServerError, "failed to send upgrade trigger")
		return
	}

	history.ID = id
	writeJSON(w, http.StatusAccepted, map[string]interface{}{
		"id":      id,
		"status":  "pending",
		"message": "upgrade trigger sent via MQTT",
	})
}

func decodeJSON(r *http.Request, v interface{}) error {
	defer r.Body.Close()
	return json.NewDecoder(r.Body).Decode(v)
}
